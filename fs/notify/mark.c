/*
 *  Copyright (C) 2008 Red Hat, Inc., Eric Paris <eparis@redhat.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * fsnotify inode mark locking/lifetime/and refcnting
 *
 * REFCNT:
 * The group->recnt and mark->refcnt tell how many "things" in the kernel
 * currently are referencing the objects. Both kind of objects typically will
 * live inside the kernel with a refcnt of 2, one for its creation and one for
 * the reference a group and a mark hold to each other.
 * If you are holding the appropriate locks, you can take a reference and the
 * object itself is guaranteed to survive until the reference is dropped.
 *
 * LOCKING:
 * There are 3 locks involved with fsnotify inode marks and they MUST be taken
 * in order as follows:
 *
 * group->mark_mutex
 * mark->lock
 * mark->connector->lock
 *
 * group->mark_mutex protects the marks_list anchored inside a given group and
 * each mark is hooked via the g_list.  It also protects the groups private
 * data (i.e group limits).

 * mark->lock protects the marks attributes like its masks and flags.
 * Furthermore it protects the access to a reference of the group that the mark
 * is assigned to as well as the access to a reference of the inode/vfsmount
 * that is being watched by the mark.
 *
 * mark->connector->lock protects the list of marks anchored inside an
 * inode / vfsmount and each mark is hooked via the i_list.
 *
 * A list of notification marks relating to inode / mnt is contained in
 * fsnotify_mark_connector. That structure is alive as long as there are any
 * marks in the list and is also protected by fsnotify_mark_srcu. A mark gets
 * detached from fsnotify_mark_connector when last reference to the mark is
 * dropped.  Thus having mark reference is enough to protect mark->connector
 * pointer and to make sure fsnotify_mark_connector cannot disappear. Also
 * because we remove mark from g_list before dropping mark reference associated
 * with that, any mark found through g_list is guaranteed to have
 * mark->connector set until we drop group->mark_mutex.
 *
 * LIFETIME:
 * Inode marks survive between when they are added to an inode and when their
 * refcnt==0. Marks are also protected by fsnotify_mark_srcu.
 *
 * The inode mark can be cleared for a number of different reasons including:
 * - The inode is unlinked for the last time.  (fsnotify_inode_remove)
 * - The inode is being evicted from cache. (fsnotify_inode_delete)
 * - The fs the inode is on is unmounted.  (fsnotify_inode_delete/fsnotify_unmount_inodes)
 * - Something explicitly requests that it be removed.  (fsnotify_destroy_mark)
 * - The fsnotify_group associated with the mark is going away and all such marks
 *   need to be cleaned up. (fsnotify_clear_marks_by_group)
 *
 * This has the very interesting property of being able to run concurrently with
 * any (or all) other directions.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>

#include <linux/atomic.h>

#include <linux/fsnotify_backend.h>
#include "fsnotify.h"

#define FSNOTIFY_REAPER_DELAY	(1)	/* 1 jiffy */

struct srcu_struct fsnotify_mark_srcu;
struct kmem_cache *fsnotify_mark_connector_cachep;

static DEFINE_SPINLOCK(destroy_lock);
static LIST_HEAD(destroy_list);
static struct fsnotify_mark_connector *connector_destroy_list;

static void fsnotify_mark_destroy_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(reaper_work, fsnotify_mark_destroy_workfn);

static void fsnotify_connector_destroy_workfn(struct work_struct *work);
static DECLARE_WORK(connector_reaper_work, fsnotify_connector_destroy_workfn);

void fsnotify_get_mark(struct fsnotify_mark *mark)
{
	WARN_ON_ONCE(!atomic_read(&mark->refcnt));
	atomic_inc(&mark->refcnt);
}

static void __fsnotify_recalc_mask(struct fsnotify_mark_connector *conn)
{
	u32 new_mask = 0;
	struct fsnotify_mark *mark;

	assert_spin_locked(&conn->lock);
	hlist_for_each_entry(mark, &conn->list, obj_list) {
		if (mark->flags & FSNOTIFY_MARK_FLAG_ATTACHED)
			new_mask |= mark->mask;
	}
	if (conn->flags & FSNOTIFY_OBJ_TYPE_INODE)
		conn->inode->i_fsnotify_mask = new_mask;
	else if (conn->flags & FSNOTIFY_OBJ_TYPE_VFSMOUNT)
		real_mount(conn->mnt)->mnt_fsnotify_mask = new_mask;
}

