/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Ltd.
 */

#ifndef __ASM_RHI_H_
#define __ASM_RHI_H_

#include <linux/types.h>

#define SMC_RHI_CALL(func)				\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,		\
			   ARM_SMCCC_SMC_64,		\
			   ARM_SMCCC_OWNER_STANDARD_HYP,\
			   (func))

unsigned long rhi_get_ipa_change_alignment(void);
#define RHI_HOSTCONF_VER_1_0		0x10000
#define RHI_HOSTCONF_VERSION		SMC_RHI_CALL(0x004E)

#define __RHI_HOSTCONF_GET_IPA_CHANGE_ALIGNMENT BIT(0)
#define RHI_HOSTCONF_FEATURES		SMC_RHI_CALL(0x004F)
#define RHI_HOSTCONF_GET_IPA_CHANGE_ALIGNMENT	SMC_RHI_CALL(0x0050)
#endif
