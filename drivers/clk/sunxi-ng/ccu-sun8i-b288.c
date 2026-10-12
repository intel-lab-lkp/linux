// SPDX-License-Identifier: GPL-2.0
/*
 * Clock Control Unit driver for the Allwinner B288 (sun8iw10p1).
 *
 * Closely related to the A64; most of its PLLs are bit-identical.
 * Transcribed from the BSP clk-sun8iw10.{c,h} and cross-checked against
 * ccu-sun50i-a64.c; divergences are called out inline.
 *
 * The audio clock tree (i2s/spdif/dsd/dmic/adda) is not implemented.
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ccu_common.h"
#include "ccu_reset.h"

#include "ccu_div.h"
#include "ccu_gate.h"
#include "ccu_mp.h"
#include "ccu_mult.h"
#include "ccu_nk.h"
#include "ccu_nkmp.h"
#include "ccu_nm.h"
#include "ccu_phase.h"
#include "ccu_sdm.h"

#include "ccu-sun8i-b288.h"

/*
 * PLL_24M @ 0x034 -- B288 only. It feeds the PLLs 24 MHz whatever the
 * crystal: it converts a 26 MHz one and is bypassed with a 24 MHz one.
 */
static struct clk_fixed_rate pll_24m_clk = {
	.fixed_rate	= 24000000,
	.hw.init	= CLK_HW_INIT_FW_NAME("pll-24m", "hosc",
					      &clk_fixed_rate_ops, 0),
};

static const struct clk_hw *pll_24m_hws[] = { &pll_24m_clk.hw };

/*
 * PLL_CPU @ 0x000 -- N[12:8] K[5:4] M[1:0] P[17:16], gate 31, lock 28.
 * Identical to sun50i-a64.
 */
static struct ccu_nkmp pll_cpux_clk = {
	.enable		= BIT(31),
	.lock		= BIT(28),
	.n		= _SUNXI_CCU_MULT(8, 5),
	.k		= _SUNXI_CCU_MULT(4, 2),
	.m		= _SUNXI_CCU_DIV(0, 2),
	.p		= _SUNXI_CCU_DIV_MAX(16, 2, 4),
	.common		= {
		.reg		= 0x000,
		.hw.init	= CLK_HW_INIT_HWS("pll-cpux", pll_24m_hws,
						  &ccu_nkmp_ops,
						  CLK_SET_RATE_UNGATE),
	},
};

/*
 * PLL_AUDIO @ 0x008 -- N[14:8] M[4:0] P[19:16], gate 31, lock 28, with
 * sigma-delta modulation for the audio rates, as on the A64 and H3. P is
 * forced to 1 at probe, so the rates are exact.
 */
#define SUN8I_B288_PLL_AUDIO_REG	0x008

static struct ccu_sdm_setting pll_audio_sdm_table[] = {
	{ .rate = 22579200, .pattern = 0xc0010d84, .m = 8, .n = 7 },
	{ .rate = 24576000, .pattern = 0xc000ac02, .m = 14, .n = 14 },
};

static struct ccu_nm pll_audio_base_clk = {
	.enable		= BIT(31),
	.lock		= BIT(28),
	.n		= _SUNXI_CCU_MULT(8, 7),
	.m		= _SUNXI_CCU_DIV(0, 5),
	.sdm		= _SUNXI_CCU_SDM(pll_audio_sdm_table, BIT(24),
					 0x284, BIT(31)),
	.common		= {
		.reg		= SUN8I_B288_PLL_AUDIO_REG,
		.features	= CCU_FEATURE_SIGMA_DELTA_MOD,
		.hw.init	= CLK_HW_INIT_HWS("pll-audio-base", pll_24m_hws,
						  &ccu_nm_ops,
						  CLK_SET_RATE_UNGATE),
	},
};

/* PLL_VIDEO0 @ 0x010 -- identical to sun50i-a64. */
static struct ccu_nm pll_video0_clk = {
	.enable		= BIT(31),
	.lock		= BIT(28),
	.n		= _SUNXI_CCU_MULT(8, 7),
	.m		= _SUNXI_CCU_DIV(0, 4),
	.frac		= _SUNXI_CCU_FRAC(BIT(24), BIT(25), 270000000, 297000000),
	.common		= {
		.reg		= 0x010,
		.features	= CCU_FEATURE_FRACTIONAL,
		.hw.init	= CLK_HW_INIT_HWS("pll-video0", pll_24m_hws,
						  &ccu_nm_ops,
						  CLK_SET_RATE_UNGATE),
	},
};

/*
 * PLL_DDR0 @ 0x020 -- N[14:8] M[1:0]. Unlike the A64 there is no K,
 * and N is 7 bits wide.
 */
