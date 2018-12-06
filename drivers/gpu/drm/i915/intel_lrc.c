/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Ben Widawsky <ben@bwidawsk.net>
 *    Michel Thierry <michel.thierry@intel.com>
 *    Thomas Daniel <thomas.daniel@intel.com>
 *    Oscar Mateo <oscar.mateo@intel.com>
 *
 */

/**
 * DOC: Logical Rings, Logical Ring Contexts and Execlists
 *
 * Motivation:
 * GEN8 brings an expansion of the HW contexts: "Logical Ring Contexts".
 * These expanded contexts enable a number of new abilities, especially
 * "Execlists" (also implemented in this file).
 *
 * One of the main differences with the legacy HW contexts is that logical
 * ring contexts incorporate many more things to the context's state, like
 * PDPs or ringbuffer control registers:
 *
 * The reason why PDPs are included in the context is straightforward: as
 * PPGTTs (per-process GTTs) are actually per-context, having the PDPs
 * contained there mean you don't need to do a ppgtt->switch_mm yourself,
 * instead, the GPU will do it for you on the context switch.
 *
 * But, what about the ringbuffer control registers (head, tail, etc..)?
 * shouldn't we just need a set of those per engine command streamer? This is
 * where the name "Logical Rings" starts to make sense: by virtualizing the
 * rings, the engine cs shifts to a new "ring buffer" with every context
 * switch. When you want to submit a workload to the GPU you: A) choose your
 * context, B) find its appropriate virtualized ring, C) write commands to it
 * and then, finally, D) tell the GPU to switch to that context.
 *
 * Instead of the legacy MI_SET_CONTEXT, the way you tell the GPU to switch
 * to a contexts is via a context execution list, ergo "Execlists".
 *
 * LRC implementation:
 * Regarding the creation of contexts, we have:
 *
 * - One global default context.
 * - One local default context for each opened fd.
 * - One local extra context for each context create ioctl call.
 *
 * Now that ringbuffers belong per-context (and not per-engine, like before)
 * and that contexts are uniquely tied to a given engine (and not reusable,
 * like before) we need:
 *
 * - One ringbuffer per-engine inside each context.
 * - One backing object per-engine inside each context.
 *
 * The global default context starts its life with these new objects fully
 * allocated and populated. The local default context for each opened fd is
 * more complex, because we don't know at creation time which engine is going
 * to use them. To handle this, we have implemented a deferred creation of LR
 * contexts:
 *
 * The local context starts its life as a hollow or blank holder, that only
 * gets populated for a given engine once we receive an execbuffer. If later
 * on we receive another execbuffer ioctl for the same context but a different
 * engine, we allocate/populate a new ringbuffer and context backing object and
 * so on.
 *
 * Finally, regarding local contexts created using the ioctl call: as they are
 * only allowed with the render ring, we can allocate & populate them right
 * away (no need to defer anything, at least for now).
 *
 * Execlists implementation:
 * Execlists are the new method by which, on gen8+ hardware, workloads are
 * submitted for execution (as opposed to the legacy, ringbuffer-based, method).
 * This method works as follows:
 *
 * When a request is committed, its commands (the BB start and any leading or
 * trailing commands, like the seqno breadcrumbs) are placed in the ringbuffer
 * for the appropriate context. The tail pointer in the hardware context is not
 * updated at this time, but instead, kept by the driver in the ringbuffer
 * structure. A structure representing this request is added to a request queue
 * for the appropriate engine: this structure contains a copy of the context's
 * tail after the request was written to the ring buffer and a pointer to the
 * context itself.
 *
 * If the engine's request queue was empty before the request was added, the
 * queue is processed immediately. Otherwise the queue will be processed during
 * a context switch interrupt. In any case, elements on the queue will get sent
 * (in pairs) to the GPU's ExecLists Submit Port (ELSP, for short) with a
 * globally unique 20-bits submission ID.
 *
 * When execution of a request completes, the GPU updates the context status
 * buffer with a context complete event and generates a context switch interrupt.
 * During the interrupt handling, the driver examines the events in the buffer:
 * for each context complete event, if the announced ID matches that on the head
 * of the request queue, then that request is retired and removed from the queue.
 *
 * After processing, if any requests were retired and the queue is not empty
 * then a new execution list can be submitted. The two requests at the front of
 * the queue are next to be submitted but since a context may not occur twice in
 * an execution list, if subsequent requests have the same ID as the first then
 * the two requests must be combined. This is done simply by discarding requests
 * at the head of the queue until either only one requests is left (in which case
 * we use a NULL second context) or the first two requests have unique IDs.
 *
 * By always executing the first two requests in the queue the driver ensures
 * that the GPU is kept as busy as possible. In the case where a single context
 * completes but a second context is still executing, the request for this second
 * context will be at the head of the queue when we remove the first one. This
 * request will then be resubmitted along with a new request for a different context,
 * which will cause the hardware to continue executing the second request and queue
 * the new request (the GPU detects the condition of a context getting preempted
 * with the same context and optimizes the context switch flow by not doing
 * preemption, but just sampling the new tail pointer).
 *
 */
#include <linux/interrupt.h>

#include <drm/drmP.h>
#include <drm/i915_drm.h>
#include "i915_drv.h"
#include "intel_mocs.h"

#define RING_EXECLIST_QFULL		(1 << 0x2)
#define RING_EXECLIST1_VALID		(1 << 0x3)
#define RING_EXECLIST0_VALID		(1 << 0x4)
#define RING_EXECLIST_ACTIVE_STATUS	(3 << 0xE)
#define RING_EXECLIST1_ACTIVE		(1 << 0x11)
#define RING_EXECLIST0_ACTIVE		(1 << 0x12)

#define GEN8_CTX_STATUS_IDLE_ACTIVE	(1 << 0)
#define GEN8_CTX_STATUS_PREEMPTED	(1 << 1)
#define GEN8_CTX_STATUS_ELEMENT_SWITCH	(1 << 2)
#define GEN8_CTX_STATUS_ACTIVE_IDLE	(1 << 3)
#define GEN8_CTX_STATUS_COMPLETE	(1 << 4)
#define GEN8_CTX_STATUS_LITE_RESTORE	(1 << 15)

#define GEN8_CTX_STATUS_COMPLETED_MASK \
	 (GEN8_CTX_STATUS_ACTIVE_IDLE | \
	  GEN8_CTX_STATUS_PREEMPTED | \
	  GEN8_CTX_STATUS_ELEMENT_SWITCH)

#define CTX_LRI_HEADER_0		0x01
#define CTX_CONTEXT_CONTROL		0x02
#define CTX_RING_HEAD			0x04
#define CTX_RING_TAIL			0x06
#define CTX_RING_BUFFER_START		0x08
#define CTX_RING_BUFFER_CONTROL		0x0a
#define CTX_BB_HEAD_U			0x0c
#define CTX_BB_HEAD_L			0x0e
#define CTX_BB_STATE			0x10
#define CTX_SECOND_BB_HEAD_U		0x12
#define CTX_SECOND_BB_HEAD_L		0x14
#define CTX_SECOND_BB_STATE		0x16
#define CTX_BB_PER_CTX_PTR		0x18
#define CTX_RCS_INDIRECT_CTX		0x1a
#define CTX_RCS_INDIRECT_CTX_OFFSET	0x1c
#define CTX_LRI_HEADER_1		0x21
#define CTX_CTX_TIMESTAMP		0x22
#define CTX_PDP3_UDW			0x24
#define CTX_PDP3_LDW			0x26
#define CTX_PDP2_UDW			0x28
#define CTX_PDP2_LDW			0x2a
#define CTX_PDP1_UDW			0x2c
#define CTX_PDP1_LDW			0x2e
#define CTX_PDP0_UDW			0x30
#define CTX_PDP0_LDW			0x32
#define CTX_LRI_HEADER_2		0x41
#define CTX_R_PWR_CLK_STATE		0x42
#define CTX_GPGPU_CSR_BASE_ADDRESS	0x44

#define CTX_REG(reg_state, pos, reg, val) do { \
	(reg_state)[(pos)+0] = i915_mmio_reg_offset(reg); \
	(reg_state)[(pos)+1] = (val); \
} while (0)

#define ASSIGN_CTX_PDP(ppgtt, reg_state, n) do {		\
	const u64 _addr = i915_page_dir_dma_addr((ppgtt), (n));	\
	reg_state[CTX_PDP ## n ## _UDW+1] = upper_32_bits(_addr); \
	reg_state[CTX_PDP ## n ## _LDW+1] = lower_32_bits(_addr); \
} while (0)

#define ASSIGN_CTX_PML4(ppgtt, reg_state) do { \
	reg_state[CTX_PDP0_UDW + 1] = upper_32_bits(px_dma(&ppgtt->pml4)); \
	reg_state[CTX_PDP0_LDW + 1] = lower_32_bits(px_dma(&ppgtt->pml4)); \
} while (0)

#define GEN8_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT	0x17
#define GEN9_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT	0x26
#define GEN10_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT	0x19

/* Typical size of the average request (2 pipecontrols and a MI_BB) */
#define EXECLISTS_REQUEST_SIZE 64 /* bytes */

#define WA_TAIL_DWORDS 2

static int execlists_context_deferred_alloc(struct i915_gem_context *ctx,
					    struct intel_engine_cs *engine);
static void execlists_init_reg_state(u32 *reg_state,
				     struct i915_gem_context *ctx,
				     struct intel_engine_cs *engine,
				     struct intel_ring *ring);

/**
 * intel_sanitize_enable_execlists() - sanitize i915.enable_execlists
 * @dev_priv: i915 device private
 * @enable_execlists: value of i915.enable_execlists module parameter.
 *
 * Only certain platforms support Execlists (the prerequisites being
 * support for Logical Ring Contexts and Aliasing PPGTT or better).
 *
 * Return: 1 if Execlists is supported and has to be enabled.
 */
