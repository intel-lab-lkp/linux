// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 MediaTek Inc.
 * Author: Guangjie Song <guangjie.song@mediatek.com>
 */
#include <dt-bindings/clock/mt8196-clk.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-mux.h"

/* MUX SEL REG */
#define CLK_CFG_UPDATE		0x0004
#define CLK_CFG_UPDATE1		0x0008
#define CLK_CFG_UPDATE2		0x000c
#define CLK_CFG_0		0x0010
#define CLK_CFG_0_SET		0x0014
#define CLK_CFG_0_CLR		0x0018
#define CLK_CFG_1		0x0020
#define CLK_CFG_1_SET		0x0024
#define CLK_CFG_1_CLR		0x0028
#define CLK_CFG_2		0x0030
#define CLK_CFG_2_SET		0x0034
#define CLK_CFG_2_CLR		0x0038
#define CLK_CFG_3		0x0040
#define CLK_CFG_3_SET		0x0044
#define CLK_CFG_3_CLR		0x0048
#define CLK_CFG_4		0x0050
#define CLK_CFG_4_SET		0x0054
#define CLK_CFG_4_CLR		0x0058
#define CLK_CFG_5		0x0060
#define CLK_CFG_5_SET		0x0064
#define CLK_CFG_5_CLR		0x0068
#define CLK_CFG_6		0x0070
#define CLK_CFG_6_SET		0x0074
#define CLK_CFG_6_CLR		0x0078
#define CLK_CFG_7		0x0080
#define CLK_CFG_7_SET		0x0084
#define CLK_CFG_7_CLR		0x0088
#define CLK_CFG_8		0x0090
#define CLK_CFG_8_SET		0x0094
#define CLK_CFG_8_CLR		0x0098
#define CLK_CFG_9		0x00a0
#define CLK_CFG_9_SET		0x00a4
#define CLK_CFG_9_CLR		0x00a8
#define CLK_CFG_10		0x00b0
#define CLK_CFG_10_SET		0x00b4
#define CLK_CFG_10_CLR		0x00b8
#define CLK_CFG_11		0x00c0
#define CLK_CFG_11_SET		0x00c4
#define CLK_CFG_11_CLR		0x00c8
#define CLK_CFG_12		0x00d0
#define CLK_CFG_12_SET		0x00d4
#define CLK_CFG_12_CLR		0x00d8
#define CLK_CFG_13		0x00e0
#define CLK_CFG_13_SET		0x00e4
#define CLK_CFG_13_CLR		0x00e8
#define CLK_CFG_14		0x00f0
#define CLK_CFG_14_SET		0x00f4
#define CLK_CFG_14_CLR		0x00f8
#define CLK_CFG_15		0x0100
#define CLK_CFG_15_SET		0x0104
#define CLK_CFG_15_CLR		0x0108
#define CLK_CFG_16		0x0110
#define CLK_CFG_16_SET		0x0114
#define CLK_CFG_16_CLR		0x0118
#define CLK_CFG_17		0x0120
#define CLK_CFG_17_SET		0x0124
#define CLK_CFG_17_CLR		0x0128
#define CLK_CFG_18		0x0130
#define CLK_CFG_18_SET		0x0134
#define CLK_CFG_18_CLR		0x0138
#define CLK_CFG_19		0x0140
#define CLK_CFG_19_SET		0x0144
#define CLK_CFG_19_CLR		0x0148
#define CLK_AUDDIV_0		0x020c
#define CLK_FENC_STATUS_MON_0	0x0270
#define CLK_FENC_STATUS_MON_1	0x0274
#define CLK_FENC_STATUS_MON_2	0x0278

/* MUX SHIFT */
#define TOP_MUX_AXI_SHIFT			0
#define TOP_MUX_MEM_SUB_SHIFT			1
#define TOP_MUX_IO_NOC_SHIFT			2
#define TOP_MUX_PERI_AXI_SHIFT			3
#define TOP_MUX_UFS_PEXTP0_AXI_SHIFT		4
#define TOP_MUX_PEXTP1_USB_AXI_SHIFT		5
#define TOP_MUX_PERI_FMEM_SUB_SHIFT		6
#define TOP_MUX_UFS_PEXPT0_MEM_SUB_SHIFT	7
#define TOP_MUX_PEXTP1_USB_MEM_SUB_SHIFT	8
#define TOP_MUX_PERI_NOC_SHIFT			9
#define TOP_MUX_EMI_N_SHIFT			10
#define TOP_MUX_EMI_S_SHIFT			11
#define TOP_MUX_AP2CONN_HOST_SHIFT		14
#define TOP_MUX_ATB_SHIFT			15
#define TOP_MUX_CIRQ_SHIFT			16
#define TOP_MUX_PBUS_156M_SHIFT			17
#define TOP_MUX_EFUSE_SHIFT			20
#define TOP_MUX_MCU_L3GIC_SHIFT			21
#define TOP_MUX_MCU_INFRA_SHIFT			22
#define TOP_MUX_DSP_SHIFT			23
#define TOP_MUX_MFG_REF_SHIFT			24
#define TOP_MUX_MFG_EB_SHIFT			26
#define TOP_MUX_UART_SHIFT			27
#define TOP_MUX_SPI0_BCLK_SHIFT			28
#define TOP_MUX_SPI1_BCLK_SHIFT			29
#define TOP_MUX_SPI2_BCLK_SHIFT			30
#define TOP_MUX_SPI3_BCLK_SHIFT			0
#define TOP_MUX_SPI4_BCLK_SHIFT			1
#define TOP_MUX_SPI5_BCLK_SHIFT			2
#define TOP_MUX_SPI6_BCLK_SHIFT			3
#define TOP_MUX_SPI7_BCLK_SHIFT			4
#define TOP_MUX_MSDC30_1_SHIFT			7
#define TOP_MUX_MSDC30_2_SHIFT			8
#define TOP_MUX_DISP_PWM_SHIFT			9
#define TOP_MUX_USB_TOP_1P_SHIFT		10
#define TOP_MUX_SSUSB_XHCI_1P_SHIFT		11
#define TOP_MUX_SSUSB_FMCNT_P1_SHIFT		12
#define TOP_MUX_I2C_PERI_SHIFT			13
#define TOP_MUX_I2C_EAST_SHIFT			14
#define TOP_MUX_I2C_WEST_SHIFT			15
#define TOP_MUX_I2C_NORTH_SHIFT			16
#define TOP_MUX_AES_UFSFDE_SHIFT		17
#define TOP_MUX_UFS_SHIFT			18
#define TOP_MUX_AUD_1_SHIFT			21
#define TOP_MUX_AUD_2_SHIFT			22
#define TOP_MUX_ADSP_SHIFT			23
#define TOP_MUX_ADSP_UARTHUB_BCLK_SHIFT		24
#define TOP_MUX_DPMAIF_MAIN_SHIFT		25
#define TOP_MUX_PWM_SHIFT			26
#define TOP_MUX_MCUPM_SHIFT			27
#define TOP_MUX_SFLASH_SHIFT			28
#define TOP_MUX_IPSEAST_SHIFT			29
#define TOP_MUX_TL_SHIFT			0
#define TOP_MUX_TL_P1_SHIFT			1
#define TOP_MUX_TL_P2_SHIFT			2
#define TOP_MUX_EMI_INTERFACE_546_SHIFT		3
#define TOP_MUX_SDF_SHIFT			4
#define TOP_MUX_UARTHUB_BCLK_SHIFT		5
#define TOP_MUX_DPSW_CMP_26M_SHIFT		6
#define TOP_MUX_SMAPCK_SHIFT			7
#define TOP_MUX_SSR_PKA_SHIFT			8
#define TOP_MUX_SSR_DMA_SHIFT			9
#define TOP_MUX_SSR_KDF_SHIFT			10
#define TOP_MUX_SSR_RNG_SHIFT			11
#define TOP_MUX_SPU0_SHIFT			12
#define TOP_MUX_SPU1_SHIFT			13
#define TOP_MUX_DXCC_SHIFT			14

/* CKSTA REG */
#define CKSTA_REG	0x01c8
#define CKSTA_REG1	0x01cc
#define CKSTA_REG2	0x01d0

/* DIVIDER REG */
#define CLK_AUDDIV_2	0x0214
#define CLK_AUDDIV_3	0x0220
#define CLK_AUDDIV_4	0x0224
#define CLK_AUDDIV_5	0x0228

