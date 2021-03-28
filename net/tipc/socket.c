/*
 * net/tipc/socket.c: TIPC socket API
 *
 * Copyright (c) 2001-2007, 2012-2016, Ericsson AB
 * Copyright (c) 2004-2008, 2010-2013, Wind River Systems
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/rhashtable.h>
#include <linux/sched/signal.h>

#include "core.h"
#include "name_table.h"
#include "node.h"
#include "link.h"
#include "name_distr.h"
#include "socket.h"
#include "bcast.h"
#include "netlink.h"

#define CONN_TIMEOUT_DEFAULT	8000	/* default connect timeout = 8s */
#define CONN_PROBING_INTERVAL	msecs_to_jiffies(3600000)  /* [ms] => 1 h */
#define TIPC_FWD_MSG		1
#define TIPC_MAX_PORT		0xffffffff
#define TIPC_MIN_PORT		1
#define TIPC_ACK_RATE		4       /* ACK at 1/4 of of rcv window size */

enum {
	TIPC_LISTEN = TCP_LISTEN,
	TIPC_ESTABLISHED = TCP_ESTABLISHED,
	TIPC_OPEN = TCP_CLOSE,
	TIPC_DISCONNECTING = TCP_CLOSE_WAIT,
	TIPC_CONNECTING = TCP_SYN_SENT,
};

/**
 * struct tipc_sock - TIPC socket structure
 * @sk: socket - interacts with 'port' and with user via the socket API
 * @conn_type: TIPC type used when connection was established
 * @conn_instance: TIPC instance used when connection was established
 * @published: non-zero if port has one or more associated names
 * @max_pkt: maximum packet size "hint" used when building messages sent by port
 * @portid: unique port identity in TIPC socket hash table
 * @phdr: preformatted message header used when sending messages
 * #cong_links: list of congested links
 * @publications: list of publications for port
 * @blocking_link: address of the congested link we are currently sleeping on
 * @pub_count: total # of publications port has made during its lifetime
 * @probing_state:
 * @conn_timeout: the time we can wait for an unresponded setup request
 * @dupl_rcvcnt: number of bytes counted twice, in both backlog and rcv queue
 * @cong_link_cnt: number of congested links
 * @sent_unacked: # messages sent by socket, and not yet acked by peer
 * @rcv_unacked: # messages read by user, but not yet acked back to peer
 * @peer: 'connected' peer for dgram/rdm
 * @node: hash table node
 * @mc_method: cookie for use between socket and broadcast layer
 * @rcu: rcu struct for tipc_sock
 */
struct tipc_sock {
	struct sock sk;
	u32 conn_type;
	u32 conn_instance;
	int published;
	u32 max_pkt;
	u32 portid;
	struct tipc_msg phdr;
	struct list_head cong_links;
	struct list_head publications;
	u32 pub_count;
	uint conn_timeout;
	atomic_t dupl_rcvcnt;
	bool probe_unacked;
	u16 cong_link_cnt;
	u16 snt_unacked;
	u16 snd_win;
	u16 peer_caps;
	u16 rcv_unacked;
	u16 rcv_win;
	struct sockaddr_tipc peer;
	struct rhash_head node;
	struct tipc_mc_method mc_method;
	struct rcu_head rcu;
};

static int tipc_backlog_rcv(struct sock *sk, struct sk_buff *skb);
static void tipc_data_ready(struct sock *sk);
static void tipc_write_space(struct sock *sk);
static void tipc_sock_destruct(struct sock *sk);
static int tipc_release(struct socket *sock);
static int tipc_accept(struct socket *sock, struct socket *new_sock, int flags,
		       bool kern);
static void tipc_sk_timeout(unsigned long data);
static int tipc_sk_publish(struct tipc_sock *tsk, uint scope,
			   struct tipc_name_seq const *seq);
static int tipc_sk_withdraw(struct tipc_sock *tsk, uint scope,
			    struct tipc_name_seq const *seq);
static struct tipc_sock *tipc_sk_lookup(struct net *net, u32 portid);
static int tipc_sk_insert(struct tipc_sock *tsk);
static void tipc_sk_remove(struct tipc_sock *tsk);
static int __tipc_sendstream(struct socket *sock, struct msghdr *m, size_t dsz);
static int __tipc_sendmsg(struct socket *sock, struct msghdr *m, size_t dsz);

static const struct proto_ops packet_ops;
static const struct proto_ops stream_ops;
static const struct proto_ops msg_ops;
static struct proto tipc_proto;
static const struct rhashtable_params tsk_rht_params;

static u32 tsk_own_node(struct tipc_sock *tsk)
{
	return msg_prevnode(&tsk->phdr);
}

static u32 tsk_peer_node(struct tipc_sock *tsk)
{
	return msg_destnode(&tsk->phdr);
}

static u32 tsk_peer_port(struct tipc_sock *tsk)
{
	return msg_destport(&tsk->phdr);
}

static  bool tsk_unreliable(struct tipc_sock *tsk)
{
	return msg_src_droppable(&tsk->phdr) != 0;
}

static void tsk_set_unreliable(struct tipc_sock *tsk, bool unreliable)
{
	msg_set_src_droppable(&tsk->phdr, unreliable ? 1 : 0);
}

static bool tsk_unreturnable(struct tipc_sock *tsk)
{
	return msg_dest_droppable(&tsk->phdr) != 0;
}

static void tsk_set_unreturnable(struct tipc_sock *tsk, bool unreturnable)
{
	msg_set_dest_droppable(&tsk->phdr, unreturnable ? 1 : 0);
}

static int tsk_importance(struct tipc_sock *tsk)
{
	return msg_importance(&tsk->phdr);
}

static int tsk_set_importance(struct tipc_sock *tsk, int imp)
{
	if (imp > TIPC_CRITICAL_IMPORTANCE)
		return -EINVAL;
	msg_set_importance(&tsk->phdr, (u32)imp);
	return 0;
}

static struct tipc_sock *tipc_sk(const struct sock *sk)
{
	return container_of(sk, struct tipc_sock, sk);
}

static bool tsk_conn_cong(struct tipc_sock *tsk)
{
	return tsk->snt_unacked > tsk->snd_win;
}

/* tsk_blocks(): translate a buffer size in bytes to number of
 * advertisable blocks, taking into account the ratio truesize(len)/len
 * We can trust that this ratio is always < 4 for len >= FLOWCTL_BLK_SZ
 */
static u16 tsk_adv_blocks(int len)
{
	return len / FLOWCTL_BLK_SZ / 4;
}

/* tsk_inc(): increment counter for sent or received data
 * - If block based flow control is not supported by peer we
 *   fall back to message based ditto, incrementing the counter
 */
static u16 tsk_inc(struct tipc_sock *tsk, int msglen)
{
	if (likely(tsk->peer_caps & TIPC_BLOCK_FLOWCTL))
		return ((msglen / FLOWCTL_BLK_SZ) + 1);
	return 1;
}

/**
 * tsk_advance_rx_queue - discard first buffer in socket receive queue
 *
 * Caller must hold socket lock
 */
static void tsk_advance_rx_queue(struct sock *sk)
{
	kfree_skb(__skb_dequeue(&sk->sk_receive_queue));
}

/* tipc_sk_respond() : send response message back to sender
 */
static void tipc_sk_respond(struct sock *sk, struct sk_buff *skb, int err)
{
	u32 selector;
	u32 dnode;
	u32 onode = tipc_own_addr(sock_net(sk));

	if (!tipc_msg_reverse(onode, &skb, err))
		return;

	dnode = msg_destnode(buf_msg(skb));
	selector = msg_origport(buf_msg(skb));
	tipc_node_xmit_skb(sock_net(sk), skb, dnode, selector);
}

/**
 * tsk_rej_rx_queue - reject all buffers in socket receive queue
 *
 * Caller must hold socket lock
 */
static void tsk_rej_rx_queue(struct sock *sk)
{
	struct sk_buff *skb;

	while ((skb = __skb_dequeue(&sk->sk_receive_queue)))
		tipc_sk_respond(sk, skb, TIPC_ERR_NO_PORT);
}

static bool tipc_sk_connected(struct sock *sk)
{
	return sk->sk_state == TIPC_ESTABLISHED;
}

/* tipc_sk_type_connectionless - check if the socket is datagram socket
 * @sk: socket
 *
 * Returns true if connection less, false otherwise
 */
static bool tipc_sk_type_connectionless(struct sock *sk)
{
	return sk->sk_type == SOCK_RDM || sk->sk_type == SOCK_DGRAM;
}

/* tsk_peer_msg - verify if message was sent by connected port's peer
 *
 * Handles cases where the node's network address has changed from
 * the default of <0.0.0> to its configured setting.
 */
static bool tsk_peer_msg(struct tipc_sock *tsk, struct tipc_msg *msg)
{
	struct sock *sk = &tsk->sk;
	struct tipc_net *tn = net_generic(sock_net(sk), tipc_net_id);
	u32 peer_port = tsk_peer_port(tsk);
	u32 orig_node;
	u32 peer_node;

	if (unlikely(!tipc_sk_connected(sk)))
		return false;

	if (unlikely(msg_origport(msg) != peer_port))
		return false;

	orig_node = msg_orignode(msg);
	peer_node = tsk_peer_node(tsk);

	if (likely(orig_node == peer_node))
		return true;

	if (!orig_node && (peer_node == tn->own_addr))
		return true;

	if (!peer_node && (orig_node == tn->own_addr))
		return true;

	return false;
}

/* tipc_set_sk_state - set the sk_state of the socket
 * @sk: socket
 *
 * Caller must hold socket lock
 *
 * Returns 0 on success, errno otherwise
 */
static int tipc_set_sk_state(struct sock *sk, int state)
{
	int oldsk_state = sk->sk_state;
	int res = -EINVAL;

	switch (state) {
	case TIPC_OPEN:
		res = 0;
		break;
	case TIPC_LISTEN:
	case TIPC_CONNECTING:
		if (oldsk_state == TIPC_OPEN)
			res = 0;
		break;
	case TIPC_ESTABLISHED:
		if (oldsk_state == TIPC_CONNECTING ||
		    oldsk_state == TIPC_OPEN)
			res = 0;
		break;
	case TIPC_DISCONNECTING:
		if (oldsk_state == TIPC_CONNECTING ||
		    oldsk_state == TIPC_ESTABLISHED)
			res = 0;
		break;
	}

	if (!res)
		sk->sk_state = state;

	return res;
}

static int tipc_sk_sock_err(struct socket *sock, long *timeout)
{
	struct sock *sk = sock->sk;
	int err = sock_error(sk);
	int typ = sock->type;

	if (err)
		return err;
	if (typ == SOCK_STREAM || typ == SOCK_SEQPACKET) {
		if (sk->sk_state == TIPC_DISCONNECTING)
			return -EPIPE;
		else if (!tipc_sk_connected(sk))
			return -ENOTCONN;
	}
	if (!*timeout)
		return -EAGAIN;
	if (signal_pending(current))
		return sock_intr_errno(*timeout);

	return 0;
}

#define tipc_wait_for_cond(sock_, timeo_, condition_)			       \
({                                                                             \
	struct sock *sk_;						       \
	int rc_;							       \
									       \
	while ((rc_ = !(condition_))) {					       \
		DEFINE_WAIT_FUNC(wait_, woken_wake_function);	               \
		sk_ = (sock_)->sk;					       \
		rc_ = tipc_sk_sock_err((sock_), timeo_);		       \
		if (rc_)						       \
			break;						       \
		prepare_to_wait(sk_sleep(sk_), &wait_, TASK_INTERRUPTIBLE);    \
		release_sock(sk_);					       \
		*(timeo_) = wait_woken(&wait_, TASK_INTERRUPTIBLE, *(timeo_)); \
		sched_annotate_sleep();				               \
		lock_sock(sk_);						       \
		remove_wait_queue(sk_sleep(sk_), &wait_);		       \
	}								       \
	rc_;								       \
})

