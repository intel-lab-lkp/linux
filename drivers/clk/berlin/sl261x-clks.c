// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Synaptics Incorporated
 *
 * Author: Jisheng Zhang <jszhang@kernel.org>
 *
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/module.h>

#include <dt-bindings/clock/syna,sl261x.h>

#include "sl261x-pll.h"
#include "sl-clk.h"
#include "sl-sysmgr-clk.h"

#define REG_CLKENABLE		0x0420

#define CLK_GATE_CTRL0		0x0600

#define SYSPLL0_OFFSET		0x0200
#define SYSPLL1_OFFSET		0x0220
#define CPUPLL_OFFSET		0x2000
#define APLL0_OFFSET		0x0008
#define APLL1_OFFSET		0x0030

static DEFINE_SPINLOCK(gate_lock);
static DEFINE_SPINLOCK(lock);

enum {
	SYSPLL0_OUT, SYSPLL0_OUTF,
	SYSPLL1_OUT, SYSPLL1_OUTF,
	CPUPLL_OUT, CPUPLL_OUTF,
	APLL0_OUT, APLL0_OUTF,
	APLL1_OUT, APLL1_OUTF,
};

static const char *clk_names[] = {
	[SYSPLL0_OUT]		= "syspll0_out",
	[SYSPLL0_OUTF]		= "syspll0_outf",
	[SYSPLL1_OUT]		= "syspll1_out",
	[SYSPLL1_OUTF]		= "syspll1_outf",
	[CPUPLL_OUT]		= "cpupll_out",
	[CPUPLL_OUTF]		= "cpupll_outf",
	[APLL0_OUT]		= "apll0_out",
	[APLL0_OUTF]		= "apll0_outf",
	[APLL1_OUT]		= "apll1_out",
	[APLL1_OUTF]		= "apll1_outf",
};

static const struct sl_gateclk_desc sl261x_gates[] = {
	{ "usb0coreclk",	"perifsysclk",	CLK_USB0CORE,		0 },
	{ "sdiosysclk",		"perifsysclk",	CLK_SDIOSYS,		1 },
	{ "emmcsysclk",		"perifsysclk",	CLK_EMMCSYS,		2 },
	{ "gpuaxiclk",		"hpcclk",	CLK_GPUAXI,		3 },
	{ "gethrgmiisysclk",	"perifsysclk",	CLK_GETHRGMIISYS,	4 },
	{ "sdio1sysclk",	"perifsysclk",	CLK_SDIO1SYS,		5 },
	{ "usb1coreclk",	"perifsysclk",	CLK_USB1CORE,		6 },
	{ "gethrgmii1sysclk",	"perifsysclk",	CLK_GETHRGMII1SYS,	7 },
	{ "usb0phyrefclk",	"perifsysclk",	CLK_USB0PHYREF,		8 },
	{ "usb1phyrefclk",	"perifsysclk",	CLK_USB1PHYREF,		9 },
	{ "apbuart0clk",	"apbcoreclk",	CLK_APBUART0,		10 },
	{ "apbuart1clk",	"apbcoreclk",	CLK_APBUART1,		11 },
	{ "apbuart2clk",	"apbcoreclk",	CLK_APBUART2,		12 },
	{ "apbuart3lck",	"apbcoreclk",	CLK_APBUART3,		13 },
	{ "apbi2c0clk",		"apbcoreclk",	CLK_APBI2C0,		14 },
	{ "apbi2c1lck",		"apbcoreclk",	CLK_APBI2C1,		15 },
	{ "apbspi0clk",		"apbcoreclk",	CLK_APBSPI0,		16 },
	{ "apbspi1clk",		"apbcoreclk",	CLK_APBSPI1,		17 },
	{ "apbspi2clk",		"apbcoreclk",	CLK_APBSPI2,		18 },
	{ "apbspi3clk",		"apbcoreclk",	CLK_APBSPI3,		19 },
	{ "apbgpioclk",		"apbcoreclk",	CLK_APBGPIO,		20 },
	{ "apbtimersclk",	"apbcoreclk",	CLK_APBTIMERS,		21 },
	{ "apbsyscntclk",	"apbcoreclk",	CLK_APBSYSCNT,		22 },
	{ "apbwdtclk",		"apbcoreclk",	CLK_APBWDT,		23 },
	{ "apbdmaclk",		"apbcoreclk",	CLK_APBDMA,		24 },
};