/* HW Voter REG */
#define VOTE_CG_0_SET	0x0000
#define VOTE_CG_0_CLR	0x0004
#define VOTE_CG_0_DONE	0x2c00
#define VOTE_CG_1_SET	0x0008
#define VOTE_CG_1_CLR	0x000c
#define VOTE_CG_1_DONE	0x2c04
#define VOTE_CG_2_SET	0x0010
#define VOTE_CG_2_CLR	0x0014
#define VOTE_CG_2_DONE	0x2c08
#define VOTE_CG_3_SET	0x0018
#define VOTE_CG_3_CLR	0x001c
#define VOTE_CG_3_DONE	0x2c0c
#define VOTE_CG_4_SET	0x0020
#define VOTE_CG_4_CLR	0x0024
#define VOTE_CG_4_DONE	0x2c10
#define VOTE_CG_5_SET	0x0028
#define VOTE_CG_5_CLR	0x002c
#define VOTE_CG_5_DONE	0x2c14
#define VOTE_CG_6_SET	0x0030
#define VOTE_CG_6_CLR	0x0034
#define VOTE_CG_6_DONE	0x2c18
#define VOTE_CG_7_SET	0x0038
#define VOTE_CG_7_CLR	0x003c
#define VOTE_CG_7_DONE	0x2c1c
#define VOTE_CG_8_SET	0x0040
#define VOTE_CG_8_CLR	0x0044
#define VOTE_CG_8_DONE	0x2c20

static DEFINE_SPINLOCK(mt8196_clk_ck_lock);

static const struct mtk_fixed_factor ck_divs[] = {
	FACTOR(CLK_CK_MAINPLL_D3, "ck_mainpll_d3", "mainpll", 1, 3),
	FACTOR(CLK_CK_MAINPLL_D4, "ck_mainpll_d4", "mainpll", 1, 4),
	FACTOR(CLK_CK_MAINPLL_D4_D2, "ck_mainpll_d4_d2", "mainpll", 1, 8),
	FACTOR(CLK_CK_MAINPLL_D4_D4, "ck_mainpll_d4_d4", "mainpll", 1, 16),
	FACTOR(CLK_CK_MAINPLL_D4_D8, "ck_mainpll_d4_d8", "mainpll", 1, 32),
	FACTOR(CLK_CK_MAINPLL_D5, "ck_mainpll_d5", "mainpll", 1, 5),
	FACTOR(CLK_CK_MAINPLL_D5_D2, "ck_mainpll_d5_d2", "mainpll", 1, 10),
	FACTOR(CLK_CK_MAINPLL_D5_D4, "ck_mainpll_d5_d4", "mainpll", 1, 20),
	FACTOR(CLK_CK_MAINPLL_D5_D8, "ck_mainpll_d5_d8", "mainpll", 1, 40),
	FACTOR(CLK_CK_MAINPLL_D6, "ck_mainpll_d6", "mainpll", 1, 6),
	FACTOR(CLK_CK_MAINPLL_D6_D2, "ck_mainpll_d6_d2", "mainpll", 1, 12),
	FACTOR(CLK_CK_MAINPLL_D7, "ck_mainpll_d7", "mainpll", 1, 7),
	FACTOR(CLK_CK_MAINPLL_D7_D2, "ck_mainpll_d7_d2", "mainpll", 1, 14),
	FACTOR(CLK_CK_MAINPLL_D7_D4, "ck_mainpll_d7_d4", "mainpll", 1, 28),
	FACTOR(CLK_CK_MAINPLL_D7_D8, "ck_mainpll_d7_d8", "mainpll", 1, 56),
	FACTOR(CLK_CK_MAINPLL_D9, "ck_mainpll_d9", "mainpll", 1, 9),
	FACTOR(CLK_CK_UNIVPLL_D4, "ck_univpll_d4", "univpll", 1, 4),
	FACTOR(CLK_CK_UNIVPLL_D4_D2, "ck_univpll_d4_d2", "univpll", 1, 8),
	FACTOR(CLK_CK_UNIVPLL_D4_D4, "ck_univpll_d4_d4", "univpll", 1, 16),
	FACTOR(CLK_CK_UNIVPLL_D4_D8, "ck_univpll_d4_d8", "univpll", 1, 32),
	FACTOR(CLK_CK_UNIVPLL_D5, "ck_univpll_d5", "univpll", 1, 5),
	FACTOR(CLK_CK_UNIVPLL_D5_D2, "ck_univpll_d5_d2", "univpll", 1, 10),
	FACTOR(CLK_CK_UNIVPLL_D5_D4, "ck_univpll_d5_d4", "univpll", 1, 20),
	FACTOR(CLK_CK_UNIVPLL_D6, "ck_univpll_d6", "univpll", 1, 6),
	FACTOR(CLK_CK_UNIVPLL_D6_D2, "ck_univpll_d6_d2", "univpll", 1, 12),
	FACTOR(CLK_CK_UNIVPLL_D6_D4, "ck_univpll_d6_d4", "univpll", 1, 24),
	FACTOR(CLK_CK_UNIVPLL_D6_D8, "ck_univpll_d6_d8", "univpll", 1, 48),
	FACTOR(CLK_CK_UNIVPLL_D6_D16, "ck_univpll_d6_d16", "univpll", 1, 96),
	FACTOR(CLK_CK_UNIVPLL_192M, "ck_univpll_192m", "univpll", 1, 13),
	FACTOR(CLK_CK_UNIVPLL_192M_D4, "ck_univpll_192m_d4", "univpll", 1, 52),
	FACTOR(CLK_CK_UNIVPLL_192M_D8, "ck_univpll_192m_d8", "univpll", 1, 104),
	FACTOR(CLK_CK_UNIVPLL_192M_D16, "ck_univpll_192m_d16", "univpll", 1, 208),
	FACTOR(CLK_CK_UNIVPLL_192M_D32, "ck_univpll_192m_d32", "univpll", 1, 416),
	FACTOR(CLK_CK_UNIVPLL_192M_D10, "ck_univpll_192m_d10", "univpll", 1, 130),
	FACTOR(CLK_CK_APLL1, "ck_apll1_ck", "vlp_apll1", 1, 1),
	FACTOR(CLK_CK_APLL1_D4, "ck_apll1_d4", "vlp_apll1", 1, 4),
	FACTOR(CLK_CK_APLL1_D8, "ck_apll1_d8", "vlp_apll1", 1, 8),
	FACTOR(CLK_CK_APLL2, "ck_apll2_ck", "vlp_apll2", 1, 1),
	FACTOR(CLK_CK_APLL2_D4, "ck_apll2_d4", "vlp_apll2", 1, 4),
	FACTOR(CLK_CK_APLL2_D8, "ck_apll2_d8", "vlp_apll2", 1, 8),
	FACTOR(CLK_CK_ADSPPLL, "ck_adsppll_ck", "adsppll", 1, 1),
	FACTOR(CLK_CK_EMIPLL1, "ck_emipll1_ck", "emipll", 1, 1),
	FACTOR(CLK_CK_TVDPLL1_D2, "ck_tvdpll1_d2", "tvdpll1", 1, 2),
	FACTOR(CLK_CK_MSDCPLL_D2, "ck_msdcpll_d2", "msdcpll", 1, 2),
	FACTOR(CLK_CK_CLKRTC, "ck_clkrtc", "clk32k", 1, 1),
	FACTOR(CLK_CK_TCK_26M_MX9, "ck_tck_26m_mx9_ck", "clk26m", 1, 1),
	FACTOR(CLK_CK_F26M, "ck_f26m_ck", "clk26m", 1, 1),
	FACTOR(CLK_CK_F26M_CK_D2, "ck_f26m_d2", "clk13m", 1, 1),
	FACTOR(CLK_CK_OSC, "ck_osc", "ulposc", 1, 1),
	FACTOR(CLK_CK_OSC_D2, "ck_osc_d2", "ulposc", 1, 2),
	FACTOR(CLK_CK_OSC_D3, "ck_osc_d3", "ulposc", 1, 3),
	FACTOR(CLK_CK_OSC_D4, "ck_osc_d4", "ulposc", 1, 4),
	FACTOR(CLK_CK_OSC_D5, "ck_osc_d5", "ulposc", 1, 5),
	FACTOR(CLK_CK_OSC_D7, "ck_osc_d7", "ulposc", 1, 7),
	FACTOR(CLK_CK_OSC_D8, "ck_osc_d8", "ulposc", 1, 8),
	FACTOR(CLK_CK_OSC_D10, "ck_osc_d10", "ulposc", 1, 10),
	FACTOR(CLK_CK_OSC_D14, "ck_osc_d14", "ulposc", 1, 14),
	FACTOR(CLK_CK_OSC_D20, "ck_osc_d20", "ulposc", 1, 20),
	FACTOR(CLK_CK_OSC_D32, "ck_osc_d32", "ulposc", 1, 32),
	FACTOR(CLK_CK_OSC_D40, "ck_osc_d40", "ulposc", 1, 40),
	FACTOR(CLK_CK_OSC3, "ck_osc3", "ulposc3", 1, 1),
	FACTOR(CLK_CK_P_AXI, "ck_p_axi_ck", "ck_p_axi_sel", 1, 1),
	FACTOR(CLK_CK_PEXTP0_AXI, "ck_pextp0_axi_ck", "ck_pextp0_axi_sel", 1, 1),
	FACTOR(CLK_CK_PEXTP1_USB_AXI, "ck_pextp1_usb_axi_ck", "ck_pextp1_usb_axi_sel", 1, 1),
	FACTOR(CLK_CK_PEXPT0_MEM_SUB, "ck_pexpt0_mem_sub_ck", "ck_pexpt0_mem_sub_sel", 1, 1),
	FACTOR(CLK_CK_PEXTP1_USB_MEM_SUB, "ck_pextp1_usb_mem_sub_ck", "ck_pextp1_usb_mem_sub_sel", 1, 1),
	FACTOR(CLK_CK_UART, "ck_uart_ck", "ck_uart_sel", 1, 1),
	FACTOR(CLK_CK_SPI0_BCLK, "ck_spi0_b_ck", "ck_spi0_b_sel", 1, 1),
	FACTOR(CLK_CK_SPI1_BCLK, "ck_spi1_b_ck", "ck_spi1_b_sel", 1, 1),
	FACTOR(CLK_CK_SPI2_BCLK, "ck_spi2_b_ck", "ck_spi2_b_sel", 1, 1),
	FACTOR(CLK_CK_SPI3_BCLK, "ck_spi3_b_ck", "ck_spi3_b_sel", 1, 1),
	FACTOR(CLK_CK_SPI4_BCLK, "ck_spi4_b_ck", "ck_spi4_b_sel", 1, 1),
	FACTOR(CLK_CK_SPI5_BCLK, "ck_spi5_b_ck", "ck_spi5_b_sel", 1, 1),
	FACTOR(CLK_CK_SPI6_BCLK, "ck_spi6_b_ck", "ck_spi6_b_sel", 1, 1),
	FACTOR(CLK_CK_SPI7_BCLK, "ck_spi7_b_ck", "ck_spi7_b_sel", 1, 1),
	FACTOR(CLK_CK_MSDC30_1, "ck_msdc30_1_ck", "ck_msdc30_1_sel", 1, 1),
	FACTOR(CLK_CK_MSDC30_2, "ck_msdc30_2_ck", "ck_msdc30_2_sel", 1, 1),
	FACTOR(CLK_CK_I2C_PERI, "ck_i2c_p_ck", "ck_i2c_p_sel", 1, 1),
	FACTOR(CLK_CK_I2C_EAST, "ck_i2c_east_ck", "ck_i2c_east_sel", 1, 1),
	FACTOR(CLK_CK_I2C_WEST, "ck_i2c_west_ck", "ck_i2c_west_sel", 1, 1),
	FACTOR(CLK_CK_I2C_NORTH, "ck_i2c_north_ck", "ck_i2c_north_sel", 1, 1),
	FACTOR(CLK_CK_AES_UFSFDE, "ck_aes_ufsfde_ck", "ck_aes_ufsfde_sel", 1, 1),
	FACTOR(CLK_CK_UFS, "ck_ck", "ck_sel", 1, 1),
	FACTOR(CLK_CK_AUD_1, "ck_aud_1_ck", "ck_aud_1_sel", 1, 1),
	FACTOR(CLK_CK_AUD_2, "ck_aud_2_ck", "ck_aud_2_sel", 1, 1),
	FACTOR(CLK_CK_DPMAIF_MAIN, "ck_dpmaif_main_ck", "ck_dpmaif_main_sel", 1, 1),
	FACTOR(CLK_CK_PWM, "ck_pwm_ck", "ck_pwm_sel", 1, 1),
	FACTOR(CLK_CK_TL, "ck_tl_ck", "ck_tl_sel", 1, 1),
	FACTOR(CLK_CK_TL_P1, "ck_tl_p1_ck", "ck_tl_p1_sel", 1, 1),
	FACTOR(CLK_CK_TL_P2, "ck_tl_p2_ck", "ck_tl_p2_sel", 1, 1),
	FACTOR(CLK_CK_SSR_RNG, "ck_ssr_rng_ck", "ck_ssr_rng_sel", 1, 1),
	FACTOR(CLK_CK_SFLASH, "ck_sflash_ck", "ck_sflash_sel", 1, 1),
};