/**
 * tipc_sk_create - create a TIPC socket
 * @net: network namespace (must be default network)
 * @sock: pre-allocated socket structure
 * @protocol: protocol indicator (must be 0)
 * @kern: caused by kernel or by userspace?
 *
 * This routine creates additional data structures used by the TIPC socket,
 * initializes them, and links them together.
 *
 * Returns 0 on success, errno otherwise
 */
static int tipc_sk_create(struct net *net, struct socket *sock,
			  int protocol, int kern)
{
	struct tipc_net *tn;
	const struct proto_ops *ops;
	struct sock *sk;
	struct tipc_sock *tsk;
	struct tipc_msg *msg;

	/* Validate arguments */
	if (unlikely(protocol != 0))
		return -EPROTONOSUPPORT;

	switch (sock->type) {
	case SOCK_STREAM:
		ops = &stream_ops;
		break;
	case SOCK_SEQPACKET:
		ops = &packet_ops;
		break;
	case SOCK_DGRAM:
	case SOCK_RDM:
		ops = &msg_ops;
		break;
	default:
		return -EPROTOTYPE;
	}

	/* Allocate socket's protocol area */
	sk = sk_alloc(net, AF_TIPC, GFP_KERNEL, &tipc_proto, kern);
	if (sk == NULL)
		return -ENOMEM;

	tsk = tipc_sk(sk);
	tsk->max_pkt = MAX_PKT_DEFAULT;
	INIT_LIST_HEAD(&tsk->publications);
	INIT_LIST_HEAD(&tsk->cong_links);
	msg = &tsk->phdr;
	tn = net_generic(sock_net(sk), tipc_net_id);

	/* Finish initializing socket data structures */
	sock->ops = ops;
	sock_init_data(sock, sk);
	tipc_set_sk_state(sk, TIPC_OPEN);
	if (tipc_sk_insert(tsk)) {
		pr_warn("Socket create failed; port number exhausted\n");
		return -EINVAL;
	}

	/* Ensure tsk is visible before we read own_addr. */
	smp_mb();

	tipc_msg_init(tn->own_addr, msg, TIPC_LOW_IMPORTANCE, TIPC_NAMED_MSG,
		      NAMED_H_SIZE, 0);

	msg_set_origport(msg, tsk->portid);
	setup_timer(&sk->sk_timer, tipc_sk_timeout, (unsigned long)tsk);
	sk->sk_shutdown = 0;
	sk->sk_backlog_rcv = tipc_backlog_rcv;
	sk->sk_rcvbuf = sysctl_tipc_rmem[1];
	sk->sk_data_ready = tipc_data_ready;
	sk->sk_write_space = tipc_write_space;
	sk->sk_destruct = tipc_sock_destruct;
	tsk->conn_timeout = CONN_TIMEOUT_DEFAULT;
	atomic_set(&tsk->dupl_rcvcnt, 0);

	/* Start out with safe limits until we receive an advertised window */
	tsk->snd_win = tsk_adv_blocks(RCVBUF_MIN);
	tsk->rcv_win = tsk->snd_win;

	if (tipc_sk_type_connectionless(sk)) {
		tsk_set_unreturnable(tsk, true);
		if (sock->type == SOCK_DGRAM)
			tsk_set_unreliable(tsk, true);
	}

	return 0;
}

static void tipc_sk_callback(struct rcu_head *head)
{
	struct tipc_sock *tsk = container_of(head, struct tipc_sock, rcu);

	sock_put(&tsk->sk);
}

/* Caller should hold socket lock for the socket. */
static void __tipc_shutdown(struct socket *sock, int error)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	struct net *net = sock_net(sk);
	long timeout = msecs_to_jiffies(CONN_TIMEOUT_DEFAULT);
	u32 dnode = tsk_peer_node(tsk);
	struct sk_buff *skb;

	/* Avoid that hi-prio shutdown msgs bypass msgs in link wakeup queue */
	tipc_wait_for_cond(sock, &timeout, (!tsk->cong_link_cnt &&
					    !tsk_conn_cong(tsk)));

	/* Reject all unreceived messages, except on an active connection
	 * (which disconnects locally & sends a 'FIN+' to peer).
	 */
	while ((skb = __skb_dequeue(&sk->sk_receive_queue)) != NULL) {
		if (TIPC_SKB_CB(skb)->bytes_read) {
			kfree_skb(skb);
			continue;
		}
		if (!tipc_sk_type_connectionless(sk) &&
		    sk->sk_state != TIPC_DISCONNECTING) {
			tipc_set_sk_state(sk, TIPC_DISCONNECTING);
			tipc_node_remove_conn(net, dnode, tsk->portid);
		}
		tipc_sk_respond(sk, skb, error);
	}

	if (tipc_sk_type_connectionless(sk))
		return;

	if (sk->sk_state != TIPC_DISCONNECTING) {
		skb = tipc_msg_create(TIPC_CRITICAL_IMPORTANCE,
				      TIPC_CONN_MSG, SHORT_H_SIZE, 0, dnode,
				      tsk_own_node(tsk), tsk_peer_port(tsk),
				      tsk->portid, error);
		if (skb)
			tipc_node_xmit_skb(net, skb, dnode, tsk->portid);
		tipc_node_remove_conn(net, dnode, tsk->portid);
		tipc_set_sk_state(sk, TIPC_DISCONNECTING);
	}
}

/**
 * tipc_release - destroy a TIPC socket
 * @sock: socket to destroy
 *
 * This routine cleans up any messages that are still queued on the socket.
 * For DGRAM and RDM socket types, all queued messages are rejected.
 * For SEQPACKET and STREAM socket types, the first message is rejected
 * and any others are discarded.  (If the first message on a STREAM socket
 * is partially-read, it is discarded and the next one is rejected instead.)
 *
 * NOTE: Rejected messages are not necessarily returned to the sender!  They
 * are returned or discarded according to the "destination droppable" setting
 * specified for the message by the sender.
 *
 * Returns 0 on success, errno otherwise
 */
static int tipc_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk;

	/*
	 * Exit if socket isn't fully initialized (occurs when a failed accept()
	 * releases a pre-allocated child socket that was never used)
	 */
	if (sk == NULL)
		return 0;

	tsk = tipc_sk(sk);
	lock_sock(sk);

	__tipc_shutdown(sock, TIPC_ERR_NO_PORT);
	sk->sk_shutdown = SHUTDOWN_MASK;
	tipc_sk_withdraw(tsk, 0, NULL);
	sk_stop_timer(sk, &sk->sk_timer);
	tipc_sk_remove(tsk);

	/* Reject any messages that accumulated in backlog queue */
	release_sock(sk);
	u32_list_purge(&tsk->cong_links);
	tsk->cong_link_cnt = 0;
	call_rcu(&tsk->rcu, tipc_sk_callback);
	sock->sk = NULL;

	return 0;
}

/**
 * tipc_bind - associate or disassocate TIPC name(s) with a socket
 * @sock: socket structure
 * @uaddr: socket address describing name(s) and desired operation
 * @uaddr_len: size of socket address data structure
 *
 * Name and name sequence binding is indicated using a positive scope value;
 * a negative scope value unbinds the specified name.  Specifying no name
 * (i.e. a socket address length of 0) unbinds all names from the socket.
 *
 * Returns 0 on success, errno otherwise
 *
 * NOTE: This routine doesn't need to take the socket lock since it doesn't
 *       access any non-constant socket information.
 */
static int tipc_bind(struct socket *sock, struct sockaddr *uaddr,
		     int uaddr_len)
{
	struct sock *sk = sock->sk;
	struct sockaddr_tipc *addr = (struct sockaddr_tipc *)uaddr;
	struct tipc_sock *tsk = tipc_sk(sk);
	int res = -EINVAL;

	lock_sock(sk);
	if (unlikely(!uaddr_len)) {
		res = tipc_sk_withdraw(tsk, 0, NULL);
		goto exit;
	}

	if (uaddr_len < sizeof(struct sockaddr_tipc)) {
		res = -EINVAL;
		goto exit;
	}
	if (addr->family != AF_TIPC) {
		res = -EAFNOSUPPORT;
		goto exit;
	}

	if (addr->addrtype == TIPC_ADDR_NAME)
		addr->addr.nameseq.upper = addr->addr.nameseq.lower;
	else if (addr->addrtype != TIPC_ADDR_NAMESEQ) {
		res = -EAFNOSUPPORT;
		goto exit;
	}

	if ((addr->addr.nameseq.type < TIPC_RESERVED_TYPES) &&
	    (addr->addr.nameseq.type != TIPC_TOP_SRV) &&
	    (addr->addr.nameseq.type != TIPC_CFG_SRV)) {
		res = -EACCES;
		goto exit;
	}

	res = (addr->scope > 0) ?
		tipc_sk_publish(tsk, addr->scope, &addr->addr.nameseq) :
		tipc_sk_withdraw(tsk, -addr->scope, &addr->addr.nameseq);
exit:
	release_sock(sk);
	return res;
}

/**
 * tipc_getname - get port ID of socket or peer socket
 * @sock: socket structure
 * @uaddr: area for returned socket address
 * @uaddr_len: area for returned length of socket address
 * @peer: 0 = own ID, 1 = current peer ID, 2 = current/former peer ID
 *
 * Returns 0 on success, errno otherwise
 *
 * NOTE: This routine doesn't need to take the socket lock since it only
 *       accesses socket information that is unchanging (or which changes in
 *       a completely predictable manner).
 */
static int tipc_getname(struct socket *sock, struct sockaddr *uaddr,
			int *uaddr_len, int peer)
{
	struct sockaddr_tipc *addr = (struct sockaddr_tipc *)uaddr;
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	struct tipc_net *tn = net_generic(sock_net(sock->sk), tipc_net_id);

	memset(addr, 0, sizeof(*addr));
	if (peer) {
		if ((!tipc_sk_connected(sk)) &&
		    ((peer != 2) || (sk->sk_state != TIPC_DISCONNECTING)))
			return -ENOTCONN;
		addr->addr.id.ref = tsk_peer_port(tsk);
		addr->addr.id.node = tsk_peer_node(tsk);
	} else {
		addr->addr.id.ref = tsk->portid;
		addr->addr.id.node = tn->own_addr;
	}

	*uaddr_len = sizeof(*addr);
	addr->addrtype = TIPC_ADDR_ID;
	addr->family = AF_TIPC;
	addr->scope = 0;
	addr->addr.name.domain = 0;

	return 0;
}

/**
 * tipc_poll - read and possibly block on pollmask
 * @file: file structure associated with the socket
 * @sock: socket for which to calculate the poll bits
 * @wait: ???
 *
 * Returns pollmask value
 *
 * COMMENTARY:
 * It appears that the usual socket locking mechanisms are not useful here
 * since the pollmask info is potentially out-of-date the moment this routine
 * exits.  TCP and other protocols seem to rely on higher level poll routines
 * to handle any preventable race conditions, so TIPC will do the same ...
 *
 * IMPORTANT: The fact that a read or write operation is indicated does NOT
 * imply that the operation will succeed, merely that it should be performed
 * and will not block.
 */
static unsigned int tipc_poll(struct file *file, struct socket *sock,
			      poll_table *wait)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	u32 mask = 0;

	sock_poll_wait(file, sk_sleep(sk), wait);

	if (sk->sk_shutdown & RCV_SHUTDOWN)
		mask |= POLLRDHUP | POLLIN | POLLRDNORM;
	if (sk->sk_shutdown == SHUTDOWN_MASK)
		mask |= POLLHUP;

	switch (sk->sk_state) {
	case TIPC_ESTABLISHED:
		if (!tsk->cong_link_cnt && !tsk_conn_cong(tsk))
			mask |= POLLOUT;
		/* fall thru' */
	case TIPC_LISTEN:
	case TIPC_CONNECTING:
		if (!skb_queue_empty_lockless(&sk->sk_receive_queue))
			mask |= (POLLIN | POLLRDNORM);
		break;
	case TIPC_OPEN:
		if (!tsk->cong_link_cnt)
			mask |= POLLOUT;
		if (tipc_sk_type_connectionless(sk) &&
		    (!skb_queue_empty_lockless(&sk->sk_receive_queue)))
			mask |= (POLLIN | POLLRDNORM);
		break;
	case TIPC_DISCONNECTING:
		mask = (POLLIN | POLLRDNORM | POLLHUP);
		break;
	}

	return mask;
}

