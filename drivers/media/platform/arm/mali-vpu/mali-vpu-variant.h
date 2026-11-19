/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_VARIANT_H__
#define __MALI_VPU_VARIANT_H__

#include <linux/types.h>

/**
 * struct mali_vpu_variant - Variant-specific hardware capability descriptor.
 * @rst_names: Array of reset control names for this variant.
 * @num_rsts: Number of reset controls for this variant.
 * @expected_hw_pid: Expected hardware product ID read from the version register.
 */
struct mali_vpu_variant {
	const char * const *rst_names;
	unsigned int num_rsts;
	u32 expected_hw_pid;
};

extern const struct mali_vpu_variant mali_vpu_v76_variant;

#endif /* __MALI_VPU_VARIANT_H__ */