static const char * const ck_axi_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20",
	"ck_osc_d8",
	"ck_osc_d4",
	"ck_mainpll_d4_d4",
	"ck_mainpll_d7_d2"
};

static const char * const ck_mem_sub_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20",
	"ck_osc_d4",
	"ck_univpll_d4_d4",
	"ck_osc_d3",
	"ck_mainpll_d5_d2",
	"ck_mainpll_d4_d2",
	"ck_mainpll_d6",
	"ck_mainpll_d5",
	"ck_univpll_d5",
	"ck_mainpll_d4",
	"ck_mainpll_d3"
};

static const char * const ck_io_noc_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20",
	"ck_osc_d8",
	"ck_osc_d4",
	"ck_mainpll_d6_d2",
	"ck_mainpll_d9"
};

static const char * const ck_p_axi_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d7_d8",
	"ck_mainpll_d5_d8",
	"ck_osc_d8",
	"ck_mainpll_d7_d4",
	"ck_mainpll_d5_d4",
	"ck_mainpll_d4_d4",
	"ck_mainpll_d7_d2"
};

static const char * const ck_pextp0_axi_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d7_d8",
	"ck_mainpll_d5_d8",
	"ck_osc_d8",
	"ck_mainpll_d7_d4",
	"ck_mainpll_d5_d4",
	"ck_mainpll_d4_d4",
	"ck_mainpll_d7_d2"
};

static const char * const ck_pextp1_usb_axi_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d7_d8",
	"ck_mainpll_d5_d8",
	"ck_osc_d8",
	"ck_mainpll_d7_d4",
	"ck_mainpll_d5_d4",
	"ck_mainpll_d4_d4",
	"ck_mainpll_d7_d2"
};

static const char * const ck_p_fmem_sub_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d5_d8",
	"ck_mainpll_d5_d4",
	"ck_osc_d4",
	"ck_univpll_d4_d4",
	"ck_mainpll_d5_d2",
	"ck_mainpll_d4_d2",
	"ck_mainpll_d6",
	"ck_mainpll_d5",
	"ck_univpll_d5",
	"ck_mainpll_d4"
};

static const char * const ck_pexpt0_mem_sub_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d5_d8",
	"ck_mainpll_d5_d4",
	"ck_osc_d4",
	"ck_univpll_d4_d4",
	"ck_mainpll_d5_d2",
	"ck_mainpll_d4_d2",
	"ck_mainpll_d6",
	"ck_mainpll_d5",
	"ck_univpll_d5",
	"ck_mainpll_d4"
};

static const char * const ck_pextp1_usb_mem_sub_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d5_d8",
	"ck_mainpll_d5_d4",
	"ck_osc_d4",
	"ck_univpll_d4_d4",
	"ck_mainpll_d5_d2",
	"ck_mainpll_d4_d2",
	"ck_mainpll_d6",
	"ck_mainpll_d5",
	"ck_univpll_d5",
	"ck_mainpll_d4"
};

static const char * const ck_p_noc_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d5_d8",
	"ck_mainpll_d5_d4",
	"ck_osc_d4",
	"ck_univpll_d4_d4",
	"ck_mainpll_d5_d2",
	"ck_mainpll_d4_d2",
	"ck_mainpll_d6",
	"ck_mainpll_d5",
	"ck_univpll_d5",
	"ck_mainpll_d4",
	"ck_mainpll_d3"
};

static const char * const ck_emi_n_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d4",
	"ck_mainpll_d5_d8",
	"ck_mainpll_d5_d4",
	"ck_mainpll_d4_d4",
	"ck_emipll1_ck"
};

static const char * const ck_emi_s_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d4",
	"ck_mainpll_d5_d8",
	"ck_mainpll_d5_d4",
	"ck_mainpll_d4_d4",
	"ck_emipll1_ck"
};

static const char * const ck_ap2conn_host_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d7_d4"
};

static const char * const ck_atb_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d5_d2",
	"ck_mainpll_d4_d2",
	"ck_mainpll_d6"
};

static const char * const ck_cirq_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20",
	"ck_mainpll_d7_d4"
};

static const char * const ck_pbus_156m_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d7_d2",
	"ck_osc_d2",
	"ck_mainpll_d7"
};

static const char * const ck_efuse_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20"
};

static const char * const ck_mcl3gic_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d8",
	"ck_mainpll_d4_d4",
	"ck_mainpll_d7_d2"
};

static const char * const ck_mcinfra_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20",
	"ck_mainpll_d7_d2",
	"ck_mainpll_d5_d2",
	"ck_mainpll_d4_d2",
	"ck_mainpll_d9",
	"ck_mainpll_d6"
};

static const char * const ck_dsp_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d5",
	"ck_osc_d4",
	"ck_osc_d3",
	"ck_univpll_d6_d2",
	"ck_osc_d2",
	"ck_univpll_d5",
	"ck_osc"
};

static const char * const ck_mfg_ref_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d7_d2"
};

static const char * const ck_mfg_eb_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d7_d2",
	"ck_mainpll_d6_d2",
	"ck_mainpll_d5_d2"
};

static const char * const ck_uart_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_d6_d8",
	"ck_univpll_d6_d4",
	"ck_univpll_d6_d2"
};

