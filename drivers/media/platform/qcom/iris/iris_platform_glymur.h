/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __IRIS_PLATFORM_GLYMUR_H__
#define __IRIS_PLATFORM_GLYMUR_H__

extern const struct platform_clk_data glymur_clk_table[9];
extern const char * const glymur_clk_reset_table[6];
extern const char * const glymur_opp_clk_table[4];
extern const char * const glymur_pmdomain_table[3];
extern const struct tz_cp_config tz_cp_config_glymur[3];
int glymur_init_cb_devs(struct iris_core *core);
void glymur_deinit_cb_devs(struct iris_core *core);

#endif
