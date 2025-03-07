// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 MediaTek Inc.
 * Author: Guangjie Song <guangjie.song@mediatek.com>
 */
#include <dt-bindings/clock/mt8196-clk.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "clk-gate.h"
#include "clk-mtk.h"

static const struct mtk_gate_regs perao0_cg_regs = {
	.set_ofs = 0x24,
	.clr_ofs = 0x28,
	.sta_ofs = 0x10,
};

static const struct mtk_gate_regs perao1_cg_regs = {
	.set_ofs = 0x2c,
	.clr_ofs = 0x30,
	.sta_ofs = 0x14,
};

static const struct mtk_gate_regs perao1_vote_regs = {
	.set_ofs = 0x0008,
	.clr_ofs = 0x000c,
	.sta_ofs = 0x2c04,
};

static const struct mtk_gate_regs perao2_cg_regs = {
	.set_ofs = 0x34,
	.clr_ofs = 0x38,
	.sta_ofs = 0x18,
};

#define GATE_PERAO0(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &perao0_cg_regs,		\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}

#define GATE_PERAO0_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_PERAO1(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &perao1_cg_regs,		\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}

#define GATE_PERAO1_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_PERAO1(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "vote-regmap",		\
		.regs = &perao1_cg_regs,		\
		.vote_regs = &perao1_vote_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote,		\
		.dma_ops = &mtk_clk_gate_ops_setclr,	\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE,		\
	}

#define GATE_PERAO2(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &perao2_cg_regs,		\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}

#define GATE_PERAO2_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