static const char * const ck_spi0_b_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_d6_d4",
	"ck_univpll_d5_d4",
	"ck_mainpll_d4_d4",
	"ck_univpll_d4_d4",
	"ck_mainpll_d6_d2",
	"ck_univpll_192m",
	"ck_univpll_d6_d2"
};

static const char * const ck_spi1_b_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_d6_d4",
	"ck_univpll_d5_d4",
	"ck_mainpll_d4_d4",
	"ck_univpll_d4_d4",
	"ck_mainpll_d6_d2",
	"ck_univpll_192m",
	"ck_univpll_d6_d2"
};

static const char * const ck_spi2_b_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_d6_d4",
	"ck_univpll_d5_d4",
	"ck_mainpll_d4_d4",
	"ck_univpll_d4_d4",
	"ck_mainpll_d6_d2",
	"ck_univpll_192m",
	"ck_univpll_d6_d2"
};

static const char * const ck_spi3_b_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_d6_d4",
	"ck_univpll_d5_d4",
	"ck_mainpll_d4_d4",
	"ck_univpll_d4_d4",
	"ck_mainpll_d6_d2",
	"ck_univpll_192m",
	"ck_univpll_d6_d2"
};

static const char * const ck_spi4_b_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_d6_d4",
	"ck_univpll_d5_d4",
	"ck_mainpll_d4_d4",
	"ck_univpll_d4_d4",
	"ck_mainpll_d6_d2",
	"ck_univpll_192m",
	"ck_univpll_d6_d2"
};

static const char * const ck_spi5_b_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_d6_d4",
	"ck_univpll_d5_d4",
	"ck_mainpll_d4_d4",
	"ck_univpll_d4_d4",
	"ck_mainpll_d6_d2",
	"ck_univpll_192m",
	"ck_univpll_d6_d2"
};

static const char * const ck_spi6_b_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_d6_d4",
	"ck_univpll_d5_d4",
	"ck_mainpll_d4_d4",
	"ck_univpll_d4_d4",
	"ck_mainpll_d6_d2",
	"ck_univpll_192m",
	"ck_univpll_d6_d2"
};

static const char * const ck_spi7_b_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_d6_d4",
	"ck_univpll_d5_d4",
	"ck_mainpll_d4_d4",
	"ck_univpll_d4_d4",
	"ck_mainpll_d6_d2",
	"ck_univpll_192m",
	"ck_univpll_d6_d2"
};

static const char * const ck_msdc30_1_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_d6_d4",
	"ck_mainpll_d6_d2",
	"ck_univpll_d6_d2",
	"ck_msdcpll_d2"
};

static const char * const ck_msdc30_2_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_d6_d4",
	"ck_mainpll_d6_d2",
	"ck_univpll_d6_d2",
	"ck_msdcpll_d2"
};

static const char * const ck_disp_pwm_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d32",
	"ck_osc_d8",
	"ck_univpll_d6_d4",
	"ck_univpll_d5_d4",
	"ck_osc_d4",
	"ck_mainpll_d4_d4"
};

static const char * const ck_usb_1p_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_d5_d4"
};

static const char * const ck_usb_xhci_1p_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_d5_d4"
};

static const char * const ck_usb_fmcnt_p1_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_192m_d4"
};

static const char * const ck_i2c_p_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d4_d8",
	"ck_univpll_d5_d4",
	"ck_mainpll_d4_d4",
	"ck_univpll_d5_d2"
};

static const char * const ck_i2c_east_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d4_d8",
	"ck_univpll_d5_d4",
	"ck_mainpll_d4_d4",
	"ck_univpll_d5_d2"
};

static const char * const ck_i2c_west_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d4_d8",
	"ck_univpll_d5_d4",
	"ck_mainpll_d4_d4",
	"ck_univpll_d5_d2"
};

static const char * const ck_i2c_north_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d4_d8",
	"ck_univpll_d5_d4",
	"ck_mainpll_d4_d4",
	"ck_univpll_d5_d2"
};

static const char * const ck_aes_ufsfde_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d4_d4",
	"ck_univpll_d6_d2",
	"ck_mainpll_d4_d2",
	"ck_univpll_d6",
	"ck_mainpll_d4"
};

static const char * const ck_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d4_d4",
	"ck_univpll_d6_d2",
	"ck_mainpll_d4_d2",
	"ck_univpll_d6",
	"ck_mainpll_d5",
	"ck_univpll_d5"
};

static const char * const ck_aud_1_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_apll1_ck"
};

static const char * const ck_aud_2_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_apll2_ck"
};

static const char * const ck_adsp_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_adsppll_ck"
};

static const char * const ck_adsp_uarthub_b_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_d6_d4",
	"ck_univpll_d6_d2"
};

static const char * const ck_dpmaif_main_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_d4_d4",
	"ck_univpll_d5_d2",
	"ck_mainpll_d4_d2",
	"ck_univpll_d4_d2",
	"ck_mainpll_d6",
	"ck_univpll_d6",
	"ck_mainpll_d5",
	"ck_univpll_d5"
};

static const char * const ck_pwm_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d7_d4",
	"ck_univpll_d4_d8"
};

static const char * const ck_mcupm_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d7_d2",
	"ck_mainpll_d6_d2",
	"ck_univpll_d6_d2",
	"ck_mainpll_d5_d2"
};

static const char * const ck_ipseast_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d6",
	"ck_mainpll_d5",
	"ck_mainpll_d4",
	"ck_mainpll_d3"
};

static const char * const ck_tl_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d7_d4",
	"ck_mainpll_d4_d4",
	"ck_mainpll_d5_d2"
};

static const char * const ck_tl_p1_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d7_d4",
	"ck_mainpll_d4_d4",
	"ck_mainpll_d5_d2"
};

static const char * const ck_tl_p2_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d7_d4",
	"ck_mainpll_d4_d4",
	"ck_mainpll_d5_d2"
};

static const char * const ck_md_emi_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d4"
};

static const char * const ck_sdf_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d5_d2",
	"ck_mainpll_d4_d2",
	"ck_mainpll_d6",
	"ck_mainpll_d4",
	"ck_univpll_d4"
};

static const char * const ck_uarthub_b_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_d6_d4",
	"ck_univpll_d6_d2"
};

static const char * const ck_dpsw_cmp_26m_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20"
};

static const char * const ck_smapck_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d4_d8"
};

static const char * const ck_ssr_pka_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d4_d4",
	"ck_mainpll_d4_d2",
	"ck_mainpll_d7",
	"ck_mainpll_d6",
	"ck_mainpll_d5"
};

static const char * const ck_ssr_dma_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d4_d4",
	"ck_mainpll_d4_d2",
	"ck_mainpll_d7",
	"ck_mainpll_d6",
	"ck_mainpll_d5"
};

static const char * const ck_ssr_kdf_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d4_d4",
	"ck_mainpll_d4_d2",
	"ck_mainpll_d7"
};

static const char * const ck_ssr_rng_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d4_d4",
	"ck_mainpll_d5_d2",
	"ck_mainpll_d4_d2"
};

static const char * const ck_spu0_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d4_d4",
	"ck_mainpll_d4_d2",
	"ck_mainpll_d7",
	"ck_mainpll_d6",
	"ck_mainpll_d5"
};

static const char * const ck_spu1_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d4_d4",
	"ck_mainpll_d4_d2",
	"ck_mainpll_d7",
	"ck_mainpll_d6",
	"ck_mainpll_d5"
};

static const char * const ck_dxcc_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d4_d8",
	"ck_mainpll_d4_d4",
	"ck_mainpll_d4_d2"
};

static const char * const ck_apll_i2sin0_m_parents[] = {
	"ck_aud_1_sel",
	"ck_aud_2_sel"
};

static const char * const ck_apll_i2sin1_m_parents[] = {
	"ck_aud_1_sel",
	"ck_aud_2_sel"
};

static const char * const ck_apll_i2sin2_m_parents[] = {
	"ck_aud_1_sel",
	"ck_aud_2_sel"
};

static const char * const ck_apll_i2sin3_m_parents[] = {
	"ck_aud_1_sel",
	"ck_aud_2_sel"
};

static const char * const ck_apll_i2sin4_m_parents[] = {
	"ck_aud_1_sel",
	"ck_aud_2_sel"
};

static const char * const ck_apll_i2sin6_m_parents[] = {
	"ck_aud_1_sel",
	"ck_aud_2_sel"
};

static const char * const ck_apll_i2sout0_m_parents[] = {
	"ck_aud_1_sel",
	"ck_aud_2_sel"
};

static const char * const ck_apll_i2sout1_m_parents[] = {
	"ck_aud_1_sel",
	"ck_aud_2_sel"
};

static const char * const ck_apll_i2sout2_m_parents[] = {
	"ck_aud_1_sel",
	"ck_aud_2_sel"
};

static const char * const ck_apll_i2sout3_m_parents[] = {
	"ck_aud_1_sel",
	"ck_aud_2_sel"
};