static struct ccu_nm pll_ddr0_clk = {
	.enable		= BIT(31),
	.lock		= BIT(28),
	.n		= _SUNXI_CCU_MULT(8, 7),
	.m		= _SUNXI_CCU_DIV(0, 2),
	.common		= {
		.reg		= 0x020,
		.hw.init	= CLK_HW_INIT_HWS("pll-ddr0", pll_24m_hws,
						  &ccu_nm_ops,
						  CLK_SET_RATE_UNGATE),
	},
};

/* PLL_PERIPH0 @ 0x028 -- identical to sun50i-a64, incl. fixed post-div 2. */
static struct ccu_nk pll_periph0_clk = {
	.enable		= BIT(31),
	.lock		= BIT(28),
	.n		= _SUNXI_CCU_MULT(8, 5),
	.k		= _SUNXI_CCU_MULT_MIN(4, 2, 2),
	.fixed_post_div	= 2,
	.common		= {
		.reg		= 0x028,
		.features	= CCU_FEATURE_FIXED_POSTDIV,
		.hw.init	= CLK_HW_INIT_HWS("pll-periph0", pll_24m_hws,
						  &ccu_nk_ops, 0),
	},
};

/* PLL_VIDEO1 @ 0x030 -- same layout as PLL_VIDEO0. */
static struct ccu_nm pll_video1_clk = {
	.enable		= BIT(31),
	.lock		= BIT(28),
	.n		= _SUNXI_CCU_MULT(8, 7),
	.m		= _SUNXI_CCU_DIV(0, 4),
	.frac		= _SUNXI_CCU_FRAC(BIT(24), BIT(25), 270000000, 297000000),
	.common		= {
		.reg		= 0x030,
		.features	= CCU_FEATURE_FRACTIONAL,
		.hw.init	= CLK_HW_INIT_HWS("pll-video1", pll_24m_hws,
						  &ccu_nm_ops,
						  CLK_SET_RATE_UNGATE),
	},
};

/* PLL_PERIPH1 @ 0x044 -- identical to sun50i-a64. */
static struct ccu_nk pll_periph1_clk = {
	.enable		= BIT(31),
	.lock		= BIT(28),
	.n		= _SUNXI_CCU_MULT(8, 5),
	.k		= _SUNXI_CCU_MULT_MIN(4, 2, 2),
	.fixed_post_div	= 2,
	.common		= {
		.reg		= 0x044,
		.features	= CCU_FEATURE_FIXED_POSTDIV,
		.hw.init	= CLK_HW_INIT_HWS("pll-periph1", pll_24m_hws,
						  &ccu_nk_ops, 0),
	},
};

/* PLL_DE @ 0x048 -- identical to sun50i-a64. */
static struct ccu_nm pll_de_clk = {
	.enable		= BIT(31),
	.lock		= BIT(28),
	.n		= _SUNXI_CCU_MULT(8, 7),
	.m		= _SUNXI_CCU_DIV(0, 4),
	.frac		= _SUNXI_CCU_FRAC(BIT(24), BIT(25), 270000000, 297000000),
	.common		= {
		.reg		= 0x048,
		.features	= CCU_FEATURE_FRACTIONAL,
		.hw.init	= CLK_HW_INIT_HWS("pll-de", pll_24m_hws,
						  &ccu_nm_ops,
						  CLK_SET_RATE_UNGATE),
	},
};

/* PLL_DDR1 @ 0x04c -- B288 only. Same layout; this is the one feeding DRAM. */
static struct ccu_nm pll_ddr1_clk = {
	.enable		= BIT(31),
	.lock		= BIT(28),
	.n		= _SUNXI_CCU_MULT(8, 7),
	.m		= _SUNXI_CCU_DIV(0, 2),
	.common		= {
		.reg		= 0x04c,
		.hw.init	= CLK_HW_INIT_HWS("pll-ddr1", pll_24m_hws,
						  &ccu_nm_ops,
						  CLK_SET_RATE_UNGATE),
	},
};

/*
 * pll_periph{0,1} carry a fixed /2 post-divider, so the 2x clocks are the
 * raw outputs.
 */
static CLK_FIXED_FACTOR_HW(pll_periph0_2x_clk, "pll-periph0-2x",
			   &pll_periph0_clk.common.hw,
			   1, 2, 0);
static CLK_FIXED_FACTOR_HW(pll_periph1_2x_clk, "pll-periph1-2x",
			   &pll_periph1_clk.common.hw,
			   1, 2, 0);

/*
 * CPU_CFG @ 0x050 -- mux[17:16], CPUAPB divider [9:8], AXI divider [1:0].
 * Same layout as the H3 and the A64.
 */
static const struct clk_parent_data cpux_parents[] = {
	{ .fw_name = "losc" },
	{ .fw_name = "hosc" },
	{ .hw = &pll_cpux_clk.common.hw },
	{ .hw = &pll_cpux_clk.common.hw },
};

static SUNXI_CCU_MUX_DATA(cpux_clk, "cpux", cpux_parents,
			  0x050, 16, 2, CLK_IS_CRITICAL | CLK_SET_RATE_PARENT);

