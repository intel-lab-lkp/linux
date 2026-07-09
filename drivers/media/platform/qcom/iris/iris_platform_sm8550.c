// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include "iris_core.h"
#include "iris_platform_common.h"
#include "iris_platform_sm8550.h"

const char * const sm8550_clk_reset_table[] = { "bus" };

const struct platform_clk_data sm8550_clk_table[] = {
	{IRIS_AXI_CLK,  "iface"        },
	{IRIS_CTRL_CLK, "core"         },
	{IRIS_HW_CLK,   "vcodec0_core" },
};

struct platform_inst_caps platform_inst_cap_sm8550 = {
	.min_frame_width = 96,
	.max_frame_width = 8192,
	.min_frame_height = 96,
	.max_frame_height = 8192,
	.max_mbpf = (8192 * 4352) / 256,
	.mb_cycles_vpp = 200,
	.mb_cycles_fw = 489583,
	.mb_cycles_fw_vpp = 66234,
	.max_frame_rate = MAXIMUM_FPS,
	.max_operating_rate = MAXIMUM_FPS,
};

static int sm8550_init_cb_devs(struct iris_core *core)
{
	struct device *dev;

	dev = iris_create_cb_dev(core, "non-pixel");
	if (IS_ERR(dev))
		return PTR_ERR(dev);

	core->np_dev = dev;

	dev = iris_create_cb_dev(core, "pixel");
	if (IS_ERR(dev))
		goto unreg_np_dev;

	core->p_dev = dev;

	return 0;

unreg_np_dev:
	if (core->np_dev)
		platform_device_unregister(to_platform_device(core->np_dev));
	core->np_dev = NULL;

	return PTR_ERR(dev);
}

static void sm8550_deinit_cb_devs(struct iris_core *core)
{
	if (core->p_dev)
		platform_device_unregister(to_platform_device(core->p_dev));
	if (core->np_dev)
		platform_device_unregister(to_platform_device(core->np_dev));

	core->p_dev = NULL;
	core->np_dev = NULL;
}

const struct iris_context_bank_ops sm8550_cb_ops = {
	.init = sm8550_init_cb_devs,
	.deinit = sm8550_deinit_cb_devs,
};
