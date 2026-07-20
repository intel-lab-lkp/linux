// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 Xing Loong <xing.xl.loong@gmail.com>
 * TEE->REE callee-side: handles interrupts from TEE, processes RPC requests.
 */
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/tee_drv.h>
#include <linux/workqueue.h>

#include "mbedtee_drv.h"

/*
 * CPU hotplug state shared across all mbedtee instances (registered once).
 */
static int mbedtee_cpuhp_state = -EINVAL;
static unsigned int mbedtee_cpuhp_instances;
static DEFINE_MUTEX(mbedtee_cpuhp_mutex);
/* rpc_routine() runs on workqueue, re-drains ring on completion */
static void rpc_routine(struct work_struct *work);

/*
 * Common CPU-online handler: add the new CPU into the eligible T2R
 * delivery set and let the transport re-apply affinity.
 */
static int mbedtee_cpu_online(unsigned int cpu, struct hlist_node *node)
{
	struct mbedtee_t2r_ctx *ctx =
		hlist_entry(node, struct mbedtee_t2r_ctx, hp_node);
	struct mbedtee_device *mbedtee =
		container_of(ctx, struct mbedtee_device, t2r);
	unsigned int target_cpu;

	if (!ctx->t2r_ring || cpumask_test_cpu(cpu, &ctx->callee_cpus))
		return 0;

	cpumask_set_cpu(cpu, &ctx->callee_cpus);
	target_cpu = cpumask_first(&ctx->callee_cpus);

	if (mbedtee->rpc_ops && mbedtee->rpc_ops->update_affinity)
		mbedtee->rpc_ops->update_affinity(mbedtee, target_cpu);

	return 0;
}

/*
 * Common CPU-offline handler: migrates T2R interrupt delivery to another
 * online CPU when the current callee CPU is being taken offline.
 *
 * The transport-specific update_affinity() callback handles the actual
 * interrupt migration (e.g. irq_set_affinity + callee_hartid update for
 * RISC-V IMSIC; irq_set_affinity only for ARM - no ring update needed).
 */
static int mbedtee_cpu_offline(unsigned int cpu, struct hlist_node *node)
{
	struct mbedtee_t2r_ctx *ctx =
		hlist_entry(node, struct mbedtee_t2r_ctx, hp_node);
	struct mbedtee_device *mbedtee =
		container_of(ctx, struct mbedtee_device, t2r);
	unsigned int new_cpu;

	if (!ctx->t2r_ring || !cpumask_test_cpu(cpu, &ctx->callee_cpus))
		return 0;

	/*
	 * Remove the dying CPU from the eligible set.  If the set is now
	 * empty (all CPUs going away at once -- highly unlikely but safe),
	 * fall back to CPU 0 which Linux guarantees to be the last offline.
	 */
	cpumask_clear_cpu(cpu, &ctx->callee_cpus);
	if (cpumask_empty(&ctx->callee_cpus))
		cpumask_set_cpu(0, &ctx->callee_cpus);

	new_cpu = cpumask_first(&ctx->callee_cpus);

	if (mbedtee->rpc_ops && mbedtee->rpc_ops->update_affinity)
		mbedtee->rpc_ops->update_affinity(mbedtee, new_cpu);
	return 0;
}

static inline size_t t2r_available_size(struct mbedtee_t2r_ctx *ctx)
{
	u32 wr;
	u32 rd;
	u32 shm_size = ctx->t2r_ring_sz;

	/* Pair with producer release store after writing ring payload. */
	wr = smp_load_acquire(&ctx->t2r_ring->wr);
	rd = READ_ONCE(ctx->t2r_ring_rd);

	if (wr > shm_size)
		return 0;

	if (wr >= rd)
		return wr - rd;
	return shm_size + wr - rd;
}

static void t2r_ring_copy(struct mbedtee_t2r_ctx *ctx,
			  void *data, size_t size, u32 rd)
{
	u32 remain;
	u32 shm_size = ctx->t2r_ring_sz;
	struct rpc_ringbuf *shm = ctx->t2r_ring;

	if (rd + size <= shm_size) {
		memcpy(data, &shm->mem[rd], size);
	} else {
		remain = rd + size - shm_size;
		memcpy(data, &shm->mem[rd], size - remain);
		memcpy((u8 *)data + size - remain,
		       &shm->mem[0], remain);
	}
}