static const struct clk_hw *cpux_hws[] = { &cpux_clk.common.hw };
static SUNXI_CCU_M_HWS(axi_clk, "axi", cpux_hws, 0x050, 0, 2, 0);
static SUNXI_CCU_M_HWS(cpuapb_clk, "cpuapb", cpux_hws, 0x050, 8, 2, 0);

/*
 * AHB1_CFG @ 0x054 -- mux[13:12], AHB1 P2 divider [5:4], PLL_PERIPH0-only
 * pre-divider [7:6], APB1 divider [9:8].
 */
static const struct clk_parent_data ahb1_parents[] = {
	{ .fw_name = "losc" },
	{ .fw_name = "hosc" },
	{ .hw = &axi_clk.common.hw },
	{ .hw = &pll_periph0_clk.common.hw },
};

static const struct ccu_mux_var_prediv ahb1_predivs[] = {
	{ .index = 3, .shift = 6, .width = 2 },
};

static struct ccu_div ahb1_clk = {
	.div		= _SUNXI_CCU_DIV_FLAGS(4, 2, CLK_DIVIDER_POWER_OF_TWO),

	.mux		= {
		.shift	= 12,
		.width	= 2,

		.var_predivs	= ahb1_predivs,
		.n_var_predivs	= ARRAY_SIZE(ahb1_predivs),
	},

	.common		= {
		.reg		= 0x054,
		.features	= CCU_FEATURE_VARIABLE_PREDIV,
		.hw.init	= CLK_HW_INIT_PARENTS_DATA("ahb1",
							   ahb1_parents,
							   &ccu_div_ops,
							   0),
	},
};

static struct clk_div_table apb1_div_table[] = {
	{ .val = 0, .div = 2 },
	{ .val = 1, .div = 2 },
	{ .val = 2, .div = 4 },
	{ .val = 3, .div = 8 },
	{ /* Sentinel */ },
};

static SUNXI_CCU_DIV_TABLE_HW(apb1_clk, "apb1", &ahb1_clk.common.hw,
			      0x054, 8, 2, apb1_div_table, 0);

/*
 * APB2_CFG @ 0x058 -- M[4:0] P[17:16] mux[25:24]. The divider matches the
 * H3, but mux index 2 selects PLL_24M here rather than PLL_PERIPH0_2X.
 */
static const struct clk_parent_data apb2_parents[] = {
	{ .fw_name = "losc" },
	{ .fw_name = "hosc" },
	{ .hw = &pll_24m_clk.hw },
	{ .hw = &pll_periph0_2x_clk.hw },
};

static SUNXI_CCU_MP_DATA_WITH_MUX(apb2_clk, "apb2", apb2_parents, 0x058,
				  0, 5,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  0);

static const struct clk_parent_data mod0_parents[] = {
	{ .fw_name = "hosc" },
	{ .hw = &pll_periph0_2x_clk.hw },
	{ .hw = &pll_periph1_2x_clk.hw },
};

static const struct clk_parent_data de_parents[] = {
	{ .hw = &pll_periph0_2x_clk.hw },
	{ .hw = &pll_de_clk.common.hw },
};

static const struct clk_parent_data tcon0_parents[] = {
	{ .hw = &pll_video0_clk.common.hw },
	{ .hw = &pll_video1_clk.common.hw },
};

static const struct clk_parent_data mbus_parents[] = {
	{ .fw_name = "hosc" },
	{ .hw = &pll_periph0_2x_clk.hw },
	{ .hw = &pll_ddr0_clk.common.hw },
	{ .hw = &pll_ddr1_clk.common.hw },
};

static const struct clk_parent_data dram_parents[] = {
	{ .hw = &pll_ddr0_clk.common.hw },
	{ .hw = &pll_ddr1_clk.common.hw },
	{ .hw = &pll_periph0_2x_clk.hw },
};

/* NAND_CFG @ 0x080 -- M[3:0] P[17:16] mux[25:24] gate 31 */
static SUNXI_CCU_MP_DATA_WITH_MUX_GATE(nand_clk, "nand", mod0_parents, 0x080,
				       0, 4,		/* M */
				       16, 2,	/* P */
				       24, 2,	/* mux */
				       BIT(31),	/* gate */
				       0);

/*
 * SD{0..3}_CFG @ 0x088/0x08c/0x090/0x094 -- same layout as NAND.
 * mmc0/mmc1 use the old timing mode: no post-divider, and they need the
 * sample and output phase clocks. mmc2/mmc3 halve the clock internally.
 */
static SUNXI_CCU_MP_DATA_WITH_MUX_GATE(mmc0_clk, "mmc0", mod0_parents, 0x088,
				       0, 4,		/* M */
				       16, 2,	/* P */
				       24, 2,	/* mux */
				       BIT(31),	/* gate */
				       0);
