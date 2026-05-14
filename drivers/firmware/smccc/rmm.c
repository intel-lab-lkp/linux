// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Arm Limited
 */

#include <linux/auxiliary_bus.h>

#include "rmm.h"

void __init register_rsi_device(struct platform_device *pdev)
{
	unsigned long ret;
	unsigned long ver_lower, ver_higher;

	if (arm_smccc_1_1_get_conduit() != SMCCC_CONDUIT_SMC)
		return;

	ret = rsi_request_version(RSI_ABI_VERSION, &ver_lower, &ver_higher);
	if (ret != RSI_SUCCESS)
		return;

	__devm_auxiliary_device_create(&pdev->dev,
				       "arm_cca_guest", RSI_DEV_NAME, NULL, 0);
}