static void t2r_ring_advance(struct mbedtee_t2r_ctx *ctx, size_t size)
{
	u32 rd = READ_ONCE(ctx->t2r_ring_rd);
	u32 shm_size = ctx->t2r_ring_sz;
	struct rpc_ringbuf *shm = ctx->t2r_ring;

	if (rd + size > shm_size)
		rd = rd + size - shm_size;
	else
		rd += size;

	WRITE_ONCE(ctx->t2r_ring_rd, rd);
	/* Publish updated consumer index after command parsing is complete. */
	smp_store_release(&shm->rd, rd);
}

/*
 * Read and consume @size bytes from the TEE-to-REE ring buffer.
 * Must be called with ctx->ring_lock held.
 */
static void t2r_ring_read(struct mbedtee_t2r_ctx *ctx,
			  void *data, size_t size)
{
	t2r_ring_copy(ctx, data, size, READ_ONCE(ctx->t2r_ring_rd));
	t2r_ring_advance(ctx, size);
}

static bool rpc_queue_complete_only(struct mbedtee_t2r_ctx *ctx,
				    u64 waiter_id)
{
	if (ctx->complete_work_pending)
		return false;

	ctx->complete_work_pending = true;
	ctx->complete_work.waiter_id = waiter_id;
	queue_work(ctx->rpc_wq, &ctx->complete_work.work);

	return true;
}

/*
 * Try to pick the next RPC command from the ring buffer.
 *
 * For asynchronous RPCs the data payload follows the cmd header in the
 * ring.  If the payload has not fully arrived yet the cmd header is still
 * consumed (it was already read) but the entry is marked incomplete so
 * that the next IRQ resumes reading the payload instead of re-reading
 * a header.
 *
 * For synchronous RPCs (waiter_id != 0) the work descriptor is allocated
 * BEFORE the ring header is consumed.  This guarantees that if the
 * allocator returns NULL the ring entry is left intact so the IRQ
 * handler will retry on the next wake-up rather than leaving a TEE
 * thread blocked with no response.
 *
 * Returns a work descriptor on success, or NULL when no complete entry
 * is available.
 */
static struct rpc_work *rpc_pick_next(struct mbedtee_device *mbedtee,
				      struct mbedtee_t2r_ctx *ctx, struct rpc_work *c)
{
	struct rpc_cmd cmd;
	void (*func)(struct mbedtee_device *mbedtee, void *data, size_t size);
	struct rpc_work *new_work = NULL;
	resource_size_t off;
	phys_addr_t shm_phys;
	u64 shm_wire;

	/* Resume reading payload of a previously incomplete async RPC */
	if (ctx->pending_async) {
		if (t2r_available_size(ctx) < ctx->pending_size)
			return NULL;
		ctx->pending_async = false;
		t2r_ring_read(ctx, c->data, ctx->pending_size);
		c->func = ctx->pending_func;
		c->size = ctx->pending_size;
		c->waiter_id = 0;
		return c;
	}

	if (t2r_available_size(ctx) < sizeof(cmd))
		return NULL;

	/*
	 * Peek at the header without advancing the ring pointer.
	 * For sync RPCs we must pre-allocate before consuming the entry.
	 */
	memset(&cmd, 0, sizeof(cmd));
	t2r_ring_copy(ctx, &cmd, sizeof(cmd), READ_ONCE(ctx->t2r_ring_rd));

	if (cmd.id >= MBEDTEE_RPC_MAX)
		goto skip;

	func = ctx->rpc_handlers[cmd.id];
	if (!func)
		goto skip;

	if (cmd.waiter_id == 0) {
		/* Async RPC: payload follows header in the ring */
		if (cmd.size > sizeof(ctx->rpc_data))
			goto skip;

		/* Consume the header now that we know it is valid */
		t2r_ring_advance(ctx, sizeof(cmd));

		if (t2r_available_size(ctx) < cmd.size) {
			/* Mark as incomplete, resume on next iteration */
			ctx->pending_async = true;
			ctx->pending_size = cmd.size;
			ctx->pending_func = func;
			return NULL;
		}

		t2r_ring_read(ctx, c->data, cmd.size);
	} else {
		/* Sync RPC: data lives in the shared memory region */
		shm_wire = cmd.shm;
		shm_phys = (phys_addr_t)shm_wire;
		if ((u64)shm_phys != shm_wire)
			goto skip;

		if (shm_phys < ctx->t2r_shm_phys)
			goto skip;

		off = shm_phys - ctx->t2r_shm_phys;
		if (cmd.size == 0 || cmd.size > ctx->t2r_shm_sz)
			goto skip;

		if (off > ctx->t2r_shm_sz - cmd.size)
			goto skip;

		new_work = kzalloc_obj(*new_work, GFP_ATOMIC);
		if (!new_work)
			return NULL;

		t2r_ring_advance(ctx, sizeof(cmd));

		c = new_work;
		c->mbedtee = mbedtee;
		c->data = ctx->t2r_shm + off;
	}