static const struct mtk_gate perao_clks[] = {
	/* PERAO0 */
	GATE_PERAO0(CLK_PERAO_UART0_BCLK, "perao_uart0_bclk", "ck_uart_ck", 0),
	GATE_PERAO0_V(CLK_PERAO_UART0_BCLK_UART, "perao_uart0_bclk_uart", "perao_uart0_bclk"),
	GATE_PERAO0(CLK_PERAO_UART1_BCLK, "perao_uart1_bclk", "ck_uart_ck", 1),
	GATE_PERAO0_V(CLK_PERAO_UART1_BCLK_UART, "perao_uart1_bclk_uart", "perao_uart1_bclk"),
	GATE_PERAO0(CLK_PERAO_UART2_BCLK, "perao_uart2_bclk", "ck_uart_ck", 2),
	GATE_PERAO0_V(CLK_PERAO_UART2_BCLK_UART, "perao_uart2_bclk_uart", "perao_uart2_bclk"),
	GATE_PERAO0(CLK_PERAO_UART3_BCLK, "perao_uart3_bclk", "ck_uart_ck", 3),
	GATE_PERAO0_V(CLK_PERAO_UART3_BCLK_UART, "perao_uart3_bclk_uart", "perao_uart3_bclk"),
	GATE_PERAO0(CLK_PERAO_UART4_BCLK, "perao_uart4_bclk", "ck_uart_ck", 4),
	GATE_PERAO0_V(CLK_PERAO_UART4_BCLK_UART, "perao_uart4_bclk_uart", "perao_uart4_bclk"),
	GATE_PERAO0(CLK_PERAO_UART5_BCLK, "perao_uart5_bclk", "ck_uart_ck", 5),
	GATE_PERAO0_V(CLK_PERAO_UART5_BCLK_UART, "perao_uart5_bclk_uart", "perao_uart5_bclk"),
	GATE_PERAO0(CLK_PERAO_PWM_X16W_HCLK, "perao_pwm_x16w", "ck_p_axi_ck", 12),
	GATE_PERAO0_V(CLK_PERAO_PWM_X16W_HCLK_PWM, "perao_pwm_x16w_pwm", "perao_pwm_x16w"),
	GATE_PERAO0(CLK_PERAO_PWM_X16W_BCLK, "perao_pwm_x16w_bclk", "ck_pwm_ck", 13),
	GATE_PERAO0_V(CLK_PERAO_PWM_X16W_BCLK_PWM, "perao_pwm_x16w_bclk_pwm",
		      "perao_pwm_x16w_bclk"),
	GATE_PERAO0(CLK_PERAO_PWM_PWM_BCLK0, "perao_pwm_pwm_bclk0", "ck_pwm_ck", 14),
	GATE_PERAO0_V(CLK_PERAO_PWM_PWM_BCLK0_PWM, "perao_pwm_pwm_bclk0_pwm",
		      "perao_pwm_pwm_bclk0"),
	GATE_PERAO0(CLK_PERAO_PWM_PWM_BCLK1, "perao_pwm_pwm_bclk1", "ck_pwm_ck", 15),
	GATE_PERAO0_V(CLK_PERAO_PWM_PWM_BCLK1_PWM, "perao_pwm_pwm_bclk1_pwm",
		      "perao_pwm_pwm_bclk1"),
	GATE_PERAO0(CLK_PERAO_PWM_PWM_BCLK2, "perao_pwm_pwm_bclk2", "ck_pwm_ck", 16),
	GATE_PERAO0_V(CLK_PERAO_PWM_PWM_BCLK2_PWM, "perao_pwm_pwm_bclk2_pwm",
		      "perao_pwm_pwm_bclk2"),
	GATE_PERAO0(CLK_PERAO_PWM_PWM_BCLK3, "perao_pwm_pwm_bclk3", "ck_pwm_ck", 17),
	GATE_PERAO0_V(CLK_PERAO_PWM_PWM_BCLK3_PWM, "perao_pwm_pwm_bclk3_pwm",
		      "perao_pwm_pwm_bclk3"),
	/* PERAO1 */
	GATE_VOTE_PERAO1(CLK_PERAO_SPI0_BCLK, "perao_spi0_bclk", "ck_spi0_b_ck", 0),
	GATE_PERAO1_V(CLK_PERAO_SPI0_BCLK_SPI, "perao_spi0_bclk_spi", "perao_spi0_bclk"),
	GATE_VOTE_PERAO1(CLK_PERAO_SPI1_BCLK, "perao_spi1_bclk", "ck_spi1_b_ck", 2),
	GATE_PERAO1_V(CLK_PERAO_SPI1_BCLK_SPI, "perao_spi1_bclk_spi", "perao_spi1_bclk"),
	GATE_VOTE_PERAO1(CLK_PERAO_SPI2_BCLK, "perao_spi2_bclk", "ck_spi2_b_ck", 3),
	GATE_PERAO1_V(CLK_PERAO_SPI2_BCLK_SPI, "perao_spi2_bclk_spi", "perao_spi2_bclk"),
	GATE_VOTE_PERAO1(CLK_PERAO_SPI3_BCLK, "perao_spi3_bclk", "ck_spi3_b_ck", 4),
	GATE_PERAO1_V(CLK_PERAO_SPI3_BCLK_SPI, "perao_spi3_bclk_spi", "perao_spi3_bclk"),
	GATE_VOTE_PERAO1(CLK_PERAO_SPI4_BCLK, "perao_spi4_bclk", "ck_spi4_b_ck", 5),
	GATE_PERAO1_V(CLK_PERAO_SPI4_BCLK_SPI, "perao_spi4_bclk_spi", "perao_spi4_bclk"),
	GATE_VOTE_PERAO1(CLK_PERAO_SPI5_BCLK, "perao_spi5_bclk", "ck_spi5_b_ck", 6),
	GATE_PERAO1_V(CLK_PERAO_SPI5_BCLK_SPI, "perao_spi5_bclk_spi", "perao_spi5_bclk"),
	GATE_VOTE_PERAO1(CLK_PERAO_SPI6_BCLK, "perao_spi6_bclk", "ck_spi6_b_ck", 7),
	GATE_PERAO1_V(CLK_PERAO_SPI6_BCLK_SPI, "perao_spi6_bclk_spi", "perao_spi6_bclk"),
	GATE_VOTE_PERAO1(CLK_PERAO_SPI7_BCLK, "perao_spi7_bclk", "ck_spi7_b_ck", 8),
	GATE_PERAO1_V(CLK_PERAO_SPI7_BCLK_SPI, "perao_spi7_bclk_spi", "perao_spi7_bclk"),
	GATE_PERAO1(CLK_PERAO_FLASHIF_FLASH, "perao_flashif_flash", "ck_sflash_ck", 18),
	GATE_PERAO1_V(CLK_PERAO_FLASHIF_FLASH_FLASHIF, "perao_flashif_flash_flashif",
		      "perao_flashif_flash"),
	GATE_PERAO1(CLK_PERAO_FLASHIF_27M, "perao_flashif_27m", "ck_sflash_ck", 19),
	GATE_PERAO1_V(CLK_PERAO_FLASHIF_27M_FLASHIF, "perao_flashif_27m_flashif",
		      "perao_flashif_27m"),
	GATE_PERAO1(CLK_PERAO_FLASHIF_DRAM, "perao_flashif_dram", "ck_p_axi_ck", 20),
	GATE_PERAO1_V(CLK_PERAO_FLASHIF_DRAM_FLASHIF, "perao_flashif_dram_flashif",
		      "perao_flashif_dram"),
	GATE_PERAO1(CLK_PERAO_FLASHIF_AXI, "perao_flashif_axi", "ck_p_axi_ck", 21),
	GATE_PERAO1_V(CLK_PERAO_FLASHIF_AXI_FLASHIF, "perao_flashif_axi_flashif",
		      "perao_flashif_axi"),
	GATE_PERAO1(CLK_PERAO_FLASHIF_BCLK, "perao_flashif_bclk", "ck_p_axi_ck", 22),
	GATE_PERAO1_V(CLK_PERAO_FLASHIF_BCLK_FLASHIF, "perao_flashif_bclk_flashif",
		      "perao_flashif_bclk"),
	GATE_PERAO1(CLK_PERAO_AP_DMA_X32W_BCLK, "perao_ap_dma_x32w_bclk", "ck_p_axi_ck", 26),
	GATE_PERAO1_V(CLK_PERAO_AP_DMA_X32W_BCLK_UART, "perao_ap_dma_x32w_bclk_uart",
		      "perao_ap_dma_x32w_bclk"),
	GATE_PERAO1_V(CLK_PERAO_AP_DMA_X32W_BCLK_I2C, "perao_ap_dma_x32w_bclk_i2c",
		      "perao_ap_dma_x32w_bclk"),
	/* PERAO2 */
	GATE_PERAO2(CLK_PERAO_MSDC1_MSDC_SRC, "perao_msdc1_msdc_src", "ck_msdc30_1_ck", 1),
	GATE_PERAO2_V(CLK_PERAO_MSDC1_MSDC_SRC_MSDC1, "perao_msdc1_msdc_src_msdc1",
		      "perao_msdc1_msdc_src"),
	GATE_PERAO2(CLK_PERAO_MSDC1_HCLK, "perao_msdc1", "ck_msdc30_1_ck", 2),
	GATE_PERAO2_V(CLK_PERAO_MSDC1_HCLK_MSDC1, "perao_msdc1_msdc1", "perao_msdc1"),
	GATE_PERAO2(CLK_PERAO_MSDC1_AXI, "perao_msdc1_axi", "ck_p_axi_ck", 3),
	GATE_PERAO2_V(CLK_PERAO_MSDC1_AXI_MSDC1, "perao_msdc1_axi_msdc1", "perao_msdc1_axi"),
	GATE_PERAO2(CLK_PERAO_MSDC1_HCLK_WRAP, "perao_msdc1_h_wrap", "ck_p_axi_ck", 4),
	GATE_PERAO2_V(CLK_PERAO_MSDC1_HCLK_WRAP_MSDC1, "perao_msdc1_h_wrap_msdc1",
		      "perao_msdc1_h_wrap"),
	GATE_PERAO2(CLK_PERAO_MSDC2_MSDC_SRC, "perao_msdc2_msdc_src", "ck_msdc30_2_ck", 10),
	GATE_PERAO2_V(CLK_PERAO_MSDC2_MSDC_SRC_MSDC2, "perao_msdc2_msdc_src_msdc2",
		      "perao_msdc2_msdc_src"),
	GATE_PERAO2(CLK_PERAO_MSDC2_HCLK, "perao_msdc2", "ck_msdc30_2_ck", 11),
	GATE_PERAO2_V(CLK_PERAO_MSDC2_HCLK_MSDC2, "perao_msdc2_msdc2", "perao_msdc2"),
	GATE_PERAO2(CLK_PERAO_MSDC2_AXI, "perao_msdc2_axi", "ck_p_axi_ck", 12),
	GATE_PERAO2_V(CLK_PERAO_MSDC2_AXI_MSDC2, "perao_msdc2_axi_msdc2", "perao_msdc2_axi"),
	GATE_PERAO2(CLK_PERAO_MSDC2_HCLK_WRAP, "perao_msdc2_h_wrap", "ck_p_axi_ck", 13),
	GATE_PERAO2_V(CLK_PERAO_MSDC2_HCLK_WRAP_MSDC2, "perao_msdc2_h_wrap_msdc2",
		      "perao_msdc2_h_wrap"),
};

static const struct mtk_clk_desc perao_mcd = {
	.clks = perao_clks,
	.num_clks = ARRAY_SIZE(perao_clks),
};

static const struct of_device_id of_match_clk_mt8196_peri_ao[] = {
	{ .compatible = "mediatek,mt8196-pericfg_ao", .data = &perao_mcd, },
	{ /* sentinel */ }
};

static struct platform_driver clk_mt8196_peri_ao_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt8196-peri_ao",
		.of_match_table = of_match_clk_mt8196_peri_ao,
	},
};

module_platform_driver(clk_mt8196_peri_ao_drv);
MODULE_LICENSE("GPL");