static struct ccu_phase mmc0_sample_clk = {
	.shift	= 20,
	.width	= 3,
	.common	= {
		.reg		= 0x088,
		.hw.init	= CLK_HW_INIT_HW("mmc0_sample", &mmc0_clk.common.hw,
						 &ccu_phase_ops, 0),
	},
};

static struct ccu_phase mmc0_output_clk = {
	.shift	= 8,
	.width	= 3,
	.common	= {
		.reg		= 0x088,
		.hw.init	= CLK_HW_INIT_HW("mmc0_output", &mmc0_clk.common.hw,
						 &ccu_phase_ops, 0),
	},
};

static SUNXI_CCU_MP_DATA_WITH_MUX_GATE(mmc1_clk, "mmc1", mod0_parents, 0x08c,
				       0, 4,		/* M */
				       16, 2,	/* P */
				       24, 2,	/* mux */
				       BIT(31),	/* gate */
				       0);
static struct ccu_phase mmc1_sample_clk = {
	.shift	= 20,
	.width	= 3,
	.common	= {
		.reg		= 0x08c,
		.hw.init	= CLK_HW_INIT_HW("mmc1_sample", &mmc1_clk.common.hw,
						 &ccu_phase_ops, 0),
	},
};

static struct ccu_phase mmc1_output_clk = {
	.shift	= 8,
	.width	= 3,
	.common	= {
		.reg		= 0x08c,
		.hw.init	= CLK_HW_INIT_HW("mmc1_output", &mmc1_clk.common.hw,
						 &ccu_phase_ops, 0),
	},
};

static SUNXI_CCU_MP_DATA_WITH_MUX_GATE_POSTDIV(mmc2_clk, "mmc2", mod0_parents,
					       0x090, 0, 4, 16, 2, 24, 2,
					       BIT(31), 2, 0);
static SUNXI_CCU_MP_DATA_WITH_MUX_GATE_POSTDIV(mmc3_clk, "mmc3", mod0_parents,
					       0x094, 0, 4, 16, 2, 24, 2,
					       BIT(31), 2, 0);

/* SPI{0..2}_CFG @ 0x0a0/0x0a4/0x0a8 -- same layout as NAND. */
static SUNXI_CCU_MP_DATA_WITH_MUX_GATE(spi0_clk, "spi0", mod0_parents, 0x0a0,
				       0, 4, 16, 2, 24, 2, BIT(31), 0);
static SUNXI_CCU_MP_DATA_WITH_MUX_GATE(spi1_clk, "spi1", mod0_parents, 0x0a4,
				       0, 4, 16, 2, 24, 2, BIT(31), 0);
static SUNXI_CCU_MP_DATA_WITH_MUX_GATE(spi2_clk, "spi2", mod0_parents, 0x0a8,
				       0, 4, 16, 2, 24, 2, BIT(31), 0);

/* DRAM_CFG @ 0x0f4 -- M[1:0] mux[21:20] gate 31 */
static SUNXI_CCU_M_DATA_WITH_MUX_GATE(dram_clk, "dram", dram_parents, 0x0f4,
				      0, 2,		/* M */
				      20, 2,		/* mux */
				      BIT(31),	/* gate */
				      CLK_IS_CRITICAL);

/* DE_CFG @ 0x104, EE_CFG @ 0x108, EDMA_CFG @ 0x10c -- M[3:0] mux[26:24] gate 31 */
static SUNXI_CCU_M_DATA_WITH_MUX_GATE(de_clk, "de", de_parents, 0x104,
				      0, 4, 24, 3, BIT(31), CLK_SET_RATE_PARENT);
static SUNXI_CCU_M_DATA_WITH_MUX_GATE(ee_clk, "ee", de_parents, 0x108,
				      0, 4, 24, 3, BIT(31), CLK_SET_RATE_PARENT);
static SUNXI_CCU_M_DATA_WITH_MUX_GATE(edma_clk, "edma", de_parents, 0x10c,
				      0, 4, 24, 3, BIT(31), CLK_SET_RATE_PARENT);

/* TCON_CFG @ 0x118 -- mux[26:24] gate 31, no divider */
static SUNXI_CCU_MUX_DATA_WITH_GATE(tcon0_clk, "tcon0", tcon0_parents, 0x118,
				    24, 3,		/* mux */
				    BIT(31),		/* gate */
				    CLK_SET_RATE_PARENT);

/* MBUS_CFG @ 0x15c -- M[2:0] mux[25:24] gate 31 */
static SUNXI_CCU_M_DATA_WITH_MUX_GATE(mbus_clk, "mbus", mbus_parents, 0x15c,
				      0, 3, 24, 2, BIT(31), CLK_IS_CRITICAL);

static const struct clk_hw *ahb1_hws[] = { &ahb1_clk.common.hw };
static const struct clk_hw *apb1_hws[] = { &apb1_clk.common.hw };
static const struct clk_hw *apb2_hws[] = { &apb2_clk.common.hw };