	c->func = func;
	c->waiter_id = cmd.waiter_id;
	c->size = cmd.size;

	return c;

skip:
	/*
	 * If this was a sync RPC (waiter_id != 0) we must notify the TEE
	 * thread so it does not wait forever.  Defer the completion fastcall
	 * to process context because transport implementations may sleep.
	 * If the single deferred-completion slot is already in use, leave the
	 * header in place and retry after the pending completion has drained.
	 */
	if (cmd.waiter_id != 0 && !rpc_queue_complete_only(ctx, cmd.waiter_id))
		return NULL;

	/*
	 * Bad or unhandled entry: consume header plus any inline payload
	 * (async RPC payloads follow the header in the ring) to keep the
	 * ring moving.  For sync RPCs payload is in t2r_shm, not the ring.
	 */
	t2r_ring_advance(ctx, sizeof(cmd) + (cmd.waiter_id ? 0 : cmd.size));

	return NULL;
}

static void rpc_drain_ring(struct mbedtee_device *mbedtee)
{
	struct mbedtee_t2r_ctx *ctx = &mbedtee->t2r;
	struct rpc_work rw = {}, *c = NULL;
	unsigned long flags;

	if (!ctx->t2r_ring)
		return;

	spin_lock_irqsave(&ctx->ring_lock, flags);
	while (READ_ONCE(ctx->t2r_ring_rd) !=
	    /* Pair with producer store-release after ring write. */
	    smp_load_acquire(&ctx->t2r_ring->wr)) {
		rw.data = ctx->rpc_data;
		c = rpc_pick_next(mbedtee, ctx, &rw);
		if (!c)
			break;

		if (c->waiter_id == 0) {
			c->func(mbedtee, c->data, c->size);
		} else {
			INIT_WORK(&c->work, rpc_routine);
			queue_work(ctx->rpc_wq, &c->work);
		}
	}
	spin_unlock_irqrestore(&ctx->ring_lock, flags);
}

static void rpc_routine(struct work_struct *work)
{
	struct rpc_work *c = container_of(work, struct rpc_work, work);
	struct mbedtee_t2r_ctx *ctx = &c->mbedtee->t2r;
	long ret;

	if (!c->complete_only)
		c->func(c->mbedtee, c->data, c->size);

	/*
	 * COMPLETE_TEE must reach the TEE thread that issued the sync RPC.
	 * On RISC-V the notification uses a shared ring buffer that may be
	 * transiently full when many RPCs are in flight concurrently.
	 * Keep retrying rather than silently dropping the completion, which
	 * would leave the TEE thread blocked in rpc_call_sync forever.
	 * On ARM the fastcall is a direct SMC so -ENOSPC never occurs.
	 */
	do {
		ret = mbedtee_rpc_fastcall(c->mbedtee, MBEDTEE_RPC_COMPLETE_TEE,
					   (unsigned long)c->waiter_id, 0, 0);
		if (ret != -ENOSPC && ret != -ENOMEM)
			break;
		cond_resched();
	} while (1);

	if (c != &ctx->complete_work) {
		kfree(c);
		return;
	}

	WRITE_ONCE(ctx->complete_work_pending, false);
	rpc_drain_ring(c->mbedtee);
}

/*
 * Hard IRQ handler -- drains the TEE-to-REE ring buffer.
 *
 * Asynchronous RPCs (waiter_id == 0) are handled inline since all async
 * handlers (complete(), ktime_get_real_ts64()) are non-blocking.
 *
 * Synchronous RPCs (waiter_id != 0) may block waiting for the
 * tee-supplicant, so they are dispatched to a workqueue.
 */
irqreturn_t mbedtee_rpc_irq_handler(int irq, void *dev_id)
{
	struct mbedtee_device *mbedtee = dev_id;

	if (!mbedtee)
		return IRQ_NONE;

	rpc_drain_ring(mbedtee);

	return IRQ_HANDLED;
}

static void mbedtee_rpc_complete(struct mbedtee_device *mbedtee,
				 void *data, size_t size)
{
	u64 waiter_id;

	if (size < sizeof(waiter_id))
		return;

	memcpy(&waiter_id, data, sizeof(waiter_id));
	if (!waiter_id)
		return;

	mbedtee_rpc_complete_call(mbedtee, waiter_id);
}

