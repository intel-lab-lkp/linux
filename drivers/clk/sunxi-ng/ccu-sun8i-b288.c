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
#include "ccu_nkm.h"
#include "ccu_nkmp.h"
#include "ccu_nm.h"
#include "ccu_phase.h"

#include "ccu-sun8i-b288.h"

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
		.hw.init	= CLK_HW_INIT("pll-cpux", "osc24M",
					      &ccu_nkmp_ops,
					      CLK_SET_RATE_UNGATE),
	},
};

/*
 * PLL_AUDIO @ 0x008 -- N[14:8] M[4:0], gate 31, lock 28.
 * The vendor also writes a sigma-delta pattern at PLL_AUDIOPAT (0x284);
 * not transcribed, so plain NM here.
 */
static SUNXI_CCU_NM_WITH_GATE_LOCK(pll_audio_base_clk, "pll-audio-base",
				   "osc24M", 0x008,
				   8, 7,	/* N */
				   0, 5,	/* M */
				   BIT(31),	/* gate */
				   BIT(28),	/* lock */
				   CLK_SET_RATE_UNGATE);

/* PLL_VIDEO0 @ 0x010 -- identical to sun50i-a64. */
static SUNXI_CCU_NM_WITH_FRAC_GATE_LOCK(pll_video0_clk, "pll-video0",
					"osc24M", 0x010,
					8, 7,		/* N */
					0, 4,		/* M */
					BIT(24),	/* frac enable */
					BIT(25),	/* frac select */
					270000000,	/* frac rate 0 */
					297000000,	/* frac rate 1 */
					BIT(31),	/* gate */
					BIT(28),	/* lock */
					CLK_SET_RATE_UNGATE);

/*
 * PLL_DDR0 @ 0x020 -- DIVERGES FROM A64, which is NKM with N[12:8] K[5:4].
 * The B288 has a 7-bit N and no K: the value boot0 leaves here sets
 * bit 13, which the A64's 5-bit N field cannot hold.
 *
 * The vendor's bit-30 apply/update has no mainline equivalent and is not
 * modelled, nor are the sigma-delta patterns at 0x2ac..0x2b8.
 */
static SUNXI_CCU_NM_WITH_GATE_LOCK(pll_ddr0_clk, "pll-ddr0",
				   "osc24M", 0x020,
				   8, 7,	/* N */
				   0, 2,	/* M */
				   BIT(31),	/* gate */
				   BIT(28),	/* lock */
				   CLK_SET_RATE_UNGATE);

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
		.hw.init	= CLK_HW_INIT("pll-periph0", "osc24M",
					      &ccu_nk_ops, 0),
	},
};

/* PLL_VIDEO1 @ 0x030 -- same layout as PLL_VIDEO0. */
static SUNXI_CCU_NM_WITH_FRAC_GATE_LOCK(pll_video1_clk, "pll-video1",
					"osc24M", 0x030,
					8, 7,		/* N */
					0, 4,		/* M */
					BIT(24),	/* frac enable */
					BIT(25),	/* frac select */
					270000000,	/* frac rate 0 */
					297000000,	/* frac rate 1 */
					BIT(31),	/* gate */
					BIT(28),	/* lock */
					CLK_SET_RATE_UNGATE);

/*
 * PLL_24M @ 0x034 -- B288 only, no a64 equivalent.
 * Vendor: N[14:8] K[20:16] M[1:0] P[7:4]. The unusual K position means the
 * generic NKMP macro cannot be used; declared longhand.
 */
