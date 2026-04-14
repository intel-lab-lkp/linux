// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <dt-bindings/media/qcom,glymur-iris.h>
#include "iris_core.h"
#include "iris_platform_common.h"
#include "iris_platform_glymur.h"

#define VIDEO_REGION_SECURE_FW_REGION_ID	0
#define VIDEO_REGION_VM0_SECURE_NP_ID		1
#define VIDEO_REGION_VM0_NONSECURE_NP_ID	5

const struct platform_clk_data glymur_clk_table[] = {
	{IRIS_AXI_VCODEC_CLK,		"iface"			},
	{IRIS_CTRL_CLK,			"core"			},
	{IRIS_VCODEC_CLK,		"vcodec0_core"		},
	{IRIS_AXI_CTRL_CLK,		"iface_ctrl"		},
	{IRIS_CTRL_FREERUN_CLK,		"core_freerun"		},
	{IRIS_VCODEC_FREERUN_CLK,	"vcodec0_core_freerun"	},
	{IRIS_AXI_VCODEC1_CLK,		"iface1"		},
	{IRIS_VCODEC1_CLK,		"vcodec1_core"		},
	{IRIS_VCODEC1_FREERUN_CLK,	"vcodec1_core_freerun"	},
};

const char * const glymur_clk_reset_table[] = {
	"bus0",
	"bus_ctrl",
	"core",
	"vcodec0_core",
	"bus1",
	"vcodec1_core",
};

const char * const glymur_opp_clk_table[] = {
	"vcodec0_core",
	"vcodec1_core",
	"core",
	NULL,
};

const char * const glymur_pmdomain_table[] = {
	"venus",
	"vcodec0",
	"vcodec1",
};

const struct tz_cp_config tz_cp_config_glymur[] = {
	{
		.cp_start = VIDEO_REGION_SECURE_FW_REGION_ID,
		.cp_size = 0,
		.cp_nonpixel_start = 0,
		.cp_nonpixel_size = 0x1000000,
	},
	{
		.cp_start = VIDEO_REGION_VM0_SECURE_NP_ID,
		.cp_size = 0,
		.cp_nonpixel_start = 0x1000000,
		.cp_nonpixel_size = 0x24800000,
	},
	{
		.cp_start = VIDEO_REGION_VM0_NONSECURE_NP_ID,
		.cp_size = 0,
		.cp_nonpixel_start = 0x25800000,
		.cp_nonpixel_size = 0xda600000,
	},
};

int glymur_init_cb_devs(struct iris_core *core)
{
	const u32 f_id = IRIS_FIRMWARE;
	struct device *dev;

	dev = iris_create_cb_dev(core, "iris_firmware", &f_id);
	if (IS_ERR(dev))
		return PTR_ERR(dev);

	if (device_iommu_mapped(dev))
		core->dev_fw = dev;
	else
		device_unregister(dev);

	return 0;
}

void glymur_deinit_cb_devs(struct iris_core *core)
{
	if (core->dev_fw)
		device_unregister(core->dev_fw);

	core->dev_fw = NULL;
}
