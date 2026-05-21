// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 NVIDIA Corporation
 *
 * Arm64 cache maintenance provider using SMCCC cache clean+invalidate calls.
 */

#include <linux/arm-smccc.h>
#include <linux/cache_coherency.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/nmi.h>

#define SMCCC_CACHE_MAX_RETRIES		5
#define SMCCC_CACHE_MAX_DELAY_US	20000

/* DEN0028 v1.7: bit[0] == 1 means implementation flushes all caches globally */
#define SMCCC_CACHE_ATTR_GLOBAL_OP	BIT(0)

struct arm64_smccc_cache {
	/* Must be first member */
	struct cache_coherency_ops_inst cci;
	struct mutex lock; /* Serializes SMCCC cache maintenance calls. */
	u32 latency_us;
	u32 rate_limit;
	bool global_op;
	u64 global_flush_gen;
};

static struct arm64_smccc_cache *arm64_smccc_cache;

static int smccc_cache_status_to_errno(s32 status)
{
	switch (status) {
	case SMCCC_RET_SUCCESS:
		return 0;
	case SMCCC_RET_NOT_SUPPORTED:
	case SMCCC_RET_NOT_REQUIRED:
		return -EOPNOTSUPP;
	case SMCCC_RET_INVALID_PARAMETER:
		return -EINVAL;
	case SMCCC_RET_RATE_LIMITED:
		return -EAGAIN;
	case SMCCC_RET_BUSY:
		return -EBUSY;
	default:
		return -EIO;
	}
}

static int smccc_cache_delay_us(const struct arm64_smccc_cache *cache)
{
	u64 delay_us = 0;

	if (cache->rate_limit)
		delay_us = DIV_ROUND_UP_ULL(USEC_PER_SEC, cache->rate_limit);

	if (cache->latency_us)
		delay_us = max_t(u64, delay_us, cache->latency_us);

	if (!delay_us)
		delay_us = 1000;

	return min_t(u64, delay_us, SMCCC_CACHE_MAX_DELAY_US);
}

static int arm64_smccc_cache_wbinv(struct cache_coherency_ops_inst *cci,
				   struct cc_inval_params *invp)
{
	struct arm64_smccc_cache *cache =
		container_of(cci, struct arm64_smccc_cache, cci);
	struct arm_smccc_res res = {};
	int delay_us = smccc_cache_delay_us(cache);
	u64 gen = 0;
	s32 status;
	int ret;
	int i;

	if (!invp->size)
		return -EINVAL;

	if (cache->global_op)
		gen = READ_ONCE(cache->global_flush_gen);

	guard(mutex)(&cache->lock);

	/*
	 * If firmware reports a global operation type, a successful operation
	 * covers every request that was already waiting behind it. Skip if the
	 * generation advanced while this request was waiting to enter the
	 * serialized firmware call path.
	 */
	if (cache->global_op && gen != READ_ONCE(cache->global_flush_gen))
		return 0;

	for (i = 0; i < SMCCC_CACHE_MAX_RETRIES; i++) {
		/* Long firmware operations can trigger watchdog checks. */
		touch_nmi_watchdog();

		arm_smccc_1_1_invoke(ARM_SMCCC_ARCH_CLEAN_INV_MEMREGION,
				     invp->addr, invp->size, 0UL, &res);
		status = (s32)res.a0;
		ret = smccc_cache_status_to_errno(status);
		if (!ret) {
			if (cache->global_op) {
				WRITE_ONCE(cache->global_flush_gen,
					   cache->global_flush_gen + 1);
			}
			return 0;
		}

		if (ret != -EBUSY && ret != -EAGAIN)
			return ret;

		usleep_range(delay_us, delay_us + 100);
	}

	return -EBUSY;
}

static const struct cache_coherency_ops arm64_smccc_cache_ops = {
	.wbinv = arm64_smccc_cache_wbinv,
};

static int __init arm64_smccc_cache_init(void)
{
	struct arm_smccc_res res = {};
	s32 status;
	int ret;

	if (arm_smccc_get_version() < ARM_SMCCC_VERSION_1_1)
		return -ENODEV;

	if (arm_smccc_1_1_get_conduit() == SMCCC_CONDUIT_NONE)
		return -ENODEV;

	arm_smccc_1_1_invoke(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
			     ARM_SMCCC_ARCH_CLEAN_INV_MEMREGION, &res);
	status = (s32)res.a0;
	if (status < 0)
		return -ENODEV;

	arm_smccc_1_1_invoke(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
			     ARM_SMCCC_ARCH_CLEAN_INV_MEMREGION_ATTRIBUTES, &res);
	status = (s32)res.a0;
	if (status < 0)
		return -ENODEV;

	arm_smccc_1_1_invoke(ARM_SMCCC_ARCH_CLEAN_INV_MEMREGION_ATTRIBUTES, &res);
	status = (s32)res.a0;
	if (status)
		return -ENODEV;

	arm64_smccc_cache =
		cache_coherency_ops_instance_alloc(&arm64_smccc_cache_ops,
						   struct arm64_smccc_cache,
						   cci);
	if (!arm64_smccc_cache)
		return -ENOMEM;

	mutex_init(&arm64_smccc_cache->lock);
	arm64_smccc_cache->latency_us = lower_32_bits(res.a2);
	arm64_smccc_cache->rate_limit = lower_32_bits(res.a3);
	arm64_smccc_cache->global_op = !!(res.a1 & SMCCC_CACHE_ATTR_GLOBAL_OP);

	ret = cache_coherency_ops_instance_register(&arm64_smccc_cache->cci);
	if (ret) {
		cache_coherency_ops_instance_put(&arm64_smccc_cache->cci);
		arm64_smccc_cache = NULL;
		return ret;
	}

	pr_info("SMCCC cache clean+invalidate provider registered\n");
	return 0;
}
arch_initcall(arm64_smccc_cache_init);