/**
 * tipc_sendmcast - send multicast message
 * @sock: socket structure
 * @seq: destination address
 * @msg: message to send
 * @dlen: length of data to send
 * @timeout: timeout to wait for wakeup
 *
 * Called from function tipc_sendmsg(), which has done all sanity checks
 * Returns the number of bytes sent on success, or errno
 */
static int tipc_sendmcast(struct  socket *sock, struct tipc_name_seq *seq,
			  struct msghdr *msg, size_t dlen, long timeout)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	struct tipc_msg *hdr = &tsk->phdr;
	struct net *net = sock_net(sk);
	int mtu = tipc_bcast_get_mtu(net);
	struct tipc_mc_method *method = &tsk->mc_method;
	u32 domain = addr_domain(net, TIPC_CLUSTER_SCOPE);
	struct sk_buff_head pkts;
	struct tipc_nlist dsts;
	int rc;

	/* Block or return if any destination link is congested */
	rc = tipc_wait_for_cond(sock, &timeout, !tsk->cong_link_cnt);
	if (unlikely(rc))
		return rc;

	/* Lookup destination nodes */
	tipc_nlist_init(&dsts, tipc_own_addr(net));
	tipc_nametbl_lookup_dst_nodes(net, seq->type, seq->lower,
				      seq->upper, domain, &dsts);
	if (!dsts.local && !dsts.remote)
		return -EHOSTUNREACH;

	/* Build message header */
	msg_set_type(hdr, TIPC_MCAST_MSG);
	msg_set_hdr_sz(hdr, MCAST_H_SIZE);
	msg_set_lookup_scope(hdr, TIPC_CLUSTER_SCOPE);
	msg_set_destport(hdr, 0);
	msg_set_destnode(hdr, 0);
	msg_set_nametype(hdr, seq->type);
	msg_set_namelower(hdr, seq->lower);
	msg_set_nameupper(hdr, seq->upper);

	/* Build message as chain of buffers */
	skb_queue_head_init(&pkts);
	rc = tipc_msg_build(hdr, msg, 0, dlen, mtu, &pkts);

	/* Send message if build was successful */
	if (unlikely(rc == dlen))
		rc = tipc_mcast_xmit(net, &pkts, method, &dsts,
				     &tsk->cong_link_cnt);

	tipc_nlist_purge(&dsts);

	return rc ? rc : dlen;
}

/**
 * tipc_sk_mcast_rcv - Deliver multicast messages to all destination sockets
 * @arrvq: queue with arriving messages, to be cloned after destination lookup
 * @inputq: queue with cloned messages, delivered to socket after dest lookup
 *
 * Multi-threaded: parallel calls with reference to same queues may occur
 */
void tipc_sk_mcast_rcv(struct net *net, struct sk_buff_head *arrvq,
		       struct sk_buff_head *inputq)
{
	struct tipc_msg *msg;
	struct list_head dports;
	u32 portid;
	u32 scope = TIPC_CLUSTER_SCOPE;
	struct sk_buff_head tmpq;
	uint hsz;
	struct sk_buff *skb, *_skb;

	__skb_queue_head_init(&tmpq);
	INIT_LIST_HEAD(&dports);

	skb = tipc_skb_peek(arrvq, &inputq->lock);
	for (; skb; skb = tipc_skb_peek(arrvq, &inputq->lock)) {
		msg = buf_msg(skb);
		hsz = skb_headroom(skb) + msg_hdr_sz(msg);

		if (in_own_node(net, msg_orignode(msg)))
			scope = TIPC_NODE_SCOPE;

		/* Create destination port list and message clones: */
		tipc_nametbl_mc_translate(net,
					  msg_nametype(msg), msg_namelower(msg),
					  msg_nameupper(msg), scope, &dports);
		portid = u32_pop(&dports);
		for (; portid; portid = u32_pop(&dports)) {
			_skb = __pskb_copy(skb, hsz, GFP_ATOMIC);
			if (_skb) {
				msg_set_destport(buf_msg(_skb), portid);
				__skb_queue_tail(&tmpq, _skb);
				continue;
			}
			pr_warn("Failed to clone mcast rcv buffer\n");
		}
		/* Append to inputq if not already done by other thread */
		spin_lock_bh(&inputq->lock);
		if (skb_peek(arrvq) == skb) {
			skb_queue_splice_tail_init(&tmpq, inputq);
			__skb_dequeue(arrvq);
		}
		spin_unlock_bh(&inputq->lock);
		__skb_queue_purge(&tmpq);
		kfree_skb(skb);
	}
	tipc_sk_rcv(net, inputq);
}

/**
 * tipc_sk_proto_rcv - receive a connection mng protocol message
 * @tsk: receiving socket
 * @skb: pointer to message buffer.
 */
static void tipc_sk_proto_rcv(struct tipc_sock *tsk, struct sk_buff *skb,
			      struct sk_buff_head *xmitq)
{
	struct sock *sk = &tsk->sk;
	u32 onode = tsk_own_node(tsk);
	struct tipc_msg *hdr = buf_msg(skb);
	int mtyp = msg_type(hdr);
	bool conn_cong;

	/* Ignore if connection cannot be validated: */
	if (!tsk_peer_msg(tsk, hdr))
		goto exit;

	if (unlikely(msg_errcode(hdr))) {
		tipc_set_sk_state(sk, TIPC_DISCONNECTING);
		tipc_node_remove_conn(sock_net(sk), tsk_peer_node(tsk),
				      tsk_peer_port(tsk));
		sk->sk_state_change(sk);
		goto exit;
	}

	tsk->probe_unacked = false;

	if (mtyp == CONN_PROBE) {
		msg_set_type(hdr, CONN_PROBE_REPLY);
		if (tipc_msg_reverse(onode, &skb, TIPC_OK))
			__skb_queue_tail(xmitq, skb);
		return;
	} else if (mtyp == CONN_ACK) {
		conn_cong = tsk_conn_cong(tsk);
		tsk->snt_unacked -= msg_conn_ack(hdr);
		if (tsk->peer_caps & TIPC_BLOCK_FLOWCTL)
			tsk->snd_win = msg_adv_win(hdr);
		if (conn_cong)
			sk->sk_write_space(sk);
	} else if (mtyp != CONN_PROBE_REPLY) {
		pr_warn("Received unknown CONN_PROTO msg\n");
	}
exit:
	kfree_skb(skb);
}

/**
 * tipc_sendmsg - send message in connectionless manner
 * @sock: socket structure
 * @m: message to send
 * @dsz: amount of user data to be sent
 *
 * Message must have an destination specified explicitly.
 * Used for SOCK_RDM and SOCK_DGRAM messages,
 * and for 'SYN' messages on SOCK_SEQPACKET and SOCK_STREAM connections.
 * (Note: 'SYN+' is prohibited on SOCK_STREAM.)
 *
 * Returns the number of bytes sent on success, or errno otherwise
 */
static int tipc_sendmsg(struct socket *sock,
			struct msghdr *m, size_t dsz)
{
	struct sock *sk = sock->sk;
	int ret;

	lock_sock(sk);
	ret = __tipc_sendmsg(sock, m, dsz);
	release_sock(sk);

	return ret;
}

static int __tipc_sendmsg(struct socket *sock, struct msghdr *m, size_t dlen)
{
	struct sock *sk = sock->sk;
	struct net *net = sock_net(sk);
	struct tipc_sock *tsk = tipc_sk(sk);
	DECLARE_SOCKADDR(struct sockaddr_tipc *, dest, m->msg_name);
	long timeout = sock_sndtimeo(sk, m->msg_flags & MSG_DONTWAIT);
	struct list_head *clinks = &tsk->cong_links;
	bool syn = !tipc_sk_type_connectionless(sk);
	struct tipc_msg *hdr = &tsk->phdr;
	struct tipc_name_seq *seq;
	struct sk_buff_head pkts;
	u32 type, inst, domain;
	u32 dnode, dport;
	int mtu, rc;

	if (unlikely(dlen > TIPC_MAX_USER_MSG_SIZE))
		return -EMSGSIZE;

	if (unlikely(!dest)) {
		dest = &tsk->peer;
		if (!syn && dest->family != AF_TIPC)
			return -EDESTADDRREQ;
	}

	if (unlikely(m->msg_namelen < sizeof(*dest)))
		return -EINVAL;

	if (unlikely(dest->family != AF_TIPC))
		return -EINVAL;

	if (unlikely(syn)) {
		if (sk->sk_state == TIPC_LISTEN)
			return -EPIPE;
		if (sk->sk_state != TIPC_OPEN)
			return -EISCONN;
		if (tsk->published)
			return -EOPNOTSUPP;
		if (dest->addrtype == TIPC_ADDR_NAME) {
			tsk->conn_type = dest->addr.name.name.type;
			tsk->conn_instance = dest->addr.name.name.instance;
		}
	}

	seq = &dest->addr.nameseq;
	if (dest->addrtype == TIPC_ADDR_MCAST)
		return tipc_sendmcast(sock, seq, m, dlen, timeout);

	if (dest->addrtype == TIPC_ADDR_NAME) {
		type = dest->addr.name.name.type;
		inst = dest->addr.name.name.instance;
		domain = dest->addr.name.domain;
		dnode = domain;
		msg_set_type(hdr, TIPC_NAMED_MSG);
		msg_set_hdr_sz(hdr, NAMED_H_SIZE);
		msg_set_nametype(hdr, type);
		msg_set_nameinst(hdr, inst);
		msg_set_lookup_scope(hdr, tipc_addr_scope(domain));
		dport = tipc_nametbl_translate(net, type, inst, &dnode);
		msg_set_destnode(hdr, dnode);
		msg_set_destport(hdr, dport);
		if (unlikely(!dport && !dnode))
			return -EHOSTUNREACH;

	} else if (dest->addrtype == TIPC_ADDR_ID) {
		dnode = dest->addr.id.node;
		msg_set_type(hdr, TIPC_DIRECT_MSG);
		msg_set_lookup_scope(hdr, 0);
		msg_set_destnode(hdr, dnode);
		msg_set_destport(hdr, dest->addr.id.ref);
		msg_set_hdr_sz(hdr, BASIC_H_SIZE);
	}

	/* Block or return if destination link is congested */
	rc = tipc_wait_for_cond(sock, &timeout, !u32_find(clinks, dnode));
	if (unlikely(rc))
		return rc;

	skb_queue_head_init(&pkts);
	mtu = tipc_node_get_mtu(net, dnode, tsk->portid);
	rc = tipc_msg_build(hdr, m, 0, dlen, mtu, &pkts);
	if (unlikely(rc != dlen))
		return rc;

	rc = tipc_node_xmit(net, &pkts, dnode, tsk->portid);
	if (unlikely(rc == -ELINKCONG)) {
		u32_push(clinks, dnode);
		tsk->cong_link_cnt++;
		rc = 0;
	}

	if (unlikely(syn && !rc))
		tipc_set_sk_state(sk, TIPC_CONNECTING);

	return rc ? rc : dlen;
}

/**
 * tipc_sendstream - send stream-oriented data
 * @sock: socket structure
 * @m: data to send
 * @dsz: total length of data to be transmitted
 *
 * Used for SOCK_STREAM data.
 *
 * Returns the number of bytes sent on success (or partial success),
 * or errno if no data sent
 */
static int tipc_sendstream(struct socket *sock, struct msghdr *m, size_t dsz)
{
	struct sock *sk = sock->sk;
	int ret;

	lock_sock(sk);
	ret = __tipc_sendstream(sock, m, dsz);
	release_sock(sk);

	return ret;
}