static SUNXI_CCU_GATE_HWS(bus_ths_clk, "bus-ths", apb1_hws,
			  0x068, BIT(8), 0);
static SUNXI_CCU_GATE_HWS(bus_nand_clk, "bus-nand", ahb1_hws,
			  0x060, BIT(13), 0);
static SUNXI_CCU_GATE_HWS(bus_sdmmc0_bus_clk, "bus-sdmmc0-bus", ahb1_hws,
			  0x060, BIT(8), 0);
static SUNXI_CCU_GATE_HWS(bus_sdmmc1_bus_clk, "bus-sdmmc1-bus", ahb1_hws,
			  0x060, BIT(9), 0);
static SUNXI_CCU_GATE_HWS(bus_sdmmc2_bus_clk, "bus-sdmmc2-bus", ahb1_hws,
			  0x060, BIT(10), 0);
static SUNXI_CCU_GATE_HWS(bus_sdmmc3_bus_clk, "bus-sdmmc3-bus", ahb1_hws,
			  0x060, BIT(11), 0);
static SUNXI_CCU_GATE_HWS(bus_spi0_clk, "bus-spi0", ahb1_hws,
			  0x060, BIT(20), 0);
static SUNXI_CCU_GATE_HWS(bus_spi1_clk, "bus-spi1", ahb1_hws,
			  0x060, BIT(21), 0);
static SUNXI_CCU_GATE_HWS(bus_spi2_clk, "bus-spi2", ahb1_hws,
			  0x060, BIT(22), 0);
static SUNXI_CCU_GATE_HWS(bus_i2s0_clk, "bus-i2s0", apb1_hws,
			  0x068, BIT(12), 0);
static SUNXI_CCU_GATE_HWS(bus_i2s1_clk, "bus-i2s1", apb1_hws,
			  0x068, BIT(13), 0);
static SUNXI_CCU_GATE_HWS(bus_spdif_clk, "bus-spdif", apb1_hws,
			  0x068, BIT(1), 0);
static SUNXI_CCU_GATE_HWS(bus_dsd_clk, "bus-dsd", apb1_hws,
			  0x068, BIT(2), 0);
static SUNXI_CCU_GATE_HWS(bus_dmic_clk, "bus-dmic", apb1_hws,
			  0x068, BIT(3), 0);
static SUNXI_CCU_GATE_HWS(bus_usbohci0_clk, "bus-usbohci0", ahb1_hws,
			  0x060, BIT(29), 0);
static SUNXI_CCU_GATE_HWS(bus_usbehci0_clk, "bus-usbehci0", ahb1_hws,
			  0x060, BIT(26), 0);
static SUNXI_CCU_GATE_HWS(bus_usbotg_clk, "bus-usbotg", ahb1_hws,
			  0x060, BIT(24), 0);
static SUNXI_CCU_GATE_HWS(bus_de_clk, "bus-de", ahb1_hws,
			  0x064, BIT(12), 0);
static SUNXI_CCU_GATE_HWS(bus_ee_clk, "bus-ee", ahb1_hws,
			  0x064, BIT(13), 0);
static SUNXI_CCU_GATE_HWS(bus_tcon0_clk, "bus-tcon0", ahb1_hws,
			  0x064, BIT(4), 0);
/* csi_s, csi_m and csi_misc share one bus gate, as on the A64. */
static SUNXI_CCU_GATE_HWS(bus_csi_clk, "bus-csi", ahb1_hws,
			  0x064, BIT(8), 0);
static SUNXI_CCU_GATE_HWS(bus_adda_clk, "bus-adda", apb1_hws,
			  0x068, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_sdram_clk, "bus-sdram", ahb1_hws,
			  0x060, BIT(14), 0);
static SUNXI_CCU_GATE_HWS(bus_psram_clk, "bus-psram", ahb1_hws,
			  0x060, BIT(15), 0);
static SUNXI_CCU_GATE_HWS(bus_dma_clk, "bus-dma", ahb1_hws,
			  0x060, BIT(6), 0);
static SUNXI_CCU_GATE_HWS(bus_uart0_clk, "bus-uart0", apb2_hws,
			  0x06c, BIT(16), 0);
static SUNXI_CCU_GATE_HWS(bus_uart1_clk, "bus-uart1", apb2_hws,
			  0x06c, BIT(17), 0);
static SUNXI_CCU_GATE_HWS(bus_uart2_clk, "bus-uart2", apb2_hws,
			  0x06c, BIT(18), 0);
static SUNXI_CCU_GATE_HWS(bus_uart3_clk, "bus-uart3", apb2_hws,
			  0x06c, BIT(19), 0);
static SUNXI_CCU_GATE_HWS(bus_uart4_clk, "bus-uart4", apb2_hws,
			  0x06c, BIT(20), 0);
static SUNXI_CCU_GATE_HWS(bus_uart5_clk, "bus-uart5", apb2_hws,
			  0x06c, BIT(21), 0);