int intel_sanitize_enable_execlists(struct drm_i915_private *dev_priv, int enable_execlists)
{
	/* On platforms with execlist available, vGPU will only
	 * support execlist mode, no ring buffer mode.
	 */
	if (HAS_LOGICAL_RING_CONTEXTS(dev_priv) && intel_vgpu_active(dev_priv))
		return 1;

	if (INTEL_GEN(dev_priv) >= 9)
		return 1;

	if (enable_execlists == 0)
		return 0;

	if (HAS_LOGICAL_RING_CONTEXTS(dev_priv) &&
	    USES_PPGTT(dev_priv) &&
	    i915.use_mmio_flip >= 0)
		return 1;

	return 0;
}

/**
 * intel_lr_context_descriptor_update() - calculate & cache the descriptor
 * 					  descriptor for a pinned context
 * @ctx: Context to work on
 * @engine: Engine the descriptor will be used with
 *
 * The context descriptor encodes various attributes of a context,
 * including its GTT address and some flags. Because it's fairly
 * expensive to calculate, we'll just do it once and cache the result,
 * which remains valid until the context is unpinned.
 *
 * This is what a descriptor looks like, from LSB to MSB::
 *
 *      bits  0-11:    flags, GEN8_CTX_* (cached in ctx->desc_template)
 *      bits 12-31:    LRCA, GTT address of (the HWSP of) this context
 *      bits 32-52:    ctx ID, a globally unique tag
 *      bits 53-54:    mbz, reserved for use by hardware
 *      bits 55-63:    group ID, currently unused and set to 0
 */
static void
intel_lr_context_descriptor_update(struct i915_gem_context *ctx,
				   struct intel_engine_cs *engine)
{
	struct intel_context *ce = &ctx->engine[engine->id];
	u64 desc;

	BUILD_BUG_ON(MAX_CONTEXT_HW_ID > (1<<GEN8_CTX_ID_WIDTH));

	desc = ctx->desc_template;				/* bits  0-11 */
	desc |= i915_ggtt_offset(ce->state) + LRC_PPHWSP_PN * PAGE_SIZE;
								/* bits 12-31 */
	desc |= (u64)ctx->hw_id << GEN8_CTX_ID_SHIFT;		/* bits 32-52 */

	ce->lrc_desc = desc;
}

uint64_t intel_lr_context_descriptor(struct i915_gem_context *ctx,
				     struct intel_engine_cs *engine)
{
	return ctx->engine[engine->id].lrc_desc;
}

static inline void
execlists_context_status_change(struct drm_i915_gem_request *rq,
				unsigned long status)
{
	/*
	 * Only used when GVT-g is enabled now. When GVT-g is disabled,
	 * The compiler should eliminate this function as dead-code.
	 */
	if (!IS_ENABLED(CONFIG_DRM_I915_GVT))
		return;

	atomic_notifier_call_chain(&rq->engine->context_status_notifier,
				   status, rq);
}

static void
execlists_update_context_pdps(struct i915_hw_ppgtt *ppgtt, u32 *reg_state)
{
	ASSIGN_CTX_PDP(ppgtt, reg_state, 3);
	ASSIGN_CTX_PDP(ppgtt, reg_state, 2);
	ASSIGN_CTX_PDP(ppgtt, reg_state, 1);
	ASSIGN_CTX_PDP(ppgtt, reg_state, 0);
}

static u64 execlists_update_context(struct drm_i915_gem_request *rq)
{
	struct intel_context *ce = &rq->ctx->engine[rq->engine->id];
	struct i915_hw_ppgtt *ppgtt =
		rq->ctx->ppgtt ?: rq->i915->mm.aliasing_ppgtt;
	u32 *reg_state = ce->lrc_reg_state;

	reg_state[CTX_RING_TAIL+1] = intel_ring_set_tail(rq->ring, rq->tail);

	/*
	 * True 32b PPGTT with dynamic page allocation: update PDP
	 * registers and point the unallocated PDPs to scratch page.
	 * PML4 is allocated during ppgtt init, so this is not needed
	 * in 48-bit mode.
	 */
	if (ppgtt && !i915_vm_is_48bit(&ppgtt->base))
		execlists_update_context_pdps(ppgtt, reg_state);

	/*
	 * Make sure the context image is complete before we submit it to HW.
	 *
	 * Ostensibly, writes (including the WCB) should be flushed prior to
	 * an uncached write such as our mmio register access, the empirical
	 * evidence (esp. on Braswell) suggests that the WC write into memory
	 * may not be visible to the HW prior to the completion of the UC
	 * register write and that we may begin execution from the context
	 * before its image is complete leading to invalid PD chasing.
	 *
	 * Furthermore, Braswell, at least, wants a full mb to be sure that
	 * the writes are coherent in memory (visible to the GPU) prior to
	 * execution, and not just visible to other CPUs (as is the result of
	 * wmb).
	 */
	mb();
	return ce->lrc_desc;
}

static void execlists_submit_ports(struct intel_engine_cs *engine)
{
	struct execlist_port *port = engine->execlist_port;
	u32 __iomem *elsp =
		engine->i915->regs + i915_mmio_reg_offset(RING_ELSP(engine));
	unsigned int n;

	for (n = ARRAY_SIZE(engine->execlist_port); n--; ) {
		struct drm_i915_gem_request *rq;
		unsigned int count;
		u64 desc;

		rq = port_unpack(&port[n], &count);
		if (rq) {
			GEM_BUG_ON(count > !n);
			if (!count++)
				execlists_context_status_change(rq, INTEL_CONTEXT_SCHEDULE_IN);
			port_set(&port[n], port_pack(rq, count));
			desc = execlists_update_context(rq);
			GEM_DEBUG_EXEC(port[n].context_id = upper_32_bits(desc));
		} else {
			GEM_BUG_ON(!n);
			desc = 0;
		}

		writel(upper_32_bits(desc), elsp);
		writel(lower_32_bits(desc), elsp);
	}
}

static bool ctx_single_port_submission(const struct i915_gem_context *ctx)
{
	return (IS_ENABLED(CONFIG_DRM_I915_GVT) &&
		i915_gem_context_force_single_submission(ctx));
}

static bool can_merge_ctx(const struct i915_gem_context *prev,
			  const struct i915_gem_context *next)
{
	if (prev != next)
		return false;

	if (ctx_single_port_submission(prev))
		return false;

	return true;
}

static void port_assign(struct execlist_port *port,
			struct drm_i915_gem_request *rq)
{
	GEM_BUG_ON(rq == port_request(port));

	if (port_isset(port))
		i915_gem_request_put(port_request(port));

	port_set(port, port_pack(i915_gem_request_get(rq), port_count(port)));
}

static void execlists_dequeue(struct intel_engine_cs *engine)
{
	struct drm_i915_gem_request *last;
	struct execlist_port *port = engine->execlist_port;
	struct rb_node *rb;
	bool submit = false;

	last = port_request(port);
	if (last)
		/* WaIdleLiteRestore:bdw,skl
		 * Apply the wa NOOPs to prevent ring:HEAD == req:TAIL
		 * as we resubmit the request. See gen8_emit_breadcrumb()
		 * for where we prepare the padding after the end of the
		 * request.
		 */
		last->tail = last->wa_tail;

	GEM_BUG_ON(port_isset(&port[1]));

	/* Hardware submission is through 2 ports. Conceptually each port
	 * has a (RING_START, RING_HEAD, RING_TAIL) tuple. RING_START is
	 * static for a context, and unique to each, so we only execute
	 * requests belonging to a single context from each ring. RING_HEAD
	 * is maintained by the CS in the context image, it marks the place
	 * where it got up to last time, and through RING_TAIL we tell the CS
	 * where we want to execute up to this time.
	 *
	 * In this list the requests are in order of execution. Consecutive
	 * requests from the same context are adjacent in the ringbuffer. We
	 * can combine these requests into a single RING_TAIL update:
	 *
	 *              RING_HEAD...req1...req2
	 *                                    ^- RING_TAIL
	 * since to execute req2 the CS must first execute req1.
	 *
	 * Our goal then is to point each port to the end of a consecutive
	 * sequence of requests as being the most optimal (fewest wake ups
	 * and context switches) submission.
	 */

	spin_lock_irq(&engine->timeline->lock);
	rb = engine->execlist_first;
	GEM_BUG_ON(rb_first(&engine->execlist_queue) != rb);
	while (rb) {
		struct i915_priolist *p = rb_entry(rb, typeof(*p), node);
		struct drm_i915_gem_request *rq, *rn;

		list_for_each_entry_safe(rq, rn, &p->requests, priotree.link) {
			/*
			 * Can we combine this request with the current port?
			 * It has to be the same context/ringbuffer and not
			 * have any exceptions (e.g. GVT saying never to
			 * combine contexts).
			 *
			 * If we can combine the requests, we can execute both
			 * by updating the RING_TAIL to point to the end of the
			 * second request, and so we never need to tell the
			 * hardware about the first.
			 */
			if (last && !can_merge_ctx(rq->ctx, last->ctx)) {
				/*
				 * If we are on the second port and cannot
				 * combine this request with the last, then we
				 * are done.
				 */
				if (port != engine->execlist_port) {
					__list_del_many(&p->requests,
							&rq->priotree.link);
					goto done;
				}

				/*
				 * If GVT overrides us we only ever submit
				 * port[0], leaving port[1] empty. Note that we
				 * also have to be careful that we don't queue
				 * the same context (even though a different
				 * request) to the second port.
				 */
				if (ctx_single_port_submission(last->ctx) ||
				    ctx_single_port_submission(rq->ctx)) {
					__list_del_many(&p->requests,
							&rq->priotree.link);
					goto done;
				}

				GEM_BUG_ON(last->ctx == rq->ctx);

				if (submit)
					port_assign(port, last);
				port++;
			}

			INIT_LIST_HEAD(&rq->priotree.link);
			rq->priotree.priority = INT_MAX;

			__i915_gem_request_submit(rq);
			trace_i915_gem_request_in(rq, port_index(port, engine));
			last = rq;
			submit = true;
		}

		rb = rb_next(rb);
		rb_erase(&p->node, &engine->execlist_queue);
		INIT_LIST_HEAD(&p->requests);
		if (p->priority != I915_PRIORITY_NORMAL)
			kmem_cache_free(engine->i915->priorities, p);
	}
done:
	engine->execlist_first = rb;
	if (submit)
		port_assign(port, last);
	spin_unlock_irq(&engine->timeline->lock);

	if (submit)
		execlists_submit_ports(engine);
}