/*
 * Calculate mask of events for a list of marks. The caller must make sure
 * connector and connector->inode cannot disappear under us.  Callers achieve
 * this by holding a mark->lock or mark->group->mark_mutex for a mark on this
 * list.
 */
void fsnotify_recalc_mask(struct fsnotify_mark_connector *conn)
{
	if (!conn)
		return;

	spin_lock(&conn->lock);
	__fsnotify_recalc_mask(conn);
	spin_unlock(&conn->lock);
	if (conn->flags & FSNOTIFY_OBJ_TYPE_INODE)
		__fsnotify_update_child_dentry_flags(conn->inode);
}

/* Free all connectors queued for freeing once SRCU period ends */
static void fsnotify_connector_destroy_workfn(struct work_struct *work)
{
	struct fsnotify_mark_connector *conn, *free;

	spin_lock(&destroy_lock);
	conn = connector_destroy_list;
	connector_destroy_list = NULL;
	spin_unlock(&destroy_lock);

	synchronize_srcu(&fsnotify_mark_srcu);
	while (conn) {
		free = conn;
		conn = conn->destroy_next;
		kmem_cache_free(fsnotify_mark_connector_cachep, free);
	}
}

static struct inode *fsnotify_detach_connector_from_object(
					struct fsnotify_mark_connector *conn)
{
	struct inode *inode = NULL;

	if (conn->flags & FSNOTIFY_OBJ_TYPE_INODE) {
		inode = conn->inode;
		rcu_assign_pointer(inode->i_fsnotify_marks, NULL);
		inode->i_fsnotify_mask = 0;
		conn->inode = NULL;
		conn->flags &= ~FSNOTIFY_OBJ_TYPE_INODE;
	} else if (conn->flags & FSNOTIFY_OBJ_TYPE_VFSMOUNT) {
		rcu_assign_pointer(real_mount(conn->mnt)->mnt_fsnotify_marks,
				   NULL);
		real_mount(conn->mnt)->mnt_fsnotify_mask = 0;
		conn->mnt = NULL;
		conn->flags &= ~FSNOTIFY_OBJ_TYPE_VFSMOUNT;
	}

	return inode;
}

static void fsnotify_final_mark_destroy(struct fsnotify_mark *mark)
{
	struct fsnotify_group *group = mark->group;

	if (WARN_ON_ONCE(!group))
		return;
	group->ops->free_mark(mark);
	fsnotify_put_group(group);
}

void fsnotify_put_mark(struct fsnotify_mark *mark)
{
	struct fsnotify_mark_connector *conn;
	struct inode *inode = NULL;
	bool free_conn = false;

	/* Catch marks that were actually never attached to object */
	if (!mark->connector) {
		if (atomic_dec_and_test(&mark->refcnt))
			fsnotify_final_mark_destroy(mark);
		return;
	}

	/*
	 * We have to be careful so that traversals of obj_list under lock can
	 * safely grab mark reference.
	 */
	if (!atomic_dec_and_lock(&mark->refcnt, &mark->connector->lock))
		return;

	conn = mark->connector;
	hlist_del_init_rcu(&mark->obj_list);
	if (hlist_empty(&conn->list)) {
		inode = fsnotify_detach_connector_from_object(conn);
		free_conn = true;
	} else {
		__fsnotify_recalc_mask(conn);
	}
	mark->connector = NULL;
	spin_unlock(&conn->lock);

	iput(inode);

	if (free_conn) {
		spin_lock(&destroy_lock);
		conn->destroy_next = connector_destroy_list;
		connector_destroy_list = conn;
		spin_unlock(&destroy_lock);
		queue_work(system_unbound_wq, &connector_reaper_work);
	}
	/*
	 * Note that we didn't update flags telling whether inode cares about
	 * what's happening with children. We update these flags from
	 * __fsnotify_parent() lazily when next event happens on one of our
	 * children.
	 */
	spin_lock(&destroy_lock);
	list_add(&mark->g_list, &destroy_list);
	spin_unlock(&destroy_lock);
	queue_delayed_work(system_unbound_wq, &reaper_work,
			   FSNOTIFY_REAPER_DELAY);
}

/*
 * Get mark reference when we found the mark via lockless traversal of object
 * list. Mark can be already removed from the list by now and on its way to be
 * destroyed once SRCU period ends.
 *
 * Also pin the group so it doesn't disappear under us.
 */
