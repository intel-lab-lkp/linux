// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 Collabora Ltd
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#include <dt-bindings/clock/mediatek,mt6893-clk.h>
#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-gate.h"
#include "clk-mtk.h"

static const struct mtk_gate_regs infra_ao0_cg_regs = {
	.set_ofs = 0x80,
	.clr_ofs = 0x84,
	.sta_ofs = 0x90,
};

static const struct mtk_gate_regs infra_ao1_cg_regs = {
	.set_ofs = 0x88,
	.clr_ofs = 0x8c,
	.sta_ofs = 0x94,
};

static const struct mtk_gate_regs infra_ao2_cg_regs = {
	.set_ofs = 0xd0,
	.clr_ofs = 0xd4,
	.sta_ofs = 0xd8,
};

static const struct mtk_gate_regs infra_ao3_cg_regs = {
	.set_ofs = 0xa4,
	.clr_ofs = 0xa8,
	.sta_ofs = 0xac,
};

static const struct mtk_gate_regs infra_ao4_cg_regs = {
	.set_ofs = 0xc0,
	.clr_ofs = 0xc4,
	.sta_ofs = 0xc8,
};

#define GATE_INFRA_AO0_FLAGS(_id, _name, _parent, _shift, _flag)                \
	GATE_MTK_FLAGS(_id, _name, _parent, &infra_ao0_cg_regs, _shift, \
		&mtk_clk_gate_ops_setclr, _flag)

#define GATE_INFRA_AO0(_id, _name, _parent, _shift)	\
	GATE_INFRA_AO0_FLAGS(_id, _name, _parent, _shift, 0)

#define GATE_INFRA_AO1_FLAGS(_id, _name, _parent, _shift, _flag)		\
	GATE_MTK_FLAGS(_id, _name, _parent, &infra_ao1_cg_regs, _shift,	\
		&mtk_clk_gate_ops_setclr, _flag)

#define GATE_INFRA_AO1(_id, _name, _parent, _shift)	\
	GATE_INFRA_AO1_FLAGS(_id, _name, _parent, _shift, 0)