static bool execlists_elsp_ready(const struct intel_engine_cs *engine)
{
	const struct execlist_port *port = engine->execlist_port;

	return port_count(&port[0]) + port_count(&port[1]) < 2;
}

/*
 * Check the unread Context Status Buffers and manage the submission of new
 * contexts to the ELSP accordingly.
 */
static void intel_lrc_irq_handler(unsigned long data)
{
	struct intel_engine_cs *engine = (struct intel_engine_cs *)data;
	struct execlist_port *port = engine->execlist_port;
	struct drm_i915_private *dev_priv = engine->i915;

	/* We can skip acquiring intel_runtime_pm_get() here as it was taken
	 * on our behalf by the request (see i915_gem_mark_busy()) and it will
	 * not be relinquished until the device is idle (see
	 * i915_gem_idle_work_handler()). As a precaution, we make sure
	 * that all ELSP are drained i.e. we have processed the CSB,
	 * before allowing ourselves to idle and calling intel_runtime_pm_put().
	 */
	GEM_BUG_ON(!dev_priv->gt.awake);

	intel_uncore_forcewake_get(dev_priv, engine->fw_domains);

	/* Prefer doing test_and_clear_bit() as a two stage operation to avoid
	 * imposing the cost of a locked atomic transaction when submitting a
	 * new request (outside of the context-switch interrupt).
	 */
	while (test_bit(ENGINE_IRQ_EXECLIST, &engine->irq_posted)) {
		u32 __iomem *csb_mmio =
			dev_priv->regs + i915_mmio_reg_offset(RING_CONTEXT_STATUS_PTR(engine));
		u32 __iomem *buf =
			dev_priv->regs + i915_mmio_reg_offset(RING_CONTEXT_STATUS_BUF_LO(engine, 0));
		unsigned int head, tail;

		/* The write will be ordered by the uncached read (itself
		 * a memory barrier), so we do not need another in the form
		 * of a locked instruction. The race between the interrupt
		 * handler and the split test/clear is harmless as we order
		 * our clear before the CSB read. If the interrupt arrived
		 * first between the test and the clear, we read the updated
		 * CSB and clear the bit. If the interrupt arrives as we read
		 * the CSB or later (i.e. after we had cleared the bit) the bit
		 * is set and we do a new loop.
		 */
		__clear_bit(ENGINE_IRQ_EXECLIST, &engine->irq_posted);
		head = readl(csb_mmio);
		tail = GEN8_CSB_WRITE_PTR(head);
		head = GEN8_CSB_READ_PTR(head);
		while (head != tail) {
			struct drm_i915_gem_request *rq;
			unsigned int status;
			unsigned int count;

			if (++head == GEN8_CSB_ENTRIES)
				head = 0;

			/* We are flying near dragons again.
			 *
			 * We hold a reference to the request in execlist_port[]
			 * but no more than that. We are operating in softirq
			 * context and so cannot hold any mutex or sleep. That
			 * prevents us stopping the requests we are processing
			 * in port[] from being retired simultaneously (the
			 * breadcrumb will be complete before we see the
			 * context-switch). As we only hold the reference to the
			 * request, any pointer chasing underneath the request
			 * is subject to a potential use-after-free. Thus we
			 * store all of the bookkeeping within port[] as
			 * required, and avoid using unguarded pointers beneath
			 * request itself. The same applies to the atomic
			 * status notifier.
			 */

			status = readl(buf + 2 * head);
			if (!(status & GEN8_CTX_STATUS_COMPLETED_MASK))
				continue;

			/* Check the context/desc id for this event matches */
			GEM_DEBUG_BUG_ON(readl(buf + 2 * head + 1) !=
					 port->context_id);

			rq = port_unpack(port, &count);
			GEM_BUG_ON(count == 0);
			if (--count == 0) {
				GEM_BUG_ON(status & GEN8_CTX_STATUS_PREEMPTED);
				GEM_BUG_ON(!i915_gem_request_completed(rq));
				execlists_context_status_change(rq, INTEL_CONTEXT_SCHEDULE_OUT);

				trace_i915_gem_request_out(rq);
				i915_gem_request_put(rq);

				port[0] = port[1];
				memset(&port[1], 0, sizeof(port[1]));
			} else {
				port_set(port, port_pack(rq, count));
			}

			/* After the final element, the hw should be idle */
			GEM_BUG_ON(port_count(port) == 0 &&
				   !(status & GEN8_CTX_STATUS_ACTIVE_IDLE));
		}

		writel(_MASKED_FIELD(GEN8_CSB_READ_PTR_MASK, head << 8),
		       csb_mmio);
	}

	if (execlists_elsp_ready(engine))
		execlists_dequeue(engine);

	intel_uncore_forcewake_put(dev_priv, engine->fw_domains);
}

static bool
insert_request(struct intel_engine_cs *engine,
	       struct i915_priotree *pt,
	       int prio)
{
	struct i915_priolist *p;
	struct rb_node **parent, *rb;
	bool first = true;

	if (unlikely(engine->no_priolist))
		prio = I915_PRIORITY_NORMAL;

find_priolist:
	/* most positive priority is scheduled first, equal priorities fifo */
	rb = NULL;
	parent = &engine->execlist_queue.rb_node;
	while (*parent) {
		rb = *parent;
		p = rb_entry(rb, typeof(*p), node);
		if (prio > p->priority) {
			parent = &rb->rb_left;
		} else if (prio < p->priority) {
			parent = &rb->rb_right;
			first = false;
		} else {
			list_add_tail(&pt->link, &p->requests);
			return false;
		}
	}

	if (prio == I915_PRIORITY_NORMAL) {
		p = &engine->default_priolist;
	} else {
		p = kmem_cache_alloc(engine->i915->priorities, GFP_ATOMIC);
		/* Convert an allocation failure to a priority bump */
		if (unlikely(!p)) {
			prio = I915_PRIORITY_NORMAL; /* recurses just once */

			/* To maintain ordering with all rendering, after an
			 * allocation failure we have to disable all scheduling.
			 * Requests will then be executed in fifo, and schedule
			 * will ensure that dependencies are emitted in fifo.
			 * There will be still some reordering with existing
			 * requests, so if userspace lied about their
			 * dependencies that reordering may be visible.
			 */
			engine->no_priolist = true;
			goto find_priolist;
		}
	}

	p->priority = prio;
	rb_link_node(&p->node, rb, parent);
	rb_insert_color(&p->node, &engine->execlist_queue);

	INIT_LIST_HEAD(&p->requests);
	list_add_tail(&pt->link, &p->requests);

	if (first)
		engine->execlist_first = &p->node;

	return first;
}

static void execlists_submit_request(struct drm_i915_gem_request *request)
{
	struct intel_engine_cs *engine = request->engine;
	unsigned long flags;

	/* Will be called from irq-context when using foreign fences. */
	spin_lock_irqsave(&engine->timeline->lock, flags);

	if (insert_request(engine,
			   &request->priotree,
			   request->priotree.priority)) {
		if (execlists_elsp_ready(engine))
			tasklet_hi_schedule(&engine->irq_tasklet);
	}

	GEM_BUG_ON(!engine->execlist_first);
	GEM_BUG_ON(list_empty(&request->priotree.link));

	spin_unlock_irqrestore(&engine->timeline->lock, flags);
}

static struct intel_engine_cs *
pt_lock_engine(struct i915_priotree *pt, struct intel_engine_cs *locked)
{
	struct intel_engine_cs *engine =
		container_of(pt, struct drm_i915_gem_request, priotree)->engine;

	GEM_BUG_ON(!locked);

	if (engine != locked) {
		spin_unlock(&locked->timeline->lock);
		spin_lock(&engine->timeline->lock);
	}

	return engine;
}

static void execlists_schedule(struct drm_i915_gem_request *request, int prio)
{
	struct intel_engine_cs *engine;
	struct i915_dependency *dep, *p;
	struct i915_dependency stack;
	LIST_HEAD(dfs);

	if (prio <= READ_ONCE(request->priotree.priority))
		return;

	/* Need BKL in order to use the temporary link inside i915_dependency */
	lockdep_assert_held(&request->i915->drm.struct_mutex);

	stack.signaler = &request->priotree;
	list_add(&stack.dfs_link, &dfs);

	/* Recursively bump all dependent priorities to match the new request.
	 *
	 * A naive approach would be to use recursion:
	 * static void update_priorities(struct i915_priotree *pt, prio) {
	 *	list_for_each_entry(dep, &pt->signalers_list, signal_link)
	 *		update_priorities(dep->signal, prio)
	 *	insert_request(pt);
	 * }
	 * but that may have unlimited recursion depth and so runs a very
	 * real risk of overunning the kernel stack. Instead, we build
	 * a flat list of all dependencies starting with the current request.
	 * As we walk the list of dependencies, we add all of its dependencies
	 * to the end of the list (this may include an already visited
	 * request) and continue to walk onwards onto the new dependencies. The
	 * end result is a topological list of requests in reverse order, the
	 * last element in the list is the request we must execute first.
	 */
	list_for_each_entry_safe(dep, p, &dfs, dfs_link) {
		struct i915_priotree *pt = dep->signaler;

		/* Within an engine, there can be no cycle, but we may
		 * refer to the same dependency chain multiple times
		 * (redundant dependencies are not eliminated) and across
		 * engines.
		 */
		list_for_each_entry(p, &pt->signalers_list, signal_link) {
			GEM_BUG_ON(p->signaler->priority < pt->priority);
			if (prio > READ_ONCE(p->signaler->priority))
				list_move_tail(&p->dfs_link, &dfs);
		}

		list_safe_reset_next(dep, p, dfs_link);
	}

	/* If we didn't need to bump any existing priorities, and we haven't
	 * yet submitted this request (i.e. there is no potential race with
	 * execlists_submit_request()), we can set our own priority and skip
	 * acquiring the engine locks.
	 */
	if (request->priotree.priority == INT_MIN) {
		GEM_BUG_ON(!list_empty(&request->priotree.link));
		request->priotree.priority = prio;
		if (stack.dfs_link.next == stack.dfs_link.prev)
			return;
		__list_del_entry(&stack.dfs_link);
	}

	engine = request->engine;
	spin_lock_irq(&engine->timeline->lock);

	/* Fifo and depth-first replacement ensure our deps execute before us */
	list_for_each_entry_safe_reverse(dep, p, &dfs, dfs_link) {
		struct i915_priotree *pt = dep->signaler;

		INIT_LIST_HEAD(&dep->dfs_link);

		engine = pt_lock_engine(pt, engine);

		if (prio <= pt->priority)
			continue;

		pt->priority = prio;
		if (!list_empty(&pt->link)) {
			__list_del_entry(&pt->link);
			insert_request(engine, pt, prio);
		}
	}

	spin_unlock_irq(&engine->timeline->lock);

	/* XXX Do we need to preempt to make room for us and our deps? */
}

