/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2023 ARM Ltd.
 */

#ifndef __ASM_RSI_CMDS_H
#define __ASM_RSI_CMDS_H

#include <linux/arm-smccc.h>

#include <asm/rsi_smc.h>

static inline void invoke_rsi_fn_smc_with_res(unsigned long function_id,
					      unsigned long arg0,
					      unsigned long arg1,
					      unsigned long arg2,
					      unsigned long arg3,
					      struct arm_smccc_res *res)
{
	arm_smccc_smc(function_id, arg0, arg1, arg2, arg3, 0, 0, 0, res);
}

static inline unsigned long rsi_get_version(unsigned long req,
					    unsigned long *out_lower,
					    unsigned long *out_higher)
{
	struct arm_smccc_res res;

	invoke_rsi_fn_smc_with_res(SMC_RSI_ABI_VERSION, req, 0, 0, 0, &res);

	if (out_lower)
		*out_lower = res.a1;
	if (out_higher)
		*out_higher = res.a2;

	return res.a0;
}

static inline unsigned long rsi_get_realm_config(struct realm_config *cfg)
{
	struct arm_smccc_res res;

	invoke_rsi_fn_smc_with_res(SMC_RSI_REALM_CONFIG, virt_to_phys(cfg), 0, 0, 0, &res);
	return res.a0;
}

#endif