static bool fsnotify_get_mark_safe(struct fsnotify_mark *mark)
{
	struct fsnotify_group *group;

	if (!mark)
		return true;

	group = mark->group;
	/*
	 * Since acquisition of mark reference is an atomic op as well, we can
	 * be sure this inc is seen before any effect of refcount increment.
	 */
	atomic_inc(&group->user_waits);
	if (atomic_inc_not_zero(&mark->refcnt))
		return true;

	if (atomic_dec_and_test(&group->user_waits) && group->shutdown)
		wake_up(&group->notification_waitq);

	return false;
}

/*
 * Puts marks and wakes up group destruction if necessary.
 *
 * Pairs with fsnotify_get_mark_safe()
 */
static void fsnotify_put_mark_wake(struct fsnotify_mark *mark)
{
	if (mark) {
		struct fsnotify_group *group = mark->group;

		fsnotify_put_mark(mark);
		/*
		 * We abuse notification_waitq on group shutdown for waiting for
		 * all marks pinned when waiting for userspace.
		 */
		if (atomic_dec_and_test(&group->user_waits) && group->shutdown)
			wake_up(&group->notification_waitq);
	}
}

bool fsnotify_prepare_user_wait(struct fsnotify_iter_info *iter_info)
{
	/* This can fail if mark is being removed */
	if (!fsnotify_get_mark_safe(iter_info->inode_mark))
		return false;
	if (!fsnotify_get_mark_safe(iter_info->vfsmount_mark)) {
		fsnotify_put_mark_wake(iter_info->inode_mark);
		return false;
	}

	/*
	 * Now that both marks are pinned by refcount in the inode / vfsmount
	 * lists, we can drop SRCU lock, and safely resume the list iteration
	 * once userspace returns.
	 */
	srcu_read_unlock(&fsnotify_mark_srcu, iter_info->srcu_idx);

	return true;
}

void fsnotify_finish_user_wait(struct fsnotify_iter_info *iter_info)
{
	iter_info->srcu_idx = srcu_read_lock(&fsnotify_mark_srcu);
	fsnotify_put_mark_wake(iter_info->inode_mark);
	fsnotify_put_mark_wake(iter_info->vfsmount_mark);
}

/*
 * Mark mark as detached, remove it from group list. Mark still stays in object
 * list until its last reference is dropped. Note that we rely on mark being
 * removed from group list before corresponding reference to it is dropped. In
 * particular we rely on mark->connector being valid while we hold
 * group->mark_mutex if we found the mark through g_list.
 *
 * Must be called with group->mark_mutex held. The caller must either hold
 * reference to the mark or be protected by fsnotify_mark_srcu.
 */
void fsnotify_detach_mark(struct fsnotify_mark *mark)
{
	struct fsnotify_group *group = mark->group;

	WARN_ON_ONCE(!mutex_is_locked(&group->mark_mutex));
	WARN_ON_ONCE(!srcu_read_lock_held(&fsnotify_mark_srcu) &&
		     atomic_read(&mark->refcnt) < 1 +
			!!(mark->flags & FSNOTIFY_MARK_FLAG_ATTACHED));

	spin_lock(&mark->lock);
	/* something else already called this function on this mark */
	if (!(mark->flags & FSNOTIFY_MARK_FLAG_ATTACHED)) {
		spin_unlock(&mark->lock);
		return;
	}
	mark->flags &= ~FSNOTIFY_MARK_FLAG_ATTACHED;
	list_del_init(&mark->g_list);
	spin_unlock(&mark->lock);

	atomic_dec(&group->num_marks);

	/* Drop mark reference acquired in fsnotify_add_mark_locked() */
	fsnotify_put_mark(mark);
}

/*
 * Free fsnotify mark. The mark is actually only marked as being freed.  The
 * freeing is actually happening only once last reference to the mark is
 * dropped from a workqueue which first waits for srcu period end.
 *
 * Caller must have a reference to the mark or be protected by
 * fsnotify_mark_srcu.
 */
void fsnotify_free_mark(struct fsnotify_mark *mark)
{
	struct fsnotify_group *group = mark->group;

	spin_lock(&mark->lock);
	/* something else already called this function on this mark */
	if (!(mark->flags & FSNOTIFY_MARK_FLAG_ALIVE)) {
		spin_unlock(&mark->lock);
		return;
	}
	mark->flags &= ~FSNOTIFY_MARK_FLAG_ALIVE;
	spin_unlock(&mark->lock);

	/*
	 * Some groups like to know that marks are being freed.  This is a
	 * callback to the group function to let it know that this mark
	 * is being freed.
	 */
	if (group->ops->freeing_mark)
		group->ops->freeing_mark(mark, group);
}

