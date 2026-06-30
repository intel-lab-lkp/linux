// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 SiFive Inc.
 */

#include <linux/of_reserved_mem.h>
#include <linux/rvtrace.h>
#include "rvtrace-v0.h"
#include "rvtrace-ramsink.h"

int rvtrace_v0_ramsink_setup(struct rvtrace_component *comp)
{
	struct rvtrace_v0_comp_features	*data = NULL;
	int ret;
	u32 val;

	data = (struct rvtrace_v0_comp_features *)comp->id.data;
	if (!data)
		return -EINVAL;

	if (comp->id.type > RVTRACE_COMPONENT_TYPE_FUNNEL)
		return -EOPNOTSUPP;

	if (data->has_sba_sink) {
		/*
		 * The pre-ratified ramsink's sink limit and write pointer registers share
		 * their upper 32 bits with the sink base address register. This hardware
		 * constraint prevents the trace buffer from crossing a 4GB memory boundary.
		 *
		 * Use of_reserved_mem_device_init() to associate the device with a
		 * reserved memory region defined in the Device Tree. This ensures the
		 * buffer allocation adheres to the necessary alignment and size constraints.
		 */
		ret = of_reserved_mem_device_init(comp->pdata->dev);
		if (ret) {
			dev_err(comp->pdata->dev, "Failed to get reserved memory region\n");
			return ret;
		}

		/* Set comp sink to SBA (system memory) */
		val = rvtrace_read32(comp->pdata, RVTRACE_COMPONENT_CTRL_OFFSET);
		val &= ~RVTRACE_V0_CTRL_SINK_MASK;
		val |= TE_SINK_SBA << RVTRACE_V0_CTRL_SINK_SHIFT;
		rvtrace_write32(comp->pdata, val, RVTRACE_COMPONENT_CTRL_OFFSET);

		return rvtrace_ramsink_setup(comp);
	}

	return 0;
}

int rvtrace_v0_sink_config(struct rvtrace_path_node *node)
{
	struct rvtrace_component *comp = node->comp;
	struct rvtrace_path_node *next_node = NULL;
	struct rvtrace_v0_comp_features	*data = NULL;
	u32 val;

	data = (struct rvtrace_v0_comp_features *)comp->id.data;
	if (!data)
		return -EINVAL;

	val = rvtrace_read32(comp->pdata, RVTRACE_COMPONENT_CTRL_OFFSET);
	val &= ~RVTRACE_V0_CTRL_SINK_MASK;

	/* last node in list */
	if (!node->conn) {
		if (data->has_sba_sink) {
			val |= TE_SINK_SBA << RVTRACE_V0_CTRL_SINK_SHIFT;
		} else {
			dev_warn(&comp->dev,
				 "Component is last node but doesn't support SBA sink\n");
		}
	} else {
		next_node = list_next_entry(node, head);
		if (next_node && next_node->comp->id.type == RVTRACE_COMPONENT_TYPE_FUNNEL &&
		    data->has_funnel_sink) {
			val |= TE_SINK_FUNNEL << RVTRACE_V0_CTRL_SINK_SHIFT;
		}
	}

	rvtrace_write32(comp->pdata, val, RVTRACE_COMPONENT_CTRL_OFFSET);

	return 0;
}

void *rvtrace_v0_get_comp_data(struct rvtrace_platform_data *pdata)
{
	struct rvtrace_v0_comp_features	*data;
	u32 impl;

	data = devm_kzalloc(pdata->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return ERR_PTR(-ENOMEM);

	impl = rvtrace_read32(pdata, RVTRACE_COMPONENT_IMPL_OFFSET);

	data->has_sram_sink = impl & RVTRACE_V0_IMPL_HAS_SRAM_SINK_MASK;
	data->has_atb_sink = impl & RVTRACE_V0_IMPL_HAS_ATB_SINK_MASK;
	data->has_pib_sink = impl & RVTRACE_V0_IMPL_HAS_PIB_SINK_MASK;
	data->has_sba_sink = impl & RVTRACE_V0_IMPL_HAS_SBA_SINK_MASK;
	data->has_funnel_sink = impl & RVTRACE_V0_IMPL_HAS_FUNNEL_SINK_MASK;

	return data;
}

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