static const struct sl_clk_desc sl261x_descs[] = {
	{ "cpufastrefclk",	CLK_CPUFASTREF,		0x440, CLK_IS_CRITICAL },
	{ "memfastrefclk",	CLK_MEMFASTREF,		0x444, CLK_IS_CRITICAL },
	{ "cfgclk",		CLK_CFG,		0x448, CLK_IS_CRITICAL },
	{ "sysclk",		CLK_SYS,		0x44c, CLK_IS_CRITICAL },
	{ "perifsysclk",	CLK_PERIFSYS,		0x450, CLK_IS_CRITICAL },
	{ "apbcoreclk",		CLK_APBCORE,		0x454, CLK_IS_CRITICAL },
	{ "apbserclk",		CLK_APBSER,		0x458, CLK_IS_CRITICAL },
	{ "atbclk",		CLK_ATB,		0x45c },
	{ "hpcclk",		CLK_HPC,		0x460, CLK_IS_CRITICAL },
	{ "emmcclk",		CLK_EMMC,		0x464 },
	{ "sd0clk",		CLK_SD0,		0x468 },
	{ "sd1clk",		CLK_SD1,		0x46c },
	{ "gethrgmiiclk",	CLK_GETHRGMII,		0x470 },
	{ "gethrgmii1clk",	CLK_GETHRGMII1,		0x474 },
	{ "ge0_ptp_refclk",	CLK_GE0_PTP_REF,	0x478 },
	{ "ge1_ptp_refclk",	CLK_GE1_PTP_REF,	0x47c },
	{ "usb2testclk",	CLK_USB2TEST,		0x480 },
	{ "usb2test480mg0clk",	CLK_USB2TEST480MG0,	0x484 },
	{ "usb2test480mg1clk",	CLK_USB2TEST480MG1,	0x488 },
	{ "usb2test480mg2clk",	CLK_USB2TEST480MG2,	0x48c },
	{ "usb2test100mg0clk",	CLK_USB2TEST100MG0,	0x490 },
	{ "usb2test100mg1clk",	CLK_USB2TEST100MG1,	0x494 },
	{ "usb2test100mg2clk",	CLK_USB2TEST100MG2,	0x498 },
	{ "usb2test100mg3clk",	CLK_USB2TEST100MG3,	0x49c },
	{ "periftest125mg0clk",	CLK_PERIFTEST125MG0,	0x4a0 },
	{ "periftest200mg0clk",	CLK_PERIFTEST200MG0,	0x4a4 },
	{ "periftest200mg1clk",	CLK_PERIFTEST200MG1,	0x4a8 },
	{ "gpuclk",		CLK_GPU,		0x4ac },
	{ "npuclk",		CLK_NPU,		0x4b0 },
	{ "aviosysclk",		CLK_AVIOSYS,		0x4b4, CLK_IS_CRITICAL },
	{ "aiosysclk",		CLK_AIOSYS,		0x4b8, CLK_IS_CRITICAL },
	{ "avio_lcdc2scanclk",	CLK_AVIO_LCDC2SCAN,	0x4bc },
	{ "avio_ipiclk",	CLK_AVIO_IPI,		0x4c0 },
	{ "avio_pclk",		CLK_AVIO_P,		0x4c4 },
	{ "avio_dphyrxtxescclk", CLK_AVIO_DPHYRXTXESC,	0x4c8 },
	{ "aviofpllclk",	CLK_AVIOFPLL,		0x4cc },
	{ "avio_rx_scanbyteclk", CLK_AVIO_RX_SCANBYTE,	0x4d0 },
	{ "avio_rx_scantestclk", CLK_AVIO_RX_SCANTEST,	0x4d4 },
};