static void mbedtee_ree_time(struct mbedtee_device *mbedtee,
			     void *data, size_t size)
{
	struct timespec64 *ts = data;

	if (size < sizeof(*ts))
		return;

	ktime_get_real_ts64(ts);
}

static void mbedtee_reefs_supp(struct mbedtee_device *mbedtee,
			       void *data, size_t size)
{
	mbedtee_supp_handler(mbedtee, MBEDTEE_SUPP_REEFS, data, size);
}

static void mbedtee_rpmb_supp(struct mbedtee_device *mbedtee,
			      void *data, size_t size)
{
	mbedtee_supp_handler(mbedtee, MBEDTEE_SUPP_RPMB, data, size);
}

static void mbedtee_register_rpc(struct mbedtee_device *mbedtee,
				 u32 id, void (*func)(struct mbedtee_device *mbedtee,
					void *data, size_t size))
{
	struct mbedtee_t2r_ctx *ctx = &mbedtee->t2r;

	if (WARN_ON(id >= MBEDTEE_RPC_MAX || !func))
		return;

	ctx->rpc_handlers[id] = func;
}

int mbedtee_rpc_init(struct mbedtee_device *mbedtee)
{
	struct mbedtee_t2r_ctx *ctx = &mbedtee->t2r;
	struct device_node *node = mbedtee->dev->of_node;
	struct resource res;
	bool cpuhp_state_created = false;
	int ret;

	memset(ctx, 0, sizeof(*ctx));
	spin_lock_init(&ctx->ring_lock);
	mbedtee->rpc_ops = mbedtee_get_rpc_transport_ops();
	if (!mbedtee->rpc_ops)
		return -EIO;

	ret = mbedtee_get_resource(node, "t2r-ring", &res);
	if (ret) {
		dev_err(mbedtee->dev, "t2r-ring not found in DT\n");
		return ret;
	}

	ctx->t2r_ring = memremap(res.start, resource_size(&res),
				 MEMREMAP_WB);
	if (!ctx->t2r_ring) {
		dev_err(mbedtee->dev, "failed to map t2r ring\n");
		return -ENOMEM;
	}
	if (resource_size(&res) <= sizeof(struct rpc_ringbuf)) {
		dev_err(mbedtee->dev, "t2r-ring too small\n");
		memunmap(ctx->t2r_ring);
		ctx->t2r_ring = NULL;
		return -EINVAL;
	}
	ctx->t2r_ring_sz = resource_size(&res) - sizeof(struct rpc_ringbuf);
	/* Observe latest producer index only after ring metadata is visible. */
	WRITE_ONCE(ctx->t2r_ring_rd, smp_load_acquire(&ctx->t2r_ring->rd));

	dev_dbg(mbedtee->dev, "t2r-ring %pa\n", &res.start);

	ret = mbedtee_get_resource(node, "t2r-shm", &res);
	if (ret) {
		dev_err(mbedtee->dev, "t2r-shm not found in DT\n");
		goto err_ring;
	}

	ctx->t2r_shm_phys = res.start;
	ctx->t2r_shm_sz = resource_size(&res);
	ctx->t2r_shm = memremap(res.start, ctx->t2r_shm_sz,
				MEMREMAP_WB);
	if (!ctx->t2r_shm) {
		dev_err(mbedtee->dev, "failed to map t2r shm\n");
		ret = -ENOMEM;
		goto err_ring;
	}

	dev_dbg(mbedtee->dev, "t2r-shm %pa\n", &res.start);

	ctx->rpc_wq = alloc_workqueue("mbedtee-rpc", WQ_UNBOUND, 0);
	if (!ctx->rpc_wq) {
		dev_err(mbedtee->dev, "failed to create rpc workqueue\n");
		ret = -ENOMEM;
		goto err_shm;
	}

	ctx->complete_work.mbedtee = mbedtee;
	ctx->complete_work.complete_only = true;
	INIT_WORK(&ctx->complete_work.work, rpc_routine);

	mbedtee_register_rpc(mbedtee, MBEDTEE_RPC_COMPLETE_REE, mbedtee_rpc_complete);
	mbedtee_register_rpc(mbedtee, MBEDTEE_RPC_REETIME, mbedtee_ree_time);
	mbedtee_register_rpc(mbedtee, MBEDTEE_RPC_REEFS, mbedtee_reefs_supp);
	mbedtee_register_rpc(mbedtee, MBEDTEE_RPC_RPMB, mbedtee_rpmb_supp);

	/*
	 * Register cpuhp instance BEFORE enabling interrupts so that
	 * T2R delivery migration is in place before the first IRQ fires.
	 */
	mutex_lock(&mbedtee_cpuhp_mutex);
	if (mbedtee_cpuhp_state < 0) {
		mbedtee_cpuhp_state =
			cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN,
						"tee/mbedtee:rpc-callee",
						mbedtee_cpu_online,
						mbedtee_cpu_offline);
		if (mbedtee_cpuhp_state < 0) {
			ret = mbedtee_cpuhp_state;
			mutex_unlock(&mbedtee_cpuhp_mutex);
			goto err_wq;
		}
		cpuhp_state_created = true;
	}

	ret = cpuhp_state_add_instance(mbedtee_cpuhp_state, &ctx->hp_node);
	if (!ret) {
		mbedtee_cpuhp_instances++;
		ctx->cpuhp_added = true;
	} else if (cpuhp_state_created) {
		cpuhp_remove_multi_state(mbedtee_cpuhp_state);
		mbedtee_cpuhp_state = -EINVAL;
	}
	mutex_unlock(&mbedtee_cpuhp_mutex);
	if (ret != 0)
		goto err_wq;

	ret = mbedtee->rpc_ops->init(mbedtee, ctx->t2r_ring,
					mbedtee_rpc_irq_handler);
	if (ret != 0)
		goto err_cpuhp;

	/* Signal the TEE that the REE callee is ready to receive. */
	smp_store_release(&ctx->t2r_ring->callee_ready, true);

	return 0;