static struct intel_ring *
execlists_context_pin(struct intel_engine_cs *engine,
		      struct i915_gem_context *ctx)
{
	struct intel_context *ce = &ctx->engine[engine->id];
	unsigned int flags;
	void *vaddr;
	int ret;

	lockdep_assert_held(&ctx->i915->drm.struct_mutex);

	if (likely(ce->pin_count++))
		goto out;
	GEM_BUG_ON(!ce->pin_count); /* no overflow please! */

	if (!ce->state) {
		ret = execlists_context_deferred_alloc(ctx, engine);
		if (ret)
			goto err;
	}
	GEM_BUG_ON(!ce->state);

	flags = PIN_GLOBAL | PIN_HIGH;
	if (ctx->ggtt_offset_bias)
		flags |= PIN_OFFSET_BIAS | ctx->ggtt_offset_bias;

	ret = i915_vma_pin(ce->state, 0, GEN8_LR_CONTEXT_ALIGN, flags);
	if (ret)
		goto err;

	vaddr = i915_gem_object_pin_map(ce->state->obj, I915_MAP_WB);
	if (IS_ERR(vaddr)) {
		ret = PTR_ERR(vaddr);
		goto unpin_vma;
	}

	ret = intel_ring_pin(ce->ring, ctx->i915, ctx->ggtt_offset_bias);
	if (ret)
		goto unpin_map;

	intel_lr_context_descriptor_update(ctx, engine);

	ce->lrc_reg_state = vaddr + LRC_STATE_PN * PAGE_SIZE;
	ce->lrc_reg_state[CTX_RING_BUFFER_START+1] =
		i915_ggtt_offset(ce->ring->vma);

	ce->state->obj->mm.dirty = true;

	i915_gem_context_get(ctx);
out:
	return ce->ring;

unpin_map:
	i915_gem_object_unpin_map(ce->state->obj);
unpin_vma:
	__i915_vma_unpin(ce->state);
err:
	ce->pin_count = 0;
	return ERR_PTR(ret);
}

static void execlists_context_unpin(struct intel_engine_cs *engine,
				    struct i915_gem_context *ctx)
{
	struct intel_context *ce = &ctx->engine[engine->id];

	lockdep_assert_held(&ctx->i915->drm.struct_mutex);
	GEM_BUG_ON(ce->pin_count == 0);

	if (--ce->pin_count)
		return;

	intel_ring_unpin(ce->ring);

	i915_gem_object_unpin_map(ce->state->obj);
	i915_vma_unpin(ce->state);

	i915_gem_context_put(ctx);
}

static int execlists_request_alloc(struct drm_i915_gem_request *request)
{
	struct intel_engine_cs *engine = request->engine;
	struct intel_context *ce = &request->ctx->engine[engine->id];
	u32 *cs;
	int ret;

	GEM_BUG_ON(!ce->pin_count);

	/* Flush enough space to reduce the likelihood of waiting after
	 * we start building the request - in which case we will just
	 * have to repeat work.
	 */
	request->reserved_space += EXECLISTS_REQUEST_SIZE;

	if (i915.enable_guc_submission) {
		/*
		 * Check that the GuC has space for the request before
		 * going any further, as the i915_add_request() call
		 * later on mustn't fail ...
		 */
		ret = i915_guc_wq_reserve(request);
		if (ret)
			goto err;
	}

	cs = intel_ring_begin(request, 0);
	if (IS_ERR(cs)) {
		ret = PTR_ERR(cs);
		goto err_unreserve;
	}

	if (!ce->initialised) {
		ret = engine->init_context(request);
		if (ret)
			goto err_unreserve;

		ce->initialised = true;
	}

	/* Note that after this point, we have committed to using
	 * this request as it is being used to both track the
	 * state of engine initialisation and liveness of the
	 * golden renderstate above. Think twice before you try
	 * to cancel/unwind this request now.
	 */

	request->reserved_space -= EXECLISTS_REQUEST_SIZE;
	return 0;

err_unreserve:
	if (i915.enable_guc_submission)
		i915_guc_wq_unreserve(request);
err:
	return ret;
}

/*
 * In this WA we need to set GEN8_L3SQCREG4[21:21] and reset it after
 * PIPE_CONTROL instruction. This is required for the flush to happen correctly
 * but there is a slight complication as this is applied in WA batch where the
 * values are only initialized once so we cannot take register value at the
 * beginning and reuse it further; hence we save its value to memory, upload a
 * constant value with bit21 set and then we restore it back with the saved value.
 * To simplify the WA, a constant value is formed by using the default value
 * of this register. This shouldn't be a problem because we are only modifying
 * it for a short period and this batch in non-premptible. We can ofcourse
 * use additional instructions that read the actual value of the register
 * at that time and set our bit of interest but it makes the WA complicated.
 *
 * This WA is also required for Gen9 so extracting as a function avoids
 * code duplication.
 */
static u32 *
gen8_emit_flush_coherentl3_wa(struct intel_engine_cs *engine, u32 *batch)
{
	*batch++ = MI_STORE_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT;
	*batch++ = i915_mmio_reg_offset(GEN8_L3SQCREG4);
	*batch++ = i915_ggtt_offset(engine->scratch) + 256;
	*batch++ = 0;

	*batch++ = MI_LOAD_REGISTER_IMM(1);
	*batch++ = i915_mmio_reg_offset(GEN8_L3SQCREG4);
	*batch++ = 0x40400000 | GEN8_LQSC_FLUSH_COHERENT_LINES;

	batch = gen8_emit_pipe_control(batch,
				       PIPE_CONTROL_CS_STALL |
				       PIPE_CONTROL_DC_FLUSH_ENABLE,
				       0);

	*batch++ = MI_LOAD_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT;
	*batch++ = i915_mmio_reg_offset(GEN8_L3SQCREG4);
	*batch++ = i915_ggtt_offset(engine->scratch) + 256;
	*batch++ = 0;

	return batch;
}

/*
 * Typically we only have one indirect_ctx and per_ctx batch buffer which are
 * initialized at the beginning and shared across all contexts but this field
 * helps us to have multiple batches at different offsets and select them based
 * on a criteria. At the moment this batch always start at the beginning of the page
 * and at this point we don't have multiple wa_ctx batch buffers.
 *
 * The number of WA applied are not known at the beginning; we use this field
 * to return the no of DWORDS written.
 *
 * It is to be noted that this batch does not contain MI_BATCH_BUFFER_END
 * so it adds NOOPs as padding to make it cacheline aligned.
 * MI_BATCH_BUFFER_END will be added to perctx batch and both of them together
 * makes a complete batch buffer.
 */
static u32 *gen8_init_indirectctx_bb(struct intel_engine_cs *engine, u32 *batch)
{
	/* WaDisableCtxRestoreArbitration:bdw,chv */
	*batch++ = MI_ARB_ON_OFF | MI_ARB_DISABLE;

	/* WaFlushCoherentL3CacheLinesAtContextSwitch:bdw */
	if (IS_BROADWELL(engine->i915))
		batch = gen8_emit_flush_coherentl3_wa(engine, batch);

	/* WaClearSlmSpaceAtContextSwitch:bdw,chv */
	/* Actual scratch location is at 128 bytes offset */
	batch = gen8_emit_pipe_control(batch,
				       PIPE_CONTROL_FLUSH_L3 |
				       PIPE_CONTROL_GLOBAL_GTT_IVB |
				       PIPE_CONTROL_CS_STALL |
				       PIPE_CONTROL_QW_WRITE,
				       i915_ggtt_offset(engine->scratch) +
				       2 * CACHELINE_BYTES);

	/* Pad to end of cacheline */
	while ((unsigned long)batch % CACHELINE_BYTES)
		*batch++ = MI_NOOP;

	/*
	 * MI_BATCH_BUFFER_END is not required in Indirect ctx BB because
	 * execution depends on the length specified in terms of cache lines
	 * in the register CTX_RCS_INDIRECT_CTX
	 */

	return batch;
}

/*
 *  This batch is started immediately after indirect_ctx batch. Since we ensure
 *  that indirect_ctx ends on a cacheline this batch is aligned automatically.
 *
 *  The number of DWORDS written are returned using this field.
 *
 *  This batch is terminated with MI_BATCH_BUFFER_END and so we need not add padding
 *  to align it with cacheline as padding after MI_BATCH_BUFFER_END is redundant.
 */
static u32 *gen8_init_perctx_bb(struct intel_engine_cs *engine, u32 *batch)
{
	/* WaDisableCtxRestoreArbitration:bdw,chv */
	*batch++ = MI_ARB_ON_OFF | MI_ARB_ENABLE;
	*batch++ = MI_BATCH_BUFFER_END;

	return batch;
}

