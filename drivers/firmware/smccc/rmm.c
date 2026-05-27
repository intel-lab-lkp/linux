// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Arm Limited
 */

#include <linux/arm-smccc-bus.h>
#include <linux/err.h>
#include <linux/printk.h>

#include "rmm.h"

void __init register_rsi_device(void)
{
	unsigned long ret;

	if (arm_smccc_1_1_get_conduit() != SMCCC_CONDUIT_SMC)
		return;

	ret = rsi_request_version(RSI_ABI_VERSION, NULL, NULL);
	if (ret != RSI_SUCCESS)
		return;

	if (IS_ERR(arm_smccc_device_register(RSI_DEV_NAME)))
		pr_err("%s: could not register device\n", RSI_DEV_NAME);
}