static const char * const ck_apll_i2sout4_m_parents[] = {
	"ck_aud_1_sel",
	"ck_aud_2_sel"
};

static const char * const ck_apll_i2sout6_m_parents[] = {
	"ck_aud_1_sel",
	"ck_aud_2_sel"
};

static const char * const ck_apll_fmi2s_m_parents[] = {
	"ck_aud_1_sel",
	"ck_aud_2_sel"
};

static const char * const ck_apll_tdmout_m_parents[] = {
	"ck_aud_1_sel",
	"ck_aud_2_sel"
};

static const char * const ck_sflash_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d7_d8",
	"ck_univpll_d6_d8"
};

static const struct mtk_mux ck_muxes[] = {
	/* CLK_CFG_0 */
	MUX_CLR_SET_UPD(CLK_CK_AXI_SEL, "ck_axi_sel",
		ck_axi_parents, CLK_CFG_0, CLK_CFG_0_SET,
		CLK_CFG_0_CLR, 0, 3,
		CLK_CFG_UPDATE, TOP_MUX_AXI_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_MEM_SUB_SEL, "ck_mem_sub_sel",
		ck_mem_sub_parents, CLK_CFG_0, CLK_CFG_0_SET,
		CLK_CFG_0_CLR, 8, 4,
		CLK_CFG_UPDATE, TOP_MUX_MEM_SUB_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_IO_NOC_SEL, "ck_io_noc_sel",
		ck_io_noc_parents, CLK_CFG_0, CLK_CFG_0_SET,
		CLK_CFG_0_CLR, 16, 3,
		CLK_CFG_UPDATE, TOP_MUX_IO_NOC_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_P_AXI_SEL, "ck_p_axi_sel",
		ck_p_axi_parents, CLK_CFG_0, CLK_CFG_0_SET,
		CLK_CFG_0_CLR, 24, 3,
		CLK_CFG_UPDATE, TOP_MUX_PERI_AXI_SHIFT),
	/* CLK_CFG_1 */
	MUX_CLR_SET_UPD(CLK_CK_PEXTP0_AXI_SEL, "ck_pextp0_axi_sel",
		ck_pextp0_axi_parents, CLK_CFG_1, CLK_CFG_1_SET,
		CLK_CFG_1_CLR, 0, 3,
		CLK_CFG_UPDATE, TOP_MUX_UFS_PEXTP0_AXI_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_PEXTP1_USB_AXI_SEL, "ck_pextp1_usb_axi_sel",
		ck_pextp1_usb_axi_parents, CLK_CFG_1, CLK_CFG_1_SET,
		CLK_CFG_1_CLR, 8, 3,
		CLK_CFG_UPDATE, TOP_MUX_PEXTP1_USB_AXI_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_P_FMEM_SUB_SEL, "ck_p_fmem_sub_sel",
		ck_p_fmem_sub_parents, CLK_CFG_1, CLK_CFG_1_SET,
		CLK_CFG_1_CLR, 16, 4,
		CLK_CFG_UPDATE, TOP_MUX_PERI_FMEM_SUB_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_PEXPT0_MEM_SUB_SEL, "ck_pexpt0_mem_sub_sel",
		ck_pexpt0_mem_sub_parents, CLK_CFG_1, CLK_CFG_1_SET,
		CLK_CFG_1_CLR, 24, 4,
		CLK_CFG_UPDATE, TOP_MUX_UFS_PEXPT0_MEM_SUB_SHIFT),
	/* CLK_CFG_2 */
	MUX_CLR_SET_UPD(CLK_CK_PEXTP1_USB_MEM_SUB_SEL, "ck_pextp1_usb_mem_sub_sel",
		ck_pextp1_usb_mem_sub_parents, CLK_CFG_2, CLK_CFG_2_SET,
		CLK_CFG_2_CLR, 0, 4,
		CLK_CFG_UPDATE, TOP_MUX_PEXTP1_USB_MEM_SUB_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_P_NOC_SEL, "ck_p_noc_sel",
		ck_p_noc_parents, CLK_CFG_2, CLK_CFG_2_SET,
		CLK_CFG_2_CLR, 8, 4,
		CLK_CFG_UPDATE, TOP_MUX_PERI_NOC_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_EMI_N_SEL, "ck_emi_n_sel",
		ck_emi_n_parents, CLK_CFG_2, CLK_CFG_2_SET,
		CLK_CFG_2_CLR, 16, 3,
		CLK_CFG_UPDATE, TOP_MUX_EMI_N_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_EMI_S_SEL, "ck_emi_s_sel",
		ck_emi_s_parents, CLK_CFG_2, CLK_CFG_2_SET,
		CLK_CFG_2_CLR, 24, 3,
		CLK_CFG_UPDATE, TOP_MUX_EMI_S_SHIFT),
	/* CLK_CFG_3 */
	MUX_CLR_SET_UPD(CLK_CK_AP2CONN_HOST_SEL, "ck_ap2conn_host_sel",
		ck_ap2conn_host_parents, CLK_CFG_3, CLK_CFG_3_SET,
		CLK_CFG_3_CLR, 16, 1,
		CLK_CFG_UPDATE, TOP_MUX_AP2CONN_HOST_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_ATB_SEL, "ck_atb_sel",
		ck_atb_parents, CLK_CFG_3, CLK_CFG_3_SET,
		CLK_CFG_3_CLR, 24, 2,
		CLK_CFG_UPDATE, TOP_MUX_ATB_SHIFT),
	/* CLK_CFG_4 */
	MUX_CLR_SET_UPD(CLK_CK_CIRQ_SEL, "ck_cirq_sel",
		ck_cirq_parents, CLK_CFG_4, CLK_CFG_4_SET,
		CLK_CFG_4_CLR, 0, 2,
		CLK_CFG_UPDATE, TOP_MUX_CIRQ_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_PBUS_156M_SEL, "ck_pbus_156m_sel",
		ck_pbus_156m_parents, CLK_CFG_4, CLK_CFG_4_SET,
		CLK_CFG_4_CLR, 8, 2,
		CLK_CFG_UPDATE, TOP_MUX_PBUS_156M_SHIFT),
	/* CLK_CFG_5 */
	MUX_CLR_SET_UPD(CLK_CK_EFUSE_SEL, "ck_efuse_sel",
		ck_efuse_parents, CLK_CFG_5, CLK_CFG_5_SET,
		CLK_CFG_5_CLR, 0, 1,
		CLK_CFG_UPDATE, TOP_MUX_EFUSE_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_MCL3GIC_SEL, "ck_mcl3gic_sel",
		ck_mcl3gic_parents, CLK_CFG_5, CLK_CFG_5_SET,
		CLK_CFG_5_CLR, 8, 2,
		CLK_CFG_UPDATE, TOP_MUX_MCU_L3GIC_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_MCINFRA_SEL, "ck_mcinfra_sel",
		ck_mcinfra_parents, CLK_CFG_5, CLK_CFG_5_SET,
		CLK_CFG_5_CLR, 16, 3,
		CLK_CFG_UPDATE, TOP_MUX_MCU_INFRA_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_DSP_SEL, "ck_dsp_sel",
		ck_dsp_parents, CLK_CFG_5, CLK_CFG_5_SET,
		CLK_CFG_5_CLR, 24, 3,
		CLK_CFG_UPDATE, TOP_MUX_DSP_SHIFT),
	/* CLK_CFG_6 */
	MUX_GATE_FENC_CLR_SET_UPD_FLAGS(CLK_CK_MFG_REF_SEL, "ck_mfg_ref_sel", ck_mfg_ref_parents,
		CLK_CFG_6, CLK_CFG_6_SET, CLK_CFG_6_CLR,
		0, 1, 7, CLK_CFG_UPDATE, TOP_MUX_MFG_REF_SHIFT,
		CLK_FENC_STATUS_MON_0, 7, CLK_IGNORE_UNUSED),
	MUX_CLR_SET_UPD(CLK_CK_MFG_EB_SEL, "ck_mfg_eb_sel",
		ck_mfg_eb_parents, CLK_CFG_6, CLK_CFG_6_SET,
		CLK_CFG_6_CLR, 16, 2,
		CLK_CFG_UPDATE, TOP_MUX_MFG_EB_SHIFT),
	MUX_MULT_VOTE_FENC(CLK_CK_UART_SEL, "ck_uart_sel", ck_uart_parents,
		CLK_CFG_6, CLK_CFG_6_SET, CLK_CFG_6_CLR, "vote-regmap",
		VOTE_CG_3_DONE, VOTE_CG_3_SET, VOTE_CG_3_CLR,
		24, 2, 31, CLK_CFG_UPDATE, TOP_MUX_UART_SHIFT,
		CLK_FENC_STATUS_MON_0, 4),
	/* CLK_CFG_7 */
	MUX_MULT_VOTE_FENC(CLK_CK_SPI0_BCLK_SEL, "ck_spi0_b_sel", ck_spi0_b_parents,
		CLK_CFG_7, CLK_CFG_7_SET, CLK_CFG_7_CLR, "vote-regmap",
		VOTE_CG_4_DONE, VOTE_CG_4_SET, VOTE_CG_4_CLR,
		0, 3, 7, CLK_CFG_UPDATE, TOP_MUX_SPI0_BCLK_SHIFT,
		CLK_FENC_STATUS_MON_0, 3),
	MUX_MULT_VOTE_FENC(CLK_CK_SPI1_BCLK_SEL, "ck_spi1_b_sel", ck_spi1_b_parents,
		CLK_CFG_7, CLK_CFG_7_SET, CLK_CFG_7_CLR, "vote-regmap",
		VOTE_CG_4_DONE, VOTE_CG_4_SET, VOTE_CG_4_CLR,
		8, 3, 15, CLK_CFG_UPDATE, TOP_MUX_SPI1_BCLK_SHIFT,
		CLK_FENC_STATUS_MON_0, 2),
	MUX_MULT_VOTE_FENC(CLK_CK_SPI2_BCLK_SEL, "ck_spi2_b_sel", ck_spi2_b_parents,
		CLK_CFG_7, CLK_CFG_7_SET, CLK_CFG_7_CLR, "vote-regmap",
		VOTE_CG_4_DONE, VOTE_CG_4_SET, VOTE_CG_4_CLR,
		16, 3, 23, CLK_CFG_UPDATE, TOP_MUX_SPI2_BCLK_SHIFT,
		CLK_FENC_STATUS_MON_0, 1),
	MUX_MULT_VOTE_FENC(CLK_CK_SPI3_BCLK_SEL, "ck_spi3_b_sel", ck_spi3_b_parents,
		CLK_CFG_7, CLK_CFG_7_SET, CLK_CFG_7_CLR, "vote-regmap",
		VOTE_CG_4_DONE, VOTE_CG_4_SET, VOTE_CG_4_CLR,
		24, 3, 31, CLK_CFG_UPDATE1, TOP_MUX_SPI3_BCLK_SHIFT,
		CLK_FENC_STATUS_MON_0, 0),
	/* CLK_CFG_8 */
	MUX_MULT_VOTE_FENC(CLK_CK_SPI4_BCLK_SEL, "ck_spi4_b_sel", ck_spi4_b_parents,
		CLK_CFG_8, CLK_CFG_8_SET, CLK_CFG_8_CLR, "vote-regmap",
		VOTE_CG_5_DONE, VOTE_CG_5_SET, VOTE_CG_5_CLR,
		0, 3, 7, CLK_CFG_UPDATE1, TOP_MUX_SPI4_BCLK_SHIFT,
		CLK_FENC_STATUS_MON_1, 31),
	MUX_MULT_VOTE_FENC(CLK_CK_SPI5_BCLK_SEL, "ck_spi5_b_sel", ck_spi5_b_parents,
		CLK_CFG_8, CLK_CFG_8_SET, CLK_CFG_8_CLR, "vote-regmap",
		VOTE_CG_5_DONE, VOTE_CG_5_SET, VOTE_CG_5_CLR,
		8, 3, 15, CLK_CFG_UPDATE1, TOP_MUX_SPI5_BCLK_SHIFT,
		CLK_FENC_STATUS_MON_1, 30),
	MUX_MULT_VOTE_FENC(CLK_CK_SPI6_BCLK_SEL, "ck_spi6_b_sel", ck_spi6_b_parents,
		CLK_CFG_8, CLK_CFG_8_SET, CLK_CFG_8_CLR, "vote-regmap",
		VOTE_CG_5_DONE, VOTE_CG_5_SET, VOTE_CG_5_CLR,
		16, 3, 23, CLK_CFG_UPDATE1, TOP_MUX_SPI6_BCLK_SHIFT,
		CLK_FENC_STATUS_MON_1, 29),
	MUX_MULT_VOTE_FENC(CLK_CK_SPI7_BCLK_SEL, "ck_spi7_b_sel", ck_spi7_b_parents,
		CLK_CFG_8, CLK_CFG_8_SET, CLK_CFG_8_CLR, "vote-regmap",
		VOTE_CG_5_DONE, VOTE_CG_5_SET, VOTE_CG_5_CLR,
		24, 3, 31, CLK_CFG_UPDATE1, TOP_MUX_SPI7_BCLK_SHIFT,
		CLK_FENC_STATUS_MON_1, 28),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_MSDC30_1_SEL, "ck_msdc30_1_sel", ck_msdc30_1_parents,
		CLK_CFG_9, CLK_CFG_9_SET, CLK_CFG_9_CLR,
		16, 3, 23, CLK_CFG_UPDATE1, TOP_MUX_MSDC30_1_SHIFT,
		CLK_FENC_STATUS_MON_1, 25),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_MSDC30_2_SEL, "ck_msdc30_2_sel", ck_msdc30_2_parents,
		CLK_CFG_9, CLK_CFG_9_SET, CLK_CFG_9_CLR,
		24, 3, 31, CLK_CFG_UPDATE1, TOP_MUX_MSDC30_2_SHIFT,
		CLK_FENC_STATUS_MON_1, 24),
	/* CLK_CFG_10 */
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_DISP_PWM_SEL, "ck_disp_pwm_sel", ck_disp_pwm_parents,
		CLK_CFG_10, CLK_CFG_10_SET, CLK_CFG_10_CLR,
		0, 3, 7, CLK_CFG_UPDATE1, TOP_MUX_DISP_PWM_SHIFT,
		CLK_FENC_STATUS_MON_1, 23),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_USB_TOP_1P_SEL, "ck_usb_1p_sel", ck_usb_1p_parents,
		CLK_CFG_10, CLK_CFG_10_SET, CLK_CFG_10_CLR,
		8, 1, 15, CLK_CFG_UPDATE1, TOP_MUX_USB_TOP_1P_SHIFT,
		CLK_FENC_STATUS_MON_1, 22),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_USB_XHCI_1P_SEL, "ck_usb_xhci_1p_sel", ck_usb_xhci_1p_parents,
		CLK_CFG_10, CLK_CFG_10_SET, CLK_CFG_10_CLR,
		16, 1, 23, CLK_CFG_UPDATE1, TOP_MUX_SSUSB_XHCI_1P_SHIFT,
		CLK_FENC_STATUS_MON_1, 21),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_USB_FMCNT_P1_SEL, "ck_usb_fmcnt_p1_sel", ck_usb_fmcnt_p1_parents,
		CLK_CFG_10, CLK_CFG_10_SET, CLK_CFG_10_CLR,
		24, 1, 31, CLK_CFG_UPDATE1, TOP_MUX_SSUSB_FMCNT_P1_SHIFT,
		CLK_FENC_STATUS_MON_1, 20),
	/* CLK_CFG_11 */
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_I2C_P_SEL, "ck_i2c_p_sel", ck_i2c_p_parents,
		CLK_CFG_11, CLK_CFG_11_SET, CLK_CFG_11_CLR,
		0, 3, 7, CLK_CFG_UPDATE1, TOP_MUX_I2C_PERI_SHIFT,
		CLK_FENC_STATUS_MON_1, 19),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_I2C_EAST_SEL, "ck_i2c_east_sel", ck_i2c_east_parents,
		CLK_CFG_11, CLK_CFG_11_SET, CLK_CFG_11_CLR,
		8, 3, 15, CLK_CFG_UPDATE1, TOP_MUX_I2C_EAST_SHIFT,
		CLK_FENC_STATUS_MON_1, 18),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_I2C_WEST_SEL, "ck_i2c_west_sel", ck_i2c_west_parents,
		CLK_CFG_11, CLK_CFG_11_SET, CLK_CFG_11_CLR,
		16, 3, 23, CLK_CFG_UPDATE1, TOP_MUX_I2C_WEST_SHIFT,
		CLK_FENC_STATUS_MON_1, 17),
	MUX_MULT_VOTE_FENC(CLK_CK_I2C_NORTH_SEL, "ck_i2c_north_sel", ck_i2c_north_parents,
		CLK_CFG_11, CLK_CFG_11_SET, CLK_CFG_11_CLR, "vote-regmap",
		VOTE_CG_6_DONE, VOTE_CG_6_SET, VOTE_CG_6_CLR,
		24, 3, 31, CLK_CFG_UPDATE1, TOP_MUX_I2C_NORTH_SHIFT,
		CLK_FENC_STATUS_MON_1, 16),
	/* CLK_CFG_12 */
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_AES_UFSFDE_SEL, "ck_aes_ufsfde_sel", ck_aes_ufsfde_parents,
		CLK_CFG_12, CLK_CFG_12_SET, CLK_CFG_12_CLR,
		0, 3, 7, CLK_CFG_UPDATE1, TOP_MUX_AES_UFSFDE_SHIFT,
		CLK_FENC_STATUS_MON_1, 15),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_SEL, "ck_sel", ck_parents,
		CLK_CFG_12, CLK_CFG_12_SET, CLK_CFG_12_CLR,
		8, 3, 15, CLK_CFG_UPDATE1, TOP_MUX_UFS_SHIFT,
		CLK_FENC_STATUS_MON_1, 14),
	/* CLK_CFG_13 */
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_AUD_1_SEL, "ck_aud_1_sel", ck_aud_1_parents,
		CLK_CFG_13, CLK_CFG_13_SET, CLK_CFG_13_CLR,
		0, 1, 7, CLK_CFG_UPDATE1, TOP_MUX_AUD_1_SHIFT,
		CLK_FENC_STATUS_MON_1, 11),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_AUD_2_SEL, "ck_aud_2_sel", ck_aud_2_parents,
		CLK_CFG_13, CLK_CFG_13_SET, CLK_CFG_13_CLR,
		8, 1, 15, CLK_CFG_UPDATE1, TOP_MUX_AUD_2_SHIFT,
		CLK_FENC_STATUS_MON_1, 10),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_ADSP_SEL, "ck_adsp_sel", ck_adsp_parents,
		CLK_CFG_13, CLK_CFG_13_SET, CLK_CFG_13_CLR,
		16, 1, 23, CLK_CFG_UPDATE1, TOP_MUX_ADSP_SHIFT,
		CLK_FENC_STATUS_MON_1, 9),
	MUX_GATE_CLR_SET_UPD(CLK_CK_ADSP_UARTHUB_BCLK_SEL, "ck_adsp_uarthub_b_sel",
		ck_adsp_uarthub_b_parents, CLK_CFG_13, CLK_CFG_13_SET,
		CLK_CFG_13_CLR, 24, 2, 31,
		CLK_CFG_UPDATE1, TOP_MUX_ADSP_UARTHUB_BCLK_SHIFT),
	/* CLK_CFG_14 */
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_DPMAIF_MAIN_SEL, "ck_dpmaif_main_sel", ck_dpmaif_main_parents,
		CLK_CFG_14, CLK_CFG_14_SET, CLK_CFG_14_CLR,
		0, 4, 7, CLK_CFG_UPDATE1, TOP_MUX_DPMAIF_MAIN_SHIFT,
		CLK_FENC_STATUS_MON_1, 7),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_PWM_SEL, "ck_pwm_sel", ck_pwm_parents,
		CLK_CFG_14, CLK_CFG_14_SET, CLK_CFG_14_CLR,
		8, 2, 15, CLK_CFG_UPDATE1, TOP_MUX_PWM_SHIFT,
		CLK_FENC_STATUS_MON_1, 6),
	MUX_CLR_SET_UPD(CLK_CK_MCUPM_SEL, "ck_mcupm_sel",
		ck_mcupm_parents, CLK_CFG_14, CLK_CFG_14_SET,
		CLK_CFG_14_CLR, 16, 3,
		CLK_CFG_UPDATE1, TOP_MUX_MCUPM_SHIFT),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_SFLASH_SEL, "ck_sflash_sel", ck_sflash_parents,
		CLK_CFG_14, CLK_CFG_14_SET, CLK_CFG_14_CLR,
		24, 2, 31, CLK_CFG_UPDATE1, TOP_MUX_SFLASH_SHIFT,
		CLK_FENC_STATUS_MON_1, 4),
	/* CLK_CFG_15 */
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_IPSEAST_SEL, "ck_ipseast_sel", ck_ipseast_parents,
		CLK_CFG_15, CLK_CFG_15_SET, CLK_CFG_15_CLR,
		0, 3, 7, CLK_CFG_UPDATE1, TOP_MUX_IPSEAST_SHIFT,
		CLK_FENC_STATUS_MON_1, 3),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_TL_SEL, "ck_tl_sel", ck_tl_parents,
		CLK_CFG_15, CLK_CFG_15_SET, CLK_CFG_15_CLR,
		16, 2, 23, CLK_CFG_UPDATE2, TOP_MUX_TL_SHIFT,
		CLK_FENC_STATUS_MON_1, 1),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_TL_P1_SEL, "ck_tl_p1_sel", ck_tl_p1_parents,
		CLK_CFG_15, CLK_CFG_15_SET, CLK_CFG_15_CLR,
		24, 2, 31, CLK_CFG_UPDATE2, TOP_MUX_TL_P1_SHIFT,
		CLK_FENC_STATUS_MON_1, 0),
	/* CLK_CFG_16 */
	MUX_GATE_FENC_CLR_SET_UPD(CLK_CK_TL_P2_SEL, "ck_tl_p2_sel", ck_tl_p2_parents,
		CLK_CFG_16, CLK_CFG_16_SET, CLK_CFG_16_CLR,
		0, 2, 7, CLK_CFG_UPDATE2, TOP_MUX_TL_P2_SHIFT,
		CLK_FENC_STATUS_MON_2, 31),
	MUX_CLR_SET_UPD(CLK_CK_EMI_INTERFACE_546_SEL, "ck_md_emi_sel",
		ck_md_emi_parents, CLK_CFG_16, CLK_CFG_16_SET,
		CLK_CFG_16_CLR, 8, 1,
		CLK_CFG_UPDATE2, TOP_MUX_EMI_INTERFACE_546_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_SDF_SEL, "ck_sdf_sel",
		ck_sdf_parents, CLK_CFG_16, CLK_CFG_16_SET,
		CLK_CFG_16_CLR, 16, 3,
		CLK_CFG_UPDATE2, TOP_MUX_SDF_SHIFT),
	MUX_MULT_VOTE_FENC(CLK_CK_UARTHUB_BCLK_SEL, "ck_uarthub_b_sel", ck_uarthub_b_parents,
		CLK_CFG_16, CLK_CFG_16_SET, CLK_CFG_16_CLR, "vote-regmap",
		VOTE_CG_7_DONE, VOTE_CG_7_SET, VOTE_CG_7_CLR,
		24, 2, 31, CLK_CFG_UPDATE2, TOP_MUX_UARTHUB_BCLK_SHIFT,
		CLK_FENC_STATUS_MON_2, 28),
	/* CLK_CFG_17 */
	MUX_CLR_SET_UPD(CLK_CK_DPSW_CMP_26M_SEL, "ck_dpsw_cmp_26m_sel",
		ck_dpsw_cmp_26m_parents, CLK_CFG_17, CLK_CFG_17_SET,
		CLK_CFG_17_CLR, 0, 1,
		CLK_CFG_UPDATE2, TOP_MUX_DPSW_CMP_26M_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_SMAPCK_SEL, "ck_smapck_sel",
		ck_smapck_parents, CLK_CFG_17, CLK_CFG_17_SET,
		CLK_CFG_17_CLR, 8, 1,
		CLK_CFG_UPDATE2, TOP_MUX_SMAPCK_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_SSR_PKA_SEL, "ck_ssr_pka_sel",
		ck_ssr_pka_parents, CLK_CFG_17, CLK_CFG_17_SET,
		CLK_CFG_17_CLR, 16, 3,
		CLK_CFG_UPDATE2, TOP_MUX_SSR_PKA_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_SSR_DMA_SEL, "ck_ssr_dma_sel",
		ck_ssr_dma_parents, CLK_CFG_17, CLK_CFG_17_SET,
		CLK_CFG_17_CLR, 24, 3,
		CLK_CFG_UPDATE2, TOP_MUX_SSR_DMA_SHIFT),
	/* CLK_CFG_18 */
	MUX_CLR_SET_UPD(CLK_CK_SSR_KDF_SEL, "ck_ssr_kdf_sel",
		ck_ssr_kdf_parents, CLK_CFG_18, CLK_CFG_18_SET,
		CLK_CFG_18_CLR, 0, 2,
		CLK_CFG_UPDATE2, TOP_MUX_SSR_KDF_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_SSR_RNG_SEL, "ck_ssr_rng_sel",
		ck_ssr_rng_parents, CLK_CFG_18, CLK_CFG_18_SET,
		CLK_CFG_18_CLR, 8, 2,
		CLK_CFG_UPDATE2, TOP_MUX_SSR_RNG_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_SPU0_SEL, "ck_spu0_sel",
		ck_spu0_parents, CLK_CFG_18, CLK_CFG_18_SET,
		CLK_CFG_18_CLR, 16, 3,
		CLK_CFG_UPDATE2, TOP_MUX_SPU0_SHIFT),
	MUX_CLR_SET_UPD(CLK_CK_SPU1_SEL, "ck_spu1_sel",
		ck_spu1_parents, CLK_CFG_18, CLK_CFG_18_SET,
		CLK_CFG_18_CLR, 24, 3,
		CLK_CFG_UPDATE2, TOP_MUX_SPU1_SHIFT),
	/* CLK_CFG_19 */
	MUX_CLR_SET_UPD(CLK_CK_DXCC_SEL, "ck_dxcc_sel",
		ck_dxcc_parents, CLK_CFG_19, CLK_CFG_19_SET,
		CLK_CFG_19_CLR, 0, 2,
		CLK_CFG_UPDATE2, TOP_MUX_DXCC_SHIFT),
};

