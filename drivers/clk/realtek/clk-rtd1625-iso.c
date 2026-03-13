// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#include <dt-bindings/clock/realtek,rtd1625-clk.h>
#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include "clk-regmap-gate.h"

#define RTD1625_ISO_CLK_MAX   19
#define RTD1625_ISO_S_CLK_MAX 5

static CLK_REGMAP_GATE_NO_PARENT(clk_en_usb_p4, 0, 0x4, 0, 0);
static CLK_REGMAP_GATE_NO_PARENT(clk_en_usb_p3, 0, 0x4, 1, 0);
static CLK_REGMAP_GATE(clk_en_misc_cec0, "clk_en_misc", 0, 0x4, 2, 0);
static CLK_REGMAP_GATE_NO_PARENT(clk_en_cbusrx_sys, 0, 0x4, 3, 0);
static CLK_REGMAP_GATE_NO_PARENT(clk_en_cbustx_sys, 0, 0x4, 4, 0);
static CLK_REGMAP_GATE_NO_PARENT(clk_en_cbus_sys, 0, 0x4, 5, 0);
static CLK_REGMAP_GATE_NO_PARENT(clk_en_cbus_osc, 0, 0x4, 6, 0);
static CLK_REGMAP_GATE_NO_PARENT(clk_en_i2c0, 0, 0x4, 9, 0);
static CLK_REGMAP_GATE_NO_PARENT(clk_en_i2c1, 0, 0x4, 10, 0);
static CLK_REGMAP_GATE_NO_PARENT(clk_en_etn_250m, 0, 0x4, 11, 0);
static CLK_REGMAP_GATE_NO_PARENT(clk_en_etn_sys, 0, 0x4, 12, 0);
static CLK_REGMAP_GATE_NO_PARENT(clk_en_usb_drd, 0, 0x4, 13, 0);
static CLK_REGMAP_GATE_NO_PARENT(clk_en_usb_host, 0, 0x4, 14, 0);
static CLK_REGMAP_GATE_NO_PARENT(clk_en_usb_u3_host, 0, 0x4, 15, 0);
static CLK_REGMAP_GATE_NO_PARENT(clk_en_usb, 0, 0x4, 16, 0);
static CLK_REGMAP_GATE_NO_PARENT(clk_en_vtc, 0, 0x4, 17, 0);
static CLK_REGMAP_GATE(clk_en_misc_vfd, "clk_en_misc", 0, 0x4, 18, 0);

static struct clk_regmap *rtd1625_clk_regmap_list[] = {
	&clk_en_usb_p4.clkr,
	&clk_en_usb_p3.clkr,
	&clk_en_misc_cec0.clkr,
	&clk_en_cbusrx_sys.clkr,
	&clk_en_cbustx_sys.clkr,
	&clk_en_cbus_sys.clkr,
	&clk_en_cbus_osc.clkr,
	&clk_en_i2c0.clkr,
	&clk_en_i2c1.clkr,
	&clk_en_etn_250m.clkr,
	&clk_en_etn_sys.clkr,
	&clk_en_usb_drd.clkr,
	&clk_en_usb_host.clkr,
	&clk_en_usb_u3_host.clkr,
	&clk_en_usb.clkr,
	&clk_en_vtc.clkr,
	&clk_en_misc_vfd.clkr,
};