static const char *default_parent_names[] = {
	"syspll0_out",
	"apll0_out",
	"syspll0_outf",
	"apll0_outf",
	"syspll1_out",
	"syspll1_outf",
};

static const char *ge_ptp_refclk_parent_names[] = {
	"osc",
	"apll0_outf",
	"syspll1_out",
	"syspll0_out",
	"clk_mcu_axi",
};

static int sl261x_soc_clk_setup(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *parent_np = of_get_parent(np);
	struct clk_hw_onecell_data *clk_data;
	const char **parent_names;
	int num_parents;
	void __iomem *base;
	struct clk_hw **hws;
	int n, ret;
	u32 id;

	base = devm_of_iomap(dev, parent_np, 0, NULL);
	of_node_put(parent_np);
	if (IS_ERR(base))
		return PTR_ERR(base);

	clk_data = devm_kzalloc(dev, struct_size(clk_data, hws, SL261X_CLK_MAX), GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;
	clk_data->num = SL261X_CLK_MAX;
	hws = clk_data->hws;

	/* sysplls */
	ret = sl261x_pll_register(dev, base + SYSPLL0_OFFSET,
				  clk_names[SYSPLL0_OUT], clk_names[SYSPLL0_OUTF]);
	if (ret)
		return ret;
	ret = sl261x_pll_register(dev, base + SYSPLL1_OFFSET,
				  clk_names[SYSPLL1_OUT], clk_names[SYSPLL1_OUTF]);
	if (ret)
		return ret;

	/* normal clocks */
	for (n = 0; n < ARRAY_SIZE(sl261x_descs); n++) {
		const struct sl_clk_desc *cd = &sl261x_descs[n];

		id = cd->id;
		if (id == CLK_GE0_PTP_REF || id == CLK_GE1_PTP_REF) {
			parent_names = ge_ptp_refclk_parent_names;
			num_parents = ARRAY_SIZE(ge_ptp_refclk_parent_names);
		} else {
			parent_names = default_parent_names;
			num_parents = ARRAY_SIZE(default_parent_names);
		}
		hws[id] = sl_clk_register(dev, base + cd->offset, cd->name,
					  parent_names, num_parents, cd->flags, &lock);
		if (IS_ERR(hws[id]))
			return PTR_ERR(hws[id]);
	}

	/* gate clocks */
	for (n = 0; n < ARRAY_SIZE(sl261x_gates); n++) {
		const struct sl_gateclk_desc *gd = &sl261x_gates[n];

		id = gd->id;
		hws[id] = devm_clk_hw_register_gate(dev, gd->name, gd->parent_name,
						    gd->flags, base + REG_CLKENABLE,
						    gd->bit_idx, 0, &gate_lock);
		if (IS_ERR(hws[id]))
			return PTR_ERR(hws[id]);
	}

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clk_data);
}

static int sl261x_cpu_clk_setup(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *parent_np = of_get_parent(np);
	struct clk_hw_onecell_data *clk_data;
	struct clk_hw **hws;
	void __iomem *base;
	int ret;

	base = devm_of_iomap(dev, parent_np, 0, NULL);
	of_node_put(parent_np);
	if (IS_ERR(base))
		return PTR_ERR(base);

	clk_data = devm_kzalloc(dev, struct_size(clk_data, hws, SL261X_CPU_CLK_MAX), GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;
	clk_data->num = SL261X_CPU_CLK_MAX;
	hws = clk_data->hws;

	/* cpupll */
	ret = sl261x_pll_register(dev, base + CPUPLL_OFFSET,
				  clk_names[CPUPLL_OUT], clk_names[CPUPLL_OUTF]);
	if (ret)
		return ret;

	/* TODO: support cpupll bypass */
	hws[CLK_CPU0] =
		devm_clk_hw_register_fixed_factor(dev, "cpu0", clk_names[CPUPLL_OUTF], 0, 1, 1);
	if (IS_ERR(hws[CLK_CPU0]))
		return PTR_ERR(hws[CLK_CPU0]);
	hws[CLK_CPU1] =
		devm_clk_hw_register_fixed_factor(dev, "cpu1", clk_names[CPUPLL_OUTF], 0, 1, 1);
	if (IS_ERR(hws[CLK_CPU1]))
		return PTR_ERR(hws[CLK_CPU1]);
	hws[CLK_DSU] =
		devm_clk_hw_register_fixed_factor(dev, "dsu", clk_names[CPUPLL_OUT], 0, 1, 1);
	if (IS_ERR(hws[CLK_DSU]))
		return PTR_ERR(hws[CLK_DSU]);

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clk_data);
}

