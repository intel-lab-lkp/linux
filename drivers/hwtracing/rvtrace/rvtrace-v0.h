/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 SiFive, Inc.
 *
 */

#ifndef __RVTRACE_V0_H__
#define __RVTRACE_V0_H__

#include <linux/rvtrace.h>

/* Register offsets */
#define RVTRACE_V0_CTRL_OFF			0x000
#define RVTRACE_V0_IMPL_OFF			0x004
#define RVTRACE_V0_IMPL_EXTRA_OFF		0x008
#define RVTRACE_V0_EV_CTRL_OFF			0x00c
#define RVTRACE_V0_SINK_BASE_OFF		0x010
#define RVTRACE_V0_SINK_BASE_HIGH_OFF		0x014
#define RVTRACE_V0_SINK_LIMIT_OFF		0x018
#define RVTRACE_V0_SINK_WP_OFF			0x01c
#define RVTRACE_V0_SINK_RP_OFF			0x020
#define RVTRACE_V0_SINK_DATA_OFF		0x024

#define RVTRACE_V0_CTRL_STOP_ON_WRAP_SHIFT	14
#define RVTRACE_V0_CTRL_SINK_SHIFT		28
#define RVTRACE_V0_CTRL_SINK_MASK		GENMASK(31, RVTRACE_V0_CTRL_SINK_SHIFT)

#define RVTRACE_V0_IMPL_HAS_SRAM_SINK_BIT	4
#define RVTRACE_V0_IMPL_HAS_ATB_SINK_BIT	5
#define RVTRACE_V0_IMPL_HAS_PIB_SINK_BIT	6
#define RVTRACE_V0_IMPL_HAS_SBA_SINK_BIT	7
#define RVTRACE_V0_IMPL_HAS_FUNNEL_SINK_BIT	8

#define RVTRACE_V0_IMPL_HAS_SRAM_SINK_MASK	BIT(RVTRACE_V0_IMPL_HAS_SRAM_SINK_BIT)
#define RVTRACE_V0_IMPL_HAS_ATB_SINK_MASK	BIT(RVTRACE_V0_IMPL_HAS_ATB_SINK_BIT)
#define RVTRACE_V0_IMPL_HAS_PIB_SINK_MASK	BIT(RVTRACE_V0_IMPL_HAS_PIB_SINK_BIT)
#define RVTRACE_V0_IMPL_HAS_SBA_SINK_MASK	BIT(RVTRACE_V0_IMPL_HAS_SBA_SINK_BIT)
#define RVTRACE_V0_IMPL_HAS_FUNNEL_SINK_MASK	BIT(RVTRACE_V0_IMPL_HAS_FUNNEL_SINK_BIT)

enum rvtrace_v0_sink {
	TE_SINK_DEFAULT = 0,
	TE_SINK_SRAM = 4,
	TE_SINK_ATB = 5,
	TE_SINK_PIB = 6,
	TE_SINK_SBA = 7,
	TE_SINK_FUNNEL = 8,
};

struct rvtrace_v0_comp_features {
	bool has_sram_sink;
	bool has_atb_sink;
	bool has_pib_sink;
	bool has_sba_sink;
	bool has_funnel_sink;
};

u32 rvtrace_v0_get_encoder_impl(struct rvtrace_platform_data *pdata);
u32 rvtrace_v0_get_funnel_impl(struct rvtrace_platform_data *pdata);
void *rvtrace_v0_get_comp_data(struct rvtrace_platform_data *pdata);

int rvtrace_v0_sink_config(struct rvtrace_path_node *node);
int rvtrace_v0_ramsink_setup(struct rvtrace_component *comp);

#endif /* __RVTRACE_V0_H__ */