static struct clk_hw_onecell_data rtd1625_iso_clk_data = {
	.num = RTD1625_ISO_CLK_MAX,
	.hws = {
		[RTD1625_ISO_CLK_EN_USB_P4]      = &__clk_regmap_gate_hw(&clk_en_usb_p4),
		[RTD1625_ISO_CLK_EN_USB_P3]      = &__clk_regmap_gate_hw(&clk_en_usb_p3),
		[RTD1625_ISO_CLK_EN_MISC_CEC0]   = &__clk_regmap_gate_hw(&clk_en_misc_cec0),
		[RTD1625_ISO_CLK_EN_CBUSRX_SYS]  = &__clk_regmap_gate_hw(&clk_en_cbusrx_sys),
		[RTD1625_ISO_CLK_EN_CBUSTX_SYS]  = &__clk_regmap_gate_hw(&clk_en_cbustx_sys),
		[RTD1625_ISO_CLK_EN_CBUS_SYS]    = &__clk_regmap_gate_hw(&clk_en_cbus_sys),
		[RTD1625_ISO_CLK_EN_CBUS_OSC]    = &__clk_regmap_gate_hw(&clk_en_cbus_osc),
		[RTD1625_ISO_CLK_EN_I2C0]        = &__clk_regmap_gate_hw(&clk_en_i2c0),
		[RTD1625_ISO_CLK_EN_I2C1]        = &__clk_regmap_gate_hw(&clk_en_i2c1),
		[RTD1625_ISO_CLK_EN_ETN_250M]    = &__clk_regmap_gate_hw(&clk_en_etn_250m),
		[RTD1625_ISO_CLK_EN_ETN_SYS]     = &__clk_regmap_gate_hw(&clk_en_etn_sys),
		[RTD1625_ISO_CLK_EN_USB_DRD]     = &__clk_regmap_gate_hw(&clk_en_usb_drd),
		[RTD1625_ISO_CLK_EN_USB_HOST]    = &__clk_regmap_gate_hw(&clk_en_usb_host),
		[RTD1625_ISO_CLK_EN_USB_U3_HOST] = &__clk_regmap_gate_hw(&clk_en_usb_u3_host),
		[RTD1625_ISO_CLK_EN_USB]         = &__clk_regmap_gate_hw(&clk_en_usb),
		[RTD1625_ISO_CLK_EN_VTC]         = &__clk_regmap_gate_hw(&clk_en_vtc),
		[RTD1625_ISO_CLK_EN_MISC_VFD]    = &__clk_regmap_gate_hw(&clk_en_misc_vfd),
		[RTD1625_ISO_CLK_MAX] = NULL,
	},
};

static struct rtk_reset_bank rtd1625_iso_reset_banks[] = {
	{.ofs = 0x0,},
};

static const struct rtk_clk_desc rtd1625_iso_desc = {
	.clk_data        = &rtd1625_iso_clk_data,
	.clks            = rtd1625_clk_regmap_list,
	.num_clks        = ARRAY_SIZE(rtd1625_clk_regmap_list),
	.reset_banks     = rtd1625_iso_reset_banks,
	.num_reset_banks = ARRAY_SIZE(rtd1625_iso_reset_banks),
};

static CLK_REGMAP_GATE_NO_PARENT(clk_en_irda, 0, 0x4, 6, 1);
static CLK_REGMAP_GATE_NO_PARENT(clk_en_ur10, 0, 0x4, 8, 1);

static struct clk_regmap *rtd1625_iso_s_clk_regmap_list[] = {
	&clk_en_irda.clkr,
	&clk_en_ur10.clkr,
};

static struct clk_hw_onecell_data rtd1625_iso_s_clk_data = {
	.num = RTD1625_ISO_S_CLK_MAX,
	.hws = {
		[RTD1625_ISO_S_CLK_EN_IRDA] = &__clk_regmap_gate_hw(&clk_en_irda),
		[RTD1625_ISO_S_CLK_EN_UR10] = &__clk_regmap_gate_hw(&clk_en_ur10),
		[RTD1625_ISO_S_CLK_MAX] = NULL,
	},
};

static struct rtk_reset_bank rtd1625_iso_s_reset_banks[] = {
	{.ofs = 0x0, .write_en = 1,},
};

static const struct rtk_clk_desc rtd1625_iso_s_desc = {
	.clk_data        = &rtd1625_iso_s_clk_data,
	.clks            = rtd1625_iso_s_clk_regmap_list,
	.num_clks        = ARRAY_SIZE(rtd1625_iso_s_clk_regmap_list),
	.reset_banks     = rtd1625_iso_s_reset_banks,
	.num_reset_banks = ARRAY_SIZE(rtd1625_iso_s_reset_banks),
};

static int rtd1625_iso_probe(struct platform_device *pdev)
{
	const struct rtk_clk_desc *desc;

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc)
		return -EINVAL;
	return rtk_clk_probe(pdev, desc);
}

static const struct of_device_id rtd1625_iso_match[] = {
	{.compatible = "realtek,rtd1625-iso-clk", .data = &rtd1625_iso_desc},
	{.compatible = "realtek,rtd1625-iso-s-clk", .data = &rtd1625_iso_s_desc},
	{/* sentinel */}
};

static struct platform_driver rtd1625_iso_driver = {
	.probe = rtd1625_iso_probe,
	.driver = {
		.name = "rtk-rtd1625-iso-clk",
		.of_match_table = rtd1625_iso_match,
	},
};

static int __init rtd1625_iso_init(void)
{
	return platform_driver_register(&rtd1625_iso_driver);
}
subsys_initcall(rtd1625_iso_init);

MODULE_DESCRIPTION("Realtek RTD1625 ISO Controller Driver");
MODULE_AUTHOR("Cheng-Yu Lee <cylee12@realtek.com>");
MODULE_LICENSE("GPL");
