/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 ARM Ltd.
 */

#ifndef __LINUX_ARM_RMI_CMDS_H_
#define __LINUX_ARM_RMI_CMDS_H_

#include <linux/arm-smccc-rmi.h>
#include <linux/bug.h>
#include <linux/types.h>

#include <asm/page.h>

struct rtt_entry {
	unsigned long walk_level;
	unsigned long desc;
	int state;
	int ripas;
};

unsigned long rmi_feat_reg(unsigned long id);

bool is_rmi_available(void);

/**
 * rmi_rmm_config_set() - Configure the RMM
 * @cfg_ptr: PA of a struct rmm_config
 *
 * Sets configuration options on the RMM.
 *
 * Return: RMI return code
 */
static inline int rmi_rmm_config_set(unsigned long cfg_ptr)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_RMM_CONFIG_SET, cfg_ptr, &res);

	return res.a0;
}

/**
 * rmi_granule_tracking_get() - Get configuration of a Granule tracking region
 * @start: Base PA of the tracking region
 * @end: End of the PA region
 * @out_category: Memory category
 * @out_state: Tracking region state
 * @out_top: Top of the memory region
 *
 * Return: RMI return code
 */
static inline int rmi_granule_tracking_get(unsigned long start,
					   unsigned long end,
					   unsigned long *out_category,
					   unsigned long *out_state,
					   unsigned long *out_top)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_GRANULE_TRACKING_GET, start, end, &res);

	if (out_category)
		*out_category = res.a1;
	if (out_state)
		*out_state = res.a2;
	if (out_top)
		*out_top = res.a3;

	return res.a0;
}

/**
 * rmi_features() - Read feature register
 * @index: Feature register index
 * @out: Feature register value is written to this pointer
 *
 * Return: RMI return code
 */
static inline int rmi_features(unsigned long index, unsigned long *out)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_FEATURES, index, &res);

	if (out)
		*out = res.a1;
	return res.a0;
}

#endif