static int __tipc_sendstream(struct socket *sock, struct msghdr *m, size_t dlen)
{
	struct sock *sk = sock->sk;
	DECLARE_SOCKADDR(struct sockaddr_tipc *, dest, m->msg_name);
	long timeout = sock_sndtimeo(sk, m->msg_flags & MSG_DONTWAIT);
	struct tipc_sock *tsk = tipc_sk(sk);
	struct tipc_msg *hdr = &tsk->phdr;
	struct net *net = sock_net(sk);
	struct sk_buff_head pkts;
	u32 dnode = tsk_peer_node(tsk);
	int send, sent = 0;
	int rc = 0;

	skb_queue_head_init(&pkts);

	if (unlikely(dlen > INT_MAX))
		return -EMSGSIZE;

	/* Handle implicit connection setup */
	if (unlikely(dest)) {
		rc = __tipc_sendmsg(sock, m, dlen);
		if (dlen && dlen == rc) {
			tsk->peer_caps = tipc_node_get_capabilities(net, dnode);
			tsk->snt_unacked = tsk_inc(tsk, dlen + msg_hdr_sz(hdr));
		}
		return rc;
	}

	do {
		rc = tipc_wait_for_cond(sock, &timeout,
					(!tsk->cong_link_cnt &&
					 !tsk_conn_cong(tsk) &&
					 tipc_sk_connected(sk)));
		if (unlikely(rc))
			break;

		send = min_t(size_t, dlen - sent, TIPC_MAX_USER_MSG_SIZE);
		rc = tipc_msg_build(hdr, m, sent, send, tsk->max_pkt, &pkts);
		if (unlikely(rc != send))
			break;

		rc = tipc_node_xmit(net, &pkts, dnode, tsk->portid);
		if (unlikely(rc == -ELINKCONG)) {
			tsk->cong_link_cnt = 1;
			rc = 0;
		}
		if (likely(!rc)) {
			tsk->snt_unacked += tsk_inc(tsk, send + MIN_H_SIZE);
			sent += send;
		}
	} while (sent < dlen && !rc);

	return sent ? sent : rc;
}

/**
 * tipc_send_packet - send a connection-oriented message
 * @sock: socket structure
 * @m: message to send
 * @dsz: length of data to be transmitted
 *
 * Used for SOCK_SEQPACKET messages.
 *
 * Returns the number of bytes sent on success, or errno otherwise
 */
static int tipc_send_packet(struct socket *sock, struct msghdr *m, size_t dsz)
{
	if (dsz > TIPC_MAX_USER_MSG_SIZE)
		return -EMSGSIZE;

	return tipc_sendstream(sock, m, dsz);
}

/* tipc_sk_finish_conn - complete the setup of a connection
 */
static void tipc_sk_finish_conn(struct tipc_sock *tsk, u32 peer_port,
				u32 peer_node)
{
	struct sock *sk = &tsk->sk;
	struct net *net = sock_net(sk);
	struct tipc_msg *msg = &tsk->phdr;

	msg_set_destnode(msg, peer_node);
	msg_set_destport(msg, peer_port);
	msg_set_type(msg, TIPC_CONN_MSG);
	msg_set_lookup_scope(msg, 0);
	msg_set_hdr_sz(msg, SHORT_H_SIZE);

	sk_reset_timer(sk, &sk->sk_timer, jiffies + CONN_PROBING_INTERVAL);
	tipc_set_sk_state(sk, TIPC_ESTABLISHED);
	tipc_node_add_conn(net, peer_node, tsk->portid, peer_port);
	tsk->max_pkt = tipc_node_get_mtu(net, peer_node, tsk->portid);
	tsk->peer_caps = tipc_node_get_capabilities(net, peer_node);
	if (tsk->peer_caps & TIPC_BLOCK_FLOWCTL)
		return;

	/* Fall back to message based flow control */
	tsk->rcv_win = FLOWCTL_MSG_WIN;
	tsk->snd_win = FLOWCTL_MSG_WIN;
}

/**
 * set_orig_addr - capture sender's address for received message
 * @m: descriptor for message info
 * @msg: received message header
 *
 * Note: Address is not captured if not requested by receiver.
 */
static void set_orig_addr(struct msghdr *m, struct tipc_msg *msg)
{
	DECLARE_SOCKADDR(struct sockaddr_tipc *, addr, m->msg_name);

	if (addr) {
		addr->family = AF_TIPC;
		addr->addrtype = TIPC_ADDR_ID;
		memset(&addr->addr, 0, sizeof(addr->addr));
		addr->addr.id.ref = msg_origport(msg);
		addr->addr.id.node = msg_orignode(msg);
		addr->addr.name.domain = 0;	/* could leave uninitialized */
		addr->scope = 0;		/* could leave uninitialized */
		m->msg_namelen = sizeof(struct sockaddr_tipc);
	}
}

/**
 * tipc_sk_anc_data_recv - optionally capture ancillary data for received message
 * @m: descriptor for message info
 * @msg: received message header
 * @tsk: TIPC port associated with message
 *
 * Note: Ancillary data is not captured if not requested by receiver.
 *
 * Returns 0 if successful, otherwise errno
 */
static int tipc_sk_anc_data_recv(struct msghdr *m, struct tipc_msg *msg,
				 struct tipc_sock *tsk)
{
	u32 anc_data[3];
	u32 err;
	u32 dest_type;
	int has_name;
	int res;

	if (likely(m->msg_controllen == 0))
		return 0;

	/* Optionally capture errored message object(s) */
	err = msg ? msg_errcode(msg) : 0;
	if (unlikely(err)) {
		anc_data[0] = err;
		anc_data[1] = msg_data_sz(msg);
		res = put_cmsg(m, SOL_TIPC, TIPC_ERRINFO, 8, anc_data);
		if (res)
			return res;
		if (anc_data[1]) {
			res = put_cmsg(m, SOL_TIPC, TIPC_RETDATA, anc_data[1],
				       msg_data(msg));
			if (res)
				return res;
		}
	}

	/* Optionally capture message destination object */
	dest_type = msg ? msg_type(msg) : TIPC_DIRECT_MSG;
	switch (dest_type) {
	case TIPC_NAMED_MSG:
		has_name = 1;
		anc_data[0] = msg_nametype(msg);
		anc_data[1] = msg_namelower(msg);
		anc_data[2] = msg_namelower(msg);
		break;
	case TIPC_MCAST_MSG:
		has_name = 1;
		anc_data[0] = msg_nametype(msg);
		anc_data[1] = msg_namelower(msg);
		anc_data[2] = msg_nameupper(msg);
		break;
	case TIPC_CONN_MSG:
		has_name = (tsk->conn_type != 0);
		anc_data[0] = tsk->conn_type;
		anc_data[1] = tsk->conn_instance;
		anc_data[2] = tsk->conn_instance;
		break;
	default:
		has_name = 0;
	}
	if (has_name) {
		res = put_cmsg(m, SOL_TIPC, TIPC_DESTNAME, 12, anc_data);
		if (res)
			return res;
	}

	return 0;
}

static void tipc_sk_send_ack(struct tipc_sock *tsk)
{
	struct sock *sk = &tsk->sk;
	struct net *net = sock_net(sk);
	struct sk_buff *skb = NULL;
	struct tipc_msg *msg;
	u32 peer_port = tsk_peer_port(tsk);
	u32 dnode = tsk_peer_node(tsk);

	if (!tipc_sk_connected(sk))
		return;
	skb = tipc_msg_create(CONN_MANAGER, CONN_ACK, INT_H_SIZE, 0,
			      dnode, tsk_own_node(tsk), peer_port,
			      tsk->portid, TIPC_OK);
	if (!skb)
		return;
	msg = buf_msg(skb);
	msg_set_conn_ack(msg, tsk->rcv_unacked);
	tsk->rcv_unacked = 0;

	/* Adjust to and advertize the correct window limit */
	if (tsk->peer_caps & TIPC_BLOCK_FLOWCTL) {
		tsk->rcv_win = tsk_adv_blocks(tsk->sk.sk_rcvbuf);
		msg_set_adv_win(msg, tsk->rcv_win);
	}
	tipc_node_xmit_skb(net, skb, dnode, msg_link_selector(msg));
}

static int tipc_wait_for_rcvmsg(struct socket *sock, long *timeop)
{
	struct sock *sk = sock->sk;
	DEFINE_WAIT(wait);
	long timeo = *timeop;
	int err = sock_error(sk);

	if (err)
		return err;

	for (;;) {
		prepare_to_wait(sk_sleep(sk), &wait, TASK_INTERRUPTIBLE);
		if (timeo && skb_queue_empty(&sk->sk_receive_queue)) {
			if (sk->sk_shutdown & RCV_SHUTDOWN) {
				err = -ENOTCONN;
				break;
			}
			release_sock(sk);
			timeo = schedule_timeout(timeo);
			lock_sock(sk);
		}
		err = 0;
		if (!skb_queue_empty(&sk->sk_receive_queue))
			break;
		err = -EAGAIN;
		if (!timeo)
			break;
		err = sock_intr_errno(timeo);
		if (signal_pending(current))
			break;

		err = sock_error(sk);
		if (err)
			break;
	}
	finish_wait(sk_sleep(sk), &wait);
	*timeop = timeo;
	return err;
}

/**
 * tipc_recvmsg - receive packet-oriented message
 * @m: descriptor for message info
 * @buflen: length of user buffer area
 * @flags: receive flags
 *
 * Used for SOCK_DGRAM, SOCK_RDM, and SOCK_SEQPACKET messages.
 * If the complete message doesn't fit in user area, truncate it.
 *
 * Returns size of returned message data, errno otherwise
 */
static int tipc_recvmsg(struct socket *sock, struct msghdr *m,
			size_t buflen,	int flags)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	struct sk_buff *skb;
	struct tipc_msg *hdr;
	bool connected = !tipc_sk_type_connectionless(sk);
	int rc, err, hlen, dlen, copy;
	long timeout;

	/* Catch invalid receive requests */
	if (unlikely(!buflen))
		return -EINVAL;

	lock_sock(sk);
	if (unlikely(connected && sk->sk_state == TIPC_OPEN)) {
		rc = -ENOTCONN;
		goto exit;
	}
	timeout = sock_rcvtimeo(sk, flags & MSG_DONTWAIT);

	do {
		/* Look at first msg in receive queue; wait if necessary */
		rc = tipc_wait_for_rcvmsg(sock, &timeout);
		if (unlikely(rc))
			goto exit;
		skb = skb_peek(&sk->sk_receive_queue);
		hdr = buf_msg(skb);
		dlen = msg_data_sz(hdr);
		hlen = msg_hdr_sz(hdr);
		err = msg_errcode(hdr);
		if (likely(dlen || err))
			break;
		tsk_advance_rx_queue(sk);
	} while (1);

	/* Collect msg meta data, including error code and rejected data */
	set_orig_addr(m, hdr);
	rc = tipc_sk_anc_data_recv(m, hdr, tsk);
	if (unlikely(rc))
		goto exit;

	/* Capture data if non-error msg, otherwise just set return value */
	if (likely(!err)) {
		copy = min_t(int, dlen, buflen);
		if (unlikely(copy != dlen))
			m->msg_flags |= MSG_TRUNC;
		rc = skb_copy_datagram_msg(skb, hlen, m, copy);
	} else {
		copy = 0;
		rc = 0;
		if (err != TIPC_CONN_SHUTDOWN && connected && !m->msg_control)
			rc = -ECONNRESET;
	}
	if (unlikely(rc))
		goto exit;

	/* Caption of data or error code/rejected data was successful */
	if (unlikely(flags & MSG_PEEK))
		goto exit;

	tsk_advance_rx_queue(sk);
	if (likely(!connected))
		goto exit;

	/* Send connection flow control ack when applicable */
	tsk->rcv_unacked += tsk_inc(tsk, hlen + dlen);
	if (tsk->rcv_unacked >= tsk->rcv_win / TIPC_ACK_RATE)
		tipc_sk_send_ack(tsk);
