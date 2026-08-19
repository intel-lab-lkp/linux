// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 Xing Loong <xing.xl.loong@gmail.com>
 * REE->TEE RPC calls for RISC-V (IMSIC)
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/mm.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/sizes.h>

#include "mbedtee_drv.h"

/*
 * Time the writer may wait for ring space before giving up. Transient
 * fullness is common under high concurrency; permanent fullness means
 * the TEE is no longer draining and must not hang ioctls forever.
 */
#define MBEDTEE_R2T_FULL_TIMEOUT (10 * HZ)
#define MBEDTEE_R2T_CALL_TIMEOUT (30 * HZ)

static bool rpc_ring_enough(struct mbedtee_r2t_ctx *ctx, size_t size)
{
	u32 wr;
	u32 rd;
	u32 remain;

	wr = READ_ONCE(ctx->ring_wr);
	/* Pair with callee release store when publishing ring->rd */
	rd = smp_load_acquire(&ctx->ring->rd);

	if (rd > ctx->ring_sz || wr > ctx->ring_sz)
		return false;

	if (rd <= wr)
		remain = ctx->ring_sz + rd - wr;
	else
		remain = rd - wr;

	return remain > size;
}

static void rpc_ring_write(struct mbedtee_r2t_ctx *ctx,
			   void *data, size_t size)
{
	struct rpc_ringbuf *shm = ctx->ring;
	u32 remain;
	u32 wr = READ_ONCE(ctx->ring_wr);

	if (wr + size > ctx->ring_sz) {
		remain = wr + size - ctx->ring_sz;
		memcpy(&shm->mem[wr], data, size - remain);
		memcpy(&shm->mem[0], (char *)data + size - remain, remain);
		wr = remain;
	} else {
		memcpy(&shm->mem[wr], data, size);
		wr += size;
	}

	WRITE_ONCE(ctx->ring_wr, wr);
	/* Publish writer index after payload bytes become visible. */
	smp_store_release(&shm->wr, wr);
}

/*
 * Wait for @size bytes of space in the R2T ring, then write @data
 * atomically under the ring lock. Returns 0 on success.
 */
static int r2t_ring_write_wait(struct mbedtee_r2t_ctx *ctx,
			       void *data, size_t size)
{
	unsigned long deadline = jiffies + MBEDTEE_R2T_FULL_TIMEOUT;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&ctx->lock, flags);
		if (READ_ONCE(ctx->stopping)) {
			spin_unlock_irqrestore(&ctx->lock, flags);
			return -ESHUTDOWN;
		}
		if (rpc_ring_enough(ctx, size)) {
			rpc_ring_write(ctx, data, size);
			spin_unlock_irqrestore(&ctx->lock, flags);
			return 0;
		}
		spin_unlock_irqrestore(&ctx->lock, flags);

		if (time_after_eq(jiffies, deadline))
			return -ENOSPC;
		usleep_range(1000, 2000);
	}
}

long mbedtee_rpc_yieldcall(struct mbedtee_device *mbedtee,
			   unsigned long fn, struct mbedtee_rpc_call *call,
			   bool interruptible)
{
	struct mbedtee_r2t_ctx *ctx = &mbedtee->r2t;
	u64 phys;
	int ret;

	if (!ctx->ring)
		return -ENXIO;

	if (MBEDTEE_RPC_IS_FASTCALL(fn))
		return -EINVAL;

	call->rpc.id = fn;

	phys = call->rpc_phys;
	ret = r2t_ring_write_wait(ctx, &phys, sizeof(phys));
	if (ret != 0) {
		dev_err_ratelimited(mbedtee->dev, "rpc ring full\n");
		return ret;
	}

	if (interruptible)
		return mbedtee_rpc_wait_for_completion(mbedtee, call, true);

	return mbedtee_rpc_wait_for_completion_timeout(mbedtee, call,
						      MBEDTEE_R2T_CALL_TIMEOUT);
}

long mbedtee_rpc_fastcall(struct mbedtee_device *mbedtee,
			  unsigned long fn, unsigned long a0,
			  unsigned long a1, unsigned long a2)
{
	struct mbedtee_r2t_ctx *ctx = &mbedtee->r2t;
	struct mbedtee_rpc_call *call;
	u64 phys;
	int ret;

	if (!ctx->ring)
		return -ENXIO;

	if (!MBEDTEE_RPC_IS_FASTCALL(fn))
		return -EINVAL;

	ret = mbedtee_rpc_call_alloc(mbedtee, 3 * sizeof(u64), &call);
	if (ret != 0)
		return ret;

	call->rpc.id = fn;
	call->rpc.size = 3 * sizeof(u64);
	call->rpc.data[0] = a0;
	call->rpc.data[1] = a1;
	call->rpc.data[2] = a2;

	phys = call->rpc_phys;
	ret = r2t_ring_write_wait(ctx, &phys, sizeof(phys));
	if (ret != 0) {
		dev_err_ratelimited(mbedtee->dev, "rpc ring full\n");
		mbedtee_rpc_call_free(mbedtee, call);
		return ret;
	}

	ret = mbedtee_rpc_wait_for_completion_timeout(mbedtee, call,
						      MBEDTEE_R2T_CALL_TIMEOUT);
	if (ret != 0) {
		mbedtee_rpc_call_free(mbedtee, call);
		return ret;
	}

	ret = call->rpc.ret;
	mbedtee_rpc_call_free(mbedtee, call);

	return ret;
}

int mbedtee_r2t_init(struct mbedtee_device *mbedtee)
{
	struct mbedtee_r2t_ctx *ctx = &mbedtee->r2t;
	struct device_node *node = mbedtee->dev->of_node;
	struct resource res;
	int ret;

	memset(ctx, 0, sizeof(*ctx));
	spin_lock_init(&ctx->lock);

	ret = mbedtee_get_resource(node, "r2t-ring", &res);
	if (ret)
		return ret;

	if (resource_size(&res) <= sizeof(struct rpc_ringbuf)) {
		dev_err(mbedtee->dev, "r2t-ring too small\n");
		return -EINVAL;
	}

	ctx->ring = memremap(res.start, resource_size(&res),
			     MEMREMAP_WB);
	if (!ctx->ring) {
		dev_err(mbedtee->dev, "failed to map r2t ring at %pa\n",
			&res.start);
		return -ENOMEM;
	}
	ctx->ring_sz = resource_size(&res) - sizeof(struct rpc_ringbuf);
	/* Read initial writer index with acquire for coherent ring bootstrap. */
	WRITE_ONCE(ctx->ring_wr, smp_load_acquire(&ctx->ring->wr));

	dev_dbg(mbedtee->dev, "r2t-ring %pa\n", &res.start);

	return 0;
}

void mbedtee_r2t_stop(struct mbedtee_device *mbedtee)
{
	struct mbedtee_r2t_ctx *ctx = &mbedtee->r2t;
	unsigned long flags;

	spin_lock_irqsave(&ctx->lock, flags);
	WRITE_ONCE(ctx->stopping, true);
	spin_unlock_irqrestore(&ctx->lock, flags);
}

void mbedtee_r2t_uninit(struct mbedtee_device *mbedtee)
{
	struct mbedtee_r2t_ctx *ctx;

	if (!mbedtee)
		return;

	ctx = &mbedtee->r2t;
	if (ctx->ring) {
		/* r2t_init() failure lands here with no prior stop; flag first. */
		mbedtee_r2t_stop(mbedtee);
		memunmap(ctx->ring);
		ctx->ring = NULL;
		ctx->ring_sz = 0;
		ctx->ring_wr = 0;
	}
}