static u32 *gen9_init_indirectctx_bb(struct intel_engine_cs *engine, u32 *batch)
{
	/* WaFlushCoherentL3CacheLinesAtContextSwitch:skl,bxt,glk */
	batch = gen8_emit_flush_coherentl3_wa(engine, batch);

	*batch++ = MI_LOAD_REGISTER_IMM(3);

	/* WaDisableGatherAtSetShaderCommonSlice:skl,bxt,kbl,glk */
	*batch++ = i915_mmio_reg_offset(COMMON_SLICE_CHICKEN2);
	*batch++ = _MASKED_BIT_DISABLE(
			GEN9_DISABLE_GATHER_AT_SET_SHADER_COMMON_SLICE);

	/* BSpec: 11391 */
	*batch++ = i915_mmio_reg_offset(FF_SLICE_CHICKEN);
	*batch++ = _MASKED_BIT_ENABLE(FF_SLICE_CHICKEN_CL_PROVOKING_VERTEX_FIX);

	/* BSpec: 11299 */
	*batch++ = i915_mmio_reg_offset(_3D_CHICKEN3);
	*batch++ = _MASKED_BIT_ENABLE(_3D_CHICKEN_SF_PROVOKING_VERTEX_FIX);

	*batch++ = MI_NOOP;

	/* WaClearSlmSpaceAtContextSwitch:kbl */
	/* Actual scratch location is at 128 bytes offset */
	if (IS_KBL_REVID(engine->i915, 0, KBL_REVID_A0)) {
		batch = gen8_emit_pipe_control(batch,
					       PIPE_CONTROL_FLUSH_L3 |
					       PIPE_CONTROL_GLOBAL_GTT_IVB |
					       PIPE_CONTROL_CS_STALL |
					       PIPE_CONTROL_QW_WRITE,
					       i915_ggtt_offset(engine->scratch)
					       + 2 * CACHELINE_BYTES);
	}

	/* WaMediaPoolStateCmdInWABB:bxt,glk */
	if (HAS_POOLED_EU(engine->i915)) {
		/*
		 * EU pool configuration is setup along with golden context
		 * during context initialization. This value depends on
		 * device type (2x6 or 3x6) and needs to be updated based
		 * on which subslice is disabled especially for 2x6
		 * devices, however it is safe to load default
		 * configuration of 3x6 device instead of masking off
		 * corresponding bits because HW ignores bits of a disabled
		 * subslice and drops down to appropriate config. Please
		 * see render_state_setup() in i915_gem_render_state.c for
		 * possible configurations, to avoid duplication they are
		 * not shown here again.
		 */
		*batch++ = GEN9_MEDIA_POOL_STATE;
		*batch++ = GEN9_MEDIA_POOL_ENABLE;
		*batch++ = 0x00777000;
		*batch++ = 0;
		*batch++ = 0;
		*batch++ = 0;
	}

	/* Pad to end of cacheline */
	while ((unsigned long)batch % CACHELINE_BYTES)
		*batch++ = MI_NOOP;

	return batch;
}

static u32 *gen9_init_perctx_bb(struct intel_engine_cs *engine, u32 *batch)
{
	*batch++ = MI_BATCH_BUFFER_END;

	return batch;
}

#define CTX_WA_BB_OBJ_SIZE (PAGE_SIZE)

static int lrc_setup_wa_ctx(struct intel_engine_cs *engine)
{
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma;
	int err;

	obj = i915_gem_object_create(engine->i915, CTX_WA_BB_OBJ_SIZE);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	vma = i915_vma_instance(obj, &engine->i915->ggtt.base, NULL);
	if (IS_ERR(vma)) {
		err = PTR_ERR(vma);
		goto err;
	}

	err = i915_vma_pin(vma, 0, PAGE_SIZE, PIN_GLOBAL | PIN_HIGH);
	if (err)
		goto err;

	engine->wa_ctx.vma = vma;
	return 0;

err:
	i915_gem_object_put(obj);
	return err;
}

static void lrc_destroy_wa_ctx(struct intel_engine_cs *engine)
{
	i915_vma_unpin_and_release(&engine->wa_ctx.vma);
}

typedef u32 *(*wa_bb_func_t)(struct intel_engine_cs *engine, u32 *batch);

static int intel_init_workaround_bb(struct intel_engine_cs *engine)
{
	struct i915_ctx_workarounds *wa_ctx = &engine->wa_ctx;
	struct i915_wa_ctx_bb *wa_bb[2] = { &wa_ctx->indirect_ctx,
					    &wa_ctx->per_ctx };
	wa_bb_func_t wa_bb_fn[2];
	struct page *page;
	void *batch, *batch_ptr;
	unsigned int i;
	int ret;

	if (WARN_ON(engine->id != RCS || !engine->scratch))
		return -EINVAL;

	switch (INTEL_GEN(engine->i915)) {
	case 9:
		wa_bb_fn[0] = gen9_init_indirectctx_bb;
		wa_bb_fn[1] = gen9_init_perctx_bb;
		break;
	case 8:
		wa_bb_fn[0] = gen8_init_indirectctx_bb;
		wa_bb_fn[1] = gen8_init_perctx_bb;
		break;
	default:
		MISSING_CASE(INTEL_GEN(engine->i915));
		return 0;
	}

	ret = lrc_setup_wa_ctx(engine);
	if (ret) {
		DRM_DEBUG_DRIVER("Failed to setup context WA page: %d\n", ret);
		return ret;
	}

	page = i915_gem_object_get_dirty_page(wa_ctx->vma->obj, 0);
	batch = batch_ptr = kmap_atomic(page);

	/*
	 * Emit the two workaround batch buffers, recording the offset from the
	 * start of the workaround batch buffer object for each and their
	 * respective sizes.
	 */
	for (i = 0; i < ARRAY_SIZE(wa_bb_fn); i++) {
		wa_bb[i]->offset = batch_ptr - batch;
		if (WARN_ON(!IS_ALIGNED(wa_bb[i]->offset, CACHELINE_BYTES))) {
			ret = -EINVAL;
			break;
		}
		batch_ptr = wa_bb_fn[i](engine, batch_ptr);
		wa_bb[i]->size = batch_ptr - (batch + wa_bb[i]->offset);
	}

	BUG_ON(batch_ptr - batch > CTX_WA_BB_OBJ_SIZE);

	kunmap_atomic(batch);
	if (ret)
		lrc_destroy_wa_ctx(engine);

	return ret;
}

static u8 gtiir[] = {
	[RCS] = 0,
	[BCS] = 0,
	[VCS] = 1,
	[VCS2] = 1,
	[VECS] = 3,
};

static int gen8_init_common_ring(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	struct execlist_port *port = engine->execlist_port;
	unsigned int n;
	bool submit;
	int ret;

	ret = intel_mocs_init_engine(engine);
	if (ret)
		return ret;

	intel_engine_reset_breadcrumbs(engine);
	intel_engine_init_hangcheck(engine);

	I915_WRITE(RING_HWSTAM(engine->mmio_base), 0xffffffff);
	I915_WRITE(RING_MODE_GEN7(engine),
		   _MASKED_BIT_ENABLE(GFX_RUN_LIST_ENABLE));
	I915_WRITE(RING_HWS_PGA(engine->mmio_base),
		   engine->status_page.ggtt_offset);
	POSTING_READ(RING_HWS_PGA(engine->mmio_base));

	DRM_DEBUG_DRIVER("Execlists enabled for %s\n", engine->name);

	GEM_BUG_ON(engine->id >= ARRAY_SIZE(gtiir));

	/*
	 * Clear any pending interrupt state.
	 *
	 * We do it twice out of paranoia that some of the IIR are double
	 * buffered, and if we only reset it once there may still be
	 * an interrupt pending.
	 */
	I915_WRITE(GEN8_GT_IIR(gtiir[engine->id]),
		   GT_CONTEXT_SWITCH_INTERRUPT << engine->irq_shift);
	I915_WRITE(GEN8_GT_IIR(gtiir[engine->id]),
		   GT_CONTEXT_SWITCH_INTERRUPT << engine->irq_shift);
	clear_bit(ENGINE_IRQ_EXECLIST, &engine->irq_posted);

	/* After a GPU reset, we may have requests to replay */
	submit = false;
	for (n = 0; n < ARRAY_SIZE(engine->execlist_port); n++) {
		if (!port_isset(&port[n]))
			break;

		DRM_DEBUG_DRIVER("Restarting %s:%d from 0x%x\n",
				 engine->name, n,
				 port_request(&port[n])->global_seqno);

		/* Discard the current inflight count */
		port_set(&port[n], port_request(&port[n]));
		submit = true;
	}

	if (submit && !i915.enable_guc_submission)
		execlists_submit_ports(engine);

	return 0;
}

static int gen8_init_render_ring(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	int ret;

	ret = gen8_init_common_ring(engine);
	if (ret)
		return ret;

	/* We need to disable the AsyncFlip performance optimisations in order
	 * to use MI_WAIT_FOR_EVENT within the CS. It should already be
	 * programmed to '1' on all products.
	 *
	 * WaDisableAsyncFlipPerfMode:snb,ivb,hsw,vlv,bdw,chv
	 */
	I915_WRITE(MI_MODE, _MASKED_BIT_ENABLE(ASYNC_FLIP_PERF_DISABLE));

	I915_WRITE(INSTPM, _MASKED_BIT_ENABLE(INSTPM_FORCE_ORDERING));

	return init_workarounds_ring(engine);
}

static int gen9_init_render_ring(struct intel_engine_cs *engine)
{
	int ret;

	ret = gen8_init_common_ring(engine);
	if (ret)
		return ret;

	return init_workarounds_ring(engine);
}

static void reset_common_ring(struct intel_engine_cs *engine,
			      struct drm_i915_gem_request *request)
{
	struct execlist_port *port = engine->execlist_port;
	struct intel_context *ce;
	unsigned int n;