exit:
	release_sock(sk);
	return rc ? rc : copy;
}

/**
 * tipc_recvstream - receive stream-oriented data
 * @m: descriptor for message info
 * @buflen: total size of user buffer area
 * @flags: receive flags
 *
 * Used for SOCK_STREAM messages only.  If not enough data is available
 * will optionally wait for more; never truncates data.
 *
 * Returns size of returned message data, errno otherwise
 */
static int tipc_recvstream(struct socket *sock, struct msghdr *m,
			   size_t buflen, int flags)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	struct sk_buff *skb;
	struct tipc_msg *hdr;
	struct tipc_skb_cb *skb_cb;
	bool peek = flags & MSG_PEEK;
	int offset, required, copy, copied = 0;
	int hlen, dlen, err, rc;
	long timeout;

	/* Catch invalid receive attempts */
	if (unlikely(!buflen))
		return -EINVAL;

	lock_sock(sk);

	if (unlikely(sk->sk_state == TIPC_OPEN)) {
		rc = -ENOTCONN;
		goto exit;
	}
	required = sock_rcvlowat(sk, flags & MSG_WAITALL, buflen);
	timeout = sock_rcvtimeo(sk, flags & MSG_DONTWAIT);

	do {
		/* Look at first msg in receive queue; wait if necessary */
		rc = tipc_wait_for_rcvmsg(sock, &timeout);
		if (unlikely(rc))
			break;
		skb = skb_peek(&sk->sk_receive_queue);
		skb_cb = TIPC_SKB_CB(skb);
		hdr = buf_msg(skb);
		dlen = msg_data_sz(hdr);
		hlen = msg_hdr_sz(hdr);
		err = msg_errcode(hdr);

		/* Discard any empty non-errored (SYN-) message */
		if (unlikely(!dlen && !err)) {
			tsk_advance_rx_queue(sk);
			continue;
		}

		/* Collect msg meta data, incl. error code and rejected data */
		if (!copied) {
			set_orig_addr(m, hdr);
			rc = tipc_sk_anc_data_recv(m, hdr, tsk);
			if (rc)
				break;
		}

		/* Copy data if msg ok, otherwise return error/partial data */
		if (likely(!err)) {
			offset = skb_cb->bytes_read;
			copy = min_t(int, dlen - offset, buflen - copied);
			rc = skb_copy_datagram_msg(skb, hlen + offset, m, copy);
			if (unlikely(rc))
				break;
			copied += copy;
			offset += copy;
			if (unlikely(offset < dlen)) {
				if (!peek)
					skb_cb->bytes_read = offset;
				break;
			}
		} else {
			rc = 0;
			if ((err != TIPC_CONN_SHUTDOWN) && !m->msg_control)
				rc = -ECONNRESET;
			if (copied || rc)
				break;
		}

		if (unlikely(peek))
			break;

		tsk_advance_rx_queue(sk);

		/* Send connection flow control advertisement when applicable */
		tsk->rcv_unacked += tsk_inc(tsk, hlen + dlen);
		if (unlikely(tsk->rcv_unacked >= tsk->rcv_win / TIPC_ACK_RATE))
			tipc_sk_send_ack(tsk);

		/* Exit if all requested data or FIN/error received */
		if (copied == buflen || err)
			break;

	} while (!skb_queue_empty(&sk->sk_receive_queue) || copied < required);
exit:
	release_sock(sk);
	return copied ? copied : rc;
}

/**
 * tipc_write_space - wake up thread if port congestion is released
 * @sk: socket
 */
static void tipc_write_space(struct sock *sk)
{
	struct socket_wq *wq;

	rcu_read_lock();
	wq = rcu_dereference(sk->sk_wq);
	if (skwq_has_sleeper(wq))
		wake_up_interruptible_sync_poll(&wq->wait, POLLOUT |
						POLLWRNORM | POLLWRBAND);
	rcu_read_unlock();
}

/**
 * tipc_data_ready - wake up threads to indicate messages have been received
 * @sk: socket
 * @len: the length of messages
 */
static void tipc_data_ready(struct sock *sk)
{
	struct socket_wq *wq;

	rcu_read_lock();
	wq = rcu_dereference(sk->sk_wq);
	if (skwq_has_sleeper(wq))
		wake_up_interruptible_sync_poll(&wq->wait, POLLIN |
						POLLRDNORM | POLLRDBAND);
	rcu_read_unlock();
}

static void tipc_sock_destruct(struct sock *sk)
{
	__skb_queue_purge(&sk->sk_receive_queue);
}

/**
 * filter_connect - Handle all incoming messages for a connection-based socket
 * @tsk: TIPC socket
 * @skb: pointer to message buffer. Set to NULL if buffer is consumed
 *
 * Returns true if everything ok, false otherwise
 */
static bool filter_connect(struct tipc_sock *tsk, struct sk_buff *skb)
{
	struct sock *sk = &tsk->sk;
	struct net *net = sock_net(sk);
	struct tipc_msg *hdr = buf_msg(skb);
	u32 pport = msg_origport(hdr);
	u32 pnode = msg_orignode(hdr);

	if (unlikely(msg_mcast(hdr)))
		return false;

	switch (sk->sk_state) {
	case TIPC_CONNECTING:
		/* Accept only ACK or NACK message */
		if (unlikely(!msg_connected(hdr))) {
			if (pport != tsk_peer_port(tsk) ||
			    pnode != tsk_peer_node(tsk))
				return false;

			tipc_set_sk_state(sk, TIPC_DISCONNECTING);
			sk->sk_err = ECONNREFUSED;
			sk->sk_state_change(sk);
			return true;
		}

		if (unlikely(msg_errcode(hdr))) {
			tipc_set_sk_state(sk, TIPC_DISCONNECTING);
			sk->sk_err = ECONNREFUSED;
			sk->sk_state_change(sk);
			return true;
		}

		if (unlikely(!msg_isdata(hdr))) {
			tipc_set_sk_state(sk, TIPC_DISCONNECTING);
			sk->sk_err = EINVAL;
			sk->sk_state_change(sk);
			return true;
		}

		tipc_sk_finish_conn(tsk, msg_origport(hdr), msg_orignode(hdr));
		msg_set_importance(&tsk->phdr, msg_importance(hdr));

		/* If 'ACK+' message, add to socket receive queue */
		if (msg_data_sz(hdr))
			return true;

		/* If empty 'ACK-' message, wake up sleeping connect() */
		sk->sk_state_change(sk);

		/* 'ACK-' message is neither accepted nor rejected: */
		msg_set_dest_droppable(hdr, 1);
		return false;

	case TIPC_OPEN:
	case TIPC_DISCONNECTING:
		break;
	case TIPC_LISTEN:
		/* Accept only SYN message */
		if (!msg_connected(hdr) && !(msg_errcode(hdr)))
			return true;
		break;
	case TIPC_ESTABLISHED:
		/* Accept only connection-based messages sent by peer */
		if (unlikely(!tsk_peer_msg(tsk, hdr)))
			return false;

		if (unlikely(msg_errcode(hdr))) {
			tipc_set_sk_state(sk, TIPC_DISCONNECTING);
			/* Let timer expire on it's own */
			tipc_node_remove_conn(net, tsk_peer_node(tsk),
					      tsk->portid);
			sk->sk_state_change(sk);
		}
		return true;
	default:
		pr_err("Unknown sk_state %u\n", sk->sk_state);
	}

	return false;
}

/**
 * rcvbuf_limit - get proper overload limit of socket receive queue
 * @sk: socket
 * @skb: message
 *
 * For connection oriented messages, irrespective of importance,
 * default queue limit is 2 MB.
 *
 * For connectionless messages, queue limits are based on message
 * importance as follows:
 *
 * TIPC_LOW_IMPORTANCE       (2 MB)
 * TIPC_MEDIUM_IMPORTANCE    (4 MB)
 * TIPC_HIGH_IMPORTANCE      (8 MB)
 * TIPC_CRITICAL_IMPORTANCE  (16 MB)
 *
 * Returns overload limit according to corresponding message importance
 */
static unsigned int rcvbuf_limit(struct sock *sk, struct sk_buff *skb)
{
	struct tipc_sock *tsk = tipc_sk(sk);
	struct tipc_msg *hdr = buf_msg(skb);

	if (unlikely(!msg_connected(hdr)))
		return sk->sk_rcvbuf << msg_importance(hdr);

	if (likely(tsk->peer_caps & TIPC_BLOCK_FLOWCTL))
		return sk->sk_rcvbuf;

	return FLOWCTL_MSG_LIM;
}

/**
 * filter_rcv - validate incoming message
 * @sk: socket
 * @skb: pointer to message.
 *
 * Enqueues message on receive queue if acceptable; optionally handles
 * disconnect indication for a connected socket.
 *
 * Called with socket lock already taken
 *
 * Returns true if message was added to socket receive queue, otherwise false
 */
static bool filter_rcv(struct sock *sk, struct sk_buff *skb,
		       struct sk_buff_head *xmitq)
{
	struct tipc_sock *tsk = tipc_sk(sk);
	struct tipc_msg *hdr = buf_msg(skb);
	unsigned int limit = rcvbuf_limit(sk, skb);
	int err = TIPC_OK;
	int usr = msg_user(hdr);
	u32 onode;

	if (unlikely(msg_user(hdr) == CONN_MANAGER)) {
		tipc_sk_proto_rcv(tsk, skb, xmitq);
		return false;
	}

	if (unlikely(usr == SOCK_WAKEUP)) {
		onode = msg_orignode(hdr);
		kfree_skb(skb);
		u32_del(&tsk->cong_links, onode);
		tsk->cong_link_cnt--;
		sk->sk_write_space(sk);
		return false;
	}

	/* Drop if illegal message type */
	if (unlikely(msg_type(hdr) > TIPC_DIRECT_MSG)) {
		kfree_skb(skb);
		return false;
	}

	/* Reject if wrong message type for current socket state */
	if (tipc_sk_type_connectionless(sk)) {
		if (msg_connected(hdr)) {
			err = TIPC_ERR_NO_PORT;
			goto reject;
		}
	} else if (unlikely(!filter_connect(tsk, skb))) {
		err = TIPC_ERR_NO_PORT;
		goto reject;
	}

	/* Reject message if there isn't room to queue it */
	if (unlikely(sk_rmem_alloc_get(sk) + skb->truesize >= limit)) {
		err = TIPC_ERR_OVERLOAD;
		goto reject;
	}

	/* Enqueue message */
	TIPC_SKB_CB(skb)->bytes_read = 0;
	__skb_queue_tail(&sk->sk_receive_queue, skb);
	skb_set_owner_r(skb, sk);

	sk->sk_data_ready(sk);
	return true;

reject:
	if (tipc_msg_reverse(tsk_own_node(tsk), &skb, err))
		__skb_queue_tail(xmitq, skb);
	return false;
}

/**
 * tipc_backlog_rcv - handle incoming message from backlog queue
 * @sk: socket
 * @skb: message
 *
 * Caller must hold socket lock
 *
 * Returns 0
 */
static int tipc_backlog_rcv(struct sock *sk, struct sk_buff *skb)
{
	unsigned int truesize = skb->truesize;
	struct sk_buff_head xmitq;
	u32 dnode, selector;

	__skb_queue_head_init(&xmitq);

	if (likely(filter_rcv(sk, skb, &xmitq))) {
		atomic_add(truesize, &tipc_sk(sk)->dupl_rcvcnt);
		return 0;
	}

	if (skb_queue_empty(&xmitq))
		return 0;

	/* Send response/rejected message */
	skb = __skb_dequeue(&xmitq);
	dnode = msg_destnode(buf_msg(skb));
	selector = msg_origport(buf_msg(skb));
	tipc_node_xmit_skb(sock_net(sk), skb, dnode, selector);
	return 0;
}

/**
 * tipc_sk_enqueue - extract all buffers with destination 'dport' from
 *                   inputq and try adding them to socket or backlog queue
 * @inputq: list of incoming buffers with potentially different destinations
 * @sk: socket where the buffers should be enqueued
 * @dport: port number for the socket
 *
 * Caller must hold socket lock
 */
