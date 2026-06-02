// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 * Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * MSM8x60 family (MSM8260/MSM8660/APQ8060) Multimedia Clock Controller driver
 *
 * Split from mmcc-msm8960.c to properly handle MSM8x60-specific clock
 * configurations, particularly the GFX3D reset bits which differ from MSM8960.
 */

#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/mfd/qcom_rpm.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>

#include <dt-bindings/clock/qcom,mmcc-msm8660.h>
#include <dt-bindings/mfd/qcom-rpm.h>
#include <dt-bindings/reset/qcom,mmcc-msm8660.h>

#include "common.h"
#include "clk-regmap.h"
#include "clk-pll.h"
#include "clk-rcg.h"
#include "clk-branch.h"
#include "reset.h"
#include "gdsc.h"

enum {
	P_PXO,
	P_PLL8,
	P_PLL2,
	P_PLL3,
	P_HDMI_PLL,
	P_DSI1_PLL_DSICLK,
	P_DSI2_PLL_DSICLK,
	P_DSI1_PLL_BYTECLK,
	P_DSI2_PLL_BYTECLK,
};

#define F_MN(f, s, _m, _n) { .freq = f, .src = s, .pre_div = 1, .m = _m, .n = _n }
/* Pure divider+source RCG (no MND), e.g. VPE on MSM8660: NS_DIVSRC(15,12,d,2,0,s) */
#define F_DIV(f, s, d) { .freq = f, .src = s, .pre_div = d }

static struct clk_pll pll2 = {
	.l_reg = 0x320,
	.m_reg = 0x324,
	.n_reg = 0x328,
	.config_reg = 0x32c,
	.mode_reg = 0x31c,
	.status_reg = 0x334,
	.status_bit = 16,
	.clkr.hw.init = &(struct clk_init_data){
		.name = "pll2",
		.parent_data = (const struct clk_parent_data[]){
			{ .fw_name = "pxo", .name = "pxo_board" },
		},
		.num_parents = 1,
		.ops = &clk_pll_ops,
	},
};

static const struct parent_map mmcc_pxo_pll8_pll2_map[] = {
	{ P_PXO, 0 },
	{ P_PLL8, 2 },
	{ P_PLL2, 1 }
};

static const struct clk_parent_data mmcc_pxo_pll8_pll2[] = {
	{ .fw_name = "pxo", .name = "pxo_board" },
	{ .fw_name = "pll8_vote", .name = "pll8_vote" },
	{ .hw = &pll2.clkr.hw },
};

static const struct parent_map mmcc_pxo_pll8_pll2_pll3_map[] = {
	{ P_PXO, 0 },
	{ P_PLL8, 2 },
	{ P_PLL2, 1 },
	{ P_PLL3, 3 }
};

static const struct clk_parent_data mmcc_pxo_pll8_pll2_pll3[] = {
	{ .fw_name = "pxo", .name = "pxo_board" },
	{ .fw_name = "pll8_vote", .name = "pll8_vote" },
	{ .hw = &pll2.clkr.hw },
	{ .fw_name = "pll3", .name = "pll3" },
};

static const struct parent_map mmcc_pxo_dsi2_dsi1_map[] = {
	{ P_PXO, 0 },
	{ P_DSI2_PLL_DSICLK, 1 },
	{ P_DSI1_PLL_DSICLK, 3 },
};

static const struct clk_parent_data mmcc_pxo_dsi2_dsi1[] = {
	{ .fw_name = "pxo", .name = "pxo_board" },
	{ .fw_name = "dsi2pll", .name = "dsi2pll" },
	{ .fw_name = "dsi1pll", .name = "dsi1pll" },
};

static const struct parent_map mmcc_pxo_dsi1_dsi2_byte_map[] = {
	{ P_PXO, 0 },
	{ P_DSI1_PLL_BYTECLK, 1 },
	{ P_DSI2_PLL_BYTECLK, 2 },
};

static const struct clk_parent_data mmcc_pxo_dsi1_dsi2_byte[] = {
	{ .fw_name = "pxo", .name = "pxo_board" },
	{ .fw_name = "dsi1pllbyte", .name = "dsi1pllbyte" },
	{ .fw_name = "dsi2pllbyte", .name = "dsi2pllbyte" },
};

static const struct freq_tbl clk_tbl_cam[] = {
	{   6000000, P_PLL8, 4, 1, 16 },
	{   8000000, P_PLL8, 4, 1, 12 },
	{  12000000, P_PLL8, 4, 1,  8 },
	{  16000000, P_PLL8, 4, 1,  6 },
	{  19200000, P_PLL8, 4, 1,  5 },
	{  24000000, P_PLL8, 4, 1,  4 },
	{  32000000, P_PLL8, 4, 1,  3 },
	{  48000000, P_PLL8, 4, 1,  2 },
	{  64000000, P_PLL8, 3, 1,  2 },
	{  96000000, P_PLL8, 4, 0,  0 },
	{ 128000000, P_PLL8, 3, 0,  0 },
	{ }
};

static struct clk_rcg camclk0_src = {
	.ns_reg = 0x0148,
	.md_reg = 0x0144,
	.mn = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 8,
		.reset_in_cc = true,
		.mnctr_mode_shift = 6,
		.n_val_shift = 24,
		.m_val_shift = 8,
		.width = 8,
	},
	.p = {
		.pre_div_shift = 14,
		.pre_div_width = 2,
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.freq_tbl = clk_tbl_cam,
	.clkr = {
		.enable_reg = 0x0140,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "camclk0_src",
			.parent_data = mmcc_pxo_pll8_pll2,
			.num_parents = ARRAY_SIZE(mmcc_pxo_pll8_pll2),
			.ops = &clk_rcg_ops,
		},
	},
};

static struct clk_branch camclk0_clk = {
	.halt_reg = 0x01e8,
	.halt_bit = 15,
	/*
	 * The legacy vendor kernel used halt_reg = NULL for this clock,
	 * meaning it never checked the halt status. The hardware doesn't
	 * properly report the clock state via the halt register. Use
	 * BRANCH_HALT_SKIP to avoid the "status stuck at 'off'" warning.
	 */
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x0140,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "camclk0_clk",
			.parent_hws = (const struct clk_hw*[]){
				&camclk0_src.clkr.hw
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_rcg camclk1_src = {
	.ns_reg = 0x015c,
	.md_reg = 0x0158,
	.mn = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 8,
		.reset_in_cc = true,
		.mnctr_mode_shift = 6,
		.n_val_shift = 24,
		.m_val_shift = 8,
		.width = 8,
	},
	.p = {
		.pre_div_shift = 14,
		.pre_div_width = 2,
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.freq_tbl = clk_tbl_cam,
	.clkr = {
		.enable_reg = 0x0154,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "camclk1_src",
			.parent_data = mmcc_pxo_pll8_pll2,
			.num_parents = ARRAY_SIZE(mmcc_pxo_pll8_pll2),
			.ops = &clk_rcg_ops,
		},
	},
};

static struct clk_branch camclk1_clk = {
	.halt_reg = 0x01e8,
	.halt_bit = 16,
	/* Same issue as camclk0_clk - hardware doesn't report halt status */
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x0154,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "camclk1_clk",
			.parent_hws = (const struct clk_hw*[]){
				&camclk1_src.clkr.hw
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
		},
	},
};

/*
 * CSI clock frequency table for MSM8660.
 * Uses simple pre-divider from PLL8 (384 MHz), NOT MND divider.
 * Reference: legacy vendor kernel clock-8x60.c clk_tbl_csi[]
 */
static const struct freq_tbl clk_tbl_csi[] = {
	{ 192000000, P_PLL8, 2, 0, 0 },
	{ 384000000, P_PLL8, 1, 0, 0 },
	{ }
};

/*
 * CSI clock for MSM8660 uses simple pre-divider, NOT MND divider.
 * CC_REG = 0x0040, NS_REG = 0x0048, no MD register.
 * Pre-divider is in NS_REG bits [15:12], source select in bits [2:0].
 * Reference: legacy vendor kernel clock-8x60.c CLK_CSI macro
 */
static struct clk_rcg csi0_src = {
	.ns_reg = 0x0048,
	/* No md_reg - CSI uses pre-divider only, not MND */
	.p = {
		.pre_div_shift = 12,
		.pre_div_width = 4,
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.freq_tbl = clk_tbl_csi,
	.clkr = {
		.enable_reg = 0x0040,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "csi0_src",
			.parent_data = mmcc_pxo_pll8_pll2,
			.num_parents = ARRAY_SIZE(mmcc_pxo_pll8_pll2),
			.ops = &clk_rcg_ops,
		},
	},
};