static int sl261x_aio_clk_setup(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *parent_np = of_get_parent(np);
	void __iomem *base;
	int ret;

	base = devm_of_iomap(dev, parent_np, 0, NULL);
	of_node_put(parent_np);
	if (IS_ERR(base))
		return PTR_ERR(base);

	/* aplls */
	ret = sl261x_pll_register(dev, base + APLL0_OFFSET,
				  clk_names[APLL0_OUT], clk_names[APLL0_OUTF]);
	if (ret)
		return ret;
	ret = sl261x_pll_register(dev, base + APLL1_OFFSET,
				  clk_names[APLL1_OUT], clk_names[APLL1_OUTF]);
	if (ret)
		return ret;

	/* TODO aio clks */

	return 0;
}

static const struct sl_sysmgr_fixedclk_desc sl261x_sysmgr_fixeds[] = {
	{ "clk_mcu_ref",	CLK_MCU_REF,	25000000 },
	{ "clk_mcu_pll",	CLK_MCU_PLL,	400000000 },
	{ "clk_mcu_io_pll",	CLK_MCU_IO_PLL,	300000000 },
	{ "clk_mcu_apb",	CLK_MCU_APB,	100000000 },
	{ "clk_mcu_axi",	CLK_MCU_AXI,	200000000 },
};

static const struct sl_sysmgr_gateclk_desc sl261x_sysmgr_gates[] = {
	{ "clk_cfg_uart0",	"clk_mcu_apb",	CLK_CFG_UART0,	0 },
	{ "clk_ser_uart0",	"clk_mcu_ref",	CLK_SER_UART0,	1 },
	{ "clk_cfg_uart1",	"clk_mcu_apb",	CLK_CFG_UART1,	2 },
	{ "clk_cfg_uart2",	"clk_mcu_apb",	CLK_CFG_UART2,	3 },
	{ "clk_cfg_uart3",	"clk_mcu_apb",	CLK_CFG_UART3,	4 },
	{ "clk_cfg_gpio",	"clk_mcu_apb",	CLK_CFG_GPIO,	5 },
	{ "clk_cfg_i2cm0",	"clk_mcu_apb",	CLK_CFG_I2CM0,	6 },
	{ "clk_cfg_i2cm1",	"clk_mcu_apb",	CLK_CFG_I2CM1,	7 },
	{ "clk_cfg_spim",	"clk_mcu_apb",	CLK_CFG_SPIM,	8 },
	{ "clk_cfg_spis",	"clk_mcu_apb",	CLK_CFG_SPIS,	9 },
	{ "clk_cfg_i3c",	"clk_mcu_apb",	CLK_CFG_I3C,	10 },
	{ "clk_cfg_xspi",	"clk_mcu_apb",	CLK_CFG_XSPI,	11 },
	{ "clk_axi_xspi",	"clk_mcu_axi",	CLK_AXI_XSPI,	12 },
	{ "clk_cfg_pvt",	"clk_mcu_apb",	CLK_CFG_PVT,	13 },
	{ "clk_cfg_adc",	"clk_mcu_apb",	CLK_CFG_ADC,	14 },
	{ "clk_cfg_pwm",	"clk_mcu_apb",	CLK_CFG_PWM,	15 },
	{ "clk_bus_can0",	"clk_mcu_axi",	CLK_BUS_CAN0,	16 },
	{ "clk_bus_can1",	"clk_mcu_axi",	CLK_BUS_CAN1,	17 },
	{ "clk_cfg_pdm",	"clk_mcu_apb",	CLK_CFG_PDM,	18 },
};

