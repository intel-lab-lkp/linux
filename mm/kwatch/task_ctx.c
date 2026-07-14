// SPDX-License-Identifier: GPL-2.0
#include <linux/slab.h>
#include <linux/hash.h>
#include <linux/sched.h>
#include <linux/log2.h>
#include "kwatch.h"

static u16 kwatch_ctx_pool_size;
static u16 kwatch_ctx_pool_mask;

static struct kwatch_tsk_ctx *kwatch_ctx_pool;

int kwatch_tsk_ctx_prealloc(u16 max_concurrency)
{
	if (!max_concurrency)
		max_concurrency = 256;

	kwatch_ctx_pool_size = roundup_pow_of_two(max_concurrency);
	kwatch_ctx_pool_mask = kwatch_ctx_pool_size - 1;

	if (unlikely(!kwatch_ctx_pool)) {
		kwatch_ctx_pool = kcalloc(kwatch_ctx_pool_size,
					  sizeof(struct kwatch_tsk_ctx),
					  GFP_KERNEL);
		if (!kwatch_ctx_pool)
			return -ENOMEM;
	}
	return 0;
}

struct kwatch_tsk_ctx *kwatch_tsk_ctx_get(bool can_alloc)
{
	int start_idx, i, idx;
	struct task_struct *t;

	if (unlikely(!kwatch_ctx_pool))
		return NULL;

	start_idx = hash_ptr(current, ilog2(kwatch_ctx_pool_size));

	for (i = 0; i < kwatch_ctx_pool_size; i++) {
		idx = (start_idx + i) & kwatch_ctx_pool_mask;
		t = READ_ONCE(kwatch_ctx_pool[idx].task);
		if (t == current)
			return &kwatch_ctx_pool[idx];
	}

	if (!can_alloc)
		return NULL;

	for (i = 0; i < kwatch_ctx_pool_size; i++) {
		idx = (start_idx + i) & kwatch_ctx_pool_mask;
		t = READ_ONCE(kwatch_ctx_pool[idx].task);
		if (!t) {
			if (!cmpxchg(&kwatch_ctx_pool[idx].task, NULL, current))
				return &kwatch_ctx_pool[idx];
		}
	}

	return NULL;
}

void kwatch_tsk_ctx_reset(struct kwatch_tsk_ctx *ctx, u32 new_epoch)
{
	struct kwatch_watchpoint *wp = xchg(&ctx->wp, NULL);

	if (wp)
		kwatch_hwbp_put(wp);
	ctx->depth = 0;
	ctx->epoch = new_epoch;
}

void kwatch_tsk_ctx_put(void)
{
	struct kwatch_tsk_ctx *ctx = kwatch_tsk_ctx_get(false);

	if (unlikely(!ctx))
		return;

	kwatch_tsk_ctx_reset(ctx, 0);

	/* Pairs with READ_ONCE() in kwatch_tsk_ctx_get() */
	smp_store_release(&ctx->task, NULL);
}

void kwatch_tsk_ctx_release_wps(void)
{
	int i;

	if (!kwatch_ctx_pool)
		return;

	for (i = 0; i < kwatch_ctx_pool_size; i++) {
		struct kwatch_watchpoint *wp = xchg(&kwatch_ctx_pool[i].wp,
						    NULL);
		if (wp)
			kwatch_hwbp_put(wp);
	}
}

void kwatch_tsk_ctx_free(void)
{
	kfree(kwatch_ctx_pool);
	kwatch_ctx_pool = NULL;
}