static SUNXI_CCU_GATE_HWS(bus_twi0_clk, "bus-twi0", apb2_hws,
			  0x06c, BIT(0), 0);
static SUNXI_CCU_GATE_HWS(bus_twi1_clk, "bus-twi1", apb2_hws,
			  0x06c, BIT(1), 0);
static SUNXI_CCU_GATE_HWS(bus_twi2_clk, "bus-twi2", apb2_hws,
			  0x06c, BIT(2), 0);
static SUNXI_CCU_GATE_HWS(bus_pio_clk, "bus-pio", apb1_hws,
			  0x068, BIT(5), 0);
static SUNXI_CCU_GATE_HWS(bus_gpadc_clk, "bus-gpadc", apb1_hws,
			  0x068, BIT(10), 0);
static SUNXI_CCU_GATE_HWS(bus_keyadc_clk, "bus-keyadc", apb1_hws,
			  0x068, BIT(9), 0);
/* losc_out is the RTC's osc32k-out */

static const struct ccu_reset_map sun8i_b288_ccu_resets[] = {
	[RST_BUS_THS]	= { 0x2d0, BIT(8) },
	[RST_BUS_NAND]	= { 0x2c0, BIT(13) },
	[RST_BUS_MMC0]	= { 0x2c0, BIT(8) },
	[RST_BUS_MMC1]	= { 0x2c0, BIT(9) },
	[RST_BUS_MMC2]	= { 0x2c0, BIT(10) },
	[RST_BUS_MMC3]	= { 0x2c0, BIT(11) },
	[RST_BUS_SPI0]	= { 0x2c0, BIT(20) },
	[RST_BUS_SPI1]	= { 0x2c0, BIT(21) },
	[RST_BUS_SPI2]	= { 0x2c0, BIT(22) },
	[RST_BUS_I2S0]	= { 0x2d0, BIT(12) },
	[RST_BUS_I2S1]	= { 0x2d0, BIT(13) },
	[RST_BUS_SPDIF]	= { 0x2d0, BIT(1) },
	[RST_BUS_DSD]	= { 0x2d0, BIT(2) },
	[RST_BUS_DMIC]	= { 0x2d0, BIT(3) },
	[RST_USBPHY0]	= { 0x0cc, BIT(0) },
	[RST_BUS_OHCI0]	= { 0x2c0, BIT(29) },
	[RST_BUS_EHCI0]	= { 0x2c0, BIT(26) },
	[RST_BUS_OTG]	= { 0x2c0, BIT(24) },
	[RST_BUS_DE]	= { 0x2c4, BIT(12) },
	[RST_BUS_EE]	= { 0x2c4, BIT(13) },
	[RST_BUS_TCON0]	= { 0x2c4, BIT(4) },
	[RST_BUS_CSI]	= { 0x2c4, BIT(8) },
	[RST_BUS_ADDA]	= { 0x2d0, BIT(0) },
	[RST_BUS_WLAN]	= { 0x2c4, BIT(1) },
	[RST_MBUS]	= { 0x0fc, BIT(31) },
	[RST_BUS_DRAM]	= { 0x2c0, BIT(14) },
	[RST_BUS_PSRAM]	= { 0x2c0, BIT(15) },
	[RST_BUS_DMA]	= { 0x2c0, BIT(6) },
	[RST_BUS_UART0]	= { 0x2d8, BIT(16) },
	[RST_BUS_UART1]	= { 0x2d8, BIT(17) },
	[RST_BUS_UART2]	= { 0x2d8, BIT(18) },
	[RST_BUS_UART3]	= { 0x2d8, BIT(19) },
	[RST_BUS_UART4]	= { 0x2d8, BIT(20) },
	[RST_BUS_UART5]	= { 0x2d8, BIT(21) },
	[RST_BUS_I2C0]	= { 0x2d8, BIT(0) },
	[RST_BUS_I2C1]	= { 0x2d8, BIT(1) },
	[RST_BUS_I2C2]	= { 0x2d8, BIT(2) },
	[RST_BUS_GPADC]	= { 0x2d0, BIT(10) },
	[RST_BUS_KEYADC]	= { 0x2d0, BIT(9) },
};