static const struct mtk_composite ck_composites[] = {
	/* CLK_AUDDIV_0 */
	MUX(CLK_CK_APLL_I2SIN0_MCK_SEL, "ck_apll_i2sin0_m_sel",
	    ck_apll_i2sin0_m_parents, 0x020c, 16, 1),
	MUX(CLK_CK_APLL_I2SIN1_MCK_SEL, "ck_apll_i2sin1_m_sel",
	    ck_apll_i2sin1_m_parents, 0x020c, 17, 1),
	MUX(CLK_CK_APLL_I2SIN2_MCK_SEL, "ck_apll_i2sin2_m_sel",
	    ck_apll_i2sin2_m_parents, 0x020c, 18, 1),
	MUX(CLK_CK_APLL_I2SIN3_MCK_SEL, "ck_apll_i2sin3_m_sel",
	    ck_apll_i2sin3_m_parents, 0x020c, 19, 1),
	MUX(CLK_CK_APLL_I2SIN4_MCK_SEL, "ck_apll_i2sin4_m_sel",
	    ck_apll_i2sin4_m_parents, 0x020c, 20, 1),
	MUX(CLK_CK_APLL_I2SIN6_MCK_SEL, "ck_apll_i2sin6_m_sel",
	    ck_apll_i2sin6_m_parents, 0x020c, 21, 1),
	MUX(CLK_CK_APLL_I2SOUT0_MCK_SEL, "ck_apll_i2sout0_m_sel",
	    ck_apll_i2sout0_m_parents, 0x020c, 22, 1),
	MUX(CLK_CK_APLL_I2SOUT1_MCK_SEL, "ck_apll_i2sout1_m_sel",
	    ck_apll_i2sout1_m_parents, 0x020c, 23, 1),
	MUX(CLK_CK_APLL_I2SOUT2_MCK_SEL, "ck_apll_i2sout2_m_sel",
	    ck_apll_i2sout2_m_parents, 0x020c, 24, 1),
	MUX(CLK_CK_APLL_I2SOUT3_MCK_SEL, "ck_apll_i2sout3_m_sel",
	    ck_apll_i2sout3_m_parents, 0x020c, 25, 1),
	MUX(CLK_CK_APLL_I2SOUT4_MCK_SEL, "ck_apll_i2sout4_m_sel",
	    ck_apll_i2sout4_m_parents, 0x020c, 26, 1),
	MUX(CLK_CK_APLL_I2SOUT6_MCK_SEL, "ck_apll_i2sout6_m_sel",
	    ck_apll_i2sout6_m_parents, 0x020c, 27, 1),
	MUX(CLK_CK_APLL_FMI2S_MCK_SEL, "ck_apll_fmi2s_m_sel",
	    ck_apll_fmi2s_m_parents, 0x020c, 28, 1),
	MUX(CLK_CK_APLL_TDMOUT_MCK_SEL, "ck_apll_tdmout_m_sel",
	    ck_apll_tdmout_m_parents, 0x020c, 29, 1),
	/* CLK_AUDDIV_2 */
	DIV_GATE(CLK_CK_APLL12_CK_DIV_I2SIN0, "ck_apll12_div_i2sin0",
		"ck_apll_i2sin0_m_sel", 0x020c,
		0, CLK_AUDDIV_2, 8, 0),
	DIV_GATE(CLK_CK_APLL12_CK_DIV_I2SIN1, "ck_apll12_div_i2sin1",
		"ck_apll_i2sin1_m_sel", 0x020c,
		1, CLK_AUDDIV_2, 8, 8),
	DIV_GATE(CLK_CK_APLL12_CK_DIV_I2SIN2, "ck_apll12_div_i2sin2",
		"ck_apll_i2sin2_m_sel", 0x020c,
		2, CLK_AUDDIV_2, 8, 16),
	DIV_GATE(CLK_CK_APLL12_CK_DIV_I2SIN3, "ck_apll12_div_i2sin3",
		"ck_apll_i2sin3_m_sel", 0x020c,
		3, CLK_AUDDIV_2, 8, 24),
	/* CLK_AUDDIV_3 */
	DIV_GATE(CLK_CK_APLL12_CK_DIV_I2SIN4, "ck_apll12_div_i2sin4",
		"ck_apll_i2sin4_m_sel", 0x020c,
		4, CLK_AUDDIV_3, 8, 0),
	DIV_GATE(CLK_CK_APLL12_CK_DIV_I2SIN6, "ck_apll12_div_i2sin6",
		"ck_apll_i2sin6_m_sel", 0x020c,
		5, CLK_AUDDIV_3, 8, 8),
	DIV_GATE(CLK_CK_APLL12_CK_DIV_I2SOUT0, "ck_apll12_div_i2sout0",
		"ck_apll_i2sout0_m_sel", 0x020c,
		6, CLK_AUDDIV_3, 8, 16),
	DIV_GATE(CLK_CK_APLL12_CK_DIV_I2SOUT1, "ck_apll12_div_i2sout1",
		"ck_apll_i2sout1_m_sel", 0x020c,
		7, CLK_AUDDIV_3, 8, 24),
	/* CLK_AUDDIV_4 */
	DIV_GATE(CLK_CK_APLL12_CK_DIV_I2SOUT2, "ck_apll12_div_i2sout2",
		"ck_apll_i2sout2_m_sel", 0x020c,
		8, CLK_AUDDIV_4, 8, 0),
	DIV_GATE(CLK_CK_APLL12_CK_DIV_I2SOUT3, "ck_apll12_div_i2sout3",
		"ck_apll_i2sout3_m_sel", 0x020c,
		9, CLK_AUDDIV_4, 8, 8),
	DIV_GATE(CLK_CK_APLL12_CK_DIV_I2SOUT4, "ck_apll12_div_i2sout4",
		"ck_apll_i2sout4_m_sel", 0x020c,
		10, CLK_AUDDIV_4, 8, 16),
	DIV_GATE(CLK_CK_APLL12_CK_DIV_I2SOUT6, "ck_apll12_div_i2sout6",
		"ck_apll_i2sout6_m_sel", 0x020c,
		11, CLK_AUDDIV_4, 8, 24),
	/* CLK_AUDDIV_5 */
	DIV_GATE(CLK_CK_APLL12_CK_DIV_FMI2S, "ck_apll12_div_fmi2s",
		"ck_apll_fmi2s_m_sel", 0x020c,
		12, CLK_AUDDIV_5, 8, 0),
	DIV_GATE(CLK_CK_APLL12_CK_DIV_TDMOUT_M, "ck_apll12_div_tdmout_m",
		"ck_apll_tdmout_m_sel", 0x020c,
		13, CLK_AUDDIV_5, 8, 8),
	DIV_GATE(CLK_CK_APLL12_CK_DIV_TDMOUT_B, "ck_apll12_div_tdmout_b",
		"ck_apll_tdmout_m_sel", 0x020c,
		14, CLK_AUDDIV_5, 8, 16),
};

