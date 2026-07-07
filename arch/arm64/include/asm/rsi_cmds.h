/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2023 ARM Ltd.
 */

#ifndef __ASM_RSI_CMDS_H
#define __ASM_RSI_CMDS_H

#include <linux/arm-smccc-rsi.h>
#include <linux/string.h>
#include <asm/memory.h>

#define RSI_GRANULE_SHIFT		12
#define RSI_GRANULE_SIZE		(_AC(1, UL) << RSI_GRANULE_SHIFT)

enum ripas {
	RSI_RIPAS_EMPTY = 0,
	RSI_RIPAS_RAM = 1,
	RSI_RIPAS_DESTROYED = 2,
	RSI_RIPAS_DEV = 3,
};

static inline unsigned long rsi_request_version(unsigned long req,
						unsigned long *out_lower,
						unsigned long *out_higher)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_ABI_VERSION, req, 0, 0, 0, 0, 0, 0, &res);

	if (out_lower)
		*out_lower = res.a1;
	if (out_higher)
		*out_higher = res.a2;

	return res.a0;
}

static inline unsigned long rsi_get_realm_config(struct realm_config *cfg)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_REALM_CONFIG, virt_to_phys(cfg),
		      0, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

static inline unsigned long rsi_ipa_state_get(phys_addr_t start,
					      phys_addr_t end,
					      enum ripas *state,
					      phys_addr_t *top)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_IPA_STATE_GET,
		      start, end, 0, 0, 0, 0, 0,
		      &res);

	if (res.a0 == RSI_SUCCESS) {
		if (top)
			*top = res.a1;
		if (state)
			*state = res.a2;
	}

	return res.a0;
}

static inline long rsi_set_addr_range_state(phys_addr_t start,
					    phys_addr_t end,
					    enum ripas state,
					    unsigned long flags,
					    phys_addr_t *top)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_IPA_STATE_SET, start, end, state,
		      flags, 0, 0, 0, &res);

	if (top)
		*top = res.a1;

	if (res.a2 != RSI_ACCEPT)
		return -EPERM;

	return res.a0;
}

#endif /* __ASM_RSI_CMDS_H */