	/*
	 * Catch up with any missed context-switch interrupts.
	 *
	 * Ideally we would just read the remaining CSB entries now that we
	 * know the gpu is idle. However, the CSB registers are sometimes^W
	 * often trashed across a GPU reset! Instead we have to rely on
	 * guessing the missed context-switch events by looking at what
	 * requests were completed.
	 */
	if (!request) {
		for (n = 0; n < ARRAY_SIZE(engine->execlist_port); n++)
			i915_gem_request_put(port_request(&port[n]));
		memset(engine->execlist_port, 0, sizeof(engine->execlist_port));
		return;
	}

	if (request->ctx != port_request(port)->ctx) {
		i915_gem_request_put(port_request(port));
		port[0] = port[1];
		memset(&port[1], 0, sizeof(port[1]));
	}

	GEM_BUG_ON(request->ctx != port_request(port)->ctx);

	/* If the request was innocent, we leave the request in the ELSP
	 * and will try to replay it on restarting. The context image may
	 * have been corrupted by the reset, in which case we may have
	 * to service a new GPU hang, but more likely we can continue on
	 * without impact.
	 *
	 * If the request was guilty, we presume the context is corrupt
	 * and have to at least restore the RING register in the context
	 * image back to the expected values to skip over the guilty request.
	 */
	if (request->fence.error != -EIO)
		return;

	/* We want a simple context + ring to execute the breadcrumb update.
	 * We cannot rely on the context being intact across the GPU hang,
	 * so clear it and rebuild just what we need for the breadcrumb.
	 * All pending requests for this context will be zapped, and any
	 * future request will be after userspace has had the opportunity
	 * to recreate its own state.
	 */
	ce = &request->ctx->engine[engine->id];
	execlists_init_reg_state(ce->lrc_reg_state,
				 request->ctx, engine, ce->ring);

	/* Move the RING_HEAD onto the breadcrumb, past the hanging batch */
	ce->lrc_reg_state[CTX_RING_BUFFER_START+1] =
		i915_ggtt_offset(ce->ring->vma);
	ce->lrc_reg_state[CTX_RING_HEAD+1] = request->postfix;

	request->ring->head = request->postfix;
	intel_ring_update_space(request->ring);

	/* Reset WaIdleLiteRestore:bdw,skl as well */
	request->tail =
		intel_ring_wrap(request->ring,
				request->wa_tail - WA_TAIL_DWORDS*sizeof(u32));
	assert_ring_tail_valid(request->ring, request->tail);
}

static int intel_logical_ring_emit_pdps(struct drm_i915_gem_request *req)
{
	struct i915_hw_ppgtt *ppgtt = req->ctx->ppgtt;
	struct intel_engine_cs *engine = req->engine;
	const int num_lri_cmds = GEN8_3LVL_PDPES * 2;
	u32 *cs;
	int i;

	cs = intel_ring_begin(req, num_lri_cmds * 2 + 2);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	*cs++ = MI_LOAD_REGISTER_IMM(num_lri_cmds);
	for (i = GEN8_3LVL_PDPES - 1; i >= 0; i--) {
		const dma_addr_t pd_daddr = i915_page_dir_dma_addr(ppgtt, i);

		*cs++ = i915_mmio_reg_offset(GEN8_RING_PDP_UDW(engine, i));
		*cs++ = upper_32_bits(pd_daddr);
		*cs++ = i915_mmio_reg_offset(GEN8_RING_PDP_LDW(engine, i));
		*cs++ = lower_32_bits(pd_daddr);
	}

	*cs++ = MI_NOOP;
	intel_ring_advance(req, cs);

	return 0;
}

static int gen8_emit_bb_start(struct drm_i915_gem_request *req,
			      u64 offset, u32 len,
			      const unsigned int flags)
{
	u32 *cs;
	int ret;

	/* Don't rely in hw updating PDPs, specially in lite-restore.
	 * Ideally, we should set Force PD Restore in ctx descriptor,
	 * but we can't. Force Restore would be a second option, but
	 * it is unsafe in case of lite-restore (because the ctx is
	 * not idle). PML4 is allocated during ppgtt init so this is
	 * not needed in 48-bit.*/
	if (req->ctx->ppgtt &&
	    (intel_engine_flag(req->engine) & req->ctx->ppgtt->pd_dirty_rings) &&
	    !i915_vm_is_48bit(&req->ctx->ppgtt->base) &&
	    !intel_vgpu_active(req->i915)) {
		ret = intel_logical_ring_emit_pdps(req);
		if (ret)
			return ret;

		req->ctx->ppgtt->pd_dirty_rings &= ~intel_engine_flag(req->engine);
	}

	cs = intel_ring_begin(req, 4);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	/* FIXME(BDW): Address space and security selectors. */
	*cs++ = MI_BATCH_BUFFER_START_GEN8 |
		(flags & I915_DISPATCH_SECURE ? 0 : BIT(8)) |
		(flags & I915_DISPATCH_RS ? MI_BATCH_RESOURCE_STREAMER : 0);
	*cs++ = lower_32_bits(offset);
	*cs++ = upper_32_bits(offset);
	*cs++ = MI_NOOP;
	intel_ring_advance(req, cs);

	return 0;
}

static void gen8_logical_ring_enable_irq(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	I915_WRITE_IMR(engine,
		       ~(engine->irq_enable_mask | engine->irq_keep_mask));
	POSTING_READ_FW(RING_IMR(engine->mmio_base));
}

static void gen8_logical_ring_disable_irq(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	I915_WRITE_IMR(engine, ~engine->irq_keep_mask);
}

static int gen8_emit_flush(struct drm_i915_gem_request *request, u32 mode)
{
	u32 cmd, *cs;

	cs = intel_ring_begin(request, 4);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	cmd = MI_FLUSH_DW + 1;

	/* We always require a command barrier so that subsequent
	 * commands, such as breadcrumb interrupts, are strictly ordered
	 * wrt the contents of the write cache being flushed to memory
	 * (and thus being coherent from the CPU).
	 */
	cmd |= MI_FLUSH_DW_STORE_INDEX | MI_FLUSH_DW_OP_STOREDW;

	if (mode & EMIT_INVALIDATE) {
		cmd |= MI_INVALIDATE_TLB;
		if (request->engine->id == VCS)
			cmd |= MI_INVALIDATE_BSD;
	}

	*cs++ = cmd;
	*cs++ = I915_GEM_HWS_SCRATCH_ADDR | MI_FLUSH_DW_USE_GTT;
	*cs++ = 0; /* upper addr */
	*cs++ = 0; /* value */
	intel_ring_advance(request, cs);

	return 0;
}

static int gen8_emit_flush_render(struct drm_i915_gem_request *request,
				  u32 mode)
{
	struct intel_engine_cs *engine = request->engine;
	u32 scratch_addr =
		i915_ggtt_offset(engine->scratch) + 2 * CACHELINE_BYTES;
	bool vf_flush_wa = false, dc_flush_wa = false;
	u32 *cs, flags = 0;
	int len;

	flags |= PIPE_CONTROL_CS_STALL;

	if (mode & EMIT_FLUSH) {
		flags |= PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH;
		flags |= PIPE_CONTROL_DEPTH_CACHE_FLUSH;
		flags |= PIPE_CONTROL_DC_FLUSH_ENABLE;
		flags |= PIPE_CONTROL_FLUSH_ENABLE;
	}

	if (mode & EMIT_INVALIDATE) {
		flags |= PIPE_CONTROL_TLB_INVALIDATE;
		flags |= PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_VF_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_CONST_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_STATE_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_QW_WRITE;
		flags |= PIPE_CONTROL_GLOBAL_GTT_IVB;

		/*
		 * On GEN9: before VF_CACHE_INVALIDATE we need to emit a NULL
		 * pipe control.
		 */
		if (IS_GEN9(request->i915))
			vf_flush_wa = true;

		/* WaForGAMHang:kbl */
		if (IS_KBL_REVID(request->i915, 0, KBL_REVID_B0))
			dc_flush_wa = true;
	}

	len = 6;

	if (vf_flush_wa)
		len += 6;

	if (dc_flush_wa)
		len += 12;

	cs = intel_ring_begin(request, len);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	if (vf_flush_wa)
		cs = gen8_emit_pipe_control(cs, 0, 0);

	if (dc_flush_wa)
		cs = gen8_emit_pipe_control(cs, PIPE_CONTROL_DC_FLUSH_ENABLE,
					    0);

	cs = gen8_emit_pipe_control(cs, flags, scratch_addr);

	if (dc_flush_wa)
		cs = gen8_emit_pipe_control(cs, PIPE_CONTROL_CS_STALL, 0);

	intel_ring_advance(request, cs);

	return 0;
}

/*
 * Reserve space for 2 NOOPs at the end of each request to be
 * used as a workaround for not being allowed to do lite
 * restore with HEAD==TAIL (WaIdleLiteRestore).
 */
static void gen8_emit_wa_tail(struct drm_i915_gem_request *request, u32 *cs)
{
	*cs++ = MI_NOOP;
	*cs++ = MI_NOOP;
	request->wa_tail = intel_ring_offset(request, cs);
}

static void gen8_emit_breadcrumb(struct drm_i915_gem_request *request, u32 *cs)
{
	/* w/a: bit 5 needs to be zero for MI_FLUSH_DW address. */
	BUILD_BUG_ON(I915_GEM_HWS_INDEX_ADDR & (1 << 5));

	*cs++ = (MI_FLUSH_DW + 1) | MI_FLUSH_DW_OP_STOREDW;
	*cs++ = intel_hws_seqno_address(request->engine) | MI_FLUSH_DW_USE_GTT;
	*cs++ = 0;
	*cs++ = request->global_seqno;
	*cs++ = MI_USER_INTERRUPT;
	*cs++ = MI_NOOP;
	request->tail = intel_ring_offset(request, cs);
	assert_ring_tail_valid(request->ring, request->tail);

	gen8_emit_wa_tail(request, cs);
}

