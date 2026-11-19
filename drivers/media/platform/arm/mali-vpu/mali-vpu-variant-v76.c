// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/array_size.h>

#include "mali-vpu-variant.h"

#define MALI_VPU_HW_PID_V76 0x56627

static const char * const mali_v76_rst_names[] = {
	"warm",
};

const struct mali_vpu_variant mali_vpu_v76_variant = {
	.expected_hw_pid = MALI_VPU_HW_PID_V76,

	.rst_names = mali_v76_rst_names,
	.num_rsts = ARRAY_SIZE(mali_v76_rst_names),
};