static void tipc_sk_enqueue(struct sk_buff_head *inputq, struct sock *sk,
			    u32 dport, struct sk_buff_head *xmitq)
{
	unsigned long time_limit = jiffies + 2;
	struct sk_buff *skb;
	unsigned int lim;
	atomic_t *dcnt;
	u32 onode;

	while (skb_queue_len(inputq)) {
		if (unlikely(time_after_eq(jiffies, time_limit)))
			return;

		skb = tipc_skb_dequeue(inputq, dport);
		if (unlikely(!skb))
			return;

		/* Add message directly to receive queue if possible */
		if (!sock_owned_by_user(sk)) {
			filter_rcv(sk, skb, xmitq);
			continue;
		}

		/* Try backlog, compensating for double-counted bytes */
		dcnt = &tipc_sk(sk)->dupl_rcvcnt;
		if (!sk->sk_backlog.len)
			atomic_set(dcnt, 0);
		lim = rcvbuf_limit(sk, skb) + atomic_read(dcnt);
		if (likely(!sk_add_backlog(sk, skb, lim)))
			continue;

		/* Overload => reject message back to sender */
		onode = tipc_own_addr(sock_net(sk));
		if (tipc_msg_reverse(onode, &skb, TIPC_ERR_OVERLOAD))
			__skb_queue_tail(xmitq, skb);
		break;
	}
}

/**
 * tipc_sk_rcv - handle a chain of incoming buffers
 * @inputq: buffer list containing the buffers
 * Consumes all buffers in list until inputq is empty
 * Note: may be called in multiple threads referring to the same queue
 */
void tipc_sk_rcv(struct net *net, struct sk_buff_head *inputq)
{
	struct sk_buff_head xmitq;
	u32 dnode, dport = 0;
	int err;
	struct tipc_sock *tsk;
	struct sock *sk;
	struct sk_buff *skb;

	__skb_queue_head_init(&xmitq);
	while (skb_queue_len(inputq)) {
		dport = tipc_skb_peek_port(inputq, dport);
		tsk = tipc_sk_lookup(net, dport);

		if (likely(tsk)) {
			sk = &tsk->sk;
			if (likely(spin_trylock_bh(&sk->sk_lock.slock))) {
				tipc_sk_enqueue(inputq, sk, dport, &xmitq);
				spin_unlock_bh(&sk->sk_lock.slock);
			}
			/* Send pending response/rejected messages, if any */
			while ((skb = __skb_dequeue(&xmitq))) {
				dnode = msg_destnode(buf_msg(skb));
				tipc_node_xmit_skb(net, skb, dnode, dport);
			}
			sock_put(sk);
			continue;
		}

		/* No destination socket => dequeue skb if still there */
		skb = tipc_skb_dequeue(inputq, dport);
		if (!skb)
			return;

		/* Try secondary lookup if unresolved named message */
		err = TIPC_ERR_NO_PORT;
		if (tipc_msg_lookup_dest(net, skb, &err))
			goto xmit;

		/* Prepare for message rejection */
		if (!tipc_msg_reverse(tipc_own_addr(net), &skb, err))
			continue;
xmit:
		dnode = msg_destnode(buf_msg(skb));
		tipc_node_xmit_skb(net, skb, dnode, dport);
	}
}

static int tipc_wait_for_connect(struct socket *sock, long *timeo_p)
{
	DEFINE_WAIT_FUNC(wait, woken_wake_function);
	struct sock *sk = sock->sk;
	int done;

	do {
		int err = sock_error(sk);
		if (err)
			return err;
		if (!*timeo_p)
			return -ETIMEDOUT;
		if (signal_pending(current))
			return sock_intr_errno(*timeo_p);

		add_wait_queue(sk_sleep(sk), &wait);
		done = sk_wait_event(sk, timeo_p,
				     sk->sk_state != TIPC_CONNECTING, &wait);
		remove_wait_queue(sk_sleep(sk), &wait);
	} while (!done);
	return 0;
}

/**
 * tipc_connect - establish a connection to another TIPC port
 * @sock: socket structure
 * @dest: socket address for destination port
 * @destlen: size of socket address data structure
 * @flags: file-related flags associated with socket
 *
 * Returns 0 on success, errno otherwise
 */
static int tipc_connect(struct socket *sock, struct sockaddr *dest,
			int destlen, int flags)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	struct sockaddr_tipc *dst = (struct sockaddr_tipc *)dest;
	struct msghdr m = {NULL,};
	long timeout = (flags & O_NONBLOCK) ? 0 : tsk->conn_timeout;
	int previous;
	int res = 0;

	lock_sock(sk);

	/* DGRAM/RDM connect(), just save the destaddr */
	if (tipc_sk_type_connectionless(sk)) {
		if (dst->family == AF_UNSPEC) {
			memset(&tsk->peer, 0, sizeof(struct sockaddr_tipc));
		} else if (destlen != sizeof(struct sockaddr_tipc)) {
			res = -EINVAL;
		} else {
			memcpy(&tsk->peer, dest, destlen);
		}
		goto exit;
	}

	/*
	 * Reject connection attempt using multicast address
	 *
	 * Note: send_msg() validates the rest of the address fields,
	 *       so there's no need to do it here
	 */
	if (dst->addrtype == TIPC_ADDR_MCAST) {
		res = -EINVAL;
		goto exit;
	}

	previous = sk->sk_state;

	switch (sk->sk_state) {
	case TIPC_OPEN:
		/* Send a 'SYN-' to destination */
		m.msg_name = dest;
		m.msg_namelen = destlen;

		/* If connect is in non-blocking case, set MSG_DONTWAIT to
		 * indicate send_msg() is never blocked.
		 */
		if (!timeout)
			m.msg_flags = MSG_DONTWAIT;

		res = __tipc_sendmsg(sock, &m, 0);
		if ((res < 0) && (res != -EWOULDBLOCK))
			goto exit;

		/* Just entered TIPC_CONNECTING state; the only
		 * difference is that return value in non-blocking
		 * case is EINPROGRESS, rather than EALREADY.
		 */
		res = -EINPROGRESS;
		/* fall thru' */
	case TIPC_CONNECTING:
		if (!timeout) {
			if (previous == TIPC_CONNECTING)
				res = -EALREADY;
			goto exit;
		}
		timeout = msecs_to_jiffies(timeout);
		/* Wait until an 'ACK' or 'RST' arrives, or a timeout occurs */
		res = tipc_wait_for_connect(sock, &timeout);
		break;
	case TIPC_ESTABLISHED:
		res = -EISCONN;
		break;
	default:
		res = -EINVAL;
	}

exit:
	release_sock(sk);
	return res;
}

/**
 * tipc_listen - allow socket to listen for incoming connections
 * @sock: socket structure
 * @len: (unused)
 *
 * Returns 0 on success, errno otherwise
 */
static int tipc_listen(struct socket *sock, int len)
{
	struct sock *sk = sock->sk;
	int res;

	lock_sock(sk);
	res = tipc_set_sk_state(sk, TIPC_LISTEN);
	release_sock(sk);

	return res;
}

static int tipc_wait_for_accept(struct socket *sock, long timeo)
{
	struct sock *sk = sock->sk;
	DEFINE_WAIT(wait);
	int err;

	/* True wake-one mechanism for incoming connections: only
	 * one process gets woken up, not the 'whole herd'.
	 * Since we do not 'race & poll' for established sockets
	 * anymore, the common case will execute the loop only once.
	*/
	for (;;) {
		prepare_to_wait_exclusive(sk_sleep(sk), &wait,
					  TASK_INTERRUPTIBLE);
		if (timeo && skb_queue_empty(&sk->sk_receive_queue)) {
			release_sock(sk);
			timeo = schedule_timeout(timeo);
			lock_sock(sk);
		}
		err = 0;
		if (!skb_queue_empty(&sk->sk_receive_queue))
			break;
		err = -EAGAIN;
		if (!timeo)
			break;
		err = sock_intr_errno(timeo);
		if (signal_pending(current))
			break;
	}
	finish_wait(sk_sleep(sk), &wait);
	return err;
}

/**
 * tipc_accept - wait for connection request
 * @sock: listening socket
 * @newsock: new socket that is to be connected
 * @flags: file-related flags associated with socket
 *
 * Returns 0 on success, errno otherwise
 */
static int tipc_accept(struct socket *sock, struct socket *new_sock, int flags,
		       bool kern)
{
	struct sock *new_sk, *sk = sock->sk;
	struct sk_buff *buf;
	struct tipc_sock *new_tsock;
	struct tipc_msg *msg;
	long timeo;
	int res;

	lock_sock(sk);

	if (sk->sk_state != TIPC_LISTEN) {
		res = -EINVAL;
		goto exit;
	}
	timeo = sock_rcvtimeo(sk, flags & O_NONBLOCK);
	res = tipc_wait_for_accept(sock, timeo);
	if (res)
		goto exit;

	buf = skb_peek(&sk->sk_receive_queue);

	res = tipc_sk_create(sock_net(sock->sk), new_sock, 0, kern);
	if (res)
		goto exit;
	security_sk_clone(sock->sk, new_sock->sk);

	new_sk = new_sock->sk;
	new_tsock = tipc_sk(new_sk);
	msg = buf_msg(buf);

	/* we lock on new_sk; but lockdep sees the lock on sk */
	lock_sock_nested(new_sk, SINGLE_DEPTH_NESTING);

	/*
	 * Reject any stray messages received by new socket
	 * before the socket lock was taken (very, very unlikely)
	 */
	tsk_rej_rx_queue(new_sk);

	/* Connect new socket to it's peer */
	tipc_sk_finish_conn(new_tsock, msg_origport(msg), msg_orignode(msg));

	tsk_set_importance(new_tsock, msg_importance(msg));
	if (msg_named(msg)) {
		new_tsock->conn_type = msg_nametype(msg);
		new_tsock->conn_instance = msg_nameinst(msg);
	}

	/*
	 * Respond to 'SYN-' by discarding it & returning 'ACK'-.
	 * Respond to 'SYN+' by queuing it on new socket.
	 */
	if (!msg_data_sz(msg)) {
		struct msghdr m = {NULL,};

		tsk_advance_rx_queue(sk);
		__tipc_sendstream(new_sock, &m, 0);
	} else {
		__skb_dequeue(&sk->sk_receive_queue);
		__skb_queue_head(&new_sk->sk_receive_queue, buf);
		skb_set_owner_r(buf, new_sk);
	}
	release_sock(new_sk);
exit:
	release_sock(sk);
	return res;
}

/**
 * tipc_shutdown - shutdown socket connection
 * @sock: socket structure
 * @how: direction to close (must be SHUT_RDWR)
 *
 * Terminates connection (if necessary), then purges socket's receive queue.
 *
 * Returns 0 on success, errno otherwise
 */
static int tipc_shutdown(struct socket *sock, int how)
{
	struct sock *sk = sock->sk;
	int res;

	if (how != SHUT_RDWR)
		return -EINVAL;

	lock_sock(sk);

	__tipc_shutdown(sock, TIPC_CONN_SHUTDOWN);
	sk->sk_shutdown = SHUTDOWN_MASK;

	if (sk->sk_state == TIPC_DISCONNECTING) {
		/* Discard any unreceived messages */
		__skb_queue_purge(&sk->sk_receive_queue);

		res = 0;
	} else {
		res = -ENOTCONN;
	}
	/* Wake up anyone sleeping in poll. */
	sk->sk_state_change(sk);

	release_sock(sk);
	return res;
}