static const struct sl_sysmgr_clk_desc sl261x_sysmgr_clks[] = {
	{ "clk_ser_uart1",	"clk_mcu_pll",	CLK_SER_UART1,	0x604,	4 },
	{ "clk_ser_uart2",	"clk_mcu_pll",	CLK_SER_UART2,	0x608,	4 },
	{ "clk_ser_uart3",	"clk_mcu_pll",	CLK_SER_UART3,	0x60c,	4 },
	{ "clk_deb_gpio",	"clk_mcu_pll",	CLK_DEB_GPIO,	0x610,	4 },
	{ "clk_ser_i2cm0",	"clk_mcu_pll",	CLK_SER_I2CM0,	0x614,	4 },
	{ "clk_ser_i2cm1",	"clk_mcu_pll",	CLK_SER_I2CM1,	0x618,	4 },
	{ "clk_ser_spim",	"clk_mcu_pll",	CLK_SER_SPIM,	0x61c,	4 },
	{ "clk_ser_spis",	"clk_mcu_pll",	CLK_SER_SPIS,	0x620,	4 },
	{ "clk_ser_i3c",	"clk_mcu_pll",	CLK_SER_I3C,	0x624,	4 },
	{ "clk_ser_xspi",	"clk_mcu_io_pll", CLK_SER_XSPI,	0x628,	4 },
	{ "clk_hs_pvt",		"clk_mcu_pll",	CLK_HS_PVT,	0x62c,	4 },
	{ "clk_core_adc",	"clk_mcu_pll",	CLK_CORE_ADC,	0x630,	8 },
	{ "clk_ser_pwm",	"clk_mcu_pll",	CLK_SER_PWM,	0x634,	4 },
	{ "clk_ser_can0",	"clk_mcu_pll",	CLK_SER_CAN0,	0x638,	4 },
	{ "clk_ser_can1",	"clk_mcu_pll",	CLK_SER_CAN1,	0x63c,	4 },
	{ "clk_ser_pdm",	"clk_mcu_pll",	CLK_SER_PDM,	0x640,	8 },
	{ "clk_ref_calib",	"clk_mcu_pll",	CLK_REF_CALIB,	0x644,	8 },
};

static int sl261x_sysmgr_clk_setup(struct platform_device *pdev)
{
	return sl_sysmgr_clk_setup(pdev, sl261x_sysmgr_fixeds, ARRAY_SIZE(sl261x_sysmgr_fixeds),
				   sl261x_sysmgr_gates, ARRAY_SIZE(sl261x_sysmgr_gates),
				   CLK_GATE_CTRL0,
				   sl261x_sysmgr_clks, ARRAY_SIZE(sl261x_sysmgr_clks));
}

static const struct of_device_id sl261x_clks_match_table[] = {
	{
		.compatible = "syna,sl261x-soc-clk",
		.data = sl261x_soc_clk_setup,
	},
	{
		.compatible = "syna,sl261x-cpu-clk",
		.data = sl261x_cpu_clk_setup,
	},
	{
		.compatible = "syna,sl261x-aio-clk",
		.data = sl261x_aio_clk_setup,
	},
	{
		.compatible = "syna,sl261x-system-clk",
		.data = sl261x_sysmgr_clk_setup,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, sl261x_clks_match_table);

static int sl261x_clks_probe(struct platform_device *pdev)
{
	int (*clk_setup)(struct platform_device *pdev);
	int ret;

	clk_setup = of_device_get_match_data(&pdev->dev);
	if (!clk_setup)
		return -ENODEV;

	ret = clk_setup(pdev);
	if (ret)
		return ret;

	return 0;
}

static struct platform_driver sl261x_clks_driver = {
	.probe		= sl261x_clks_probe,
	.driver		= {
		.name	= "syna-sl261x-clks",
		.of_match_table = sl261x_clks_match_table,
	},
};
module_platform_driver(sl261x_clks_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jisheng Zhang <jszhang@kernel.org>");
MODULE_DESCRIPTION("Synaptics sl261x clks Driver");
