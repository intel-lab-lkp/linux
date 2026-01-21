// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (c) 2021 Samuel Holland <samuel@sholland.org>
//

#include <linux/auxiliary_bus.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>

#include "ccu_common.h"

#include "ccu_gate.h"
#include "ccu_mux.h"
#include "ccu_rtc.h"

#include "ccu-sun6i-rtc.h"

struct sun6i_rtc_match_data {
	bool				have_ext_osc32k		: 1;
	bool				have_iosc_calibration	: 1;
	bool				rtc_32k_single_parent	: 1;
	const struct clk_parent_data	*osc32k_fanout_parents;
	u8				osc32k_fanout_nparents;
};

static struct ccu_common iosc_clk = {
	.reg		= DCXO_CTRL_REG,
	.hw.init	= CLK_HW_INIT_NO_PARENT("iosc", &ccu_iosc_ops,
						CLK_GET_RATE_NOCACHE),
};

static struct ccu_common iosc_32k_clk = {
	.hw.init	= CLK_HW_INIT_HW("iosc-32k", &iosc_clk.hw,
					 &ccu_iosc_32k_ops,
					 CLK_GET_RATE_NOCACHE),
};

static const struct clk_hw *ext_osc32k[] = { NULL }; /* updated during probe */

static SUNXI_CCU_GATE_HWS(ext_osc32k_gate_clk, "ext-osc32k-gate",
			  ext_osc32k, 0x0, BIT(4), 0);

static const struct clk_hw *osc32k_parents[] = {
	&iosc_32k_clk.hw,
	&ext_osc32k_gate_clk.common.hw
};

static struct clk_init_data osc32k_init_data = {
	.name		= "osc32k",
	.ops		= &ccu_mux_ops,
	.parent_hws	= osc32k_parents,
	.num_parents	= ARRAY_SIZE(osc32k_parents), /* updated during probe */
};

static struct ccu_mux osc32k_clk = {
	.mux	= _SUNXI_CCU_MUX(0, 1),
	.common	= {
		.reg		= LOSC_CTRL_REG,
		.features	= CCU_FEATURE_KEY_FIELD,
		.hw.init	= &osc32k_init_data,
	},
};

/* This falls back to the global name for fwnodes without a named reference. */
static const struct clk_parent_data osc24M[] = {
	{ .fw_name = "hosc", .name = "osc24M" }
};

static struct ccu_gate osc24M_32k_clk = {
	.enable	= BIT(16),
	.common	= {
		.reg		= LOSC_OUT_GATING_REG,
		.prediv		= 750,
		.features	= CCU_FEATURE_ALL_PREDIV,
		.hw.init	= CLK_HW_INIT_PARENTS_DATA("osc24M-32k", osc24M,
							   &ccu_gate_ops, 0),
	},
};

static const struct clk_hw *rtc_32k_parents[] = {
	&osc32k_clk.common.hw,
	&osc24M_32k_clk.common.hw
};

static struct clk_init_data rtc_32k_init_data = {
	.name		= "rtc-32k",
	.ops		= &ccu_mux_ops,
	.parent_hws	= rtc_32k_parents,
	.num_parents	= ARRAY_SIZE(rtc_32k_parents), /* updated during probe */
	.flags		= CLK_IS_CRITICAL,
};

static struct ccu_mux rtc_32k_clk = {
	.mux	= _SUNXI_CCU_MUX(1, 1),
	.common	= {
		.reg		= LOSC_CTRL_REG,
		.features	= CCU_FEATURE_KEY_FIELD,
		.hw.init	= &rtc_32k_init_data,
	},
};

static struct clk_init_data osc32k_fanout_init_data = {
	.name		= "osc32k-fanout",
	.ops		= &ccu_mux_ops,
	/* parents are set during probe */
};

static struct ccu_mux osc32k_fanout_clk = {
	.enable	= BIT(0),
	.mux	= _SUNXI_CCU_MUX(1, 2),
	.common	= {
		.reg		= LOSC_OUT_GATING_REG,
		.hw.init	= &osc32k_fanout_init_data,
	},
};

static struct ccu_common *sun6i_rtc_ccu_clks[] = {
	&iosc_clk,
	&iosc_32k_clk,
	&ext_osc32k_gate_clk.common,
	&osc32k_clk.common,
	&osc24M_32k_clk.common,
	&rtc_32k_clk.common,
	&osc32k_fanout_clk.common,
};

static struct clk_hw_onecell_data sun6i_rtc_ccu_hw_clks = {
	.num = CLK_NUMBER,
	.hws = {
		[CLK_OSC32K]		= &osc32k_clk.common.hw,
		[CLK_OSC32K_FANOUT]	= &osc32k_fanout_clk.common.hw,
		[CLK_IOSC]		= &iosc_clk.hw,
		[CLK_IOSC_32K]		= &iosc_32k_clk.hw,
		[CLK_EXT_OSC32K_GATE]	= &ext_osc32k_gate_clk.common.hw,
		[CLK_OSC24M_32K]	= &osc24M_32k_clk.common.hw,
		[CLK_RTC_32K]		= &rtc_32k_clk.common.hw,
	},
};

