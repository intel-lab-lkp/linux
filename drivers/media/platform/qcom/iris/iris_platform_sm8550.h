/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __IRIS_PLATFORM_SM8550_H__
#define __IRIS_PLATFORM_SM8550_H__

extern const char * const sm8550_clk_reset_table[1];
extern const struct platform_clk_data sm8550_clk_table[3];
extern struct platform_inst_caps platform_inst_cap_sm8550;
extern const struct iris_context_bank_ops sm8550_cb_ops;

#endif