void fsnotify_destroy_mark(struct fsnotify_mark *mark,
			   struct fsnotify_group *group)
{
	mutex_lock_nested(&group->mark_mutex, SINGLE_DEPTH_NESTING);
	fsnotify_detach_mark(mark);
	mutex_unlock(&group->mark_mutex);
	fsnotify_free_mark(mark);
}

/*
 * Sorting function for lists of fsnotify marks.
 *
 * Fanotify supports different notification classes (reflected as priority of
 * notification group). Events shall be passed to notification groups in
 * decreasing priority order. To achieve this marks in notification lists for
 * inodes and vfsmounts are sorted so that priorities of corresponding groups
 * are descending.
 *
 * Furthermore correct handling of the ignore mask requires processing inode
 * and vfsmount marks of each group together. Using the group address as
 * further sort criterion provides a unique sorting order and thus we can
 * merge inode and vfsmount lists of marks in linear time and find groups
 * present in both lists.
 *
 * A return value of 1 signifies that b has priority over a.
 * A return value of 0 signifies that the two marks have to be handled together.
 * A return value of -1 signifies that a has priority over b.
 */
int fsnotify_compare_groups(struct fsnotify_group *a, struct fsnotify_group *b)
{
	if (a == b)
		return 0;
	if (!a)
		return 1;
	if (!b)
		return -1;
	if (a->priority < b->priority)
		return 1;
	if (a->priority > b->priority)
		return -1;
	if (a < b)
		return 1;
	return -1;
}

static int fsnotify_attach_connector_to_object(
				struct fsnotify_mark_connector __rcu **connp,
				struct inode *inode,
				struct vfsmount *mnt)
{
	struct fsnotify_mark_connector *conn;

	conn = kmem_cache_alloc(fsnotify_mark_connector_cachep, GFP_KERNEL);
	if (!conn)
		return -ENOMEM;
	spin_lock_init(&conn->lock);
	INIT_HLIST_HEAD(&conn->list);
	if (inode) {
		conn->flags = FSNOTIFY_OBJ_TYPE_INODE;
		conn->inode = igrab(inode);
	} else {
		conn->flags = FSNOTIFY_OBJ_TYPE_VFSMOUNT;
		conn->mnt = mnt;
	}
	/*
	 * cmpxchg() provides the barrier so that readers of *connp can see
	 * only initialized structure
	 */
	if (cmpxchg(connp, NULL, conn)) {
		/* Someone else created list structure for us */
		if (inode)
			iput(inode);
		kmem_cache_free(fsnotify_mark_connector_cachep, conn);
	}

	return 0;
}

/*
 * Get mark connector, make sure it is alive and return with its lock held.
 * This is for users that get connector pointer from inode or mount. Users that
 * hold reference to a mark on the list may directly lock connector->lock as
 * they are sure list cannot go away under them.
 */
static struct fsnotify_mark_connector *fsnotify_grab_connector(
				struct fsnotify_mark_connector __rcu **connp)
{
	struct fsnotify_mark_connector *conn;
	int idx;

	idx = srcu_read_lock(&fsnotify_mark_srcu);
	conn = srcu_dereference(*connp, &fsnotify_mark_srcu);
	if (!conn)
		goto out;
	spin_lock(&conn->lock);
	if (!(conn->flags & (FSNOTIFY_OBJ_TYPE_INODE |
			     FSNOTIFY_OBJ_TYPE_VFSMOUNT))) {
		spin_unlock(&conn->lock);
		srcu_read_unlock(&fsnotify_mark_srcu, idx);
		return NULL;
	}
out:
	srcu_read_unlock(&fsnotify_mark_srcu, idx);
	return conn;
}

/*
 * Add mark into proper place in given list of marks. These marks may be used
 * for the fsnotify backend to determine which event types should be delivered
 * to which group and for which inodes. These marks are ordered according to
 * priority, highest number first, and then by the group's location in memory.
 */