static struct clk_branch csi0_clk = {
	.halt_reg = 0x01cc,
	.halt_bit = 13,
	/*
	 * The CSI clock halt status is unreliable when the CSI block is not
	 * actively receiving data. Use BRANCH_HALT_SKIP to avoid timeouts.
	 * This matches the behavior of camclk and vfe_ahb_clk on MSM8660.
	 */
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x0040,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.parent_hws = (const struct clk_hw*[]){
				&csi0_src.clkr.hw
			},
			.num_parents = 1,
			.name = "csi0_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch csi0_phy_clk = {
	.halt_reg = 0x01e8,
	.halt_bit = 9,
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x0040,
		.enable_mask = BIT(8),
		.hw.init = &(struct clk_init_data){
			.parent_hws = (const struct clk_hw*[]){
				&csi0_src.clkr.hw
			},
			.num_parents = 1,
			.name = "csi0_phy_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

/*
 * CSI1 on MSM8660 shares the SAME hardware source-clock register
 * (NS_REG 0x0048, CC_REG 0x0040) as CSI0 -- the legacy vendor
 * kernel has a single CSI_SRC that fans out to both CSI0 and CSI1
 * branches. Expose it under its own CCF name (CSI1_SRC) for binding
 * ABI compatibility, but implement it as a passthrough branch
 * parented on csi0_src so clk_set_rate() always flows to the single
 * underlying RCG. This avoids the race where independent
 * clk_set_rate(csi0_src) / clk_set_rate(csi1_src) would clobber
 * each other's M/N/D and source-mux fields in 0x0048.
 */
static struct clk_branch csi1_src = {
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.hw.init = &(struct clk_init_data){
			.name = "csi1_src",
			.parent_hws = (const struct clk_hw*[]){
				&csi0_src.clkr.hw,
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch csi1_clk = {
	.halt_reg = 0x01cc,
	.halt_bit = 14,
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		/* CSI1 enable is in CSI_CC_REG (0x0040) BIT(7) per legacy vendor kernel */
		.enable_reg = 0x0040,
		.enable_mask = BIT(7),
		.hw.init = &(struct clk_init_data){
			.parent_hws = (const struct clk_hw*[]){
				&csi1_src.clkr.hw
			},
			.num_parents = 1,
			.name = "csi1_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch csi1_phy_clk = {
	.halt_reg = 0x01e8,
	.halt_bit = 10,
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		/* Use CSI_CC_REG (0x0040) like csi0_phy_clk, with BIT(9) for CSI1 */
		.enable_reg = 0x0040,
		.enable_mask = BIT(9),
		.hw.init = &(struct clk_init_data){
			.parent_hws = (const struct clk_hw*[]){
				&csi1_src.clkr.hw
			},
			.num_parents = 1,
			.name = "csi1_phy_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

/*
 * CSI PIX/RDI clock mux support
 *
 * On MSM8660, the csi_pix_clk and csi_rdi_clk can source from either
 * CSI0 or CSI1. The selection is controlled by bits in CSC_CC_REG (0x0058):
 *   - BIT(25): csi_pix_clk source (0=CSI0, 1=CSI1)
 *   - BIT(12): csi_rdi_clk source (0=CSI0, 1=CSI1)
 *
 * This mux is critical for HP TouchPad which uses CSI1 for the front camera.
 */
struct clk_pix_rdi {
	u32 s_reg;
	u32 s_mask;
	struct clk_regmap clkr;
};

#define to_clk_pix_rdi(_hw) \
	container_of(to_clk_regmap(_hw), struct clk_pix_rdi, clkr)

static int pix_rdi_set_parent(struct clk_hw *hw, u8 index)
{
	struct clk_pix_rdi *rdi = to_clk_pix_rdi(hw);
	u32 val;

	/*
	 * The clock framework guarantees that both the current and the
	 * target parent are prepared and enabled when .set_parent is
	 * invoked on a running mux, so the hardware is always switching
	 * between two live sources -- safe for a glitch-free transition.
	 *
	 * Select parent: 0 = CSI0, 1 = CSI1.
	 */
	val = (index == 1) ? rdi->s_mask : 0;
	regmap_update_bits(rdi->clkr.regmap, rdi->s_reg, rdi->s_mask, val);

	/* Wait at least 6 cycles of the slowest source for the mux to settle. */
	udelay(1);

	return 0;
}

static u8 pix_rdi_get_parent(struct clk_hw *hw)
{
	struct clk_pix_rdi *rdi = to_clk_pix_rdi(hw);
	u32 val;

	regmap_read(rdi->clkr.regmap, rdi->s_reg, &val);
	return (val & rdi->s_mask) ? 1 : 0;
}

static const struct clk_ops clk_ops_pix_rdi = {
	.enable = clk_enable_regmap,
	.disable = clk_disable_regmap,
	.is_enabled = clk_is_enabled_regmap,
	.set_parent = pix_rdi_set_parent,
	.get_parent = pix_rdi_get_parent,
	.determine_rate = __clk_mux_determine_rate,
};

static const struct clk_hw *pix_rdi_parents[] = {
	&csi0_clk.clkr.hw,
	&csi1_clk.clkr.hw,
};

static struct clk_pix_rdi csi_pix_clk = {
	.s_reg = 0x0058,
	.s_mask = BIT(25),
	.clkr = {
		.enable_reg = 0x0058,
		.enable_mask = BIT(26),
		.hw.init = &(struct clk_init_data){
			.name = "csi_pix_clk",
			.parent_hws = pix_rdi_parents,
			.num_parents = ARRAY_SIZE(pix_rdi_parents),
			.ops = &clk_ops_pix_rdi,
		},
	},
};

static struct clk_pix_rdi csi_rdi_clk = {
	.s_reg = 0x0058,
	.s_mask = BIT(12),
	.clkr = {
		.enable_reg = 0x0058,
		.enable_mask = BIT(13),
		.hw.init = &(struct clk_init_data){
			.name = "csi_rdi_clk",
			.parent_hws = pix_rdi_parents,
			.num_parents = ARRAY_SIZE(pix_rdi_parents),
			.ops = &clk_ops_pix_rdi,
		},
	},
};

static const struct freq_tbl clk_tbl_csiphytimer[] = {
	{  85330000, P_PLL8, 1, 2, 9 },
	{ 177780000, P_PLL2, 1, 2, 9 },
	{ }
};

static struct clk_rcg csiphytimer_src = {
	.ns_reg = 0x0168,
	.md_reg = 0x0164,
	.mn = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 8,
		.reset_in_cc = true,
		.mnctr_mode_shift = 6,
		.n_val_shift = 24,
		.m_val_shift = 8,
		.width = 8,
	},
	.p = {
		.pre_div_shift = 14,
		.pre_div_width = 2,
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.freq_tbl = clk_tbl_csiphytimer,
	.clkr = {
		.enable_reg = 0x0160,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "csiphytimer_src",
			.parent_data = mmcc_pxo_pll8_pll2,
			.num_parents = ARRAY_SIZE(mmcc_pxo_pll8_pll2),
			.ops = &clk_rcg_ops,
		},
	},
};

static struct clk_branch csiphy0_timer_clk = {
	.halt_reg = 0x01e8,
	.halt_bit = 17,
	.clkr = {
		.enable_reg = 0x0160,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.parent_hws = (const struct clk_hw*[]){
				&csiphytimer_src.clkr.hw
			},
			.num_parents = 1,
			.name = "csiphy0_timer_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch csiphy1_timer_clk = {
	.halt_reg = 0x01e8,
	.halt_bit = 18,
	.clkr = {
		.enable_reg = 0x0160,
		.enable_mask = BIT(9),
		.hw.init = &(struct clk_init_data){
			.parent_hws = (const struct clk_hw*[]){
				&csiphytimer_src.clkr.hw
			},
			.num_parents = 1,
			.name = "csiphy1_timer_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static const struct freq_tbl clk_tbl_dsi[] = {
	{ }
};

static struct clk_rcg dsi1_src = {
	.ns_reg = 0x0054,
	.md_reg = 0x0050,
	.mn = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 7,
		.mnctr_mode_shift = 6,
		.n_val_shift = 24,
		.m_val_shift = 8,
		.width = 8,
	},
	.p = {
		.pre_div_shift = 14,
		.pre_div_width = 2,
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_dsi2_dsi1_map,
	},
	.freq_tbl = clk_tbl_dsi,
	.clkr = {
		.enable_reg = 0x004c,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "dsi1_src",
			.parent_data = mmcc_pxo_dsi2_dsi1,
			.num_parents = ARRAY_SIZE(mmcc_pxo_dsi2_dsi1),
			.ops = &clk_rcg_bypass_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch dsi1_clk = {
	.halt_reg = 0x01d0,
	.halt_bit = 2,
	.clkr = {
		.enable_reg = 0x004c,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "dsi1_clk",
			.parent_hws = (const struct clk_hw*[]){
				&dsi1_src.clkr.hw
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_rcg dsi1_byte_src = {
	.ns_reg = 0x00b0,
	.p = {
		.pre_div_shift = 12,
		.pre_div_width = 4,
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_dsi1_dsi2_byte_map,
	},
	.clkr = {
		.enable_reg = 0x0090,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "dsi1_byte_src",
			.parent_data = mmcc_pxo_dsi1_dsi2_byte,
			.num_parents = ARRAY_SIZE(mmcc_pxo_dsi1_dsi2_byte),
			.ops = &clk_rcg_bypass_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch dsi1_byte_clk = {
	.halt_reg = 0x01cc,
	.halt_bit = 21,
	.clkr = {
		.enable_reg = 0x0090,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "dsi1_byte_clk",
			.parent_hws = (const struct clk_hw*[]){
				&dsi1_byte_src.clkr.hw
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_rcg dsi1_esc_src = {
	.ns_reg = 0x00b8,
	.p = {
		.pre_div_shift = 12,
		.pre_div_width = 4,
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_dsi1_dsi2_byte_map,
	},
	.clkr = {
		.enable_reg = 0x00b4,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "dsi1_esc_src",
			.parent_data = mmcc_pxo_dsi1_dsi2_byte,
			.num_parents = ARRAY_SIZE(mmcc_pxo_dsi1_dsi2_byte),
			.ops = &clk_rcg_bypass_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch dsi1_esc_clk = {
	.halt_reg = 0x01e8,
	.halt_bit = 1,
	.clkr = {
		.enable_reg = 0x00b4,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "dsi1_esc_clk",
			.parent_hws = (const struct clk_hw*[]){
				&dsi1_esc_src.clkr.hw
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_rcg dsi1_pixel_src = {
	.ns_reg = 0x0138,
	.md_reg = 0x0134,
	.mn = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 7,
		.mnctr_mode_shift = 6,
		.n_val_shift = 16,
		.m_val_shift = 8,
		.width = 8,
	},
	.p = {
		.pre_div_shift = 12,
		.pre_div_width = 4,
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_dsi2_dsi1_map,
	},
	.clkr = {
		.enable_reg = 0x0130,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "dsi1_pixel_src",
			.parent_data = mmcc_pxo_dsi2_dsi1,
			.num_parents = ARRAY_SIZE(mmcc_pxo_dsi2_dsi1),
			.ops = &clk_rcg_ops,
		},
	},
};

static struct clk_branch dsi1_pixel_clk = {
	.halt_reg = 0x01d0,
	.halt_bit = 6,
	.clkr = {
		.enable_reg = 0x0130,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "dsi1_pixel_clk",
			.parent_hws = (const struct clk_hw*[]){
				&dsi1_pixel_src.clkr.hw
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static const struct freq_tbl clk_tbl_gfx2d[] = {
	F_MN( 27000000, P_PXO,  1,  0),
	F_MN( 48000000, P_PLL8, 1,  8),
	F_MN( 54857000, P_PLL8, 1,  7),
	F_MN( 64000000, P_PLL8, 1,  6),
	F_MN( 76800000, P_PLL8, 1,  5),
	F_MN( 96000000, P_PLL8, 1,  4),
	F_MN(128000000, P_PLL8, 1,  3),
	F_MN(145455000, P_PLL2, 2, 11),
	F_MN(160000000, P_PLL2, 1,  5),
	F_MN(177778000, P_PLL2, 2,  9),
	F_MN(200000000, P_PLL2, 1,  4),
	F_MN(228571000, P_PLL2, 2,  7),
	{ }
};

static struct clk_dyn_rcg gfx2d0_src = {
	.ns_reg[0] = 0x0070,
	.ns_reg[1] = 0x0070,
	.md_reg[0] = 0x0064,
	.md_reg[1] = 0x0068,
	.bank_reg = 0x0060,
	.mn[0] = {
		.mnctr_en_bit = 8,
		.mnctr_reset_bit = 25,
		.mnctr_mode_shift = 9,
		.n_val_shift = 20,
		.m_val_shift = 4,
		.width = 4,
	},
	.mn[1] = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 24,
		.mnctr_mode_shift = 6,
		.n_val_shift = 16,
		.m_val_shift = 4,
		.width = 4,
	},
	.s[0] = {
		.src_sel_shift = 3,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.s[1] = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.mux_sel_bit = 11,
	.freq_tbl = clk_tbl_gfx2d,
	.clkr = {
		.enable_reg = 0x0060,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "gfx2d0_src",
			.parent_data = mmcc_pxo_pll8_pll2,
			.num_parents = ARRAY_SIZE(mmcc_pxo_pll8_pll2),
			.ops = &clk_dyn_rcg_ops,
		},
	},
};

static struct clk_branch gfx2d0_clk = {
	.halt_reg = 0x01c8,
	.halt_bit = 9,
	.clkr = {
		.enable_reg = 0x0060,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gfx2d0_clk",
			.parent_hws = (const struct clk_hw*[]){
				&gfx2d0_src.clkr.hw
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_dyn_rcg gfx2d1_src = {
	.ns_reg[0] = 0x007c,
	.ns_reg[1] = 0x007c,
	.md_reg[0] = 0x0078,
	.md_reg[1] = 0x006c,
	.bank_reg = 0x0074,
	.mn[0] = {
		.mnctr_en_bit = 8,
		.mnctr_reset_bit = 25,
		.mnctr_mode_shift = 9,
		.n_val_shift = 20,
		.m_val_shift = 4,
		.width = 4,
	},
	.mn[1] = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 24,
		.mnctr_mode_shift = 6,
		.n_val_shift = 16,
		.m_val_shift = 4,
		.width = 4,
	},
	.s[0] = {
		.src_sel_shift = 3,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.s[1] = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.mux_sel_bit = 11,
	.freq_tbl = clk_tbl_gfx2d,
	.clkr = {
		.enable_reg = 0x0074,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "gfx2d1_src",
			.parent_data = mmcc_pxo_pll8_pll2,
			.num_parents = ARRAY_SIZE(mmcc_pxo_pll8_pll2),
			.ops = &clk_dyn_rcg_ops,
		},
	},
};

static struct clk_branch gfx2d1_clk = {
	.halt_reg = 0x01c8,
	.halt_bit = 14,
	.clkr = {
		.enable_reg = 0x0074,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gfx2d1_clk",
			.parent_hws = (const struct clk_hw*[]){
				&gfx2d1_src.clkr.hw
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static const struct freq_tbl clk_tbl_gfx3d[] = {
	F_MN( 27000000, P_PXO,  1,  0),
	F_MN( 48000000, P_PLL8, 1,  8),
	F_MN( 54857000, P_PLL8, 1,  7),
	F_MN( 64000000, P_PLL8, 1,  6),
	F_MN( 76800000, P_PLL8, 1,  5),
	F_MN( 96000000, P_PLL8, 1,  4),
	F_MN(128000000, P_PLL8, 1,  3),
	F_MN(145455000, P_PLL2, 2, 11),
	F_MN(160000000, P_PLL2, 1,  5),
	F_MN(177778000, P_PLL2, 2,  9),
	F_MN(200000000, P_PLL2, 1,  4),
	F_MN(228571000, P_PLL2, 2,  7),
	F_MN(266667000, P_PLL2, 1,  3),
	F_MN(320000000, P_PLL2, 2,  5),
	{ }
};

/*
 * MSM8x60-specific GFX3D clocks
 *
 * MSM8660 uses different reset bits for the GFX3D banked MND divider:
 *   - Bank 0: mnctr_reset_bit = 23 (MSM8960 uses 25)
 *   - Bank 1: mnctr_reset_bit = 22 (MSM8960 uses 24)
 *
 * Values cross-checked against three independent downstream MSM8660
 * sources (HP webOS Opal, Samsung MSM8660, HTC MSM8660 vendor trees)
 * which all program these exact bit positions during the GFX3D power-
 * domain entry/exit sequence on register 0x0210. Empirical evidence:
 * GPU comes up cleanly and survives multiple power-collapse cycles on
 * MSM8660-based HP TouchPad with this driver, so the bit positions
 * are correct in practice. No public datasheet citation is available
 * (QCT did not release MSM8660 register documentation), but the
 * cross-vendor agreement and on-hardware verification are the
 * strongest evidence the kernel community can have for this family.
 */
static struct clk_dyn_rcg gfx3d_src = {
	.ns_reg[0] = 0x008c,
	.ns_reg[1] = 0x008c,
	.md_reg[0] = 0x0084,
	.md_reg[1] = 0x0088,
	.bank_reg = 0x0080,
	.mn[0] = {
		.mnctr_en_bit = 8,
		.mnctr_reset_bit = 23,
		.mnctr_mode_shift = 9,
		.n_val_shift = 18,
		.m_val_shift = 4,
		.width = 4,
	},
	.mn[1] = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 22,
		.mnctr_mode_shift = 6,
		.n_val_shift = 14,
		.m_val_shift = 4,
		.width = 4,
	},
	.s[0] = {
		.src_sel_shift = 3,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.s[1] = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.mux_sel_bit = 11,
	.freq_tbl = clk_tbl_gfx3d,
	.clkr = {
		.enable_reg = 0x0080,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "gfx3d_src",
			.parent_data = mmcc_pxo_pll8_pll2,
			.num_parents = ARRAY_SIZE(mmcc_pxo_pll8_pll2),
			.ops = &clk_dyn_rcg_ops,
		},
	},
};

static struct clk_branch gfx3d_clk = {
	.halt_reg = 0x01c8,
	.halt_bit = 4,
	.clkr = {
		.enable_reg = 0x0080,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gfx3d_clk",
			.parent_hws = (const struct clk_hw*[]){
				&gfx3d_src.clkr.hw
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static const struct freq_tbl clk_tbl_ijpeg[] = {
	F_MN( 27000000, P_PXO,  1, 0),
	F_MN( 36570000, P_PLL8, 2, 21),
	F_MN( 54860000, P_PLL8, 1, 7),
	F_MN( 96000000, P_PLL8, 1, 4),
	F_MN(109710000, P_PLL8, 2, 7),
	F_MN(128000000, P_PLL8, 1, 3),
	F_MN(153600000, P_PLL8, 2, 5),
	F_MN(200000000, P_PLL2, 1, 4),
	F_MN(228571000, P_PLL2, 2, 7),
	{ }
};

static struct clk_rcg ijpeg_src = {
	.ns_reg = 0x00a0,
	.md_reg = 0x009c,
	.mn = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 7,
		.mnctr_mode_shift = 6,
		.n_val_shift = 16,
		.m_val_shift = 8,
		.width = 8,
	},
	.p = {
		.pre_div_shift = 12,
		.pre_div_width = 2,
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.freq_tbl = clk_tbl_ijpeg,
	.clkr = {
		.enable_reg = 0x0098,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "ijpeg_src",
			.parent_data = mmcc_pxo_pll8_pll2,
			.num_parents = ARRAY_SIZE(mmcc_pxo_pll8_pll2),
			.ops = &clk_rcg_ops,
		},
	},
};

static struct clk_branch ijpeg_clk = {
	.halt_reg = 0x01c8,
	.halt_bit = 24,
	.clkr = {
		.enable_reg = 0x0098,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "ijpeg_clk",
			.parent_hws = (const struct clk_hw*[]){
				&ijpeg_src.clkr.hw
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static const struct freq_tbl clk_tbl_jpegd[] = {
	F_MN( 64000000, P_PLL8, 1, 6),
	F_MN( 76800000, P_PLL8, 1, 5),
	F_MN( 96000000, P_PLL8, 1, 4),
	F_MN(160000000, P_PLL2, 1, 5),
	F_MN(200000000, P_PLL2, 1, 4),
	{ }
};

static struct clk_rcg jpegd_src = {
	.ns_reg = 0x00ac,
	.p = {
		.pre_div_shift = 12,
		.pre_div_width = 2,
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.freq_tbl = clk_tbl_jpegd,
	.clkr = {
		.enable_reg = 0x00a4,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "jpegd_src",
			.parent_data = mmcc_pxo_pll8_pll2,
			.num_parents = ARRAY_SIZE(mmcc_pxo_pll8_pll2),
			.ops = &clk_rcg_ops,
		},
	},
};

static struct clk_branch jpegd_clk = {
	.halt_reg = 0x01c8,
	.halt_bit = 19,
	.clkr = {
		.enable_reg = 0x00a4,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "jpegd_clk",
			.parent_hws = (const struct clk_hw*[]){
				&jpegd_src.clkr.hw
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static const struct freq_tbl clk_tbl_mdp[] = {
	F_MN(  9600000, P_PLL8, 1, 40),
	F_MN( 13710000, P_PLL8, 1, 28),
	F_MN( 27000000, P_PXO,  1,  0),
	F_MN( 29540000, P_PLL8, 1, 13),
	F_MN( 34910000, P_PLL8, 1, 11),
	F_MN( 38400000, P_PLL8, 1, 10),
	F_MN( 59080000, P_PLL8, 2, 13),
	F_MN( 76800000, P_PLL8, 1,  5),
	F_MN( 85330000, P_PLL8, 2,  9),
	F_MN( 96000000, P_PLL8, 1,  4),
	F_MN(128000000, P_PLL8, 1,  3),
	F_MN(160000000, P_PLL2, 1,  5),
	F_MN(177780000, P_PLL2, 2,  9),
	F_MN(200000000, P_PLL2, 1,  4),
	{ }
};

static struct clk_dyn_rcg mdp_src = {
	.ns_reg[0] = 0x00d0,
	.ns_reg[1] = 0x00d0,
	.md_reg[0] = 0x00c4,
	.md_reg[1] = 0x00c8,
	.bank_reg = 0x00c0,
	.mn[0] = {
		.mnctr_en_bit = 8,
		.mnctr_reset_bit = 31,
		.mnctr_mode_shift = 9,
		.n_val_shift = 22,
		.m_val_shift = 8,
		.width = 8,
	},
	.mn[1] = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 30,
		.mnctr_mode_shift = 6,
		.n_val_shift = 14,
		.m_val_shift = 8,
		.width = 8,
	},
	.s[0] = {
		.src_sel_shift = 3,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.s[1] = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.mux_sel_bit = 11,
	.freq_tbl = clk_tbl_mdp,
	.clkr = {
		.enable_reg = 0x00c0,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "mdp_src",
			.parent_data = mmcc_pxo_pll8_pll2,
			.num_parents = ARRAY_SIZE(mmcc_pxo_pll8_pll2),
			.ops = &clk_dyn_rcg_ops,
		},
	},
};

static struct clk_branch mdp_clk = {
	.halt_reg = 0x01d0,
	.halt_bit = 10,
	.clkr = {
		.enable_reg = 0x00c0,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "mdp_clk",
			.parent_hws = (const struct clk_hw*[]){
				&mdp_src.clkr.hw
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch mdp_lut_clk = {
	.halt_reg = 0x01e8,
	.halt_bit = 13,
	.clkr = {
		.enable_reg = 0x016c,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.parent_hws = (const struct clk_hw*[]){
				&mdp_src.clkr.hw
			},
			.num_parents = 1,
			.name = "mdp_lut_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch mdp_vsync_clk = {
	.halt_reg = 0x01cc,
	.halt_bit = 22,
	.clkr = {
		.enable_reg = 0x0058,
		.enable_mask = BIT(6),
		.hw.init = &(struct clk_init_data){
			.name = "mdp_vsync_clk",
			.parent_data = (const struct clk_parent_data[]){
				{ .fw_name = "pxo", .name = "pxo_board" },
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
		},
	},
};

/*
 * MDP pixel clock (LCDC primary panel path).
 *
 * MSM8x60 uses the MD16 register format: the MD register holds M in bits
 * [31:16] and ~N in bits [15:0]; the NS register holds ~(N - M) in bits
 * [31:16]. mmcc-msm8960 by contrast uses the MD8 format (M and ~N
 * packed as 8-bit halves of the MD register and N in NS bits [23:16]).
 * The common clk_rcg_ops covers both layouts -- configure width = 16
 * and m_val_shift = 16 to select MD16. NS register sits at 0x00dc on
 * MSM8x60 (vs 0x00e0 on MSM8960).
 */
static const struct freq_tbl clk_tbl_mdp_pixel[] = {
	{  25600000, P_PLL8, 3,   1,    5 },	/* 384 / 3 * 1/5     =  25.6 MHz */
	{  27000000, P_PXO,  1,   0,    0 },	/* PXO direct        =  27 MHz */
	{  42667000, P_PLL8, 1,   1,    9 },	/* 384 * 1/9         =  42.667 MHz */
	{  43192000, P_PLL8, 1,  64,  569 },	/* 384 * 64/569      =  43.192 MHz */
	{  48000000, P_PLL8, 4,   1,    2 },	/* 384 / 4 * 1/2     =  48 MHz */
	{  53990000, P_PLL8, 2, 169,  601 },	/* 384 / 2 * 169/601 =  53.99 MHz */
	{  64000000, P_PLL8, 3,   1,    2 },	/* 384 / 3 * 1/2     =  64 MHz */
	{  69300000, P_PLL8, 1, 231, 1280 },	/* 384 * 231/1280    =  69.3 MHz - HP TouchPad panel */
	{  76800000, P_PLL8, 1,   1,    5 },	/* 384 * 1/5         =  76.8 MHz */
	{  85333000, P_PLL8, 1,   2,    9 },	/* 384 * 2/9         =  85.333 MHz */
	{  96000000, P_PLL8, 4,   0,    0 },	/* 384 / 4           =  96 MHz */
	{ 100030000, P_PLL8, 2, 211,  405 },	/* 384 / 2 * 211/405 = 100.03 MHz */
	{ 106500000, P_PLL8, 1,  71,  256 },	/* 384 * 71/256      = 106.5 MHz */
	{ 109714000, P_PLL8, 1,   2,    7 },	/* 384 * 2/7         = 109.714 MHz */
	{ 128000000, P_PLL8, 3,   0,    0 },	/* 384 / 3           = 128 MHz */
	{ }
};

static struct clk_rcg mdp_pixel_src = {
	.ns_reg = 0x00dc,
	.md_reg = 0x00d8,
	.mn = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 7,
		.mnctr_mode_shift = 6,
		.n_val_shift = 16,
		.m_val_shift = 16,	/* MD16: M at MD[31:16], ~N at MD[15:0] */
		.width = 16,
	},
	.p = {
		.pre_div_shift = 14,
		.pre_div_width = 2,
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.freq_tbl = clk_tbl_mdp_pixel,
	.clkr = {
		.enable_reg = 0x00d4,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "mdp_pixel_src",
			.parent_data = mmcc_pxo_pll8_pll2,
			.num_parents = ARRAY_SIZE(mmcc_pxo_pll8_pll2),
			.ops = &clk_rcg_ops,
		},
	},
};

static struct clk_branch mdp_pixel_clk = {
	.halt_reg = 0x01d0,
	.halt_bit = 23,
	.clkr = {
		.enable_reg = 0x00d4,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "mdp_pixel_clk",
			.parent_hws = (const struct clk_hw*[]){
				&mdp_pixel_src.clkr.hw,
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch mdp_lcdc_clk = {
	.halt_reg = 0x01d0,
	.halt_bit = 21,
	.clkr = {
		.enable_reg = 0x00d4,
		.enable_mask = BIT(8),
		.hw.init = &(struct clk_init_data){
			.name = "mdp_lcdc_clk",
			.parent_hws = (const struct clk_hw*[]){
				&mdp_pixel_src.clkr.hw,
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static const struct freq_tbl clk_tbl_rot[] = {
	F_MN( 27000000, P_PXO,  1,  0),
	F_MN( 29540000, P_PLL8, 1, 13),
	F_MN( 32000000, P_PLL8, 1, 12),
	F_MN( 38400000, P_PLL8, 1, 10),
	F_MN( 48000000, P_PLL8, 1,  8),
	F_MN( 54860000, P_PLL8, 1,  7),
	F_MN( 64000000, P_PLL8, 1,  6),
	F_MN( 76800000, P_PLL8, 1,  5),
	F_MN( 96000000, P_PLL8, 1,  4),
	F_MN(100000000, P_PLL2, 1,  8),
	F_MN(114290000, P_PLL2, 2, 14),
	F_MN(128000000, P_PLL8, 1,  3),
	F_MN(133330000, P_PLL2, 1,  6),
	F_MN(160000000, P_PLL2, 1,  5),
	{ }
};

static struct clk_dyn_rcg rot_src = {
	.ns_reg[0] = 0x00e8,
	.ns_reg[1] = 0x00e8,
	.md_reg[0] = 0x00e0,
	.md_reg[1] = 0x00e4,
	.bank_reg = 0x00e8,
	.mn[0] = {
		.mnctr_en_bit = 8,
		.mnctr_reset_bit = 25,
		.mnctr_mode_shift = 9,
		.n_val_shift = 22,
		.m_val_shift = 8,
		.width = 8,
	},
	.mn[1] = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 24,
		.mnctr_mode_shift = 6,
		.n_val_shift = 14,
		.m_val_shift = 8,
		.width = 8,
	},
	.s[0] = {
		.src_sel_shift = 3,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.s[1] = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.mux_sel_bit = 11,
	.freq_tbl = clk_tbl_rot,
	.clkr = {
		.enable_reg = 0x00e0,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "rot_src",
			.parent_data = mmcc_pxo_pll8_pll2,
			.num_parents = ARRAY_SIZE(mmcc_pxo_pll8_pll2),
			.ops = &clk_dyn_rcg_ops,
		},
	},
};

static struct clk_branch rot_clk = {
	.halt_reg = 0x01d0,
	.halt_bit = 15,
	/*
	 * The rotator core branch is fed through the 'rot' GDSC, which uses
	 * the legacy footswitch sequence. During rotator_runtime_suspend the
	 * clock is disabled before genpd collapses the footswitch, and the
	 * branch halt status does not assert while the legacy footswitch is
	 * still powered — so clk_branch_toggle() times out with "rot_clk
	 * status stuck at 'on'". The enable bit is still cleared (the clock
	 * is gated); only the readback is unreliable, exactly like the
	 * rot_axi / gfx3d_axi / vcodec_axi branches below. Skip the poll.
	 */
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x00e0,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "rot_clk",
			.parent_hws = (const struct clk_hw*[]){
				&rot_src.clkr.hw
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

#define F_TV(f, s, p_r, _m, _n) \
	{ \
		.freq = f, \
		.src = s, \
		.pre_div = p_r, \
		.m = _m, \
		.n = _n, \
	}

static const struct freq_tbl clk_tbl_tv[] = {
	F_TV( 25200000, P_HDMI_PLL, 1, 0, 0),
	F_TV( 27000000, P_HDMI_PLL, 1, 0, 0),
	F_TV( 27030000, P_HDMI_PLL, 1, 0, 0),
	F_TV( 74250000, P_HDMI_PLL, 1, 0, 0),
	F_TV(148500000, P_HDMI_PLL, 1, 0, 0),
	{ }
};

static const struct parent_map mmcc_pxo_hdmi_map[] = {
	{ P_PXO, 0 },
	{ P_HDMI_PLL, 3 }
};

static const struct clk_parent_data mmcc_pxo_hdmi[] = {
	{ .fw_name = "pxo", .name = "pxo_board" },
	{ .fw_name = "hdmipll", .name = "hdmi_pll" },
};

static struct clk_rcg tv_src = {
	.ns_reg = 0x00f4,
	.md_reg = 0x00f0,
	.mn = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 7,
		.mnctr_mode_shift = 6,
		.n_val_shift = 16,
		.m_val_shift = 8,
		.width = 8,
	},
	.p = {
		.pre_div_shift = 14,
		.pre_div_width = 2,
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_hdmi_map,
	},
	.freq_tbl = clk_tbl_tv,
	.clkr = {
		.enable_reg = 0x00ec,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "tv_src",
			.parent_data = mmcc_pxo_hdmi,
			.num_parents = ARRAY_SIZE(mmcc_pxo_hdmi),
			.ops = &clk_rcg_bypass_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch tv_enc_clk = {
	.halt_reg = 0x01d4,
	.halt_bit = 9,
	.clkr = {
		.enable_reg = 0x00ec,
		.enable_mask = BIT(8),
		.hw.init = &(struct clk_init_data){
			.parent_hws = (const struct clk_hw*[]){
				&tv_src.clkr.hw
			},
			.num_parents = 1,
			.name = "tv_enc_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch tv_dac_clk = {
	.halt_reg = 0x01d4,
	.halt_bit = 10,
	.clkr = {
		.enable_reg = 0x00ec,
		.enable_mask = BIT(10),
		.hw.init = &(struct clk_init_data){
			.parent_hws = (const struct clk_hw*[]){
				&tv_src.clkr.hw
			},
			.num_parents = 1,
			.name = "tv_dac_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch mdp_tv_clk = {
	.halt_reg = 0x01d4,
	.halt_bit = 12,
	.clkr = {
		.enable_reg = 0x00ec,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.parent_hws = (const struct clk_hw*[]){
				&tv_src.clkr.hw
			},
			.num_parents = 1,
			.name = "mdp_tv_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch hdmi_tv_clk = {
	.halt_reg = 0x01d4,
	.halt_bit = 11,
	.clkr = {
		.enable_reg = 0x00ec,
		.enable_mask = BIT(12),
		.hw.init = &(struct clk_init_data){
			.parent_hws = (const struct clk_hw*[]){
				&tv_src.clkr.hw
			},
			.num_parents = 1,
			.name = "hdmi_tv_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch hdmi_app_clk = {
	.halt_reg = 0x01cc,
	.halt_bit = 25,
	.clkr = {
		.enable_reg = 0x005c,
		.enable_mask = BIT(11),
		.hw.init = &(struct clk_init_data){
			.parent_data = (const struct clk_parent_data[]){
				{ .fw_name = "pxo", .name = "pxo_board" },
			},
			.num_parents = 1,
			.name = "hdmi_app_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static const struct freq_tbl clk_tbl_vcodec[] = {
	F_MN( 27000000, P_PXO,  1,  0),
	F_MN( 32000000, P_PLL8, 1, 12),
	F_MN( 48000000, P_PLL8, 1,  8),
	F_MN( 54860000, P_PLL8, 1,  7),
	F_MN( 96000000, P_PLL8, 1,  4),
	F_MN(133330000, P_PLL2, 1,  6),
	F_MN(200000000, P_PLL2, 1,  4),
	F_MN(228570000, P_PLL2, 2,  7),
	{ }
};

static struct clk_dyn_rcg vcodec_src = {
	.ns_reg[0] = 0x0100,
	.ns_reg[1] = 0x0100,
	.md_reg[0] = 0x00fc,
	.md_reg[1] = 0x0128,
	.bank_reg = 0x00f8,
	.mn[0] = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 31,
		.mnctr_mode_shift = 6,
		.n_val_shift = 11,
		.m_val_shift = 8,
		.width = 8,
	},
	.mn[1] = {
		.mnctr_en_bit = 10,
		.mnctr_reset_bit = 30,
		.mnctr_mode_shift = 11,
		.n_val_shift = 19,
		.m_val_shift = 8,
		.width = 8,
	},
	.s[0] = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.s[1] = {
		.src_sel_shift = 14,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.mux_sel_bit = 13,
	.freq_tbl = clk_tbl_vcodec,
	.clkr = {
		.enable_reg = 0x00f8,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "vcodec_src",
			.parent_data = mmcc_pxo_pll8_pll2,
			.num_parents = ARRAY_SIZE(mmcc_pxo_pll8_pll2),
			.ops = &clk_dyn_rcg_ops,
		},
	},
};

static struct clk_branch vcodec_clk = {
	.halt_reg = 0x01d0,
	.halt_bit = 29,
	.clkr = {
		.enable_reg = 0x00f8,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "vcodec_clk",
			.parent_hws = (const struct clk_hw*[]){
				&vcodec_src.clkr.hw
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

/*
 * VPE source is a simple divider+source RCG (no MND), per legacy clock-8x60.c
 * (NS_DIVSRC(15,12,d,2,0,s), set_rate_nop). The divisor lives in NS[15:12]
 * (4 bits); 160 MHz = PLL2 / 5. Using F_MN here silently forces pre_div=1 and
 * drops the divisor into the nonexistent MND, leaving vpe_src grossly
 * misclocked -> the first VPE register access hangs the AXI bus.
 */
static const struct freq_tbl clk_tbl_vpe[] = {
	F_DIV( 27000000, P_PXO,   1),
	F_DIV( 34909000, P_PLL8, 11),
	F_DIV( 38400000, P_PLL8, 10),
	F_DIV( 64000000, P_PLL8,  6),
	F_DIV( 76800000, P_PLL8,  5),
	F_DIV( 96000000, P_PLL8,  4),
	F_DIV(100000000, P_PLL2,  8),
	F_DIV(160000000, P_PLL2,  5),
	{ }
};

static struct clk_rcg vpe_src = {
	.ns_reg = 0x0118,
	.p = {
		.pre_div_shift = 12,
		.pre_div_width = 4,	/* NS[15:12], legacy BM(15,12); holds /11 */
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.freq_tbl = clk_tbl_vpe,
	.clkr = {
		.enable_reg = 0x0110,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "vpe_src",
			.parent_data = mmcc_pxo_pll8_pll2,
			.num_parents = ARRAY_SIZE(mmcc_pxo_pll8_pll2),
			.ops = &clk_rcg_ops,
		},
	},
};

static struct clk_branch vpe_clk = {
	.halt_reg = 0x01c8,
	.halt_bit = 28,
	/*
	 * Same as rot_clk: the VPE core branch runs through the legacy-
	 * footswitch 'vpe' GDSC, so its halt status is unreliable at
	 * runtime-suspend disable time ("vpe_clk status stuck at 'on'").
	 * The enable bit still gates the clock; skip the readback poll.
	 */
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x0110,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "vpe_clk",
			.parent_hws = (const struct clk_hw*[]){
				&vpe_src.clkr.hw
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static const struct freq_tbl clk_tbl_vfe[] = {
	F_MN( 13960000, P_PLL8, 2, 55),
	F_MN( 27000000, P_PXO,  1,  0),
	F_MN( 36570000, P_PLL8, 2, 21),
	F_MN( 38400000, P_PLL8, 2, 20),
	F_MN( 45180000, P_PLL8, 2, 17),
	F_MN( 48000000, P_PLL8, 2, 16),
	F_MN( 54860000, P_PLL8, 1,  7),
	F_MN( 64000000, P_PLL8, 2, 12),
	F_MN( 76800000, P_PLL8, 1,  5),
	F_MN( 96000000, P_PLL8, 2,  8),
	F_MN(109710000, P_PLL8, 2,  7),
	F_MN(128000000, P_PLL8, 1,  3),
	F_MN(153600000, P_PLL8, 2,  5),
	F_MN(200000000, P_PLL2, 2,  8),
	F_MN(228570000, P_PLL2, 2,  7),
	F_MN(266667000, P_PLL2, 1,  3),
	{ }
};

static struct clk_rcg vfe_src = {
	.ns_reg = 0x010c,
	.md_reg = 0x0108,
	.mn = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 7,
		.mnctr_mode_shift = 6,
		.n_val_shift = 16,
		.m_val_shift = 8,
		.width = 8,
	},
	.p = {
		.pre_div_shift = 10,
		.pre_div_width = 2,
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.freq_tbl = clk_tbl_vfe,
	.clkr = {
		.enable_reg = 0x0104,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "vfe_src",
			.parent_data = mmcc_pxo_pll8_pll2,
			.num_parents = ARRAY_SIZE(mmcc_pxo_pll8_pll2),
			.ops = &clk_rcg_ops,
		},
	},
};

static struct clk_branch vfe_clk = {
	.halt_reg = 0x01cc,
	.halt_bit = 6,
	/*
	 * VFE clock halt status is unreliable when VFE is not actively
	 * processing data. Use BRANCH_HALT_SKIP to avoid timeouts during
	 * clock enable before camera streaming starts.
	 */
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x0104,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "vfe_clk",
			.parent_hws = (const struct clk_hw*[]){
				&vfe_src.clkr.hw
			},
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch vfe_csi0_clk = {
	.halt_reg = 0x01cc,
	.halt_bit = 7,
	/*
	 * The VFE CSI clock halt status is unreliable when the CSI block
	 * is not actively receiving data. Use BRANCH_HALT_SKIP to avoid
	 * timeouts during clock enable.
	 */
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x0104,
		.enable_mask = BIT(12),
		.hw.init = &(struct clk_init_data){
			.parent_hws = (const struct clk_hw*[]){
				&vfe_src.clkr.hw
			},
			.num_parents = 1,
			.name = "vfe_csi0_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

/*
 * VFE CSI1 clock - enables CSI1 to VFE data path.
 * Legacy kernel had separate CSI0_VFE (BIT 12) and CSI1_VFE (BIT 10).
 */
static struct clk_branch vfe_csi1_clk = {
	.halt_reg = 0x01cc,
	.halt_bit = 8,
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x0104,
		.enable_mask = BIT(10),
		.hw.init = &(struct clk_init_data){
			.parent_hws = (const struct clk_hw*[]){
				&vfe_src.clkr.hw
			},
			.num_parents = 1,
			.name = "vfe_csi1_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch gmem_axi_clk = {
	.halt_reg = 0x01d8,
	.halt_bit = 6,
	.clkr = {
		.enable_reg = 0x0018,
		.enable_mask = BIT(24),
		.hw.init = &(struct clk_init_data){
			.name = "gmem_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch ijpeg_axi_clk = {
	.halt_reg = 0x01d8,
	.halt_bit = 4,
	.clkr = {
		.enable_reg = 0x0018,
		.enable_mask = BIT(21),
		.hw.init = &(struct clk_init_data){
			.name = "ijpeg_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch mmss_imem_axi_clk = {
	.halt_reg = 0x01d8,
	.halt_bit = 7,
	.clkr = {
		.enable_reg = 0x0018,
		.enable_mask = BIT(22),
		.hw.init = &(struct clk_init_data){
			.name = "mmss_imem_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch jpegd_axi_clk = {
	.halt_reg = 0x01d8,
	.halt_bit = 5,
	.clkr = {
		.enable_reg = 0x0018,
		.enable_mask = BIT(25),
		.hw.init = &(struct clk_init_data){
			.name = "jpegd_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch vcodec_axi_clk = {
	.halt_reg = 0x01d8,
	.halt_bit = 3,
	.clkr = {
		.enable_reg = 0x0018,
		.enable_mask = BIT(19),
		.hw.init = &(struct clk_init_data){
			.name = "vcodec_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch vcodec_axi_a_clk = {
	.halt_reg = 0x01e8,
	.halt_bit = 26,
	.clkr = {
		.enable_reg = 0x0020,
		.enable_mask = BIT(25),
		.hw.init = &(struct clk_init_data){
			.name = "vcodec_axi_a_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch vcodec_axi_b_clk = {
	.halt_reg = 0x01e8,
	.halt_bit = 25,
	.clkr = {
		.enable_reg = 0x0020,
		.enable_mask = BIT(26),
		.hw.init = &(struct clk_init_data){
			.name = "vcodec_axi_b_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch vfe_axi_clk = {
	.halt_reg = 0x01d8,
	.halt_bit = 0,
	/*
	 * VFE clocks don't properly report halt status when the VFE power
	 * domain is off. Skip halt checking to avoid enable failures.
	 */
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x0018,
		.enable_mask = BIT(18),
		.hw.init = &(struct clk_init_data){
			.name = "vfe_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch vpe_axi_clk = {
	.halt_reg = 0x01d8,
	.halt_bit = 1,
	.clkr = {
		.enable_reg = 0x0018,
		.enable_mask = BIT(26),
		.hw.init = &(struct clk_init_data){
			.name = "vpe_axi_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_IS_CRITICAL,
		},
	},
};

static struct clk_branch mdp_axi_clk = {
	.halt_reg = 0x01d8,
	.halt_bit = 8,
	.halt_check = BRANCH_HALT_DELAY,
	.clkr = {
		.enable_reg = 0x0018,
		.enable_mask = BIT(23),
		.hw.init = &(struct clk_init_data){
			.name = "mdp_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch rot_axi_clk = {
	.halt_reg = 0x01d8,
	.halt_bit = 2,
	/*
	 * The rotator AXI clock shares the MMSS fabric with MDP. While MDP is
	 * scanning out to the display the fabric never idles, so this branch
	 * cannot halt and its status stays stuck at 'on'. Without skipping the
	 * halt check, every rotator runtime-PM suspend triggers a
	 * "rot_axi_clk status stuck at 'on'" WARN storm (same class as
	 * gfx3d_axi_clk / vcodec_axi_b_clk). Skip the halt poll.
	 */
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x0020,
		.enable_mask = BIT(22),
		.hw.init = &(struct clk_init_data){
			.name = "rot_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch gfx3d_axi_clk = {
	.halt_reg = 0x01e8,
	.halt_bit = 21,
	/*
	 * This branch clock shares the MMSS fabric with MDP. When MDP is
	 * actively scanning out to the display, the fabric never idles,
	 * preventing this clock from halting. Use BRANCH_HALT_SKIP to avoid
	 * the "status stuck at 'on'" warning during GPU runtime PM suspend.
	 */
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x0244,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gfx3d_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch amp_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 18,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(24),
		.hw.init = &(struct clk_init_data){
			.name = "amp_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch csi0_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 16,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(7),
		.hw.init = &(struct clk_init_data){
			.name = "csi0_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

/*
 * CSI1 AHB clock - separate from CSI0.
 * Legacy kernel had CSI0_PCLK (BIT 7) and CSI1_PCLK (BIT 20).
 */
static struct clk_branch csi1_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 17,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(20),
		.hw.init = &(struct clk_init_data){
			.name = "csi1_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch dsi_m_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 19,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(9),
		.hw.init = &(struct clk_init_data){
			.name = "dsi_m_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch dsi_s_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 21,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(18),
		.hw.init = &(struct clk_init_data){
			.name = "dsi_s_ahb_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_IS_CRITICAL,
		},
	},
};

static struct clk_branch gfx2d0_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 2,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(19),
		.hw.init = &(struct clk_init_data){
			.name = "gfx2d0_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch gfx2d1_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 3,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "gfx2d1_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch gfx3d_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 4,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(3),
		.hw.init = &(struct clk_init_data){
			.name = "gfx3d_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch hdmi_m_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 5,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(14),
		.hw.init = &(struct clk_init_data){
			.name = "hdmi_m_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch hdmi_s_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 6,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(4),
		.hw.init = &(struct clk_init_data){
			.name = "hdmi_s_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch ijpeg_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 9,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(5),
		.hw.init = &(struct clk_init_data){
			.name = "ijpeg_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch jpegd_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 7,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(21),
		.hw.init = &(struct clk_init_data){
			.name = "jpegd_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch mdp_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 11,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(10),
		.hw.init = &(struct clk_init_data){
			.name = "mdp_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch mmss_imem_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 12,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(6),
		.hw.init = &(struct clk_init_data){
			.name = "mmss_imem_ahb_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_IS_CRITICAL,
		},
	},
};

static struct clk_branch rot_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 13,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(12),
		.hw.init = &(struct clk_init_data){
			.name = "rot_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch smmu_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 22,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(15),
		.hw.init = &(struct clk_init_data){
			.name = "smmu_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch tv_enc_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 23,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(25),
		.hw.init = &(struct clk_init_data){
			.name = "tv_enc_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch vcodec_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 10,
	/*
	 * The halt bit at 0x01dc[10] does not settle within the standard
	 * 200 µs poll window when this branch is disabled (e.g. on VIDC
	 * runtime suspend), producing "vcodec_ahb_clk status stuck at 'on'"
	 * WARN traces with an EBUSY return.  Skip the halt check like
	 * vfe_ahb_clk below — the AHB bus stays clocked while other MMSS
	 * peripherals share it, so the halt bit can't be relied on.
	 */
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(11),
		.hw.init = &(struct clk_init_data){
			.name = "vcodec_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch vfe_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 14,
	/* Same as vfe_axi_clk - skip halt check */
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(13),
		.hw.init = &(struct clk_init_data){
			.name = "vfe_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch vpe_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 15,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(16),
		.hw.init = &(struct clk_init_data){
			.name = "vpe_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_regmap *mmcc_msm8660_clks[] = {
	[TV_ENC_AHB_CLK] = &tv_enc_ahb_clk.clkr,
	[AMP_AHB_CLK] = &amp_ahb_clk.clkr,
	[JPEGD_AHB_CLK] = &jpegd_ahb_clk.clkr,
	[GFX2D0_AHB_CLK] = &gfx2d0_ahb_clk.clkr,
	[DSI_S_AHB_CLK] = &dsi_s_ahb_clk.clkr,
	[VPE_AHB_CLK] = &vpe_ahb_clk.clkr,
	[SMMU_AHB_CLK] = &smmu_ahb_clk.clkr,
	[HDMI_M_AHB_CLK] = &hdmi_m_ahb_clk.clkr,
	[VFE_AHB_CLK] = &vfe_ahb_clk.clkr,
	[ROT_AHB_CLK] = &rot_ahb_clk.clkr,
	[VCODEC_AHB_CLK] = &vcodec_ahb_clk.clkr,
	[MDP_AHB_CLK] = &mdp_ahb_clk.clkr,
	[DSI_M_AHB_CLK] = &dsi_m_ahb_clk.clkr,
	[CSI0_AHB_CLK] = &csi0_ahb_clk.clkr,
	[CSI1_AHB_CLK] = &csi1_ahb_clk.clkr,
	[MMSS_IMEM_AHB_CLK] = &mmss_imem_ahb_clk.clkr,
	[IJPEG_AHB_CLK] = &ijpeg_ahb_clk.clkr,
	[HDMI_S_AHB_CLK] = &hdmi_s_ahb_clk.clkr,
	[GFX3D_AHB_CLK] = &gfx3d_ahb_clk.clkr,
	[GFX2D1_AHB_CLK] = &gfx2d1_ahb_clk.clkr,
	[JPEGD_AXI_CLK] = &jpegd_axi_clk.clkr,
	[GMEM_AXI_CLK] = &gmem_axi_clk.clkr,
	[MDP_AXI_CLK] = &mdp_axi_clk.clkr,
	[MMSS_IMEM_AXI_CLK] = &mmss_imem_axi_clk.clkr,
	[IJPEG_AXI_CLK] = &ijpeg_axi_clk.clkr,
	[GFX3D_AXI_CLK] = &gfx3d_axi_clk.clkr,
	[VCODEC_AXI_CLK] = &vcodec_axi_clk.clkr,
	[VFE_AXI_CLK] = &vfe_axi_clk.clkr,
	[VPE_AXI_CLK] = &vpe_axi_clk.clkr,
	[ROT_AXI_CLK] = &rot_axi_clk.clkr,
	[VCODEC_AXI_A_CLK] = &vcodec_axi_a_clk.clkr,
	[VCODEC_AXI_B_CLK] = &vcodec_axi_b_clk.clkr,
	[CSI0_SRC] = &csi0_src.clkr,
	[CSI0_CLK] = &csi0_clk.clkr,
	[CSI0_PHY_CLK] = &csi0_phy_clk.clkr,
	[CSI1_SRC] = &csi1_src.clkr,
	[CSI1_CLK] = &csi1_clk.clkr,
	[CSI1_PHY_CLK] = &csi1_phy_clk.clkr,
	[DSI_SRC] = &dsi1_src.clkr,
	[DSI_CLK] = &dsi1_clk.clkr,
	[CSI_PIX_CLK] = &csi_pix_clk.clkr,
	[CSI_RDI_CLK] = &csi_rdi_clk.clkr,
	[MDP_VSYNC_CLK] = &mdp_vsync_clk.clkr,
	[HDMI_APP_CLK] = &hdmi_app_clk.clkr,
	[GFX2D0_SRC] = &gfx2d0_src.clkr,
	[GFX2D0_CLK] = &gfx2d0_clk.clkr,
	[GFX2D1_SRC] = &gfx2d1_src.clkr,
	[GFX2D1_CLK] = &gfx2d1_clk.clkr,
	[GFX3D_SRC] = &gfx3d_src.clkr,
	[GFX3D_CLK] = &gfx3d_clk.clkr,
	[IJPEG_SRC] = &ijpeg_src.clkr,
	[IJPEG_CLK] = &ijpeg_clk.clkr,
	[JPEGD_SRC] = &jpegd_src.clkr,
	[JPEGD_CLK] = &jpegd_clk.clkr,
	[MDP_SRC] = &mdp_src.clkr,
	[MDP_CLK] = &mdp_clk.clkr,
	[MDP_LUT_CLK] = &mdp_lut_clk.clkr,
	[MDP_PIXEL_SRC] = &mdp_pixel_src.clkr,
	[MDP_PIXEL_CLK] = &mdp_pixel_clk.clkr,
	[MDP_LCDC_CLK] = &mdp_lcdc_clk.clkr,
	[DSI1_BYTE_SRC] = &dsi1_byte_src.clkr,
	[DSI1_BYTE_CLK] = &dsi1_byte_clk.clkr,
	[DSI1_ESC_SRC] = &dsi1_esc_src.clkr,
	[DSI1_ESC_CLK] = &dsi1_esc_clk.clkr,
	[ROT_SRC] = &rot_src.clkr,
	[ROT_CLK] = &rot_clk.clkr,
	[TV_ENC_CLK] = &tv_enc_clk.clkr,
	[TV_DAC_CLK] = &tv_dac_clk.clkr,
	[HDMI_TV_CLK] = &hdmi_tv_clk.clkr,
	[MDP_TV_CLK] = &mdp_tv_clk.clkr,
	[TV_SRC] = &tv_src.clkr,
	[VCODEC_SRC] = &vcodec_src.clkr,
	[VCODEC_CLK] = &vcodec_clk.clkr,
	[VFE_SRC] = &vfe_src.clkr,
	[VFE_CLK] = &vfe_clk.clkr,
	[VFE_CSI0_CLK] = &vfe_csi0_clk.clkr,
	[VFE_CSI1_CLK] = &vfe_csi1_clk.clkr,
	[VPE_SRC] = &vpe_src.clkr,
	[VPE_CLK] = &vpe_clk.clkr,
	[DSI_PIXEL_SRC] = &dsi1_pixel_src.clkr,
	[DSI_PIXEL_CLK] = &dsi1_pixel_clk.clkr,
	[CAMCLK0_SRC] = &camclk0_src.clkr,
	[CAMCLK0_CLK] = &camclk0_clk.clkr,
	[CAMCLK1_SRC] = &camclk1_src.clkr,
	[CAMCLK1_CLK] = &camclk1_clk.clkr,
	[CSIPHYTIMER_SRC] = &csiphytimer_src.clkr,
	[CSIPHY1_TIMER_CLK] = &csiphy1_timer_clk.clkr,
	[CSIPHY0_TIMER_CLK] = &csiphy0_timer_clk.clkr,
	[PLL2] = &pll2.clkr,
};

static const struct qcom_reset_map mmcc_msm8660_resets[] = {
	[GFX3D_AXI_RESET] = { 0x0208, 17 },
	[VPE_AXI_RESET] = { 0x0208, 15 },
	[IJPEG_AXI_RESET] = { 0x0208, 14 },
	[MPD_AXI_RESET] = { 0x0208, 13 },
	[VFE_AXI_RESET] = { 0x0208, 9 },
	[SP_AXI_RESET] = { 0x0208, 8 },
	[VCODEC_AXI_RESET] = { 0x0208, 7 },
	[ROT_AXI_RESET] = { 0x0208, 6 },
	[VCODEC_AXI_A_RESET] = { 0x0208, 5 },
	[VCODEC_AXI_B_RESET] = { 0x0208, 4 },
	[FAB_S3_AXI_RESET] = { 0x0208, 3 },
	[FAB_S2_AXI_RESET] = { 0x0208, 2 },
	[FAB_S1_AXI_RESET] = { 0x0208, 1 },
	[FAB_S0_AXI_RESET] = { 0x0208 },
	[SMMU_GFX3D_ABH_RESET] = { 0x020c, 31 },
	[SMMU_VPE_AHB_RESET] = { 0x020c, 30 },
	[SMMU_VFE_AHB_RESET] = { 0x020c, 29 },
	[SMMU_ROT_AHB_RESET] = { 0x020c, 28 },
	[SMMU_VCODEC_B_AHB_RESET] = { 0x020c, 27 },
	[SMMU_VCODEC_A_AHB_RESET] = { 0x020c, 26 },
	[SMMU_MDP1_AHB_RESET] = { 0x020c, 25 },
	[SMMU_MDP0_AHB_RESET] = { 0x020c, 24 },
	[SMMU_JPEGD_AHB_RESET] = { 0x020c, 23 },
	[SMMU_IJPEG_AHB_RESET] = { 0x020c, 22 },
	[APU_AHB_RESET] = { 0x020c, 18 },
	[CSI_AHB_RESET] = { 0x020c, 17 },
	[TV_ENC_AHB_RESET] = { 0x020c, 15 },
	[VPE_AHB_RESET] = { 0x020c, 14 },
	[FABRIC_AHB_RESET] = { 0x020c, 13 },
	[GFX3D_AHB_RESET] = { 0x020c, 10 },
	[HDMI_AHB_RESET] = { 0x020c, 9 },
	[MSSS_IMEM_AHB_RESET] = { 0x020c, 8 },
	[IJPEG_AHB_RESET] = { 0x020c, 7 },
	[DSI_M_AHB_RESET] = { 0x020c, 6 },
	[DSI_S_AHB_RESET] = { 0x020c, 5 },
	[JPEGD_AHB_RESET] = { 0x020c, 4 },
	[MDP_AHB_RESET] = { 0x020c, 3 },
	[ROT_AHB_RESET] = { 0x020c, 2 },
	[VCODEC_AHB_RESET] = { 0x020c, 1 },
	[VFE_AHB_RESET] = { 0x020c, 0 },
	[CSIPHY0_RESET] = { 0x0210, 29 },
	[CSIPHY1_RESET] = { 0x0210, 28 },
	[CSI_RDI_RESET] = { 0x0210, 27 },
	[CSI_PIX_RESET] = { 0x0210, 26 },
	[VFE_CSI_RESET] = { 0x0210, 24 },
	[MDP_RESET] = { 0x0210, 21 },
	[AMP_RESET] = { 0x0210, 20 },
	[JPEGD_RESET] = { 0x0210, 19 },
	[CSI1_RESET] = { 0x0210, 18 },
	[VPE_RESET] = { 0x0210, 17 },
	[MMSS_FABRIC_RESET] = { 0x0210, 16 },
	[VFE_RESET] = { 0x0210, 15 },
	[GFX3D_RESET] = { 0x0210, 12 },
	[HDMI_RESET] = { 0x0210, 11 },
	[MMSS_IMEM_RESET] = { 0x0210, 10 },
	[IJPEG_RESET] = { 0x0210, 9 },
	[CSI0_RESET] = { 0x0210, 8 },
	[DSI_RESET] = { 0x0210, 7 },
	[VCODEC_RESET] = { 0x0210, 6 },
	[MDP_TV_RESET] = { 0x0210, 4 },
	[MDP_VSYNC_RESET] = { 0x0210, 3 },
	[ROT_RESET] = { 0x0210, 2 },
	[TV_HDMI_RESET] = { 0x0210, 1 },
	[TV_ENC_RESET] = { 0x0210, 0 },
};

/*
 * MSM8x60 legacy footswitches.
 * These use a different register layout than modern GDSCs:
 * - Bit 8: ENABLE (set to enable power)
 * - Bit 5: CLAMP (set to clamp I/O)
 * - No status bit, requires timed delays
 */
static struct gdsc gfx2d0_gdsc = {
	.gdscr = 0x0180,
	.resets = (unsigned int []){ GFX2D0_AHB_RESET },
	.reset_count = 1,
	.pd = {
		.name = "gfx2d0",
	},
	.pwrsts = PWRSTS_OFF_ON,
	.flags = LEGACY_FOOTSWITCH | SW_RESET,
};

static struct gdsc gfx2d1_gdsc = {
	.gdscr = 0x0184,
	.resets = (unsigned int []){ GFX2D1_AHB_RESET },
	.reset_count = 1,
	.pd = {
		.name = "gfx2d1",
	},
	.pwrsts = PWRSTS_OFF_ON,
	.flags = LEGACY_FOOTSWITCH | SW_RESET,
};

static struct gdsc gfx3d_gdsc = {
	.gdscr = 0x0188,
	/*
	 * GFX3D (Adreno 220) requires the core reset to be toggled on every
	 * power-on, in addition to the AHB reset. The legacy MSM8x60
	 * footswitch driver (mach-msm/footswitch-8x60.c) does exactly this:
	 * after powering the GFX3D rail it issues an extra
	 * clk_reset(core_clk, ASSERT/DEASSERT). Without it the Adreno 220
	 * core (parameter cache) comes up free-running, producing the
	 * deterministic period-8 render cycle on the HP TouchPad. Listing
	 * GFX3D_RESET here lets the GDSC framework assert/deassert it around
	 * the rail charge (see gdsc_enable LEGACY_FOOTSWITCH path), which is
	 * the framework-correct equivalent of the legacy toggle.
	 */
	.resets = (unsigned int []){ GFX3D_AHB_RESET, GFX3D_RESET },
	.reset_count = 2,
	.pd = {
		.name = "gfx3d",
	},
	.pwrsts = PWRSTS_OFF_ON,
	/*
	 * RPM_ALWAYS_ON: keep the GFX3D footswitch powered across runtime PM
	 * idle (clocks still gate), collapsing it only on system suspend. The
	 * Adreno 220 shares the MMSS AXI fabric with the MDP4 display; cold-
	 * cycling this footswitch on every GPU idle forces an a2xx_hw_init
	 * microcode reload whose MMIO burst can stall the shared bus when it
	 * lands during an MDP client-switch underrun, hard-hanging the SoC.
	 * Mirrors legacy KGSL, which parked the GPU in SLEEP (rail up, clocks
	 * gated) during use and only power-collapsed (SLUMBER) on suspend.
	 */
	.flags = LEGACY_FOOTSWITCH | SW_RESET | RPM_ALWAYS_ON,
};

static struct gdsc ijpeg_gdsc = {
	.gdscr = 0x01a0,
	/*
	 * IJPEG (Gemini) requires the AXI and CORE resets to be toggled on
	 * every power-on, in addition to the AHB reset. The legacy
	 * mach-msm/footswitch-8x60.c does exactly this: setup_clocks ->
	 * clk_reset(axi, ASSERT) + clk_reset(ahb, ASSERT) + clk_reset(core,
	 * ASSERT) -> udelay -> rail charge -> deassert in reverse -> extra
	 * core ASSERT/DEASSERT toggle. multiple downstream vendor kernels MSM8660 trees use the
	 * same sequence. With only the AHB reset toggled the JPEG register
	 * file comes up healthy (CPU reads/writes look fine) but the FE's
	 * AXI-side address generator and burst sequencer stay in whatever
	 * sub-state QSBL left them -- typically wedged waiting for an
	 * AR-channel ready that never comes. The WE survives because writes
	 * are post-and-forget and don't drive AR. Symptom: FE reads return
	 * idle 0x80 regardless of buffer contents while WE writes succeed.
	 * Listing AXI + CORE here lets the GDSC framework assert/deassert
	 * them around rail charge (gdsc_enable LEGACY_FOOTSWITCH | SW_RESET
	 * path), matching the legacy/downstream vendor kernels convergent recipe and
	 * mirroring the gfx3d_gdsc precedent above.
	 *
	 * The actual implementation of the AXI-AHB-CORE assert / rail
	 * charge / reverse-deassert sequence lives in the LEGACY_FOOTSWITCH
	 * branch of gdsc_enable() in drivers/clk/qcom/gdsc.c (added in the
	 * companion "clk: qcom: gdsc: add LEGACY_FOOTSWITCH support" patch
	 * for the MSM8x60 family). MSM8x60 footswitch register layout has
	 * the CLAMP / ENABLE / RETENTION bits in the main GDSCR (no
	 * separate clamp_io_ctrl), so the framework implements it as a
	 * distinct path rather than reusing the modern GDSC code path.
	 * If this driver is built against a tree that lacks
	 * LEGACY_FOOTSWITCH support in gdsc.c, the IJPEG block will fail
	 * to initialise -- FE reads return 0x80 idle, WE writes succeed
	 * but never complete a transfer.
	 */
	.resets = (unsigned int []){
		IJPEG_AXI_RESET,
		IJPEG_AHB_RESET,
		IJPEG_RESET,
	},
	.reset_count = 3,
	.pd = {
		.name = "ijpeg",
	},
	.pwrsts = PWRSTS_OFF_ON,
	.flags = LEGACY_FOOTSWITCH | SW_RESET,
};

static struct gdsc mdp_gdsc = {
	.gdscr = 0x0190,
	.resets = (unsigned int []){ MDP_AHB_RESET },
	.reset_count = 1,
	.pd = {
		.name = "mdp",
	},
	.pwrsts = PWRSTS_OFF_ON,
	.flags = LEGACY_FOOTSWITCH | SW_RESET,
};

static struct gdsc rot_gdsc = {
	.gdscr = 0x018c,
	.resets = (unsigned int []){ ROT_AHB_RESET },
	.reset_count = 1,
	.pd = {
		.name = "rot",
	},
	.pwrsts = PWRSTS_OFF_ON,
	/*
	 * Skip SW_RESET during power domain enable, similar to VFE_GDSC.
	 * Asserting ROT_AHB_RESET during GDSC enable may cause a glitch
	 * affecting other MMSS peripherals during early boot.
	 *
	 * RPM_ALWAYS_ON: the rotator is an m2m block that pm_runtime-cycles per
	 * job; without this its footswitch re-enables on the shared MMSS fabric
	 * on every rotation burst, glitching MDP scanout. Keep it powered across
	 * runtime idle (clocks still gate); collapse only on system suspend.
	 */
	.flags = LEGACY_FOOTSWITCH | RPM_ALWAYS_ON,
};

static struct gdsc ved_gdsc = {
	.gdscr = 0x0194,
	.resets = (unsigned int []){ VCODEC_AHB_RESET },
	.reset_count = 1,
	.pd = {
		.name = "ved",
	},
	.pwrsts = PWRSTS_OFF_ON,
	.flags = LEGACY_FOOTSWITCH | SW_RESET,
};

static struct gdsc vfe_gdsc = {
	.gdscr = 0x0198,
	.resets = (unsigned int []){ VFE_AHB_RESET },
	.reset_count = 1,
	.pd = {
		.name = "vfe",
	},
	.pwrsts = PWRSTS_OFF_ON,
	/*
	 * Skip SW_RESET during power domain enable. Asserting VFE_AHB_RESET
	 * during GDSC enable appears to cause a glitch that affects other
	 * MMSS peripherals (specifically MDP display). The VFE driver performs
	 * its own software reset via vfe_reset() after enabling clocks, so
	 * the GDSC-level reset is not strictly required.
	 *
	 * Note: VFE is session-scoped (powered at streamon, collapsed at
	 * streamoff) so it only re-enables once per capture session -- not
	 * worth RPM_ALWAYS_ON (cf. rot_gdsc, which cycles per m2m job).
	 */
	.flags = LEGACY_FOOTSWITCH,
};

static struct gdsc vpe_gdsc = {
	.gdscr = 0x019c,
	.resets = (unsigned int []){ VPE_AHB_RESET },
	.reset_count = 1,
	.pd = {
		.name = "vpe",
	},
	.pwrsts = PWRSTS_OFF_ON,
	/*
	 * Skip SW_RESET during power domain enable, similar to VFE_GDSC.
	 * Asserting VPE_AHB_RESET during GDSC enable may cause a glitch
	 * affecting other MMSS peripherals during early boot.
	 *
	 * Note: VPE is session-scoped (powered at streamon, collapsed at
	 * streamoff) so it only re-enables once per session -- not worth
	 * RPM_ALWAYS_ON (cf. rot_gdsc, which cycles per m2m job).
	 */
	.flags = LEGACY_FOOTSWITCH,
};

static struct gdsc *mmcc_msm8660_gdscs[] = {
	[GFX2D0_GDSC] = &gfx2d0_gdsc,
	[GFX2D1_GDSC] = &gfx2d1_gdsc,
	[GFX3D_GDSC] = &gfx3d_gdsc,
	[IJPEG_GDSC] = &ijpeg_gdsc,
	[MDP_GDSC] = &mdp_gdsc,
	[ROT_GDSC] = &rot_gdsc,
	[VED_GDSC] = &ved_gdsc,
	[VFE_GDSC] = &vfe_gdsc,
	[VPE_GDSC] = &vpe_gdsc,
};

static const struct regmap_config mmcc_msm8660_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= 0x334,
	.fast_io	= true,
};

static const struct qcom_cc_desc mmcc_msm8660_desc = {
	.config = &mmcc_msm8660_regmap_config,
	.clks = mmcc_msm8660_clks,
	.num_clks = ARRAY_SIZE(mmcc_msm8660_clks),
	.resets = mmcc_msm8660_resets,
	.num_resets = ARRAY_SIZE(mmcc_msm8660_resets),
	.gdscs = mmcc_msm8660_gdscs,
	.num_gdscs = ARRAY_SIZE(mmcc_msm8660_gdscs),
};

static const struct of_device_id mmcc_msm8660_match_table[] = {
	{ .compatible = "qcom,mmcc-msm8660", .data = &mmcc_msm8660_desc },
	{ .compatible = "qcom,mmcc-apq8060", .data = &mmcc_msm8660_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, mmcc_msm8660_match_table);

/*
 * MSM8660 MMCC register offsets for initialization.
 * Based on legacy vendor kernel arch/arm/mach-msm/clock-8x60.c
 */

/* Reset registers - safe to deassert */
#define SW_RESET_ALL_REG	0x0204
#define SW_RESET_AHB_REG	0x020c
#define SW_RESET_AXI_REG	0x0208
#define SW_RESET_CORE_REG	0x0210

/* AHB enable registers - contain both control bits and clock enables */
#define AHB_EN_REG		0x0008
#define AHB_EN2_REG		0x0038

/* AXI enable registers - control AXI bus paths for memory access */
#define MAXI_EN_REG		0x0018
#define MAXI_EN3_REG		0x002c
#define SAXI_EN_REG		0x01d8

/* Misc CC registers */
#define CSI_CC_REG		0x0040
#define MISC_CC_REG		0x0058
#define MISC_CC2_REG		0x005c

/* CC registers - FORCE_CORE_ON in upper bits, enable in lower bits */
#define GFX2D0_CC_REG		0x0060
#define GFX2D1_CC_REG		0x0074
#define GFX3D_CC_REG		0x0080
#define IJPEG_CC_REG		0x0098
#define JPEGD_CC_REG		0x00a4
#define MDP_CC_REG		0x00c0
#define PIXEL_CC_REG		0x00d4
#define PIXEL_CC2_REG		0x0120  /* Not used as enable_reg */
#define ROT_CC_REG		0x00e0
#define TV_CC_REG		0x00ec
#define TV_CC2_REG		0x0124  /* Not used as enable_reg */
#define VCODEC_CC_REG		0x00f8
#define VFE_CC_REG		0x0104
#define VPE_CC_REG		0x0110

/*
 * Mask for FORCE_CORE_ON and sleep/wakeup delay bits in CC registers.
 * Only touches upper bits to avoid conflicting with clock enable bits
 * in the lower portion of the register.
 */
#define CC_FORCE_CORE_ON_MASK	0xe0ff0000
#define CC_FORCE_CORE_ON_VAL	0x80ff0000  /* FORCE_CORE_ON + 0xFF delays */
#define VCODEC_FORCE_CORE_ON	0xc0ff0000  /* VCODEC uses different bits */

static int mmcc_msm8660_init_hw(struct regmap *regmap)
{
	u32 val;
	int ret;

	/*
	 * Configure PLL2 (MM_PLL1) to 800 MHz for VFE and other MM clocks.
	 *
	 * PLL rate = PXO * (L + M/N) = 27 MHz * (29 + 17/27) = 800 MHz
	 *
	 * The bootloader (moboot) enables PLL2 but does not configure L/M/N,
	 * leaving it at whatever values are in the hardware. We must set it
	 * properly for VFE to reach 228 MHz. Without this, VFE gets ~66 MHz.
	 */
	regmap_read(regmap, 0x320, &val);  /* PLL2 L value */
	if (val != 29) {
		u32 m_val, n_val;
		/* PLL2 is not configured for 800 MHz, configure it */
		regmap_read(regmap, 0x324, &m_val);
		regmap_read(regmap, 0x328, &n_val);
		pr_info("mmcc-msm8660: Configuring PLL2 for 800 MHz (was L=%u M=%u N=%u)\n",
			val, m_val, n_val);

		/* Disable PLL output first */
		regmap_update_bits(regmap, 0x31c, BIT(0), 0);
		udelay(10);

		/* Set L=29, M=17, N=27 for 800 MHz from 27 MHz PXO */
		regmap_write(regmap, 0x320, 29);   /* L value */
		regmap_write(regmap, 0x324, 17);   /* M value */
		regmap_write(regmap, 0x328, 27);   /* N value */

		/* Configure PLL: enable main output, set VCO */
		regmap_write(regmap, 0x32c, 0x00800000);

		/* Enable PLL: bypass off, reset deassert, output enable */
		regmap_update_bits(regmap, 0x31c, BIT(1), BIT(1));  /* Bypass off */
		udelay(10);
		regmap_update_bits(regmap, 0x31c, BIT(2), BIT(2));  /* Reset deassert */
		udelay(50);
		regmap_update_bits(regmap, 0x31c, BIT(0), BIT(0));  /* Output enable */
		udelay(50);

		/*
		 * Verify PLL2 locked (status register 0x334, bit 16).
		 * Poll up to 200 us at 5 us intervals -- lock typically asserts
		 * within ~50 us after output enable, but we tolerate longer to
		 * cope with slow PVT corners. If lock never asserts the PLL is
		 * mis-configured or hardware is faulty; fail probe rather than
		 * letting downstream clocks calculate frequencies from a
		 * non-locked PLL (VFE would land at the wrong rate, MDP and
		 * IJPEG would mis-train, etc.).
		 */
		ret = regmap_read_poll_timeout(regmap, 0x334, val,
					       val & BIT(16), 5, 200);
		if (ret) {
			pr_err("mmcc-msm8660: PLL2 lock timeout, status=0x%x\n",
			       val);
			return ret;
		}
		pr_info("mmcc-msm8660: PLL2 locked at 800 MHz\n");
	}

	/*
	 * MSM8660 MMCC hardware initialization based on legacy vendor kernel.
	 *
	 * legacy vendor kernel sets specific control bits in AHB_EN_REG:
	 *   rmwreg(0x00000003, AHB_EN_REG, 0x0F7FFFFF);
	 * BIT(0) and BIT(1) are control bits (FPB enable, HW gating disable),
	 * NOT clock enables. Clock enables start at BIT(2) and above.
	 *
	 * We initialize these control bits but leave clock enable bits
	 * for the clock framework to manage.
	 */

	/*
	 * Set FPB enable and disable HW gating in AHB_EN_REG.
	 * BIT(0) = FPB clock enable
	 * BIT(1) = Disable HW gating for all AHB clocks
	 * These are required for CSI register writes to work.
	 */
	regmap_update_bits(regmap, AHB_EN_REG, 0x3, 0x3);

	/*
	 * AHB_EN2_REG contains additional control bits including
	 * VFE_AHB FORCE_CORE_ON to prevent memory collapse.
	 * legacy vendor kernel: rmwreg(0x000007F9, AHB_EN2_REG, 0x7FFFBFFF);
	 */
	regmap_update_bits(regmap, AHB_EN2_REG, 0x7fffbfff, 0x000007f9);

	/*
	 * Initialize AXI bus registers for memory access paths.
	 * These enable HW gating and set FORCE_CORE_ON bits for AXI clocks.
	 * legacy vendor kernel: rmwreg(0x000307F9, MAXI_EN_REG, 0x0FFFFFFF);
	 *        writel(0x3FE7FCFF, MAXI_EN3_REG);
	 *        writel(0x000001D8, SAXI_EN_REG);
	 * Note: MAXI_EN2_REG is owned by RPM, don't touch it.
	 */
	regmap_update_bits(regmap, MAXI_EN_REG, 0x0fffffff, 0x000307f9);
	regmap_write(regmap, MAXI_EN3_REG, 0x3fe7fcff);
	regmap_write(regmap, SAXI_EN_REG, 0x000001d8);

	/* Deassert all MM resets */
	regmap_write(regmap, SW_RESET_ALL_REG, 0);
	regmap_write(regmap, SW_RESET_AHB_REG, 0);
	regmap_write(regmap, SW_RESET_AXI_REG, 0);
	regmap_write(regmap, SW_RESET_CORE_REG, 0);

	/*
	 * Initialize MISC CC registers.
	 *
	 * CSI clocks are managed by the common clock framework on consumer
	 * request via the registered csi*_src/csi*_clk/csi*_phy_clk
	 * structures; no unconditional CSI_CC_REG write is needed here.
	 *
	 * MISC_CC_REG (0x058): bit 10 enables the CSI1-to-VFE async bridge.
	 * MISC_CC2_REG: additional enables observed from the legacy vendor kernel reference
	 *   register dump (0x004007fd).
	 */
	regmap_update_bits(regmap, MISC_CC_REG, 0xfefff7ff, 0x00000400);
	regmap_update_bits(regmap, MISC_CC2_REG, 0xffff7fff, 0x000007fd);
	/* Set dsi_byte_clk src to DSI PHY PLL, hdmi_app_clk src to PXO */
	regmap_update_bits(regmap, MISC_CC2_REG, 0x00424003, 0x00400001);

	/*
	 * Set FORCE_CORE_ON bits in all multimedia CC registers to prevent
	 * core memories from being collapsed when clocks are halted.
	 *
	 * Value 0x80ff0000 sets:
	 *   - Bit 31: FORCE_CORE_ON
	 *   - Bits 16-23: Safe sleep/wakeup delay values (0xFF)
	 *
	 * We use regmap_update_bits to only touch upper bits, avoiding
	 * conflict with clock enable bits in the lower portion.
	 */

	/* Graphics */
	regmap_update_bits(regmap, GFX2D0_CC_REG, CC_FORCE_CORE_ON_MASK, CC_FORCE_CORE_ON_VAL);
	regmap_update_bits(regmap, GFX2D1_CC_REG, CC_FORCE_CORE_ON_MASK, CC_FORCE_CORE_ON_VAL);
	regmap_update_bits(regmap, GFX3D_CC_REG, CC_FORCE_CORE_ON_MASK, CC_FORCE_CORE_ON_VAL);

	/* Display */
	regmap_update_bits(regmap, MDP_CC_REG, CC_FORCE_CORE_ON_MASK, CC_FORCE_CORE_ON_VAL);
	regmap_update_bits(regmap, PIXEL_CC_REG, CC_FORCE_CORE_ON_MASK, CC_FORCE_CORE_ON_VAL);
	regmap_write(regmap, PIXEL_CC2_REG, 0x000004ff);
	regmap_update_bits(regmap, TV_CC_REG, CC_FORCE_CORE_ON_MASK, CC_FORCE_CORE_ON_VAL);
	regmap_write(regmap, TV_CC2_REG, 0x000004ff);
	regmap_update_bits(regmap, ROT_CC_REG, CC_FORCE_CORE_ON_MASK, CC_FORCE_CORE_ON_VAL);

	/* Video */
	regmap_update_bits(regmap, VCODEC_CC_REG, CC_FORCE_CORE_ON_MASK, VCODEC_FORCE_CORE_ON);
	regmap_update_bits(regmap, VPE_CC_REG, CC_FORCE_CORE_ON_MASK, CC_FORCE_CORE_ON_VAL);

	/* Camera */
	regmap_update_bits(regmap, VFE_CC_REG, CC_FORCE_CORE_ON_MASK, CC_FORCE_CORE_ON_VAL);

	/* JPEG */
	regmap_update_bits(regmap, IJPEG_CC_REG, CC_FORCE_CORE_ON_MASK, CC_FORCE_CORE_ON_VAL);
	regmap_update_bits(regmap, JPEGD_CC_REG, CC_FORCE_CORE_ON_MASK, CC_FORCE_CORE_ON_VAL);

	return 0;
}

/*
 * Unhalt MMSS fabric AXI master ports.
 *
 * legacy vendor kernels (msm_bus_board_8660.c) program halt registers
 * on three fabrics, but only the MMSS fabric is owned by this driver:
 * MDP/ROTATOR/GFX2D/GFX3D/VFE/VPE/JPEG/HDCODEC all sit behind ports on
 * the MMSS NoC. The downstream GDSC driver calls msm_bus_axi_portunhalt()
 * in footswitch_enable() when each MMSS power domain comes up; mainline
 * GDSC does not, leaving MMSS master ports in their default (potentially
 * halted) state. This causes DMA stalls when the multimedia blocks first
 * power on.
 *
 * The APPS and SYSTEM fabrics are owned by other subsystems (apps CPU /
 * GCC-side peripherals) and are not this driver's responsibility -- they
 * are handled by the qcom-msm8660 interconnect provider on platforms
 * that need it.
 *
 * MMSS fabric master ports (port:name from legacy vendor kernel enum):
 *   0:MDP_PORT0   1:MDP_PORT1   2:ADM1_PORT0  3:ROTATOR
 *   4:GFX3D       5:JPEG_DEC    6:GFX2D0      7:VFE
 *   8:VPE         9:JPEG_ENC   10:GFX2D1     11:APPS_FAB
 *  12:HDCODEC0   13:HDCODEC1
 *
 * Driven from mmcc probe because it runs after the qcom_rpm platform
 * driver registers (mmcc is module_platform_driver, RPM is platform_init).
 * Doing the same in gcc probe (core_initcall) is too early -- RPM is
 * not yet bound and qcom_rpm_write() has no target.
 */
static void mmcc_msm8660_unhalt_fabric_ports(struct device *dev)
{
	struct device_node *rpm_node;
	struct platform_device *rpm_pdev;
	struct device_link *link;
	struct qcom_rpm *rpm;
	/* halt_data[0]=0 = CLK_UNHALT for all bits; halt_data[1] = port mask */
	u32 mmss_halt[2] = {0, GENMASK(13, 0)};
	int rc;

	rpm_node = of_find_compatible_node(NULL, NULL, "qcom,rpm-msm8660");
	if (!rpm_node)
		return;

	rpm_pdev = of_find_device_by_node(rpm_node);
	of_node_put(rpm_node);
	if (!rpm_pdev)
		return;

	/*
	 * Pin the RPM supplier to this consumer device so that the
	 * qcom_rpm driver cannot unbind (and free its drvdata) while we
	 * are reading or using the pointer below. The link is dropped
	 * automatically when the mmcc device goes away.
	 */
	link = device_link_add(dev, &rpm_pdev->dev,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		put_device(&rpm_pdev->dev);
		return;
	}

	rpm = dev_get_drvdata(&rpm_pdev->dev);
	if (!rpm) {
		/*
		 * Supplier exists but has not bound yet -- skip the
		 * unhalt rather than blocking mmcc probe; downstream
		 * MMSS clients will be enabled on demand.
		 */
		put_device(&rpm_pdev->dev);
		return;
	}

	rc = qcom_rpm_write(rpm, QCOM_RPM_ACTIVE_STATE,
			    QCOM_RPM_MM_FABRIC_HALT, mmss_halt, 2);
	if (rc)
		dev_warn(dev, "MMSS fabric unhalt failed: %d\n", rc);
	else
		dev_info(dev, "MMSS fabric: unhalted all master ports (0-13)\n");

	put_device(&rpm_pdev->dev);
}

static int mmcc_msm8660_probe(struct platform_device *pdev)
{
	struct regmap *regmap;
	int ret;

	regmap = qcom_cc_map(pdev, &mmcc_msm8660_desc);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	/* Initialize MMCC hardware before registering clocks */
	ret = mmcc_msm8660_init_hw(regmap);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "MMCC hardware init failed\n");

	/*
	 * Unhalt MMSS fabric AXI master ports before any MMSS peripheral
	 * (MDP / ROTATOR / GFX2D / GFX3D / VFE / VPE / JPEG / HDCODEC)
	 * performs DMA. Driven from mmcc probe because it runs after the
	 * qcom_rpm platform driver has bound (gcc core_initcall is too
	 * early). APPS and SYSTEM fabrics belong to other subsystems and
	 * are not touched here.
	 */
	mmcc_msm8660_unhalt_fabric_ports(&pdev->dev);

	return qcom_cc_really_probe(&pdev->dev, &mmcc_msm8660_desc, regmap);
}

static struct platform_driver mmcc_msm8660_driver = {
	.probe		= mmcc_msm8660_probe,
	.driver		= {
		.name	= "mmcc-msm8660",
		.of_match_table = mmcc_msm8660_match_table,
	},
};

module_platform_driver(mmcc_msm8660_driver);

MODULE_DESCRIPTION("Qualcomm MSM8x60 Multimedia Clock Controller driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:mmcc-msm8660");