static int clk_mt8196_ck_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	int r;
	struct device_node *node = pdev->dev.of_node;

	void __iomem *base;
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base)) {
		dev_err(&pdev->dev, "%s(): ioremap failed\n", __func__);
		return PTR_ERR(base);
	}

	clk_data = mtk_alloc_clk_data(CLK_CK_NR_CLK);

	mtk_clk_register_factors(ck_divs, ARRAY_SIZE(ck_divs),
			clk_data);

	mtk_clk_register_muxes(&pdev->dev, ck_muxes, ARRAY_SIZE(ck_muxes), node,
			&mt8196_clk_ck_lock, clk_data);

	mtk_clk_register_composites(&pdev->dev, ck_composites, ARRAY_SIZE(ck_composites),
			base, &mt8196_clk_ck_lock, clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		dev_err(&pdev->dev, "%s(): could not register clock provider: %d\n",
			__func__, r);

	return r;
}

static void clk_mt8196_ck_remove(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);
	struct device_node *node = pdev->dev.of_node;

	of_clk_del_provider(node);
	mtk_clk_unregister_composites(ck_composites, ARRAY_SIZE(ck_composites), clk_data);
	mtk_clk_unregister_muxes(ck_muxes, ARRAY_SIZE(ck_muxes), clk_data);
	mtk_clk_unregister_factors(ck_divs, ARRAY_SIZE(ck_divs), clk_data);
	mtk_free_clk_data(clk_data);
}

static const struct of_device_id of_match_clk_mt8196_ck[] = {
	{ .compatible = "mediatek,mt8196-cksys", },
	{ /* sentinel */ }
};

static struct platform_driver clk_mt8196_ck_drv = {
	.probe = clk_mt8196_ck_probe,
	.remove = clk_mt8196_ck_remove,
	.driver = {
		.name = "clk-mt8196-ck",
		.owner = THIS_MODULE,
		.of_match_table = of_match_clk_mt8196_ck,
	},
};

module_platform_driver(clk_mt8196_ck_drv);
MODULE_LICENSE("GPL");