static int fsnotify_add_mark_list(struct fsnotify_mark *mark,
				  struct inode *inode, struct vfsmount *mnt,
				  int allow_dups)
{
	struct fsnotify_mark *lmark, *last = NULL;
	struct fsnotify_mark_connector *conn;
	struct fsnotify_mark_connector __rcu **connp;
	int cmp;
	int err = 0;

	if (WARN_ON(!inode && !mnt))
		return -EINVAL;
	if (inode)
		connp = &inode->i_fsnotify_marks;
	else
		connp = &real_mount(mnt)->mnt_fsnotify_marks;
restart:
	spin_lock(&mark->lock);
	conn = fsnotify_grab_connector(connp);
	if (!conn) {
		spin_unlock(&mark->lock);
		err = fsnotify_attach_connector_to_object(connp, inode, mnt);
		if (err)
			return err;
		goto restart;
	}

	/* is mark the first mark? */
	if (hlist_empty(&conn->list)) {
		hlist_add_head_rcu(&mark->obj_list, &conn->list);
		goto added;
	}

	/* should mark be in the middle of the current list? */
	hlist_for_each_entry(lmark, &conn->list, obj_list) {
		last = lmark;

		if ((lmark->group == mark->group) &&
		    (lmark->flags & FSNOTIFY_MARK_FLAG_ATTACHED) &&
		    !allow_dups) {
			err = -EEXIST;
			goto out_err;
		}

		cmp = fsnotify_compare_groups(lmark->group, mark->group);
		if (cmp >= 0) {
			hlist_add_before_rcu(&mark->obj_list, &lmark->obj_list);
			goto added;
		}
	}

	BUG_ON(last == NULL);
	/* mark should be the last entry.  last is the current last entry */
	hlist_add_behind_rcu(&mark->obj_list, &last->obj_list);
added:
	mark->connector = conn;
out_err:
	spin_unlock(&conn->lock);
	spin_unlock(&mark->lock);
	return err;
}

/*
 * Attach an initialized mark to a given group and fs object.
 * These marks may be used for the fsnotify backend to determine which
 * event types should be delivered to which group.
 */
int fsnotify_add_mark_locked(struct fsnotify_mark *mark, struct inode *inode,
			     struct vfsmount *mnt, int allow_dups)
{
	struct fsnotify_group *group = mark->group;
	int ret = 0;

	BUG_ON(inode && mnt);
	BUG_ON(!inode && !mnt);
	BUG_ON(!mutex_is_locked(&group->mark_mutex));

	/*
	 * LOCKING ORDER!!!!
	 * group->mark_mutex
	 * mark->lock
	 * mark->connector->lock
	 */
	spin_lock(&mark->lock);
	mark->flags |= FSNOTIFY_MARK_FLAG_ALIVE | FSNOTIFY_MARK_FLAG_ATTACHED;

	list_add(&mark->g_list, &group->marks_list);
	atomic_inc(&group->num_marks);
	fsnotify_get_mark(mark); /* for g_list */
	spin_unlock(&mark->lock);

	ret = fsnotify_add_mark_list(mark, inode, mnt, allow_dups);
	if (ret)
		goto err;

	if (mark->mask)
		fsnotify_recalc_mask(mark->connector);

	return ret;
err:
	mark->flags &= ~(FSNOTIFY_MARK_FLAG_ALIVE |
			 FSNOTIFY_MARK_FLAG_ATTACHED);
	list_del_init(&mark->g_list);
	atomic_dec(&group->num_marks);

	fsnotify_put_mark(mark);
	return ret;
}

int fsnotify_add_mark(struct fsnotify_mark *mark, struct inode *inode,
		      struct vfsmount *mnt, int allow_dups)
{
	int ret;
	struct fsnotify_group *group = mark->group;

	mutex_lock(&group->mark_mutex);
	ret = fsnotify_add_mark_locked(mark, inode, mnt, allow_dups);
	mutex_unlock(&group->mark_mutex);
	return ret;
}

/*
 * Given a list of marks, find the mark associated with given group. If found
 * take a reference to that mark and return it, else return NULL.
 */
struct fsnotify_mark *fsnotify_find_mark(
				struct fsnotify_mark_connector __rcu **connp,
				struct fsnotify_group *group)
{
	struct fsnotify_mark_connector *conn;
	struct fsnotify_mark *mark;

	conn = fsnotify_grab_connector(connp);
	if (!conn)
		return NULL;

	hlist_for_each_entry(mark, &conn->list, obj_list) {
		if (mark->group == group &&
		    (mark->flags & FSNOTIFY_MARK_FLAG_ATTACHED)) {
			fsnotify_get_mark(mark);
			spin_unlock(&conn->lock);
			return mark;
		}
	}
	spin_unlock(&conn->lock);
	return NULL;
}

