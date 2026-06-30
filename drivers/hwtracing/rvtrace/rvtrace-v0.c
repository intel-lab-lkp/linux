// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 SiFive Inc.
 */

#include <linux/rvtrace.h>
#include "rvtrace-v0.h"

static u32 rvtrace_v0_get_impl(struct rvtrace_platform_data *pdata, u32 type)
{
	u32 impl, major, minor;

	impl = rvtrace_read32(pdata, RVTRACE_COMPONENT_IMPL_OFFSET);
	major = 0;
	minor = (impl >> RVTRACE_COMPONENT_IMPL_VERMAJOR_SHIFT) &
		RVTRACE_COMPONENT_IMPL_VERMAJOR_MASK;

	/* Encode to standard rvtrace impl format */
	return (type << RVTRACE_COMPONENT_IMPL_TYPE_SHIFT) |
	       (minor << RVTRACE_COMPONENT_IMPL_VERMINOR_SHIFT) |
	       (major << RVTRACE_COMPONENT_IMPL_VERMAJOR_SHIFT);
}

u32 rvtrace_v0_get_encoder_impl(struct rvtrace_platform_data *pdata)
{
	return rvtrace_v0_get_impl(pdata, RVTRACE_COMPONENT_TYPE_ENCODER);
}

u32 rvtrace_v0_get_funnel_impl(struct rvtrace_platform_data *pdata)
{
	return rvtrace_v0_get_impl(pdata, RVTRACE_COMPONENT_TYPE_FUNNEL);
}