static const int gen8_emit_breadcrumb_sz = 6 + WA_TAIL_DWORDS;

static void gen8_emit_breadcrumb_render(struct drm_i915_gem_request *request,
					u32 *cs)
{
	/* We're using qword write, seqno should be aligned to 8 bytes. */
	BUILD_BUG_ON(I915_GEM_HWS_INDEX & 1);

	/* w/a for post sync ops following a GPGPU operation we
	 * need a prior CS_STALL, which is emitted by the flush
	 * following the batch.
	 */
	*cs++ = GFX_OP_PIPE_CONTROL(6);
	*cs++ = PIPE_CONTROL_GLOBAL_GTT_IVB | PIPE_CONTROL_CS_STALL |
		PIPE_CONTROL_QW_WRITE;
	*cs++ = intel_hws_seqno_address(request->engine);
	*cs++ = 0;
	*cs++ = request->global_seqno;
	/* We're thrashing one dword of HWS. */
	*cs++ = 0;
	*cs++ = MI_USER_INTERRUPT;
	*cs++ = MI_NOOP;
	request->tail = intel_ring_offset(request, cs);
	assert_ring_tail_valid(request->ring, request->tail);

	gen8_emit_wa_tail(request, cs);
}

static const int gen8_emit_breadcrumb_render_sz = 8 + WA_TAIL_DWORDS;

static int gen8_init_rcs_context(struct drm_i915_gem_request *req)
{
	int ret;

	ret = intel_ring_workarounds_emit(req);
	if (ret)
		return ret;

	ret = intel_rcs_context_init_mocs(req);
	/*
	 * Failing to program the MOCS is non-fatal.The system will not
	 * run at peak performance. So generate an error and carry on.
	 */
	if (ret)
		DRM_ERROR("MOCS failed to program: expect performance issues.\n");

	return i915_gem_render_state_emit(req);
}

/**
 * intel_logical_ring_cleanup() - deallocate the Engine Command Streamer
 * @engine: Engine Command Streamer.
 */
void intel_logical_ring_cleanup(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv;

	/*
	 * Tasklet cannot be active at this point due intel_mark_active/idle
	 * so this is just for documentation.
	 */
	if (WARN_ON(test_bit(TASKLET_STATE_SCHED, &engine->irq_tasklet.state)))
		tasklet_kill(&engine->irq_tasklet);

	dev_priv = engine->i915;

	if (engine->buffer) {
		WARN_ON((I915_READ_MODE(engine) & MODE_IDLE) == 0);
	}

	if (engine->cleanup)
		engine->cleanup(engine);

	if (engine->status_page.vma) {
		i915_gem_object_unpin_map(engine->status_page.vma->obj);
		engine->status_page.vma = NULL;
	}

	intel_engine_cleanup_common(engine);

	lrc_destroy_wa_ctx(engine);
	engine->i915 = NULL;
	dev_priv->engine[engine->id] = NULL;
	kfree(engine);
}

static void execlists_set_default_submission(struct intel_engine_cs *engine)
{
	engine->submit_request = execlists_submit_request;
	engine->schedule = execlists_schedule;
	engine->irq_tasklet.func = intel_lrc_irq_handler;
}

static void
logical_ring_default_vfuncs(struct intel_engine_cs *engine)
{
	/* Default vfuncs which can be overriden by each engine. */
	engine->init_hw = gen8_init_common_ring;
	engine->reset_hw = reset_common_ring;

	engine->context_pin = execlists_context_pin;
	engine->context_unpin = execlists_context_unpin;

	engine->request_alloc = execlists_request_alloc;

	engine->emit_flush = gen8_emit_flush;
	engine->emit_breadcrumb = gen8_emit_breadcrumb;
	engine->emit_breadcrumb_sz = gen8_emit_breadcrumb_sz;

	engine->set_default_submission = execlists_set_default_submission;

	engine->irq_enable = gen8_logical_ring_enable_irq;
	engine->irq_disable = gen8_logical_ring_disable_irq;
	engine->emit_bb_start = gen8_emit_bb_start;
}

static inline void
logical_ring_default_irqs(struct intel_engine_cs *engine)
{
	unsigned shift = engine->irq_shift;
	engine->irq_enable_mask = GT_RENDER_USER_INTERRUPT << shift;
	engine->irq_keep_mask = GT_CONTEXT_SWITCH_INTERRUPT << shift;
}

static int
lrc_setup_hws(struct intel_engine_cs *engine, struct i915_vma *vma)
{
	const int hws_offset = LRC_PPHWSP_PN * PAGE_SIZE;
	void *hws;

	/* The HWSP is part of the default context object in LRC mode. */
	hws = i915_gem_object_pin_map(vma->obj, I915_MAP_WB);
	if (IS_ERR(hws))
		return PTR_ERR(hws);

	engine->status_page.page_addr = hws + hws_offset;
	engine->status_page.ggtt_offset = i915_ggtt_offset(vma) + hws_offset;
	engine->status_page.vma = vma;

	return 0;
}

static void
logical_ring_setup(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	enum forcewake_domains fw_domains;

	intel_engine_setup_common(engine);

	/* Intentionally left blank. */
	engine->buffer = NULL;

	fw_domains = intel_uncore_forcewake_for_reg(dev_priv,
						    RING_ELSP(engine),
						    FW_REG_WRITE);

	fw_domains |= intel_uncore_forcewake_for_reg(dev_priv,
						     RING_CONTEXT_STATUS_PTR(engine),
						     FW_REG_READ | FW_REG_WRITE);

	fw_domains |= intel_uncore_forcewake_for_reg(dev_priv,
						     RING_CONTEXT_STATUS_BUF_BASE(engine),
						     FW_REG_READ);

	engine->fw_domains = fw_domains;

	tasklet_init(&engine->irq_tasklet,
		     intel_lrc_irq_handler, (unsigned long)engine);

	logical_ring_default_vfuncs(engine);
	logical_ring_default_irqs(engine);
}

static int
logical_ring_init(struct intel_engine_cs *engine)
{
	struct i915_gem_context *dctx = engine->i915->kernel_context;
	int ret;

	ret = intel_engine_init_common(engine);
	if (ret)
		goto error;

	/* And setup the hardware status page. */
	ret = lrc_setup_hws(engine, dctx->engine[engine->id].state);
	if (ret) {
		DRM_ERROR("Failed to set up hws %s: %d\n", engine->name, ret);
		goto error;
	}

	return 0;

error:
	intel_logical_ring_cleanup(engine);
	return ret;
}

int logical_render_ring_init(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	int ret;

	logical_ring_setup(engine);

	if (HAS_L3_DPF(dev_priv))
		engine->irq_keep_mask |= GT_RENDER_L3_PARITY_ERROR_INTERRUPT;

	/* Override some for render ring. */
	if (INTEL_GEN(dev_priv) >= 9)
		engine->init_hw = gen9_init_render_ring;
	else
		engine->init_hw = gen8_init_render_ring;
	engine->init_context = gen8_init_rcs_context;
	engine->emit_flush = gen8_emit_flush_render;
	engine->emit_breadcrumb = gen8_emit_breadcrumb_render;
	engine->emit_breadcrumb_sz = gen8_emit_breadcrumb_render_sz;

	ret = intel_engine_create_scratch(engine, PAGE_SIZE);
	if (ret)
		return ret;

	ret = intel_init_workaround_bb(engine);
	if (ret) {
		/*
		 * We continue even if we fail to initialize WA batch
		 * because we only expect rare glitches but nothing
		 * critical to prevent us from using GPU
		 */
		DRM_ERROR("WA batch buffer initialization failed: %d\n",
			  ret);
	}

	return logical_ring_init(engine);
}

int logical_xcs_ring_init(struct intel_engine_cs *engine)
{
	logical_ring_setup(engine);

	return logical_ring_init(engine);
}

static u32
make_rpcs(struct drm_i915_private *dev_priv)
{
	u32 rpcs = 0;

	/*
	 * No explicit RPCS request is needed to ensure full
	 * slice/subslice/EU enablement prior to Gen9.
	*/
	if (INTEL_GEN(dev_priv) < 9)
		return 0;

	/*
	 * Starting in Gen9, render power gating can leave
	 * slice/subslice/EU in a partially enabled state. We
	 * must make an explicit request through RPCS for full
	 * enablement.
	*/
	if (INTEL_INFO(dev_priv)->sseu.has_slice_pg) {
		rpcs |= GEN8_RPCS_S_CNT_ENABLE;
		rpcs |= hweight8(INTEL_INFO(dev_priv)->sseu.slice_mask) <<
			GEN8_RPCS_S_CNT_SHIFT;
		rpcs |= GEN8_RPCS_ENABLE;
	}

	if (INTEL_INFO(dev_priv)->sseu.has_subslice_pg) {
		rpcs |= GEN8_RPCS_SS_CNT_ENABLE;
		rpcs |= hweight8(INTEL_INFO(dev_priv)->sseu.subslice_mask) <<
			GEN8_RPCS_SS_CNT_SHIFT;
		rpcs |= GEN8_RPCS_ENABLE;
	}

	if (INTEL_INFO(dev_priv)->sseu.has_eu_pg) {
		rpcs |= INTEL_INFO(dev_priv)->sseu.eu_per_subslice <<
			GEN8_RPCS_EU_MIN_SHIFT;
		rpcs |= INTEL_INFO(dev_priv)->sseu.eu_per_subslice <<
			GEN8_RPCS_EU_MAX_SHIFT;
		rpcs |= GEN8_RPCS_ENABLE;
	}

	return rpcs;
}

static u32 intel_lr_indirect_ctx_offset(struct intel_engine_cs *engine)
{
	u32 indirect_ctx_offset;

	switch (INTEL_GEN(engine->i915)) {
	default:
		MISSING_CASE(INTEL_GEN(engine->i915));
		/* fall through */
	case 10:
		indirect_ctx_offset =
			GEN10_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT;
		break;
	case 9:
		indirect_ctx_offset =
			GEN9_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT;
		break;
	case 8:
		indirect_ctx_offset =
			GEN8_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT;
		break;
	}

	return indirect_ctx_offset;
}