static struct ccu_common *sun8i_b288_ccu_clks[] = {
	&pll_cpux_clk.common,
	&pll_audio_base_clk.common,
	&pll_video0_clk.common,
	&pll_ddr0_clk.common,
	&pll_periph0_clk.common,
	&pll_video1_clk.common,
	&pll_periph1_clk.common,
	&pll_de_clk.common,
	&pll_ddr1_clk.common,
	&cpux_clk.common,
	&axi_clk.common,
	&cpuapb_clk.common,
	&ahb1_clk.common,
	&apb1_clk.common,
	&apb2_clk.common,
	&nand_clk.common,
	&mmc0_clk.common,
	&mmc0_sample_clk.common,
	&mmc0_output_clk.common,
	&mmc1_clk.common,
	&mmc1_sample_clk.common,
	&mmc1_output_clk.common,
	&mmc2_clk.common,
	&mmc3_clk.common,
	&spi0_clk.common,
	&spi1_clk.common,
	&spi2_clk.common,
	&dram_clk.common,
	&de_clk.common,
	&ee_clk.common,
	&edma_clk.common,
	&tcon0_clk.common,
	&mbus_clk.common,
	&bus_ths_clk.common,
	&bus_nand_clk.common,
	&bus_sdmmc0_bus_clk.common,
	&bus_sdmmc1_bus_clk.common,
	&bus_sdmmc2_bus_clk.common,
	&bus_sdmmc3_bus_clk.common,
	&bus_spi0_clk.common,
	&bus_spi1_clk.common,
	&bus_spi2_clk.common,
	&bus_i2s0_clk.common,
	&bus_i2s1_clk.common,
	&bus_spdif_clk.common,
	&bus_dsd_clk.common,
	&bus_dmic_clk.common,
	&bus_usbohci0_clk.common,
	&bus_usbehci0_clk.common,
	&bus_usbotg_clk.common,
	&bus_de_clk.common,
	&bus_ee_clk.common,
	&bus_tcon0_clk.common,
	&bus_csi_clk.common,
	&bus_adda_clk.common,
	&bus_sdram_clk.common,
	&bus_psram_clk.common,
	&bus_dma_clk.common,
	&bus_uart0_clk.common,
	&bus_uart1_clk.common,
	&bus_uart2_clk.common,
	&bus_uart3_clk.common,
	&bus_uart4_clk.common,
	&bus_uart5_clk.common,
	&bus_twi0_clk.common,
	&bus_twi1_clk.common,
	&bus_twi2_clk.common,
	&bus_pio_clk.common,
	&bus_gpadc_clk.common,
	&bus_keyadc_clk.common,
};