static void tipc_sk_timeout(unsigned long data)
{
	struct tipc_sock *tsk = (struct tipc_sock *)data;
	struct sock *sk = &tsk->sk;
	struct sk_buff *skb = NULL;
	u32 peer_port, peer_node;
	u32 own_node = tsk_own_node(tsk);

	bh_lock_sock(sk);
	if (!tipc_sk_connected(sk)) {
		bh_unlock_sock(sk);
		goto exit;
	}
	peer_port = tsk_peer_port(tsk);
	peer_node = tsk_peer_node(tsk);

	if (tsk->probe_unacked) {
		if (!sock_owned_by_user(sk)) {
			tipc_set_sk_state(sk, TIPC_DISCONNECTING);
			tipc_node_remove_conn(sock_net(sk), tsk_peer_node(tsk),
					      tsk_peer_port(tsk));
			sk->sk_state_change(sk);
		} else {
			/* Try again later */
			sk_reset_timer(sk, &sk->sk_timer, (HZ / 20));
		}

		bh_unlock_sock(sk);
		goto exit;
	}

	skb = tipc_msg_create(CONN_MANAGER, CONN_PROBE,
			      INT_H_SIZE, 0, peer_node, own_node,
			      peer_port, tsk->portid, TIPC_OK);
	tsk->probe_unacked = true;
	sk_reset_timer(sk, &sk->sk_timer, jiffies + CONN_PROBING_INTERVAL);
	bh_unlock_sock(sk);
	if (skb)
		tipc_node_xmit_skb(sock_net(sk), skb, peer_node, tsk->portid);
exit:
	sock_put(sk);
}

static int tipc_sk_publish(struct tipc_sock *tsk, uint scope,
			   struct tipc_name_seq const *seq)
{
	struct sock *sk = &tsk->sk;
	struct net *net = sock_net(sk);
	struct publication *publ;
	u32 key;

	if (tipc_sk_connected(sk))
		return -EINVAL;
	key = tsk->portid + tsk->pub_count + 1;
	if (key == tsk->portid)
		return -EADDRINUSE;

	publ = tipc_nametbl_publish(net, seq->type, seq->lower, seq->upper,
				    scope, tsk->portid, key);
	if (unlikely(!publ))
		return -EINVAL;

	list_add(&publ->pport_list, &tsk->publications);
	tsk->pub_count++;
	tsk->published = 1;
	return 0;
}

static int tipc_sk_withdraw(struct tipc_sock *tsk, uint scope,
			    struct tipc_name_seq const *seq)
{
	struct net *net = sock_net(&tsk->sk);
	struct publication *publ;
	struct publication *safe;
	int rc = -EINVAL;

	list_for_each_entry_safe(publ, safe, &tsk->publications, pport_list) {
		if (seq) {
			if (publ->scope != scope)
				continue;
			if (publ->type != seq->type)
				continue;
			if (publ->lower != seq->lower)
				continue;
			if (publ->upper != seq->upper)
				break;
			tipc_nametbl_withdraw(net, publ->type, publ->lower,
					      publ->ref, publ->key);
			rc = 0;
			break;
		}
		tipc_nametbl_withdraw(net, publ->type, publ->lower,
				      publ->ref, publ->key);
		rc = 0;
	}
	if (list_empty(&tsk->publications))
		tsk->published = 0;
	return rc;
}

/* tipc_sk_reinit: set non-zero address in all existing sockets
 *                 when we go from standalone to network mode.
 */
void tipc_sk_reinit(struct net *net)
{
	struct tipc_net *tn = net_generic(net, tipc_net_id);
	struct rhashtable_iter iter;
	struct tipc_sock *tsk;
	struct tipc_msg *msg;

	rhashtable_walk_enter(&tn->sk_rht, &iter);

	do {
		tsk = ERR_PTR(rhashtable_walk_start(&iter));
		if (IS_ERR(tsk))
			goto walk_stop;

		while ((tsk = rhashtable_walk_next(&iter)) && !IS_ERR(tsk)) {
			sock_hold(&tsk->sk);
			rhashtable_walk_stop(&iter);
			lock_sock(&tsk->sk);
			msg = &tsk->phdr;
			msg_set_prevnode(msg, tn->own_addr);
			msg_set_orignode(msg, tn->own_addr);
			release_sock(&tsk->sk);
			rhashtable_walk_start(&iter);
			sock_put(&tsk->sk);
		}
walk_stop:
		rhashtable_walk_stop(&iter);
	} while (tsk == ERR_PTR(-EAGAIN));

	rhashtable_walk_exit(&iter);
}

static struct tipc_sock *tipc_sk_lookup(struct net *net, u32 portid)
{
	struct tipc_net *tn = net_generic(net, tipc_net_id);
	struct tipc_sock *tsk;

	rcu_read_lock();
	tsk = rhashtable_lookup_fast(&tn->sk_rht, &portid, tsk_rht_params);
	if (tsk)
		sock_hold(&tsk->sk);
	rcu_read_unlock();

	return tsk;
}

static int tipc_sk_insert(struct tipc_sock *tsk)
{
	struct sock *sk = &tsk->sk;
	struct net *net = sock_net(sk);
	struct tipc_net *tn = net_generic(net, tipc_net_id);
	u32 remaining = (TIPC_MAX_PORT - TIPC_MIN_PORT) + 1;
	u32 portid = prandom_u32() % remaining + TIPC_MIN_PORT;

	while (remaining--) {
		portid++;
		if ((portid < TIPC_MIN_PORT) || (portid > TIPC_MAX_PORT))
			portid = TIPC_MIN_PORT;
		tsk->portid = portid;
		sock_hold(&tsk->sk);
		if (!rhashtable_lookup_insert_fast(&tn->sk_rht, &tsk->node,
						   tsk_rht_params))
			return 0;
		sock_put(&tsk->sk);
	}

	return -1;
}

static void tipc_sk_remove(struct tipc_sock *tsk)
{
	struct sock *sk = &tsk->sk;
	struct tipc_net *tn = net_generic(sock_net(sk), tipc_net_id);

	if (!rhashtable_remove_fast(&tn->sk_rht, &tsk->node, tsk_rht_params)) {
		WARN_ON(refcount_read(&sk->sk_refcnt) == 1);
		__sock_put(sk);
	}
}

static const struct rhashtable_params tsk_rht_params = {
	.nelem_hint = 192,
	.head_offset = offsetof(struct tipc_sock, node),
	.key_offset = offsetof(struct tipc_sock, portid),
	.key_len = sizeof(u32), /* portid */
	.max_size = 1048576,
	.min_size = 256,
	.automatic_shrinking = true,
};

int tipc_sk_rht_init(struct net *net)
{
	struct tipc_net *tn = net_generic(net, tipc_net_id);

	return rhashtable_init(&tn->sk_rht, &tsk_rht_params);
}

void tipc_sk_rht_destroy(struct net *net)
{
	struct tipc_net *tn = net_generic(net, tipc_net_id);

	/* Wait for socket readers to complete */
	synchronize_net();

	rhashtable_destroy(&tn->sk_rht);
}

/**
 * tipc_setsockopt - set socket option
 * @sock: socket structure
 * @lvl: option level
 * @opt: option identifier
 * @ov: pointer to new option value
 * @ol: length of option value
 *
 * For stream sockets only, accepts and ignores all IPPROTO_TCP options
 * (to ease compatibility).
 *
 * Returns 0 on success, errno otherwise
 */
static int tipc_setsockopt(struct socket *sock, int lvl, int opt,
			   char __user *ov, unsigned int ol)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	u32 value = 0;
	int res = 0;

	if ((lvl == IPPROTO_TCP) && (sock->type == SOCK_STREAM))
		return 0;
	if (lvl != SOL_TIPC)
		return -ENOPROTOOPT;

	switch (opt) {
	case TIPC_IMPORTANCE:
	case TIPC_SRC_DROPPABLE:
	case TIPC_DEST_DROPPABLE:
	case TIPC_CONN_TIMEOUT:
		if (ol < sizeof(value))
			return -EINVAL;
		res = get_user(value, (u32 __user *)ov);
		if (res)
			return res;
		break;
	default:
		if (ov || ol)
			return -EINVAL;
	}

	lock_sock(sk);

	switch (opt) {
	case TIPC_IMPORTANCE:
		res = tsk_set_importance(tsk, value);
		break;
	case TIPC_SRC_DROPPABLE:
		if (sock->type != SOCK_STREAM)
			tsk_set_unreliable(tsk, value);
		else
			res = -ENOPROTOOPT;
		break;
	case TIPC_DEST_DROPPABLE:
		tsk_set_unreturnable(tsk, value);
		break;
	case TIPC_CONN_TIMEOUT:
		tipc_sk(sk)->conn_timeout = value;
		break;
	case TIPC_MCAST_BROADCAST:
		tsk->mc_method.rcast = false;
		tsk->mc_method.mandatory = true;
		break;
	case TIPC_MCAST_REPLICAST:
		tsk->mc_method.rcast = true;
		tsk->mc_method.mandatory = true;
		break;
	default:
		res = -EINVAL;
	}

	release_sock(sk);

	return res;
}

/**
 * tipc_getsockopt - get socket option
 * @sock: socket structure
 * @lvl: option level
 * @opt: option identifier
 * @ov: receptacle for option value
 * @ol: receptacle for length of option value
 *
 * For stream sockets only, returns 0 length result for all IPPROTO_TCP options
 * (to ease compatibility).
 *
 * Returns 0 on success, errno otherwise
 */
static int tipc_getsockopt(struct socket *sock, int lvl, int opt,
			   char __user *ov, int __user *ol)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	int len;
	u32 value;
	int res;

	if ((lvl == IPPROTO_TCP) && (sock->type == SOCK_STREAM))
		return put_user(0, ol);
	if (lvl != SOL_TIPC)
		return -ENOPROTOOPT;
	res = get_user(len, ol);
	if (res)
		return res;

	lock_sock(sk);

	switch (opt) {
	case TIPC_IMPORTANCE:
		value = tsk_importance(tsk);
		break;
	case TIPC_SRC_DROPPABLE:
		value = tsk_unreliable(tsk);
		break;
	case TIPC_DEST_DROPPABLE:
		value = tsk_unreturnable(tsk);
		break;
	case TIPC_CONN_TIMEOUT:
		value = tsk->conn_timeout;
		/* no need to set "res", since already 0 at this point */
		break;
	case TIPC_NODE_RECVQ_DEPTH:
		value = 0; /* was tipc_queue_size, now obsolete */
		break;
	case TIPC_SOCK_RECVQ_DEPTH:
		value = skb_queue_len(&sk->sk_receive_queue);
		break;
	default:
		res = -EINVAL;
	}

	release_sock(sk);

	if (res)
		return res;	/* "get" failed */

	if (len < sizeof(value))
		return -EINVAL;

	if (copy_to_user(ov, &value, sizeof(value)))
		return -EFAULT;

	return put_user(sizeof(value), ol);
}

static int tipc_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct sock *sk = sock->sk;
	struct tipc_sioc_ln_req lnr;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case SIOCGETLINKNAME:
		if (copy_from_user(&lnr, argp, sizeof(lnr)))
			return -EFAULT;
		if (!tipc_node_get_linkname(sock_net(sk),
					    lnr.bearer_id & 0xffff, lnr.peer,
					    lnr.linkname, TIPC_MAX_LINK_NAME)) {
			if (copy_to_user(argp, &lnr, sizeof(lnr)))
				return -EFAULT;
			return 0;
		}
		return -EADDRNOTAVAIL;
	default:
		return -ENOIOCTLCMD;
	}
}

static int tipc_socketpair(struct socket *sock1, struct socket *sock2)
{
	struct tipc_sock *tsk2 = tipc_sk(sock2->sk);
	struct tipc_sock *tsk1 = tipc_sk(sock1->sk);
	u32 onode = tipc_own_addr(sock_net(sock1->sk));

	tsk1->peer.family = AF_TIPC;
	tsk1->peer.addrtype = TIPC_ADDR_ID;
	tsk1->peer.scope = TIPC_NODE_SCOPE;
	tsk1->peer.addr.id.ref = tsk2->portid;
	tsk1->peer.addr.id.node = onode;
	tsk2->peer.family = AF_TIPC;
	tsk2->peer.addrtype = TIPC_ADDR_ID;
	tsk2->peer.scope = TIPC_NODE_SCOPE;
	tsk2->peer.addr.id.ref = tsk1->portid;
	tsk2->peer.addr.id.node = onode;

	tipc_sk_finish_conn(tsk1, tsk2->portid, onode);
	tipc_sk_finish_conn(tsk2, tsk1->portid, onode);
	return 0;
}

