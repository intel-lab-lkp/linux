// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Junhui Liu <junhui.liu@pigmoral.tech>
 */

#include <linux/array_size.h>
#include <linux/auxiliary_bus.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/module.h>

#include "ccu_common.h"

#include "ccu_gate.h"
#include "ccu_mux.h"
#include "ccu_rtc.h"

#include "ccu-sun60i-a733-rtc.h"

static struct ccu_common iosc_clk = {
	.reg		= DCXO_CTRL_REG,
	.features	= CCU_FEATURE_IOSC_CALIBRATION,
	.hw.init	= CLK_HW_INIT_NO_PARENT("iosc", &ccu_iosc_ops,
						CLK_GET_RATE_NOCACHE),
};

static struct ccu_common iosc_32k_clk = {
	.features	= CCU_FEATURE_IOSC_CALIBRATION,
	.hw.init	= CLK_HW_INIT_HW("iosc-32k", &iosc_clk.hw,
					 &ccu_iosc_32k_ops,
					 CLK_GET_RATE_NOCACHE),
};

static SUNXI_CCU_GATE_FW(ext_osc32k_gate_clk, "ext-osc32k-gate",
			 "ext-osc32k", 0x0, BIT(4), 0);

static const struct clk_hw *osc32k_parents[] = {
	&iosc_32k_clk.hw,
	&ext_osc32k_gate_clk.common.hw,
};

static struct ccu_mux osc32k_clk = {
	.mux	= _SUNXI_CCU_MUX(0, 1),
	.common	= {
		.reg		= LOSC_CTRL_REG,
		.features	= CCU_FEATURE_KEY_FIELD,
		.hw.init	= CLK_HW_INIT_PARENTS_HW("osc32k",
							 osc32k_parents,
							 &ccu_mux_ops,
							 0),
	},
};

static const struct clk_parent_data hosc_parents[] = {
	{ .fw_name = "osc24M" },
	{ .fw_name = "osc19M" },
	{ .fw_name = "osc26M" },
	{ .fw_name = "osc24M" },
};

struct ccu_mux hosc_clk = {
	.enable	= DCXO_CTRL_DCXO_EN,
	.mux	= _SUNXI_CCU_MUX(14, 2),
	.common	= {
		.reg		= DCXO_CTRL_REG,
		.hw.init	= CLK_HW_INIT_PARENTS_DATA("hosc",
							   hosc_parents,
							   &ccu_mux_ro_ops,
							   0),
	},
};

static const struct ccu_mux_fixed_prediv hosc_32k_predivs[] = {
	{ .index = 0, .div = 732 },
	{ .index = 1, .div = 586 },
	{ .index = 2, .div = 793 },
	{ .index = 3, .div = 732 },
};

static struct ccu_mux hosc_32k_mux_clk = {
	.enable		= DCXO_CTRL_DCXO_EN,
	.mux		= {
		.shift		= 14,
		.width		= 2,
		.fixed_predivs	= hosc_32k_predivs,
		.n_predivs	= ARRAY_SIZE(hosc_32k_predivs),
	},
	.common		= {
		.reg		= DCXO_CTRL_REG,
		.features	= CCU_FEATURE_FIXED_PREDIV,
		.hw.init	= CLK_HW_INIT_PARENTS_DATA("hosc-32k-mux",
							   hosc_parents,
							   &ccu_mux_ro_ops,
							   0),
	},
};

static SUNXI_CCU_GATE_HW(hosc_32k_clk, "hosc-32k", &hosc_32k_mux_clk.common.hw,
			 LOSC_OUT_GATING_REG, BIT(16), 0);

static const struct clk_hw *rtc_32k_parents[] = {
	&osc32k_clk.common.hw,
	&hosc_32k_clk.common.hw,
};

static struct ccu_mux rtc_32k_clk = {
	.mux	= _SUNXI_CCU_MUX(1, 1),
	.common	= {
		.reg		= LOSC_CTRL_REG,
		.features	= CCU_FEATURE_KEY_FIELD,
		.hw.init	= CLK_HW_INIT_PARENTS_HW("rtc-32k",
							 rtc_32k_parents,
							 &ccu_mux_ops,
							 0),
	},
};

static const struct clk_parent_data osc32k_fanout_parents[] = {
	{ .hw = &osc32k_clk.common.hw },
	{ .hw = &ext_osc32k_gate_clk.common.hw },
	{ .hw = &hosc_32k_clk.common.hw },
};