#define GATE_INFRA_AO2(_id, _name, _parent, _shift)			\
	GATE_MTK(_id, _name, _parent, &infra_ao2_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

#define GATE_INFRA_AO2_FLAGS(_id, _name, _parent, _shift, _flag)		\
	GATE_MTK_FLAGS(_id, _name, _parent, &infra_ao2_cg_regs, _shift,	\
		&mtk_clk_gate_ops_setclr, _flag)

#define GATE_INFRA_AO3_FLAGS(_id, _name, _parent, _shift, _flag)		\
	GATE_MTK_FLAGS(_id, _name, _parent, &infra_ao3_cg_regs, _shift,	\
		&mtk_clk_gate_ops_setclr, _flag)

#define GATE_INFRA_AO3(_id, _name, _parent, _shift)	\
	GATE_INFRA_AO3_FLAGS(_id, _name, _parent, _shift, 0)

#define GATE_INFRA_AO4_FLAGS(_id, _name, _parent, _shift, _flag)		\
	GATE_MTK_FLAGS(_id, _name, _parent, &infra_ao4_cg_regs, _shift,	\
		&mtk_clk_gate_ops_setclr, _flag)

#define GATE_INFRA_AO4(_id, _name, _parent, _shift)	\
	GATE_INFRA_AO4_FLAGS(_id, _name, _parent, _shift, 0)

static const struct mtk_gate infra_ao_clks[] = {
	/* INFRA_AO0 */
	GATE_INFRA_AO0(CLK_INFRA_AO_PMIC_TMR, "infra_ao_pmic_tmr", "pwrap_ulposc_sel", 0),
	GATE_INFRA_AO0(CLK_INFRA_AO_PMIC_AP, "infra_ao_pmic_ap", "pwrap_ulposc_sel", 1),
	GATE_INFRA_AO0(CLK_INFRA_AO_GCE, "infra_ao_gce", "axi_sel", 8),
	GATE_INFRA_AO0(CLK_INFRA_AO_GCE2, "infra_ao_gce2", "axi_sel", 9),
	GATE_INFRA_AO0(CLK_INFRA_AO_THERM, "infra_ao_therm", "axi_sel", 10),
	GATE_INFRA_AO0(CLK_INFRA_AO_I2C0, "infra_ao_i2c0", "i2c_sel", 11),
	GATE_INFRA_AO0(CLK_INFRA_AO_I2C1, "infra_ao_i2c1", "i2c_sel", 12),
	GATE_INFRA_AO0(CLK_INFRA_AO_I2C2, "infra_ao_i2c2", "i2c_sel", 13),
	GATE_INFRA_AO0(CLK_INFRA_AO_I2C3, "infra_ao_i2c3", "i2c_sel", 14),
	GATE_INFRA_AO0(CLK_INFRA_AO_PWM_HCLK, "infra_ao_pwm_hclk", "axi_sel", 15),
	GATE_INFRA_AO0(CLK_INFRA_AO_PWM1, "infra_ao_pwm1", "pwm_sel", 16),
	GATE_INFRA_AO0(CLK_INFRA_AO_PWM2, "infra_ao_pwm2", "pwm_sel", 17),
	GATE_INFRA_AO0(CLK_INFRA_AO_PWM3, "infra_ao_pwm3", "pwm_sel", 18),
	GATE_INFRA_AO0(CLK_INFRA_AO_PWM4, "infra_ao_pwm4", "pwm_sel", 19),
	GATE_INFRA_AO0(CLK_INFRA_AO_PWM, "infra_ao_pwm", "pwm_sel", 21),
	GATE_INFRA_AO0(CLK_INFRA_AO_UART0, "infra_ao_uart0", "uart_sel", 22),
	GATE_INFRA_AO0(CLK_INFRA_AO_UART1, "infra_ao_uart1", "uart_sel", 23),
	GATE_INFRA_AO0(CLK_INFRA_AO_UART2, "infra_ao_uart2", "uart_sel", 24),
	GATE_INFRA_AO0(CLK_INFRA_AO_UART3, "infra_ao_uart3", "uart_sel", 25),
	GATE_INFRA_AO0(CLK_INFRA_AO_GCE_26M, "infra_ao_gce_26m", "axi_sel", 27),
	GATE_INFRA_AO0(CLK_INFRA_AO_CQ_DMA_FPC, "infra_ao_dma", "axi_sel", 28),
	GATE_INFRA_AO0(CLK_INFRA_AO_BTIF, "infra_ao_btif", "axi_sel", 31),
	/* INFRA_AO1 */
	GATE_INFRA_AO1(CLK_INFRA_AO_SPI0, "infra_ao_spi0", "spi_sel", 1),
	GATE_INFRA_AO1(CLK_INFRA_AO_MSDC0, "infra_ao_msdc0", "axi_sel", 2),
	GATE_INFRA_AO1(CLK_INFRA_AO_MSDC1, "infra_ao_msdc1", "axi_sel", 4),
	GATE_INFRA_AO1(CLK_INFRA_AO_MSDC0_SRC, "infra_ao_msdc0_clk", "msdc50_0_sel", 6),
	GATE_INFRA_AO1(CLK_INFRA_AO_AUXADC, "infra_ao_auxadc", "clk26m", 10),
	GATE_INFRA_AO1(CLK_INFRA_AO_CPUM, "infra_ao_cpum", "axi_sel", 11),
	GATE_INFRA_AO1(CLK_INFRA_AO_CCIF1_AP, "infra_ao_ccif1_ap", "axi_sel", 12),
	GATE_INFRA_AO1(CLK_INFRA_AO_CCIF1_MD, "infra_ao_ccif1_md", "axi_sel", 13),
	GATE_INFRA_AO1(CLK_INFRA_AO_MSDC1_SRC, "infra_ao_msdc1_clk", "msdc30_1_sel", 16),
	GATE_INFRA_AO1(CLK_INFRA_AO_AP_DMA_PS, "infra_ao_ap_dma_ps", "axi_sel", 18),
	GATE_INFRA_AO1(CLK_INFRA_AO_DEVICE_APC, "infra_ao_dapc", "axi_sel", 20),
	GATE_INFRA_AO1(CLK_INFRA_AO_CCIF_AP, "infra_ao_ccif_ap", "axi_sel", 23),
	GATE_INFRA_AO1(CLK_INFRA_AO_AUDIO, "infra_ao_audio", "axi_sel", 25),
	GATE_INFRA_AO1(CLK_INFRA_AO_CCIF_MD, "infra_ao_ccif_md", "axi_sel", 26),
	GATE_INFRA_AO1(CLK_INFRA_AO_DXCC_SEC_CORE, "infra_ao_secore", "dxcc_sel", 27),
	/* INFRA_AO2 */
	GATE_INFRA_AO2(CLK_INFRA_AO_APDMA, "infra_ao_apdma", "infra_ao_ap_dma_ps", 31),
	/* INFRA_AO3 */
	GATE_INFRA_AO3(CLK_INFRA_AO_SSUSB, "infra_ao_ssusb", "usb_sel", 1),
	GATE_INFRA_AO3(CLK_INFRA_AO_DISP_PWM, "infra_ao_disp_pwm", "disp_pwm_sel", 2),
	GATE_INFRA_AO3(CLK_INFRA_AO_DPMAIF, "infra_ao_dpmaif_ck", "axi_sel", 3),
	GATE_INFRA_AO3(CLK_INFRA_AO_AUDIO_26M_BCLK, "infra_ao_audio26m", "clk26m", 4),
	GATE_INFRA_AO3(CLK_INFRA_AO_SPI1, "infra_ao_spi1", "spi_sel", 6),
	GATE_INFRA_AO3(CLK_INFRA_AO_I2C4, "infra_ao_i2c4", "i2c_sel", 7),
	GATE_INFRA_AO3(CLK_INFRA_AO_SPI2, "infra_ao_spi2", "spi_sel", 9),
	GATE_INFRA_AO3(CLK_INFRA_AO_SPI3, "infra_ao_spi3", "spi_sel", 10),
	GATE_INFRA_AO3(CLK_INFRA_AO_UNIPRO_SYSCLK, "infra_ao_unipro_sysclk", "ufs_sel", 11),
	GATE_INFRA_AO3(CLK_INFRA_AO_UFS_MP_SAP_BCLK, "infra_ao_ufs_bclk", "clk26m", 13),
	GATE_INFRA_AO3(CLK_INFRA_AO_I2C5, "infra_ao_i2c5", "i2c_sel", 18),
	GATE_INFRA_AO3(CLK_INFRA_AO_I2C5_ARBITER, "infra_ao_i2c5a", "i2c_sel", 19),
	GATE_INFRA_AO3(CLK_INFRA_AO_I2C5_IMM, "infra_ao_i2c5_imm", "i2c_sel", 20),
	GATE_INFRA_AO3(CLK_INFRA_AO_I2C1_ARBITER, "infra_ao_i2c1a", "i2c_sel", 21),
	GATE_INFRA_AO3(CLK_INFRA_AO_I2C1_IMM, "infra_ao_i2c1_imm", "i2c_sel", 22),
	GATE_INFRA_AO3(CLK_INFRA_AO_I2C2_ARBITER, "infra_ao_i2c2a", "i2c_sel", 23),
	GATE_INFRA_AO3(CLK_INFRA_AO_I2C2_IMM, "infra_ao_i2c2_imm", "i2c_sel", 24),
	GATE_INFRA_AO3(CLK_INFRA_AO_SPI4, "infra_ao_spi4", "spi_sel", 25),
	GATE_INFRA_AO3(CLK_INFRA_AO_SPI5, "infra_ao_spi5", "spi_sel", 26),
	GATE_INFRA_AO3(CLK_INFRA_AO_CQ_DMA, "infra_ao_cq_dma", "axi_sel", 27),
	GATE_INFRA_AO3(CLK_INFRA_AO_UFS, "infra_ao_ufs", "ufs_sel", 28),
	GATE_INFRA_AO3(CLK_INFRA_AO_AES, "infra_ao_aes", "aes_ufsfde_sel", 29),
	GATE_INFRA_AO3(CLK_INFRA_AO_SSUSB_XHCI, "infra_ao_ssusb_xhci", "ssusb_xhci_sel", 31),
	/* INFRA_AO4 */
	GATE_INFRA_AO4(CLK_INFRA_AO_MSDC0_SELF, "infra_ao_msdc0sf", "msdc50_0_sel", 0),
	GATE_INFRA_AO4(CLK_INFRA_AO_MSDC1_SELF, "infra_ao_msdc1sf", "msdc50_0_sel", 1),
	GATE_INFRA_AO4(CLK_INFRA_AO_MSDC2_SELF, "infra_ao_msdc2sf", "msdc50_0_sel", 2),
	GATE_INFRA_AO4(CLK_INFRA_AO_I2C6, "infra_ao_i2c6", "i2c_sel", 6),
	GATE_INFRA_AO4(CLK_INFRA_AO_AP_MSDC0, "infra_ao_ap_msdc0", "axi_sel", 7),
	GATE_INFRA_AO4(CLK_INFRA_AO_MD_MSDC0, "infra_ao_md_msdc0", "axi_sel", 8),
	GATE_INFRA_AO4(CLK_INFRA_AO_I2C7, "infra_ao_i2c7", "i2c_sel", 22),
	GATE_INFRA_AO4(CLK_INFRA_AO_I2C8, "infra_ao_i2c8", "i2c_sel", 23),
	GATE_INFRA_AO4(CLK_INFRA_AO_FBIST2FPC, "infra_ao_fbist2fpc", "msdc50_0_sel", 24),
	GATE_INFRA_AO4_FLAGS(CLK_INFRA_AO_DEVICE_DAPC_SYNC, "infra_ao_dapc_sync", "axi_sel", 25,
			     CLK_IS_CRITICAL),
	GATE_INFRA_AO4(CLK_INFRA_AO_DPMAIF_MAIN, "infra_ao_dpmaif_main", "dpmaif_main_sel", 26),
	GATE_INFRA_AO4(CLK_INFRA_AO_SPI6_CK, "infra_ao_spi6_ck", "spi_sel", 30),
	GATE_INFRA_AO4(CLK_INFRA_AO_SPI7_CK, "infra_ao_spi7_ck", "spi_sel", 31),
};

static const struct mtk_clk_desc infra_ao_desc = {
	.clks = infra_ao_clks,
	.num_clks = ARRAY_SIZE(infra_ao_clks),
};

static const struct of_device_id of_match_clk_mt6893_infra_ao[] = {
	{ .compatible = "mediatek,mt6893-infracfg-ao", .data = &infra_ao_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6893_infra_ao);

static struct platform_driver clk_mt6893_infra_ao_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6893-infra_ao",
		.of_match_table = of_match_clk_mt6893_infra_ao,
	},
};
module_platform_driver(clk_mt6893_infra_ao_drv);

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_DESCRIPTION("MediaTek MT6893 infracfg clocks driver");
MODULE_LICENSE("GPL");
