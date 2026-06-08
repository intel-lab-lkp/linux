// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 NVIDIA Corporation
 *
 * Arm SMCCC cache maintenance provider using cache clean+invalidate calls.
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
#define SMCCC_CACHE_DEFAULT_DELAY_US	1000UL
#define SMCCC_CACHE_MAX_DELAY_US	20000UL

struct smccc_cache {
	/* Must be first member */
	struct cache_coherency_ops_inst cci;
	struct mutex lock; /* Serializes SMCCC cache maintenance calls. */
	u32 latency_us;
	u32 rate_limit;
};

static int smccc_cache_status_to_errno(s32 status)
{
	switch (status) {
	case SMCCC_RET_SUCCESS:
		return 0;
	case SMCCC_RET_NOT_SUPPORTED:
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

static unsigned long smccc_cache_delay_us(const struct smccc_cache *cache)
{
	unsigned long delay_us = 0;

	if (cache->rate_limit)
		delay_us = DIV_ROUND_UP_ULL(USEC_PER_SEC, cache->rate_limit);

	if (cache->latency_us)
		delay_us = max(delay_us, (unsigned long)cache->latency_us);

	/*
	 * Firmware may advertise neither a rate limit nor a latency hint; use
	 * a small bounded backoff instead of retrying in a tight loop.
	 */
	if (!delay_us)
		delay_us = SMCCC_CACHE_DEFAULT_DELAY_US;

	return min(delay_us, SMCCC_CACHE_MAX_DELAY_US);
}

static int smccc_cache_wbinv(struct cache_coherency_ops_inst *cci,
			     struct cc_inval_params *invp)
{
	struct smccc_cache *cache = container_of(cci, struct smccc_cache, cci);
	struct arm_smccc_res res = {};
	unsigned long delay_us = smccc_cache_delay_us(cache);
	int ret;

	if (!invp->size)
		return -EINVAL;

	/*
	 * Serialize the full retry sequence. With the default bounds, a caller
	 * may hold the mutex across up to five 20ms backoff sleeps.
	 */
	guard(mutex)(&cache->lock);

	for (unsigned int i = 0; i < SMCCC_CACHE_MAX_RETRIES; i++) {
		/* Long firmware operations can trigger watchdog checks. */
		touch_nmi_watchdog();

		arm_smccc_1_1_invoke(ARM_SMCCC_ARCH_CLEAN_INV_MEMREGION,
				     invp->addr, invp->size, 0UL, &res);

		ret = smccc_cache_status_to_errno((s32)res.a0);
		if (!ret)
			return 0;

		if (ret != -EBUSY && ret != -EAGAIN)
			return ret;

		fsleep(delay_us);
	}

	return -EBUSY;
}

static const struct cache_coherency_ops smccc_cache_ops = {
	.wbinv = smccc_cache_wbinv,
};

static int __init smccc_cache_init(void)
{
	struct smccc_cache *cache;
	struct arm_smccc_res res = {};
	int ret;

	if (arm_smccc_get_version() < ARM_SMCCC_VERSION_1_1)
		return -ENODEV;

	if (arm_smccc_1_1_get_conduit() == SMCCC_CONDUIT_NONE)
		return -ENODEV;

	arm_smccc_1_1_invoke(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
			     ARM_SMCCC_ARCH_CLEAN_INV_MEMREGION, &res);
	if ((s32)res.a0 < 0)
		return -ENODEV;

	arm_smccc_1_1_invoke(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
			     ARM_SMCCC_ARCH_CLEAN_INV_MEMREGION_ATTRIBUTES,
			     &res);
	if ((s32)res.a0 < 0)
		return -ENODEV;

	arm_smccc_1_1_invoke(ARM_SMCCC_ARCH_CLEAN_INV_MEMREGION_ATTRIBUTES,
			     &res);
	if ((s32)res.a0)
		return -ENODEV;

	cache = cache_coherency_ops_instance_alloc(&smccc_cache_ops,
						   struct smccc_cache, cci);
	if (!cache)
		return -ENOMEM;

	mutex_init(&cache->lock);
	cache->latency_us = lower_32_bits(res.a2);
	cache->rate_limit = lower_32_bits(res.a3);

	ret = cache_coherency_ops_instance_register(&cache->cci);
	if (ret) {
		mutex_destroy(&cache->lock);
		cache_coherency_ops_instance_put(&cache->cci);
		return ret;
	}

	pr_info("SMCCC cache clean+invalidate provider registered\n");

	return 0;
}
arch_initcall(smccc_cache_init);