/* Protocol switches for the various types of TIPC sockets */

static const struct proto_ops msg_ops = {
	.owner		= THIS_MODULE,
	.family		= AF_TIPC,
	.release	= tipc_release,
	.bind		= tipc_bind,
	.connect	= tipc_connect,
	.socketpair	= tipc_socketpair,
	.accept		= sock_no_accept,
	.getname	= tipc_getname,
	.poll		= tipc_poll,
	.ioctl		= tipc_ioctl,
	.listen		= sock_no_listen,
	.shutdown	= tipc_shutdown,
	.setsockopt	= tipc_setsockopt,
	.getsockopt	= tipc_getsockopt,
	.sendmsg	= tipc_sendmsg,
	.recvmsg	= tipc_recvmsg,
	.mmap		= sock_no_mmap,
	.sendpage	= sock_no_sendpage
};

static const struct proto_ops packet_ops = {
	.owner		= THIS_MODULE,
	.family		= AF_TIPC,
	.release	= tipc_release,
	.bind		= tipc_bind,
	.connect	= tipc_connect,
	.socketpair	= tipc_socketpair,
	.accept		= tipc_accept,
	.getname	= tipc_getname,
	.poll		= tipc_poll,
	.ioctl		= tipc_ioctl,
	.listen		= tipc_listen,
	.shutdown	= tipc_shutdown,
	.setsockopt	= tipc_setsockopt,
	.getsockopt	= tipc_getsockopt,
	.sendmsg	= tipc_send_packet,
	.recvmsg	= tipc_recvmsg,
	.mmap		= sock_no_mmap,
	.sendpage	= sock_no_sendpage
};

static const struct proto_ops stream_ops = {
	.owner		= THIS_MODULE,
	.family		= AF_TIPC,
	.release	= tipc_release,
	.bind		= tipc_bind,
	.connect	= tipc_connect,
	.socketpair	= tipc_socketpair,
	.accept		= tipc_accept,
	.getname	= tipc_getname,
	.poll		= tipc_poll,
	.ioctl		= tipc_ioctl,
	.listen		= tipc_listen,
	.shutdown	= tipc_shutdown,
	.setsockopt	= tipc_setsockopt,
	.getsockopt	= tipc_getsockopt,
	.sendmsg	= tipc_sendstream,
	.recvmsg	= tipc_recvstream,
	.mmap		= sock_no_mmap,
	.sendpage	= sock_no_sendpage
};

static const struct net_proto_family tipc_family_ops = {
	.owner		= THIS_MODULE,
	.family		= AF_TIPC,
	.create		= tipc_sk_create
};

static struct proto tipc_proto = {
	.name		= "TIPC",
	.owner		= THIS_MODULE,
	.obj_size	= sizeof(struct tipc_sock),
	.sysctl_rmem	= sysctl_tipc_rmem
};

/**
 * tipc_socket_init - initialize TIPC socket interface
 *
 * Returns 0 on success, errno otherwise
 */
int tipc_socket_init(void)
{
	int res;

	res = proto_register(&tipc_proto, 1);
	if (res) {
		pr_err("Failed to register TIPC protocol type\n");
		goto out;
	}

	res = sock_register(&tipc_family_ops);
	if (res) {
		pr_err("Failed to register TIPC socket type\n");
		proto_unregister(&tipc_proto);
		goto out;
	}
 out:
	return res;
}

/**
 * tipc_socket_stop - stop TIPC socket interface
 */
void tipc_socket_stop(void)
{
	sock_unregister(tipc_family_ops.family);
	proto_unregister(&tipc_proto);
}

/* Caller should hold socket lock for the passed tipc socket. */
static int __tipc_nl_add_sk_con(struct sk_buff *skb, struct tipc_sock *tsk)
{
	u32 peer_node;
	u32 peer_port;
	struct nlattr *nest;

	peer_node = tsk_peer_node(tsk);
	peer_port = tsk_peer_port(tsk);

	nest = nla_nest_start(skb, TIPC_NLA_SOCK_CON);

	if (nla_put_u32(skb, TIPC_NLA_CON_NODE, peer_node))
		goto msg_full;
	if (nla_put_u32(skb, TIPC_NLA_CON_SOCK, peer_port))
		goto msg_full;

	if (tsk->conn_type != 0) {
		if (nla_put_flag(skb, TIPC_NLA_CON_FLAG))
			goto msg_full;
		if (nla_put_u32(skb, TIPC_NLA_CON_TYPE, tsk->conn_type))
			goto msg_full;
		if (nla_put_u32(skb, TIPC_NLA_CON_INST, tsk->conn_instance))
			goto msg_full;
	}
	nla_nest_end(skb, nest);

	return 0;

msg_full:
	nla_nest_cancel(skb, nest);

	return -EMSGSIZE;
}

/* Caller should hold socket lock for the passed tipc socket. */
static int __tipc_nl_add_sk(struct sk_buff *skb, struct netlink_callback *cb,
			    struct tipc_sock *tsk)
{
	int err;
	void *hdr;
	struct nlattr *attrs;
	struct net *net = sock_net(skb->sk);
	struct tipc_net *tn = net_generic(net, tipc_net_id);
	struct sock *sk = &tsk->sk;

	hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
			  &tipc_genl_family, NLM_F_MULTI, TIPC_NL_SOCK_GET);
	if (!hdr)
		goto msg_cancel;

	attrs = nla_nest_start(skb, TIPC_NLA_SOCK);
	if (!attrs)
		goto genlmsg_cancel;
	if (nla_put_u32(skb, TIPC_NLA_SOCK_REF, tsk->portid))
		goto attr_msg_cancel;
	if (nla_put_u32(skb, TIPC_NLA_SOCK_ADDR, tn->own_addr))
		goto attr_msg_cancel;

	if (tipc_sk_connected(sk)) {
		err = __tipc_nl_add_sk_con(skb, tsk);
		if (err)
			goto attr_msg_cancel;
	} else if (!list_empty(&tsk->publications)) {
		if (nla_put_flag(skb, TIPC_NLA_SOCK_HAS_PUBL))
			goto attr_msg_cancel;
	}
	nla_nest_end(skb, attrs);
	genlmsg_end(skb, hdr);

	return 0;

attr_msg_cancel:
	nla_nest_cancel(skb, attrs);
genlmsg_cancel:
	genlmsg_cancel(skb, hdr);
msg_cancel:
	return -EMSGSIZE;
}

int tipc_nl_sk_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
	int err;
	struct tipc_sock *tsk;
	const struct bucket_table *tbl;
	struct rhash_head *pos;
	struct net *net = sock_net(skb->sk);
	struct tipc_net *tn = net_generic(net, tipc_net_id);
	u32 tbl_id = cb->args[0];
	u32 prev_portid = cb->args[1];

	rcu_read_lock();
	tbl = rht_dereference_rcu((&tn->sk_rht)->tbl, &tn->sk_rht);
	for (; tbl_id < tbl->size; tbl_id++) {
		rht_for_each_entry_rcu(tsk, pos, tbl, tbl_id, node) {
			spin_lock_bh(&tsk->sk.sk_lock.slock);
			if (prev_portid && prev_portid != tsk->portid) {
				spin_unlock_bh(&tsk->sk.sk_lock.slock);
				continue;
			}

			err = __tipc_nl_add_sk(skb, cb, tsk);
			if (err) {
				prev_portid = tsk->portid;
				spin_unlock_bh(&tsk->sk.sk_lock.slock);
				goto out;
			}
			prev_portid = 0;
			spin_unlock_bh(&tsk->sk.sk_lock.slock);
		}
	}
out:
	rcu_read_unlock();
	cb->args[0] = tbl_id;
	cb->args[1] = prev_portid;

	return skb->len;
}

/* Caller should hold socket lock for the passed tipc socket. */
static int __tipc_nl_add_sk_publ(struct sk_buff *skb,
				 struct netlink_callback *cb,
				 struct publication *publ)
{
	void *hdr;
	struct nlattr *attrs;

	hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
			  &tipc_genl_family, NLM_F_MULTI, TIPC_NL_PUBL_GET);
	if (!hdr)
		goto msg_cancel;

	attrs = nla_nest_start(skb, TIPC_NLA_PUBL);
	if (!attrs)
		goto genlmsg_cancel;

	if (nla_put_u32(skb, TIPC_NLA_PUBL_KEY, publ->key))
		goto attr_msg_cancel;
	if (nla_put_u32(skb, TIPC_NLA_PUBL_TYPE, publ->type))
		goto attr_msg_cancel;
	if (nla_put_u32(skb, TIPC_NLA_PUBL_LOWER, publ->lower))
		goto attr_msg_cancel;
	if (nla_put_u32(skb, TIPC_NLA_PUBL_UPPER, publ->upper))
		goto attr_msg_cancel;

	nla_nest_end(skb, attrs);
	genlmsg_end(skb, hdr);

	return 0;

attr_msg_cancel:
	nla_nest_cancel(skb, attrs);
genlmsg_cancel:
	genlmsg_cancel(skb, hdr);
msg_cancel:
	return -EMSGSIZE;
}

/* Caller should hold socket lock for the passed tipc socket. */
static int __tipc_nl_list_sk_publ(struct sk_buff *skb,
				  struct netlink_callback *cb,
				  struct tipc_sock *tsk, u32 *last_publ)
{
	int err;
	struct publication *p;

	if (*last_publ) {
		list_for_each_entry(p, &tsk->publications, pport_list) {
			if (p->key == *last_publ)
				break;
		}
		if (p->key != *last_publ) {
			/* We never set seq or call nl_dump_check_consistent()
			 * this means that setting prev_seq here will cause the
			 * consistence check to fail in the netlink callback
			 * handler. Resulting in the last NLMSG_DONE message
			 * having the NLM_F_DUMP_INTR flag set.
			 */
			cb->prev_seq = 1;
			*last_publ = 0;
			return -EPIPE;
		}
	} else {
		p = list_first_entry(&tsk->publications, struct publication,
				     pport_list);
	}

	list_for_each_entry_from(p, &tsk->publications, pport_list) {
		err = __tipc_nl_add_sk_publ(skb, cb, p);
		if (err) {
			*last_publ = p->key;
			return err;
		}
	}
	*last_publ = 0;

	return 0;
}

int tipc_nl_publ_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
	int err;
	u32 tsk_portid = cb->args[0];
	u32 last_publ = cb->args[1];
	u32 done = cb->args[2];
	struct net *net = sock_net(skb->sk);
	struct tipc_sock *tsk;

	if (!tsk_portid) {
		struct nlattr **attrs;
		struct nlattr *sock[TIPC_NLA_SOCK_MAX + 1];

		err = tipc_nlmsg_parse(cb->nlh, &attrs);
		if (err)
			return err;

		if (!attrs[TIPC_NLA_SOCK])
			return -EINVAL;

		err = nla_parse_nested(sock, TIPC_NLA_SOCK_MAX,
				       attrs[TIPC_NLA_SOCK],
				       tipc_nl_sock_policy, NULL);
		if (err)
			return err;

		if (!sock[TIPC_NLA_SOCK_REF])
			return -EINVAL;

		tsk_portid = nla_get_u32(sock[TIPC_NLA_SOCK_REF]);
	}

	if (done)
		return 0;

	tsk = tipc_sk_lookup(net, tsk_portid);
	if (!tsk)
		return -EINVAL;

	lock_sock(&tsk->sk);
	err = __tipc_nl_list_sk_publ(skb, cb, tsk, &last_publ);
	if (!err)
		done = 1;
	release_sock(&tsk->sk);
	sock_put(&tsk->sk);

	cb->args[0] = tsk_portid;
	cb->args[1] = last_publ;
	cb->args[2] = done;

	return skb->len;
}