err_cpuhp:
	mutex_lock(&mbedtee_cpuhp_mutex);
	if (mbedtee_cpuhp_state >= 0 && ctx->cpuhp_added) {
		cpuhp_state_remove_instance_nocalls(mbedtee_cpuhp_state,
						    &ctx->hp_node);
		ctx->cpuhp_added = false;
		if (mbedtee_cpuhp_instances)
			mbedtee_cpuhp_instances--;
		if (!mbedtee_cpuhp_instances) {
			cpuhp_remove_multi_state(mbedtee_cpuhp_state);
			mbedtee_cpuhp_state = -EINVAL;
		}
	}
	mutex_unlock(&mbedtee_cpuhp_mutex);
err_wq:
	destroy_workqueue(ctx->rpc_wq);
	ctx->rpc_wq = NULL;
err_shm:
	memunmap(ctx->t2r_shm);
	ctx->t2r_shm = NULL;
err_ring:
	memunmap(ctx->t2r_ring);
	ctx->t2r_ring = NULL;
	return ret;
}

void mbedtee_rpc_uninit(struct mbedtee_device *mbedtee)
{
	struct mbedtee_t2r_ctx *ctx;

	if (!mbedtee)
		return;

	ctx = &mbedtee->t2r;
	/* Stop advertising the REE callee before interrupt teardown. */
	if (ctx->t2r_ring)
		WRITE_ONCE(ctx->t2r_ring->callee_ready, false);

	mutex_lock(&mbedtee_cpuhp_mutex);
	if (mbedtee_cpuhp_state >= 0 && ctx->cpuhp_added) {
		cpuhp_state_remove_instance_nocalls(mbedtee_cpuhp_state,
						    &ctx->hp_node);
		ctx->cpuhp_added = false;
		if (mbedtee_cpuhp_instances)
			mbedtee_cpuhp_instances--;
		if (!mbedtee_cpuhp_instances) {
			cpuhp_remove_multi_state(mbedtee_cpuhp_state);
			mbedtee_cpuhp_state = -EINVAL;
		}
	}
	mutex_unlock(&mbedtee_cpuhp_mutex);

	if (mbedtee->rpc_ops)
		mbedtee->rpc_ops->uninit(mbedtee);

	/*
	 * Work queued before IRQ teardown may be blocked in the supplicant
	 * path. Abort those requests and reject later ones before draining the
	 * workqueue. R2T must stay alive until destroy_workqueue() returns
	 * because rpc_routine() sends COMPLETE_TEE replies over R2T.
	 */
	mbedtee_supp_abort_all(&mbedtee->supp);

	if (ctx->rpc_wq) {
		destroy_workqueue(ctx->rpc_wq);
		ctx->rpc_wq = NULL;
	}

	if (ctx->t2r_shm) {
		memunmap(ctx->t2r_shm);
		ctx->t2r_shm = NULL;
	}

	if (ctx->t2r_ring) {
		memunmap(ctx->t2r_ring);
		ctx->t2r_ring = NULL;
	}
}