/* Clear any marks in a group with given type */
void fsnotify_clear_marks_by_group(struct fsnotify_group *group,
				   unsigned int type)
{
	struct fsnotify_mark *lmark, *mark;
	LIST_HEAD(to_free);
	struct list_head *head = &to_free;

	/* Skip selection step if we want to clear all marks. */
	if (type == FSNOTIFY_OBJ_ALL_TYPES) {
		head = &group->marks_list;
		goto clear;
	}
	/*
	 * We have to be really careful here. Anytime we drop mark_mutex, e.g.
	 * fsnotify_clear_marks_by_inode() can come and free marks. Even in our
	 * to_free list so we have to use mark_mutex even when accessing that
	 * list. And freeing mark requires us to drop mark_mutex. So we can
	 * reliably free only the first mark in the list. That's why we first
	 * move marks to free to to_free list in one go and then free marks in
	 * to_free list one by one.
	 */
	mutex_lock_nested(&group->mark_mutex, SINGLE_DEPTH_NESTING);
	list_for_each_entry_safe(mark, lmark, &group->marks_list, g_list) {
		if (mark->connector->flags & type)
			list_move(&mark->g_list, &to_free);
	}
	mutex_unlock(&group->mark_mutex);

clear:
	while (1) {
		mutex_lock_nested(&group->mark_mutex, SINGLE_DEPTH_NESTING);
		if (list_empty(head)) {
			mutex_unlock(&group->mark_mutex);
			break;
		}
		mark = list_first_entry(head, struct fsnotify_mark, g_list);
		fsnotify_get_mark(mark);
		fsnotify_detach_mark(mark);
		mutex_unlock(&group->mark_mutex);
		fsnotify_free_mark(mark);
		fsnotify_put_mark(mark);
	}
}

/* Destroy all marks attached to inode / vfsmount */
void fsnotify_destroy_marks(struct fsnotify_mark_connector __rcu **connp)
{
	struct fsnotify_mark_connector *conn;
	struct fsnotify_mark *mark, *old_mark = NULL;
	struct inode *inode;

	conn = fsnotify_grab_connector(connp);
	if (!conn)
		return;
	/*
	 * We have to be careful since we can race with e.g.
	 * fsnotify_clear_marks_by_group() and once we drop the conn->lock, the
	 * list can get modified. However we are holding mark reference and
	 * thus our mark cannot be removed from obj_list so we can continue
	 * iteration after regaining conn->lock.
	 */
	hlist_for_each_entry(mark, &conn->list, obj_list) {
		fsnotify_get_mark(mark);
		spin_unlock(&conn->lock);
		if (old_mark)
			fsnotify_put_mark(old_mark);
		old_mark = mark;
		fsnotify_destroy_mark(mark, mark->group);
		spin_lock(&conn->lock);
	}
	/*
	 * Detach list from object now so that we don't pin inode until all
	 * mark references get dropped. It would lead to strange results such
	 * as delaying inode deletion or blocking unmount.
	 */
	inode = fsnotify_detach_connector_from_object(conn);
	spin_unlock(&conn->lock);
	if (old_mark)
		fsnotify_put_mark(old_mark);
	iput(inode);
}

/*
 * Nothing fancy, just initialize lists and locks and counters.
 */
void fsnotify_init_mark(struct fsnotify_mark *mark,
			struct fsnotify_group *group)
{
	memset(mark, 0, sizeof(*mark));
	spin_lock_init(&mark->lock);
	atomic_set(&mark->refcnt, 1);
	fsnotify_get_group(group);
	mark->group = group;
}

/*
 * Destroy all marks in destroy_list, waits for SRCU period to finish before
 * actually freeing marks.
 */
static void fsnotify_mark_destroy_workfn(struct work_struct *work)
{
	struct fsnotify_mark *mark, *next;
	struct list_head private_destroy_list;

	spin_lock(&destroy_lock);
	/* exchange the list head */
	list_replace_init(&destroy_list, &private_destroy_list);
	spin_unlock(&destroy_lock);

	synchronize_srcu(&fsnotify_mark_srcu);

	list_for_each_entry_safe(mark, next, &private_destroy_list, g_list) {
		list_del_init(&mark->g_list);
		fsnotify_final_mark_destroy(mark);
	}
}

/* Wait for all marks queued for destruction to be actually destroyed */
void fsnotify_wait_marks_destroyed(void)
{
	flush_delayed_work(&reaper_work);
}
