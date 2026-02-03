// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * CIX System Reset Controller (SRC) driver
 *
 * Author: Jerry Zhu <jerry.zhu@cixtech.com>
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset/sky1.h>

#include <dt-bindings/reset/cix,sky1-rst-fch.h>

enum {
	FCH_SW_RST_FUNC			= 0x8,
	FCH_SW_RST_BUS			= 0xc,
	FCH_SW_XSPI			= 0x10,
};

static const struct sky1_src_signal sky1_src_fch_signals[] = {
	/* resets for fch_sw_rst_func */
	[SW_I3C0_RST_FUNC_G_N]	= { FCH_SW_RST_FUNC, BIT(0) },
	[SW_I3C0_RST_FUNC_I_N]	= { FCH_SW_RST_FUNC, BIT(1) },
	[SW_I3C1_RST_FUNC_G_N]	= { FCH_SW_RST_FUNC, BIT(2) },
	[SW_I3C1_RST_FUNC_I_N]	= { FCH_SW_RST_FUNC, BIT(3) },
	[SW_UART0_RST_FUNC_N]	= { FCH_SW_RST_FUNC, BIT(4) },
	[SW_UART1_RST_FUNC_N]	= { FCH_SW_RST_FUNC, BIT(5) },
	[SW_UART2_RST_FUNC_N]	= { FCH_SW_RST_FUNC, BIT(6) },
	[SW_UART3_RST_FUNC_N]	= { FCH_SW_RST_FUNC, BIT(7) },
	[SW_TIMER_RST_FUNC_N]	= { FCH_SW_RST_FUNC, BIT(20) },

	/* resets for fch_sw_rst_bus */
	[SW_I3C0_RST_APB_N]	= { FCH_SW_RST_BUS, BIT(0) },
	[SW_I3C1_RST_APB_N]	= { FCH_SW_RST_BUS, BIT(1) },
	[SW_DMA_RST_AXI_N]	= { FCH_SW_RST_BUS, BIT(2) },
	[SW_UART0_RST_APB_N]	= { FCH_SW_RST_BUS, BIT(4) },
	[SW_UART1_RST_APB_N]	= { FCH_SW_RST_BUS, BIT(5) },
	[SW_UART2_RST_APB_N]	= { FCH_SW_RST_BUS, BIT(6) },
	[SW_UART3_RST_APB_N]	= { FCH_SW_RST_BUS, BIT(7) },
	[SW_SPI0_RST_APB_N]	= { FCH_SW_RST_BUS, BIT(8) },
	[SW_SPI1_RST_APB_N]	= { FCH_SW_RST_BUS, BIT(9) },
	[SW_I2C0_RST_APB_N]	= { FCH_SW_RST_BUS, BIT(12) },
	[SW_I2C1_RST_APB_N]	= { FCH_SW_RST_BUS, BIT(13) },
	[SW_I2C2_RST_APB_N]	= { FCH_SW_RST_BUS, BIT(14) },
	[SW_I2C3_RST_APB_N]	= { FCH_SW_RST_BUS, BIT(15) },
	[SW_I2C4_RST_APB_N]	= { FCH_SW_RST_BUS, BIT(16) },
	[SW_I2C5_RST_APB_N]	= { FCH_SW_RST_BUS, BIT(17) },
	[SW_I2C6_RST_APB_N]	= { FCH_SW_RST_BUS, BIT(18) },
	[SW_I2C7_RST_APB_N]	= { FCH_SW_RST_BUS, BIT(19) },
	[SW_GPIO_RST_APB_N]	= { FCH_SW_RST_BUS, BIT(21) },

	/* resets for fch_sw_xspi */
	[SW_XSPI_REG_RST_N]	= { FCH_SW_XSPI, BIT(0) },
	[SW_XSPI_SYS_RST_N]	= { FCH_SW_XSPI, BIT(1) },
};

static const struct sky1_src_variant variant_sky1_fch = {
	.signals = sky1_src_fch_signals,
	.signals_num = ARRAY_SIZE(sky1_src_fch_signals),
};

static int sky1_reset_fch_probe(struct platform_device *pdev)
{
	return sky1_reset_common_probe(pdev, &variant_sky1_fch);
}

static struct platform_driver sky1_reset_driver = {
	.probe	= sky1_reset_fch_probe,
	.driver = {
		.name		= "cix,sky1-rst-fch",
	},
};
module_platform_driver(sky1_reset_driver)

MODULE_AUTHOR("Jerry Zhu <jerry.zhu@cixtech.com>");
MODULE_DESCRIPTION("Cix Sky1 reset driver");
MODULE_LICENSE("GPL");