static SUNXI_CCU_MUX_DATA_WITH_GATE(osc32k_fanout_clk, "osc32k-fanout", osc32k_fanout_parents,
				    LOSC_OUT_GATING_REG,
				    1, 2,	/* mux */
				    BIT(0),	/* gate */
				    0);

static SUNXI_CCU_GATE_HW(hosc_serdes1_clk, "hosc-serdes1", &hosc_clk.common.hw,
			 DCXO_GATING_REG, DCXO_SERDES1_GATING, 0);
static SUNXI_CCU_GATE_HW(hosc_serdes0_clk, "hosc-serdes0", &hosc_clk.common.hw,
			 DCXO_GATING_REG, DCXO_SERDES0_GATING, 0);
static SUNXI_CCU_GATE_HW(hosc_hdmi_clk, "hosc-hdmi", &hosc_clk.common.hw,
			 DCXO_GATING_REG, DCXO_HDMI_GATING, 0);
static SUNXI_CCU_GATE_HW(hosc_ufs_clk, "hosc-ufs", &hosc_clk.common.hw,
			 DCXO_GATING_REG, DCXO_UFS_GATING, 0);

static struct ccu_common *sun60i_rtc_ccu_clks[] = {
	&iosc_clk,
	&iosc_32k_clk,
	&ext_osc32k_gate_clk.common,
	&osc32k_clk.common,
	&hosc_clk.common,
	&hosc_32k_mux_clk.common,
	&hosc_32k_clk.common,
	&rtc_32k_clk.common,
	&osc32k_fanout_clk.common,
	&hosc_serdes1_clk.common,
	&hosc_serdes0_clk.common,
	&hosc_hdmi_clk.common,
	&hosc_ufs_clk.common,
};

static struct clk_hw_onecell_data sun60i_rtc_ccu_hw_clks = {
	.num = CLK_NUMBER,
	.hws = {
		[CLK_IOSC]		= &iosc_clk.hw,
		[CLK_OSC32K]		= &osc32k_clk.common.hw,
		[CLK_HOSC]		= &hosc_clk.common.hw,
		[CLK_RTC_32K]		= &rtc_32k_clk.common.hw,
		[CLK_OSC32K_FANOUT]	= &osc32k_fanout_clk.common.hw,
		[CLK_HOSC_SERDES1]	= &hosc_serdes1_clk.common.hw,
		[CLK_HOSC_SERDES0]	= &hosc_serdes0_clk.common.hw,
		[CLK_HOSC_HDMI]		= &hosc_hdmi_clk.common.hw,
		[CLK_HOSC_UFS]		= &hosc_ufs_clk.common.hw,
		[CLK_IOSC_32K]		= &iosc_32k_clk.hw,
		[CLK_EXT_OSC32K_GATE]	= &ext_osc32k_gate_clk.common.hw,
		[CLK_HOSC_32K_MUX]	= &hosc_32k_mux_clk.common.hw,
		[CLK_HOSC_32K]		= &hosc_32k_clk.common.hw,
	},
};

static const struct sunxi_ccu_desc sun60i_rtc_ccu_desc = {
	.ccu_clks	= sun60i_rtc_ccu_clks,
	.num_ccu_clks	= ARRAY_SIZE(sun60i_rtc_ccu_clks),

	.hw_clks	= &sun60i_rtc_ccu_hw_clks,
};

static int sun60i_rtc_ccu_probe(struct auxiliary_device *adev,
				const struct auxiliary_device_id *id)
{
	struct device *dev = &adev->dev;
	void __iomem *reg = dev->platform_data;

	return devm_sunxi_ccu_probe(dev, reg, &sun60i_rtc_ccu_desc);
}

static const struct auxiliary_device_id sun60i_ccu_rtc_ids[] = {
	{ .name = SUN6I_RTC_AUX_ID(sun60i) },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(auxiliary, sun60i_ccu_rtc_ids);

static struct auxiliary_driver sun60i_ccu_rtc_driver = {
	.probe = sun60i_rtc_ccu_probe,
	.id_table = sun60i_ccu_rtc_ids,
};
module_auxiliary_driver(sun60i_ccu_rtc_driver);

MODULE_IMPORT_NS("SUNXI_CCU");
MODULE_DESCRIPTION("Support for the Allwinner A733 RTC CCU");
MODULE_LICENSE("GPL");