static void execlists_init_reg_state(u32 *regs,
				     struct i915_gem_context *ctx,
				     struct intel_engine_cs *engine,
				     struct intel_ring *ring)
{
	struct drm_i915_private *dev_priv = engine->i915;
	struct i915_hw_ppgtt *ppgtt = ctx->ppgtt ?: dev_priv->mm.aliasing_ppgtt;
	u32 base = engine->mmio_base;
	bool rcs = engine->id == RCS;

	/* A context is actually a big batch buffer with several
	 * MI_LOAD_REGISTER_IMM commands followed by (reg, value) pairs. The
	 * values we are setting here are only for the first context restore:
	 * on a subsequent save, the GPU will recreate this batchbuffer with new
	 * values (including all the missing MI_LOAD_REGISTER_IMM commands that
	 * we are not initializing here).
	 */
	regs[CTX_LRI_HEADER_0] = MI_LOAD_REGISTER_IMM(rcs ? 14 : 11) |
				 MI_LRI_FORCE_POSTED;

	CTX_REG(regs, CTX_CONTEXT_CONTROL, RING_CONTEXT_CONTROL(engine),
		_MASKED_BIT_ENABLE(CTX_CTRL_INHIBIT_SYN_CTX_SWITCH |
				   CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT |
				   (HAS_RESOURCE_STREAMER(dev_priv) ?
				   CTX_CTRL_RS_CTX_ENABLE : 0)));
	CTX_REG(regs, CTX_RING_HEAD, RING_HEAD(base), 0);
	CTX_REG(regs, CTX_RING_TAIL, RING_TAIL(base), 0);
	CTX_REG(regs, CTX_RING_BUFFER_START, RING_START(base), 0);
	CTX_REG(regs, CTX_RING_BUFFER_CONTROL, RING_CTL(base),
		RING_CTL_SIZE(ring->size) | RING_VALID);
	CTX_REG(regs, CTX_BB_HEAD_U, RING_BBADDR_UDW(base), 0);
	CTX_REG(regs, CTX_BB_HEAD_L, RING_BBADDR(base), 0);
	CTX_REG(regs, CTX_BB_STATE, RING_BBSTATE(base), RING_BB_PPGTT);
	CTX_REG(regs, CTX_SECOND_BB_HEAD_U, RING_SBBADDR_UDW(base), 0);
	CTX_REG(regs, CTX_SECOND_BB_HEAD_L, RING_SBBADDR(base), 0);
	CTX_REG(regs, CTX_SECOND_BB_STATE, RING_SBBSTATE(base), 0);
	if (rcs) {
		CTX_REG(regs, CTX_BB_PER_CTX_PTR, RING_BB_PER_CTX_PTR(base), 0);
		CTX_REG(regs, CTX_RCS_INDIRECT_CTX, RING_INDIRECT_CTX(base), 0);
		CTX_REG(regs, CTX_RCS_INDIRECT_CTX_OFFSET,
			RING_INDIRECT_CTX_OFFSET(base), 0);

		if (engine->wa_ctx.vma) {
			struct i915_ctx_workarounds *wa_ctx = &engine->wa_ctx;
			u32 ggtt_offset = i915_ggtt_offset(wa_ctx->vma);

			regs[CTX_RCS_INDIRECT_CTX + 1] =
				(ggtt_offset + wa_ctx->indirect_ctx.offset) |
				(wa_ctx->indirect_ctx.size / CACHELINE_BYTES);

			regs[CTX_RCS_INDIRECT_CTX_OFFSET + 1] =
				intel_lr_indirect_ctx_offset(engine) << 6;

			regs[CTX_BB_PER_CTX_PTR + 1] =
				(ggtt_offset + wa_ctx->per_ctx.offset) | 0x01;
		}
	}

	regs[CTX_LRI_HEADER_1] = MI_LOAD_REGISTER_IMM(9) | MI_LRI_FORCE_POSTED;

	CTX_REG(regs, CTX_CTX_TIMESTAMP, RING_CTX_TIMESTAMP(base), 0);
	/* PDP values well be assigned later if needed */
	CTX_REG(regs, CTX_PDP3_UDW, GEN8_RING_PDP_UDW(engine, 3), 0);
	CTX_REG(regs, CTX_PDP3_LDW, GEN8_RING_PDP_LDW(engine, 3), 0);
	CTX_REG(regs, CTX_PDP2_UDW, GEN8_RING_PDP_UDW(engine, 2), 0);
	CTX_REG(regs, CTX_PDP2_LDW, GEN8_RING_PDP_LDW(engine, 2), 0);
	CTX_REG(regs, CTX_PDP1_UDW, GEN8_RING_PDP_UDW(engine, 1), 0);
	CTX_REG(regs, CTX_PDP1_LDW, GEN8_RING_PDP_LDW(engine, 1), 0);
	CTX_REG(regs, CTX_PDP0_UDW, GEN8_RING_PDP_UDW(engine, 0), 0);
	CTX_REG(regs, CTX_PDP0_LDW, GEN8_RING_PDP_LDW(engine, 0), 0);

	if (ppgtt && i915_vm_is_48bit(&ppgtt->base)) {
		/* 64b PPGTT (48bit canonical)
		 * PDP0_DESCRIPTOR contains the base address to PML4 and
		 * other PDP Descriptors are ignored.
		 */
		ASSIGN_CTX_PML4(ppgtt, regs);
	}

	if (rcs) {
		regs[CTX_LRI_HEADER_2] = MI_LOAD_REGISTER_IMM(1);
		CTX_REG(regs, CTX_R_PWR_CLK_STATE, GEN8_R_PWR_CLK_STATE,
			make_rpcs(dev_priv));

		i915_oa_init_reg_state(engine, ctx, regs);
	}
}

static int
populate_lr_context(struct i915_gem_context *ctx,
		    struct drm_i915_gem_object *ctx_obj,
		    struct intel_engine_cs *engine,
		    struct intel_ring *ring)
{
	void *vaddr;
	int ret;

	ret = i915_gem_object_set_to_cpu_domain(ctx_obj, true);
	if (ret) {
		DRM_DEBUG_DRIVER("Could not set to CPU domain\n");
		return ret;
	}

	vaddr = i915_gem_object_pin_map(ctx_obj, I915_MAP_WB);
	if (IS_ERR(vaddr)) {
		ret = PTR_ERR(vaddr);
		DRM_DEBUG_DRIVER("Could not map object pages! (%d)\n", ret);
		return ret;
	}
	ctx_obj->mm.dirty = true;

	/* The second page of the context object contains some fields which must
	 * be set up prior to the first execution. */

	execlists_init_reg_state(vaddr + LRC_STATE_PN * PAGE_SIZE,
				 ctx, engine, ring);

	i915_gem_object_unpin_map(ctx_obj);

	return 0;
}

static int execlists_context_deferred_alloc(struct i915_gem_context *ctx,
					    struct intel_engine_cs *engine)
{
	struct drm_i915_gem_object *ctx_obj;
	struct intel_context *ce = &ctx->engine[engine->id];
	struct i915_vma *vma;
	uint32_t context_size;
	struct intel_ring *ring;
	int ret;

	WARN_ON(ce->state);

	context_size = round_up(engine->context_size, I915_GTT_PAGE_SIZE);

	/* One extra page as the sharing data between driver and GuC */
	context_size += PAGE_SIZE * LRC_PPHWSP_PN;

	ctx_obj = i915_gem_object_create(ctx->i915, context_size);
	if (IS_ERR(ctx_obj)) {
		DRM_DEBUG_DRIVER("Alloc LRC backing obj failed.\n");
		return PTR_ERR(ctx_obj);
	}

	vma = i915_vma_instance(ctx_obj, &ctx->i915->ggtt.base, NULL);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto error_deref_obj;
	}

	ring = intel_engine_create_ring(engine, ctx->ring_size);
	if (IS_ERR(ring)) {
		ret = PTR_ERR(ring);
		goto error_deref_obj;
	}

	ret = populate_lr_context(ctx, ctx_obj, engine, ring);
	if (ret) {
		DRM_DEBUG_DRIVER("Failed to populate LRC: %d\n", ret);
		goto error_ring_free;
	}

	ce->ring = ring;
	ce->state = vma;
	ce->initialised |= engine->init_context == NULL;

	return 0;

error_ring_free:
	intel_ring_free(ring);
error_deref_obj:
	i915_gem_object_put(ctx_obj);
	return ret;
}

void intel_lr_context_resume(struct drm_i915_private *dev_priv)
{
	struct intel_engine_cs *engine;
	struct i915_gem_context *ctx;
	enum intel_engine_id id;

	/* Because we emit WA_TAIL_DWORDS there may be a disparity
	 * between our bookkeeping in ce->ring->head and ce->ring->tail and
	 * that stored in context. As we only write new commands from
	 * ce->ring->tail onwards, everything before that is junk. If the GPU
	 * starts reading from its RING_HEAD from the context, it may try to
	 * execute that junk and die.
	 *
	 * So to avoid that we reset the context images upon resume. For
	 * simplicity, we just zero everything out.
	 */
	list_for_each_entry(ctx, &dev_priv->contexts.list, link) {
		for_each_engine(engine, dev_priv, id) {
			struct intel_context *ce = &ctx->engine[engine->id];
			u32 *reg;

			if (!ce->state)
				continue;

			reg = i915_gem_object_pin_map(ce->state->obj,
						      I915_MAP_WB);
			if (WARN_ON(IS_ERR(reg)))
				continue;

			reg += LRC_STATE_PN * PAGE_SIZE / sizeof(*reg);
			reg[CTX_RING_HEAD+1] = 0;
			reg[CTX_RING_TAIL+1] = 0;

			ce->state->obj->mm.dirty = true;
			i915_gem_object_unpin_map(ce->state->obj);

			intel_ring_reset(ce->ring, 0);
		}
	}
}
