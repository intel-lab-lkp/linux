/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 SiFive, Inc.
 *
 */

#ifndef __RVTRACE_V0_H__
#define __RVTRACE_V0_H__

#include <linux/rvtrace.h>

u32 rvtrace_v0_get_encoder_impl(struct rvtrace_platform_data *pdata);
u32 rvtrace_v0_get_funnel_impl(struct rvtrace_platform_data *pdata);

#endif /* __RVTRACE_V0_H__ */

