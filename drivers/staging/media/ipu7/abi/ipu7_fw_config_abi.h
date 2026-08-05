/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2021 - 2025 Intel Corporation
 */

#ifndef IPU7_FW_CONFIG_ABI_H
#define IPU7_FW_CONFIG_ABI_H

#include <linux/types.h>

struct ipu7_wdt_abi {
	u32 wdt_timer1_us;
	u32 wdt_timer2_us;
};

#endif
