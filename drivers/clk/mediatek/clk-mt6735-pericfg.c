// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Yassine Oudjana <y.oudjana@protonmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-gate.h"
#include "clk-mtk.h"

#include <dt-bindings/clock/mediatek,mt6735-pericfg.h>

#define PERI_GLOBALCON_RST0		0x00
#define PERI_GLOBALCON_RST1		0x04
#define PERI_GLOBALCON_PDN0_SET		0x08
#define PERI_GLOBALCON_PDN0_CLR		0x10
#define	PERI_GLOBALCON_PDN0_STA		0x18

static struct mtk_gate_regs peri_cg_regs = {
	.set_ofs = PERI_GLOBALCON_PDN0_SET,
	.clr_ofs = PERI_GLOBALCON_PDN0_CLR,
	.sta_ofs = PERI_GLOBALCON_PDN0_STA,
};

static const struct mtk_gate pericfg_gates[] = {
	GATE_MTK(CLK_DISP_PWM, "disp_pwm", "disppwm_sel", &peri_cg_regs, 0, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_THERM, "therm", "axi_sel", &peri_cg_regs, 1, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_PWM1, "pwm1", "axi_sel", &peri_cg_regs, 2, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_PWM2, "pwm2", "axi_sel", &peri_cg_regs, 3, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_PWM3, "pwm3", "axi_sel", &peri_cg_regs, 4, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_PWM4, "pwm4", "axi_sel", &peri_cg_regs, 5, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_PWM5, "pwm5", "axi_sel", &peri_cg_regs, 6, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_PWM6, "pwm6", "axi_sel", &peri_cg_regs, 7, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_PWM7, "pwm7", "axi_sel", &peri_cg_regs, 8, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_PWM, "pwm", "axi_sel", &peri_cg_regs, 9, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_USB0, "usb0", "usb20_sel", &peri_cg_regs, 10, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_IRDA, "irda", "irda_sel", &peri_cg_regs, 11, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_APDMA, "apdma", "axi_sel", &peri_cg_regs, 12, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_MSDC30_0, "msdc30_0", "msdc30_0_sel", &peri_cg_regs, 13, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_MSDC30_1, "msdc30_1", "msdc30_1_sel", &peri_cg_regs, 14, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_MSDC30_2, "msdc30_2", "msdc30_2_sel", &peri_cg_regs, 15, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_MSDC30_3, "msdc30_3", "msdc30_3_sel", &peri_cg_regs, 16, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_UART0, "uart0", "uart_sel", &peri_cg_regs, 17, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_UART1, "uart1", "uart_sel", &peri_cg_regs, 18, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_UART2, "uart2", "uart_sel", &peri_cg_regs, 19, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_UART3, "uart3", "uart_sel", &peri_cg_regs, 20, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_UART4, "uart4", "uart_sel", &peri_cg_regs, 21, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_BTIF, "btif", "axi_sel", &peri_cg_regs, 22, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_I2C0, "i2c0", "axi_sel", &peri_cg_regs, 23, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_I2C1, "i2c1", "axi_sel", &peri_cg_regs, 24, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_I2C2, "i2c2", "axi_sel", &peri_cg_regs, 25, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_I2C3, "i2c3", "axi_sel", &peri_cg_regs, 26, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_AUXADC, "auxadc", "axi_sel", &peri_cg_regs, 27, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_SPI0, "spi0", "spi_sel", &peri_cg_regs, 28, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_IRTX, "irtx", "irtx_sel", &peri_cg_regs, 29, &mtk_clk_gate_ops_setclr)
};

static u16 pericfg_rst_ofs[] = { PERI_GLOBALCON_RST0, PERI_GLOBALCON_RST1 };

static const struct mtk_clk_rst_desc pericfg_resets = {
	.version = MTK_RST_SIMPLE,
	.rst_bank_ofs = pericfg_rst_ofs,
	.rst_bank_nr = ARRAY_SIZE(pericfg_rst_ofs)
};

static const struct mtk_clk_desc pericfg_clks = {
	.clks = pericfg_gates,
	.num_clks = ARRAY_SIZE(pericfg_gates),

	.rst_desc = &pericfg_resets
};

static const struct of_device_id of_match_mt6735_pericfg[] = {
	{ .compatible = "mediatek,mt6735-pericfg", .data = &pericfg_clks },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_mt6735_pericfg);

static struct platform_driver clk_mt6735_pericfg = {
	.probe = mtk_clk_simple_probe,
	.remove_new = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6735-pericfg",
		.of_match_table = of_match_mt6735_pericfg,
	},
};
module_platform_driver(clk_mt6735_pericfg);

MODULE_AUTHOR("Yassine Oudjana <y.oudjana@protonmail.com>");
MODULE_DESCRIPTION("MediaTek MT6735 pericfg clock driver");
MODULE_LICENSE("GPL");