static const struct sunxi_ccu_desc sun6i_rtc_ccu_desc = {
	.ccu_clks	= sun6i_rtc_ccu_clks,
	.num_ccu_clks	= ARRAY_SIZE(sun6i_rtc_ccu_clks),

	.hw_clks	= &sun6i_rtc_ccu_hw_clks,
};

static const struct clk_parent_data sun50i_h616_osc32k_fanout_parents[] = {
	{ .hw = &osc32k_clk.common.hw },
	{ .fw_name = "pll-32k" },
	{ .hw = &osc24M_32k_clk.common.hw }
};

static const struct clk_parent_data sun50i_r329_osc32k_fanout_parents[] = {
	{ .hw = &osc32k_clk.common.hw },
	{ .hw = &ext_osc32k_gate_clk.common.hw },
	{ .hw = &osc24M_32k_clk.common.hw }
};

static const struct sun6i_rtc_match_data sun50i_h616_rtc_ccu_data = {
	.have_iosc_calibration	= true,
	.rtc_32k_single_parent	= true,
	.osc32k_fanout_parents	= sun50i_h616_osc32k_fanout_parents,
	.osc32k_fanout_nparents	= ARRAY_SIZE(sun50i_h616_osc32k_fanout_parents),
};

static const struct sun6i_rtc_match_data sun50i_r329_rtc_ccu_data = {
	.have_ext_osc32k	= true,
	.osc32k_fanout_parents	= sun50i_r329_osc32k_fanout_parents,
	.osc32k_fanout_nparents	= ARRAY_SIZE(sun50i_r329_osc32k_fanout_parents),
};

static const struct sun6i_rtc_match_data sun55i_a523_rtc_ccu_data = {
	.have_ext_osc32k	= true,
	.have_iosc_calibration	= true,
	.osc32k_fanout_parents	= sun50i_r329_osc32k_fanout_parents,
	.osc32k_fanout_nparents	= ARRAY_SIZE(sun50i_r329_osc32k_fanout_parents),
};

static const struct of_device_id sun6i_rtc_ccu_match[] = {
	{
		.compatible	= "allwinner,sun50i-h616-rtc",
		.data		= &sun50i_h616_rtc_ccu_data,
	},
	{
		.compatible	= "allwinner,sun50i-r329-rtc",
		.data		= &sun50i_r329_rtc_ccu_data,
	},
	{
		.compatible	= "allwinner,sun55i-a523-rtc",
		.data		= &sun55i_a523_rtc_ccu_data,
	},
	{},
};
MODULE_DEVICE_TABLE(of, sun6i_rtc_ccu_match);

static int sun6i_rtc_ccu_probe(struct auxiliary_device *adev,
			       const struct auxiliary_device_id *id)
{
	const struct sun6i_rtc_match_data *data;
	struct clk *ext_osc32k_clk = NULL;
	const struct of_device_id *match;
	struct device *dev = &adev->dev;
	void __iomem *reg = dev->platform_data;
	struct device *parent = dev->parent;

	/* This driver is only used for newer variants of the hardware. */
	match = of_match_device(sun6i_rtc_ccu_match, parent);
	if (!match)
		return 0;

	data = match->data;
	if (data->have_iosc_calibration) {
		iosc_clk.features |= CCU_FEATURE_IOSC_CALIBRATION;
		iosc_32k_clk.features |= CCU_FEATURE_IOSC_CALIBRATION;
	}

	if (data->have_ext_osc32k) {
		const char *fw_name;

		/* ext-osc32k was the only input clock in the old binding. */
		fw_name = of_property_present(parent->of_node, "clock-names")
			? "ext-osc32k" : NULL;
		ext_osc32k_clk = devm_clk_get_optional(parent, fw_name);
		if (IS_ERR(ext_osc32k_clk))
			return PTR_ERR(ext_osc32k_clk);
	}

	if (ext_osc32k_clk) {
		/* Link ext-osc32k-gate to its parent. */
		*ext_osc32k = __clk_get_hw(ext_osc32k_clk);
	} else {
		/* ext-osc32k-gate is an orphan, so do not register it. */
		sun6i_rtc_ccu_hw_clks.hws[CLK_EXT_OSC32K_GATE] = NULL;
		osc32k_init_data.num_parents = 1;
	}

	if (data->rtc_32k_single_parent)
		rtc_32k_init_data.num_parents = 1;

	osc32k_fanout_init_data.parent_data = data->osc32k_fanout_parents;
	osc32k_fanout_init_data.num_parents = data->osc32k_fanout_nparents;

	return devm_sunxi_ccu_probe(dev, reg, &sun6i_rtc_ccu_desc);
}

static const struct auxiliary_device_id sun6i_ccu_rtc_ids[] = {
	{ .name = SUN6I_RTC_AUX_ID(sun6i) },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(auxiliary, sun6i_ccu_rtc_ids);

static struct auxiliary_driver sun6i_ccu_rtc_driver = {
	.probe = sun6i_rtc_ccu_probe,
	.id_table = sun6i_ccu_rtc_ids,
};
module_auxiliary_driver(sun6i_ccu_rtc_driver);

MODULE_IMPORT_NS("SUNXI_CCU");
MODULE_DESCRIPTION("Support for the Allwinner H616/R329 RTC CCU");
MODULE_LICENSE("GPL");
