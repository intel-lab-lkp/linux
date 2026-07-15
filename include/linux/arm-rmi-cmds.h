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

#define RMI_MAX_ADDR_LIST	256

struct rmi_sro_state {
	struct arm_smccc_1_2_regs regs;
	unsigned long addr_count;
	unsigned long addr_list[RMI_MAX_ADDR_LIST];
};

unsigned long rmi_feat_reg(unsigned long id);

int rmi_delegate_range(phys_addr_t phys, unsigned long size,
		       phys_addr_t *out_phys);
int rmi_undelegate_range(phys_addr_t phys, unsigned long size);
int free_delegated_page(phys_addr_t phys);

static inline int rmi_delegate_page(phys_addr_t phys)
{
	return rmi_delegate_range(phys, PAGE_SIZE, NULL);
}

static inline int rmi_undelegate_page(phys_addr_t phys)
{
	return rmi_undelegate_range(phys, PAGE_SIZE);
}

bool is_rmi_available(void);

unsigned long rmi_sro_memxfer_execute(struct rmi_sro_state *sro, gfp_t gfp);
void rmi_sro_free(struct rmi_sro_state *sro);
unsigned long rmi_sro_execute(struct arm_smccc_1_2_regs *regs);

#define rmi_sro_memxfer_cmd(sro, gfp, ...) ({				\
	struct rmi_sro_state *__sro = (sro);				\
	*__sro = (struct rmi_sro_state){ .regs = {__VA_ARGS__} };	\
	int __ret = rmi_sro_memxfer_execute(__sro, gfp);		\
	rmi_sro_free(__sro);						\
	__ret;								\
})

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
 * rmi_rmm_activate() - Activate the RMM
 * @sro: Preallocated SRO context to be used
 *
 * Return: RMI return code
 */
static inline int rmi_rmm_activate(struct rmi_sro_state *sro)
{
	return rmi_sro_memxfer_cmd(sro, GFP_KERNEL, SMC_RMI_RMM_ACTIVATE);
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
 * rmi_gpt_l1_create() - Create a Level 1 GPT
 * @addr: Base of physical address region described by the L1GPT
 * @sro: Preallocated SRO context to be used
 * @gfp: Allocation flags for SRO memory donation requests
 *
 * Return: RMI return code
 */
static inline int rmi_gpt_l1_create(unsigned long addr,
				    struct rmi_sro_state *sro,
				    gfp_t gfp)
{
	return rmi_sro_memxfer_cmd(sro, gfp, SMC_RMI_GPT_L1_CREATE, addr);
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

/**
 * rmi_granule_range_delegate() - Delegate granules
 * @base: PA of the first granule of the range
 * @top: PA of the first granule after the range
 * @out_top: PA of the first granule not delegated
 *
 * Delegate a range of granule for use by the realm world. If the entire range
 * was delegated then @out_top == @top, otherwise the function should be called
 * again with @base == @out_top.
 *
 * Return: RMI return code
 */
static inline int rmi_granule_range_delegate(unsigned long base,
					     unsigned long top,
					     unsigned long *out_top)
{
	struct arm_smccc_1_2_regs regs = {
		SMC_RMI_GRANULE_RANGE_DELEGATE, base, top
	};
	int ret = rmi_sro_execute(&regs);

	if (out_top)
		*out_top = regs.a1;

	return ret;
}

/**
 * rmi_granule_range_undelegate() - Undelegate a range of granules
 * @base: Base PA of the target range
 * @top: Top PA of the target range
 * @out_top: Returns the top PA of range whose state is undelegated
 *
 * Undelegate a range of granules to allow use by the normal world. Will fail if
 * the granules are in use.
 *
 * Return: RMI return code
 */
static inline int rmi_granule_range_undelegate(unsigned long base,
					       unsigned long top,
					       unsigned long *out_top)
{
	struct arm_smccc_1_2_regs regs = {
		SMC_RMI_GRANULE_RANGE_UNDELEGATE, base, top
	};
	int ret = rmi_sro_execute(&regs);

	if (out_top)
		*out_top = regs.a1;

	return ret;
}

#endif