static struct clk_hw_onecell_data sun8i_b288_hw_clks = {
	.hws	= {
		[CLK_PLL_CPUX]		= &pll_cpux_clk.common.hw,
		[CLK_PLL_AUDIO_BASE]	= &pll_audio_base_clk.common.hw,
		[CLK_PLL_VIDEO0]	= &pll_video0_clk.common.hw,
		[CLK_PLL_DDR0]		= &pll_ddr0_clk.common.hw,
		[CLK_PLL_PERIPH0]	= &pll_periph0_clk.common.hw,
		[CLK_PLL_VIDEO1]	= &pll_video1_clk.common.hw,
		[CLK_PLL_24M]		= &pll_24m_clk.hw,
		[CLK_PLL_PERIPH1]	= &pll_periph1_clk.common.hw,
		[CLK_PLL_DE]		= &pll_de_clk.common.hw,
		[CLK_PLL_DDR1]		= &pll_ddr1_clk.common.hw,
		[CLK_PLL_PERIPH0_2X]	= &pll_periph0_2x_clk.hw,
		[CLK_PLL_PERIPH1_2X]	= &pll_periph1_2x_clk.hw,
		[CLK_CPUX]	= &cpux_clk.common.hw,
		[CLK_AXI]	= &axi_clk.common.hw,
		[CLK_CPUAPB]	= &cpuapb_clk.common.hw,
		[CLK_AHB1]	= &ahb1_clk.common.hw,
		[CLK_APB1]	= &apb1_clk.common.hw,
		[CLK_APB2]	= &apb2_clk.common.hw,
		[CLK_NAND]	= &nand_clk.common.hw,
		[CLK_MMC0]	= &mmc0_clk.common.hw,
		[CLK_MMC0_SAMPLE]	= &mmc0_sample_clk.common.hw,
		[CLK_MMC0_OUTPUT]	= &mmc0_output_clk.common.hw,
		[CLK_MMC1]	= &mmc1_clk.common.hw,
		[CLK_MMC1_SAMPLE]	= &mmc1_sample_clk.common.hw,
		[CLK_MMC1_OUTPUT]	= &mmc1_output_clk.common.hw,
		[CLK_MMC2]	= &mmc2_clk.common.hw,
		[CLK_MMC3]	= &mmc3_clk.common.hw,
		[CLK_SPI0]	= &spi0_clk.common.hw,
		[CLK_SPI1]	= &spi1_clk.common.hw,
		[CLK_SPI2]	= &spi2_clk.common.hw,
		[CLK_DRAM]	= &dram_clk.common.hw,
		[CLK_DE]	= &de_clk.common.hw,
		[CLK_EE]	= &ee_clk.common.hw,
		[CLK_EDMA]	= &edma_clk.common.hw,
		[CLK_TCON0]	= &tcon0_clk.common.hw,
		[CLK_MBUS]	= &mbus_clk.common.hw,
		[CLK_BUS_THS]	= &bus_ths_clk.common.hw,
		[CLK_BUS_NAND]	= &bus_nand_clk.common.hw,
		[CLK_BUS_SDMMC0_BUS]	= &bus_sdmmc0_bus_clk.common.hw,
		[CLK_BUS_SDMMC1_BUS]	= &bus_sdmmc1_bus_clk.common.hw,
		[CLK_BUS_SDMMC2_BUS]	= &bus_sdmmc2_bus_clk.common.hw,
		[CLK_BUS_SDMMC3_BUS]	= &bus_sdmmc3_bus_clk.common.hw,
		[CLK_BUS_SPI0]	= &bus_spi0_clk.common.hw,
		[CLK_BUS_SPI1]	= &bus_spi1_clk.common.hw,
		[CLK_BUS_SPI2]	= &bus_spi2_clk.common.hw,
		[CLK_BUS_I2S0]	= &bus_i2s0_clk.common.hw,
		[CLK_BUS_I2S1]	= &bus_i2s1_clk.common.hw,
		[CLK_BUS_SPDIF]	= &bus_spdif_clk.common.hw,
		[CLK_BUS_DSD]	= &bus_dsd_clk.common.hw,
		[CLK_BUS_DMIC]	= &bus_dmic_clk.common.hw,
		[CLK_BUS_USBOHCI0]	= &bus_usbohci0_clk.common.hw,
		[CLK_BUS_USBEHCI0]	= &bus_usbehci0_clk.common.hw,
		[CLK_BUS_USBOTG]	= &bus_usbotg_clk.common.hw,
		[CLK_BUS_DE]	= &bus_de_clk.common.hw,
		[CLK_BUS_EE]	= &bus_ee_clk.common.hw,
		[CLK_BUS_TCON0]	= &bus_tcon0_clk.common.hw,
		[CLK_BUS_CSI]		= &bus_csi_clk.common.hw,
		[CLK_BUS_ADDA]	= &bus_adda_clk.common.hw,
		[CLK_BUS_SDRAM]	= &bus_sdram_clk.common.hw,
		[CLK_BUS_PSRAM]	= &bus_psram_clk.common.hw,
		[CLK_BUS_DMA]	= &bus_dma_clk.common.hw,
		[CLK_BUS_UART0]	= &bus_uart0_clk.common.hw,
		[CLK_BUS_UART1]	= &bus_uart1_clk.common.hw,
		[CLK_BUS_UART2]	= &bus_uart2_clk.common.hw,
		[CLK_BUS_UART3]	= &bus_uart3_clk.common.hw,
		[CLK_BUS_UART4]	= &bus_uart4_clk.common.hw,
		[CLK_BUS_UART5]	= &bus_uart5_clk.common.hw,
		[CLK_BUS_TWI0]	= &bus_twi0_clk.common.hw,
		[CLK_BUS_TWI1]	= &bus_twi1_clk.common.hw,
		[CLK_BUS_TWI2]	= &bus_twi2_clk.common.hw,
		[CLK_BUS_PIO]	= &bus_pio_clk.common.hw,
		[CLK_BUS_GPADC]	= &bus_gpadc_clk.common.hw,
		[CLK_BUS_KEYADC]	= &bus_keyadc_clk.common.hw,
	},
	.num	= CLK_NUMBER,
};

static const struct sunxi_ccu_desc sun8i_b288_ccu_desc = {
	.ccu_clks	= sun8i_b288_ccu_clks,
	.num_ccu_clks	= ARRAY_SIZE(sun8i_b288_ccu_clks),

	.hw_clks	= &sun8i_b288_hw_clks,

	.resets		= sun8i_b288_ccu_resets,
	.num_resets	= ARRAY_SIZE(sun8i_b288_ccu_resets),

};

static int sun8i_b288_ccu_probe(struct platform_device *pdev)
{
	void __iomem *reg;
	u32 val;

	reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	/* Force the PLL-Audio-1x divider to 1 */
	val = readl(reg + SUN8I_B288_PLL_AUDIO_REG);
	val &= ~GENMASK(19, 16);
	writel(val, reg + SUN8I_B288_PLL_AUDIO_REG);

	return devm_sunxi_ccu_probe(&pdev->dev, reg, &sun8i_b288_ccu_desc);
}

static const struct of_device_id sun8i_b288_ccu_ids[] = {
	{ .compatible = "allwinner,sun8i-b288-ccu" },
	{ }
};
MODULE_DEVICE_TABLE(of, sun8i_b288_ccu_ids);

static struct platform_driver sun8i_b288_ccu_driver = {
	.probe	= sun8i_b288_ccu_probe,
	.driver	= {
		.name			= "sun8i-b288-ccu",
		.suppress_bind_attrs	= true,
		.of_match_table		= sun8i_b288_ccu_ids,
	},
};
module_platform_driver(sun8i_b288_ccu_driver);

MODULE_IMPORT_NS("SUNXI_CCU");
MODULE_DESCRIPTION("Allwinner B288 (sun8iw10p1) CCU driver");
MODULE_LICENSE("GPL");