static struct ccu_nkmp pll_24m_clk = {
	.enable		= BIT(31),
	.lock		= BIT(28),
	.n		= _SUNXI_CCU_MULT(8, 7),
	.k		= _SUNXI_CCU_MULT(16, 5),
	.m		= _SUNXI_CCU_DIV(0, 2),
	.p		= _SUNXI_CCU_DIV(4, 4),
	.common		= {
		.reg		= 0x034,
		.hw.init	= CLK_HW_INIT("pll-24m", "osc24M",
					      &ccu_nkmp_ops,
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
		.hw.init	= CLK_HW_INIT("pll-periph1", "osc24M",
					      &ccu_nk_ops, 0),
	},
};

/* PLL_DE @ 0x048 -- identical to sun50i-a64. */
static SUNXI_CCU_NM_WITH_FRAC_GATE_LOCK(pll_de_clk, "pll-de",
					"osc24M", 0x048,
					8, 7,		/* N */
					0, 4,		/* M */
					BIT(24),	/* frac enable */
					BIT(25),	/* frac select */
					270000000,	/* frac rate 0 */
					297000000,	/* frac rate 1 */
					BIT(31),	/* gate */
					BIT(28),	/* lock */
					CLK_SET_RATE_UNGATE);

/* PLL_DDR1 @ 0x04c -- B288 only. Same layout; this is the one feeding DRAM. */
static SUNXI_CCU_NM_WITH_GATE_LOCK(pll_ddr1_clk, "pll-ddr1",
				   "osc24M", 0x04c,
				   8, 7,	/* N */
				   0, 2,	/* M */
				   BIT(31),	/* gate */
				   BIT(28),	/* lock */
				   CLK_SET_RATE_UNGATE);

/*
 * Multiplied PLL outputs. pll_periph{0,1} carry a fixed /2 post-divider, so
 * the "2x" clocks the vendor's periphx2_parents refer to are the raw outputs.
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
/* vendor: cpu_parents */
static const char * const cpux_parents[] = { "osc32k", "osc24M",
					     "pll-cpux", "pll-cpux" };
static SUNXI_CCU_MUX(cpux_clk, "cpux", cpux_parents,
		     0x050, 16, 2, CLK_IS_CRITICAL | CLK_SET_RATE_PARENT);

static SUNXI_CCU_M(axi_clk, "axi", "cpux", 0x050, 0, 2, 0);
static SUNXI_CCU_M(cpuapb_clk, "cpuapb", "cpux", 0x050, 8, 2, 0);

/*
 * AHB1_CFG @ 0x054 -- mux[13:12], AHB1 P2 divider [5:4], PLL_PERIPH0-only
 * pre-divider [7:6], APB1 divider [9:8].
 */
/* vendor: ahb1_parents, with pll_periphahb0 folded into the prediv */
static const char * const ahb1_parents[] = { "osc32k", "osc24M",
					     "axi", "pll-periph0" };
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
		.hw.init	= CLK_HW_INIT_PARENTS("ahb1",
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

static SUNXI_CCU_DIV_TABLE(apb1_clk, "apb1", "ahb1",
			   0x054, 8, 2, apb1_div_table, 0);

/*
 * APB2_CFG @ 0x058 -- M[4:0] P[17:16] mux[25:24]. The divider matches the
 * H3, but mux index 2 selects PLL_24M here rather than PLL_PERIPH0_2X.
 */
/* vendor: apb2_parents */
static const char * const apb2_parents[] = { "osc32k", "osc24M",
					     "pll-24m", "pll-periph0-2x" };
static SUNXI_CCU_MP_WITH_MUX(apb2_clk, "apb2", apb2_parents, 0x058,
			     0, 5,	/* M */
			     16, 2,	/* P */
			     24, 2,	/* mux */
			     0);

/* vendor: periphx2_parents */
static const char * const mod0_parents[] = { "osc24M", "pll-periph0-2x",
					     "pll-periph1-2x" };
/* vendor: de_parents */
static const char * const de_parents[] = { "pll-periph0-2x", "pll-de" };
/* vendor: tcon0_parents */
static const char * const tcon0_parents[] = { "pll-video0", "pll-video1" };
/* vendor: mbus_parents */
static const char * const mbus_parents[] = { "osc24M", "pll-periph0-2x",
					     "pll-ddr0", "pll-ddr1" };
/* vendor: sdram_parents */
static const char * const dram_parents[] = { "pll-ddr0", "pll-ddr1",
					     "pll-periph0-2x" };

/* NAND_CFG @ 0x080 -- M[3:0] P[17:16] mux[25:24] gate 31 */
static SUNXI_CCU_MP_WITH_MUX_GATE(nand_clk, "nand", mod0_parents, 0x080,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);

/*
 * SD{0..3}_CFG @ 0x088/0x08c/0x090/0x094 -- same layout as NAND.
 *
 * The four controllers are not the same IP. The vendor device tree calls
 * mmc0 and mmc1 "sunxi-mmc-v4p1x" and mmc3 "sunxi-mmc-v4p5x" (mmc2 is a
 * different block again). The v4p1x pair run in the old timing mode: no
 * internal halving, so no post-divider, and they need the sample and output
 * phase clocks. mmc2 and mmc3 use the new timing mode like the A64, where
 * the module clock is internally halved -- hence the post-divider of two.
 */
static SUNXI_CCU_MP_WITH_MUX_GATE(mmc0_clk, "mmc0", mod0_parents, 0x088,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);
static SUNXI_CCU_PHASE(mmc0_sample_clk, "mmc0_sample", "mmc0", 0x088, 20, 3, 0);
static SUNXI_CCU_PHASE(mmc0_output_clk, "mmc0_output", "mmc0", 0x088, 8, 3, 0);

static SUNXI_CCU_MP_WITH_MUX_GATE(mmc1_clk, "mmc1", mod0_parents, 0x08c,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);
static SUNXI_CCU_PHASE(mmc1_sample_clk, "mmc1_sample", "mmc1", 0x08c, 20, 3, 0);
static SUNXI_CCU_PHASE(mmc1_output_clk, "mmc1_output", "mmc1", 0x08c, 8, 3, 0);
static SUNXI_CCU_MP_WITH_MUX_GATE_POSTDIV(mmc2_clk, "mmc2", mod0_parents,
					  0x090, 0, 4, 16, 2, 24, 2,
					  BIT(31), 2, 0);
static SUNXI_CCU_MP_WITH_MUX_GATE_POSTDIV(mmc3_clk, "mmc3", mod0_parents,
					  0x094, 0, 4, 16, 2, 24, 2,
					  BIT(31), 2, 0);

/* SPI{0..2}_CFG @ 0x0a0/0x0a4/0x0a8 -- same layout as NAND. */
static SUNXI_CCU_MP_WITH_MUX_GATE(spi0_clk, "spi0", mod0_parents, 0x0a0,
				  0, 4, 16, 2, 24, 2, BIT(31), 0);
static SUNXI_CCU_MP_WITH_MUX_GATE(spi1_clk, "spi1", mod0_parents, 0x0a4,
				  0, 4, 16, 2, 24, 2, BIT(31), 0);
static SUNXI_CCU_MP_WITH_MUX_GATE(spi2_clk, "spi2", mod0_parents, 0x0a8,
				  0, 4, 16, 2, 24, 2, BIT(31), 0);

/* DRAM_CFG @ 0x0f4 -- M[1:0] mux[21:20] gate 31 */
static SUNXI_CCU_M_WITH_MUX_GATE(dram_clk, "dram", dram_parents, 0x0f4,
				 0, 2,		/* M */
				 20, 2,		/* mux */
				 BIT(31),	/* gate */
				 CLK_IS_CRITICAL);

/* DE_CFG @ 0x104, EE_CFG @ 0x108, EDMA_CFG @ 0x10c -- M[3:0] mux[26:24] gate 31 */
static SUNXI_CCU_M_WITH_MUX_GATE(de_clk, "de", de_parents, 0x104,
				 0, 4, 24, 3, BIT(31), CLK_SET_RATE_PARENT);
static SUNXI_CCU_M_WITH_MUX_GATE(ee_clk, "ee", de_parents, 0x108,
				 0, 4, 24, 3, BIT(31), CLK_SET_RATE_PARENT);
static SUNXI_CCU_M_WITH_MUX_GATE(edma_clk, "edma", de_parents, 0x10c,
				 0, 4, 24, 3, BIT(31), CLK_SET_RATE_PARENT);

/* TCON_CFG @ 0x118 -- mux[26:24] gate 31, no divider */
static SUNXI_CCU_MUX_WITH_GATE(tcon0_clk, "tcon0", tcon0_parents, 0x118,
			       24, 3,		/* mux */
			       BIT(31),		/* gate */
			       CLK_SET_RATE_PARENT);

/* MBUS_CFG @ 0x15c -- M[2:0] mux[25:24] gate 31 */
static SUNXI_CCU_M_WITH_MUX_GATE(mbus_clk, "mbus", mbus_parents, 0x15c,
				 0, 3, 24, 2, BIT(31), CLK_IS_CRITICAL);

static SUNXI_CCU_GATE(bus_ths_clk, "bus-ths", "apb1",
		      0x068, BIT(8), 0);
static SUNXI_CCU_GATE(bus_nand_clk, "bus-nand", "ahb1",
		      0x060, BIT(13), 0);
static SUNXI_CCU_GATE(bus_sdmmc0_bus_clk, "bus-sdmmc0-bus", "ahb1",
		      0x060, BIT(8), 0);
static SUNXI_CCU_GATE(bus_sdmmc1_bus_clk, "bus-sdmmc1-bus", "ahb1",
		      0x060, BIT(9), 0);
static SUNXI_CCU_GATE(bus_sdmmc2_bus_clk, "bus-sdmmc2-bus", "ahb1",
		      0x060, BIT(10), 0);
static SUNXI_CCU_GATE(bus_sdmmc3_bus_clk, "bus-sdmmc3-bus", "ahb1",
		      0x060, BIT(11), 0);
static SUNXI_CCU_GATE(bus_spi0_clk, "bus-spi0", "ahb1",
		      0x060, BIT(20), 0);
static SUNXI_CCU_GATE(bus_spi1_clk, "bus-spi1", "ahb1",
		      0x060, BIT(21), 0);
static SUNXI_CCU_GATE(bus_spi2_clk, "bus-spi2", "ahb1",
		      0x060, BIT(22), 0);
static SUNXI_CCU_GATE(bus_i2s0_clk, "bus-i2s0", "apb1",
		      0x068, BIT(12), 0);
static SUNXI_CCU_GATE(bus_i2s1_clk, "bus-i2s1", "apb1",
		      0x068, BIT(13), 0);
static SUNXI_CCU_GATE(bus_spdif_clk, "bus-spdif", "apb1",
		      0x068, BIT(1), 0);
static SUNXI_CCU_GATE(bus_dsd_clk, "bus-dsd", "apb1",
		      0x068, BIT(2), 0);
static SUNXI_CCU_GATE(bus_dmic_clk, "bus-dmic", "apb1",
		      0x068, BIT(3), 0);
static SUNXI_CCU_GATE(bus_usbohci0_clk, "bus-usbohci0", "ahb1",
		      0x060, BIT(29), 0);
static SUNXI_CCU_GATE(bus_usbehci0_clk, "bus-usbehci0", "ahb1",
		      0x060, BIT(26), 0);
static SUNXI_CCU_GATE(bus_usbotg_clk, "bus-usbotg", "ahb1",
		      0x060, BIT(24), 0);
static SUNXI_CCU_GATE(bus_de_clk, "bus-de", "ahb1",
		      0x064, BIT(12), 0);
static SUNXI_CCU_GATE(bus_ee_clk, "bus-ee", "ahb1",
		      0x064, BIT(13), 0);
static SUNXI_CCU_GATE(bus_tcon0_clk, "bus-tcon0", "ahb1",
		      0x064, BIT(4), 0);
/* csi_s, csi_m and csi_misc share one bus gate, as on the A64. */
static SUNXI_CCU_GATE(bus_csi_clk, "bus-csi", "ahb1",
		      0x064, BIT(8), 0);
static SUNXI_CCU_GATE(bus_adda_clk, "bus-adda", "apb1",
		      0x068, BIT(0), 0);
static SUNXI_CCU_GATE(bus_sdram_clk, "bus-sdram", "ahb1",
		      0x060, BIT(14), 0);
static SUNXI_CCU_GATE(bus_psram_clk, "bus-psram", "ahb1",
		      0x060, BIT(15), 0);
static SUNXI_CCU_GATE(bus_dma_clk, "bus-dma", "ahb1",
		      0x060, BIT(6), 0);
static SUNXI_CCU_GATE(bus_uart0_clk, "bus-uart0", "apb2",
		      0x06c, BIT(16), 0);
static SUNXI_CCU_GATE(bus_uart1_clk, "bus-uart1", "apb2",
		      0x06c, BIT(17), 0);
static SUNXI_CCU_GATE(bus_uart2_clk, "bus-uart2", "apb2",
		      0x06c, BIT(18), 0);
static SUNXI_CCU_GATE(bus_uart3_clk, "bus-uart3", "apb2",
		      0x06c, BIT(19), 0);
static SUNXI_CCU_GATE(bus_uart4_clk, "bus-uart4", "apb2",
		      0x06c, BIT(20), 0);
static SUNXI_CCU_GATE(bus_uart5_clk, "bus-uart5", "apb2",
		      0x06c, BIT(21), 0);
static SUNXI_CCU_GATE(bus_twi0_clk, "bus-twi0", "apb2",
		      0x06c, BIT(0), 0);
static SUNXI_CCU_GATE(bus_twi1_clk, "bus-twi1", "apb2",
		      0x06c, BIT(1), 0);
static SUNXI_CCU_GATE(bus_twi2_clk, "bus-twi2", "apb2",
		      0x06c, BIT(2), 0);
static SUNXI_CCU_GATE(bus_pio_clk, "bus-pio", "apb1",
		      0x068, BIT(5), 0);
static SUNXI_CCU_GATE(bus_gpadc_clk, "bus-gpadc", "apb1",
		      0x068, BIT(10), 0);
static SUNXI_CCU_GATE(bus_keyadc_clk, "bus-keyadc", "apb1",
		      0x068, BIT(9), 0);
/* losc_out: its gate lies outside the CCU window, in another block */

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
	&pll_24m_clk.common,
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
		[CLK_PLL_24M]		= &pll_24m_clk.common.hw,
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

	reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

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
