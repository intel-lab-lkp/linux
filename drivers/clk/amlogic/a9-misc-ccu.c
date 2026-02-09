// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (c) 2026 Amlogic, Inc. All rights reserved
 */

#include <linux/err.h>
#include <linux/module.h>

#include "clk.h"
#include "clk-basic.h"
#include "clk-composite.h"
#include "clk-dualdiv.h"
#include "clk-module.h"
#include "clk-noglitch.h"
#include "clk-pll.h"

#include <dt-bindings/clock/amlogic,a9-misc-ccu.h>

static struct aml_clk a9_sc_pre = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_composite_ops,
	},
	.type = AML_CLKTYPE_COMPOSITE,
	.data = &(struct aml_clk_composite_data) {
		.div_width = 8,
	},
};

static struct aml_clk a9_sc = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_sc_pre.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
	.type = AML_CLKTYPE_DIV,
	.data = &(struct aml_clk_divider_data) {
		.shift = 16,
		.width = 4,
	},
};

struct clk_hw_onecell_data a9_sc_clk_hw_data = {
	.hws = {
		[A9_CLK_SC_PRE]	= &a9_sc_pre.hw,
		[A9_CLK_SC]	= &a9_sc.hw,
	},
	.num = 2,
};

static struct aml_clk a9_ts_div = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_divider_ops,
	},
	.type = AML_CLKTYPE_DIV,
	.data = &(struct aml_clk_divider_data) {
		.shift = 0,
		.width = 8,
	},
};

static struct aml_clk a9_ts = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_ts_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
	.type = AML_CLKTYPE_GATE,
	.data = &(struct aml_clk_gate_data) {
		.bit_idx = 8,
	},
};

struct clk_hw_onecell_data a9_ts_clk_hw_data = {
	.hws = {
		[A9_CLK_TS_DIV]	= &a9_ts_div.hw,
		[A9_CLK_TS]	= &a9_ts.hw,
	},
	.num = 2,
};

static struct aml_clk a9_genout_sel = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_mux_ops,
	},
	.type = AML_CLKTYPE_MUX,
	.data = &(struct aml_clk_mux_data) {
		.mask = 0x1f,
		.shift = 12,
	},
};

static struct aml_clk a9_genout_div = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_genout_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
	.type = AML_CLKTYPE_DIV,
	.data = &(struct aml_clk_divider_data) {
		.shift = 0,
		.width = 11,
	},
};

static struct aml_clk a9_genout = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_genout_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
	.type = AML_CLKTYPE_GATE,
	.data = &(struct aml_clk_gate_data) {
		.bit_idx = 11,
	},
};

struct clk_hw_onecell_data a9_genout_clk_hw_data = {
	.hws = {
		[A9_CLK_GENOUT_SEL]	= &a9_genout_sel.hw,
		[A9_CLK_GENOUT_DIV]	= &a9_genout_div.hw,
		[A9_CLK_GENOUT]		= &a9_genout.hw,
	},
	.num = 3,
};

static struct aml_clk a9_clk24m_in = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_gate_ops,
	},
	.type = AML_CLKTYPE_GATE,
	.data = &(struct aml_clk_gate_data) {
		.bit_idx = 11,
	},
};

static struct aml_clk a9_clk12_24m = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_clk24m_in.hw
		},
		.num_parents = 1,
	},
	.type = AML_CLKTYPE_DIV,
	.data = &(struct aml_clk_divider_data) {
		.shift = 10,
		.width = 1,
	},
};

struct clk_hw_onecell_data a9_clk12_24m_clk_hw_data = {
	.hws = {
		[A9_CLK_24M_IN]	= &a9_clk24m_in.hw,
		[A9_CLK_12_24M]	= &a9_clk12_24m.hw,
	},
	.num = 2,
};

static struct aml_clk a9_vapb = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_noglitch_ops,
	},
	.type = AML_CLKTYPE_NOGLITCH,
	.data = &(struct aml_clk_composite_data) {
		.div_width = 7,
	},
};

static struct aml_clk a9_ge2d = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_vapb.hw
		},
		.num_parents = 1,
	},
	.type = AML_CLKTYPE_GATE,
	.data = &(struct aml_clk_gate_data) {
		.bit_idx = 30,
	},
};

struct clk_hw_onecell_data a9_vapbge2d_clk_hw_data = {
	.hws = {
		[A9_CLK_VAPB]	= &a9_vapb.hw,
		[A9_CLK_GE2D]	= &a9_ge2d.hw,
	},
	.num = 2,
};

static struct aml_clk a9_vpu_clkb_temp = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_composite_ops,
	},
	.type = AML_CLKTYPE_COMPOSITE,
	.data = &(struct aml_clk_composite_data) {
		.bit_offset = 16,
		.div_width = 4,
	},
};

static struct aml_clk a9_vpu_clkb_div = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_vpu_clkb_temp.hw
		},
		.num_parents = 1,
	},
	.type = AML_CLKTYPE_DIV,
	.data = &(struct aml_clk_divider_data) {
		.shift = 0,
		.width = 8,
	},
};

static struct aml_clk a9_vpu_clkb = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_vpu_clkb_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
	.type = AML_CLKTYPE_GATE,
	.data = &(struct aml_clk_gate_data) {
		.bit_idx = 8,
	},
};

struct clk_hw_onecell_data a9_di_clk_hw_data = {
	.hws = {
		[A9_CLK_VPU_CLKB_TEMP]	= &a9_vpu_clkb_temp.hw,
		[A9_CLK_VPU_CLKB_DIV]	= &a9_vpu_clkb_div.hw,
		[A9_CLK_VPU_CLKB]	= &a9_vpu_clkb.hw,
	},
	.num = 3,
};

static int of_aml_clk_misc_set_parent_table(struct device *dev,
					    struct aml_clk *clk,
					    int start_index, int end_index)
{
	u32 *ptab = of_aml_clk_get_parent_table(dev, start_index, end_index);
	struct aml_clk_mux_data *mux_data;
	struct aml_clk_composite_data *comp_data;
	struct aml_clk_noglitch_data *noglitch_data;

	if (IS_ERR(ptab))
		return PTR_ERR(ptab);
	else if (!ptab) /* parent clock indices are contiguous */
		return 0;

	switch (clk->type) {
	case AML_CLKTYPE_MUX:
		mux_data = clk->data;
		mux_data->table = ptab;
		return 0;

	case AML_CLKTYPE_COMPOSITE:
		comp_data = clk->data;
		comp_data->table = ptab;
		return 0;

	case AML_CLKTYPE_NOGLITCH:
		noglitch_data = clk->data;
		noglitch_data->table = ptab;
		return 0;

	default:
		return -EINVAL;
	}
}

/*
 * The default CCU block diagram is shown below:
 *            +---------------------------------+
 *            |  +-------+           +-------+  |
 *            |  |       |           |       |  |
 *   clkin0----->|   1   |           |   n   |  |
 *           ... | level |--> ... -->| level |------>clkout
 *           ... | clock |           | clock |  |
 *   clkinn----->|       |           |       |  |
 *            |  +-------+           +-------+  |
 *            +---------------------------------+
 *
 * By default, the CCU has the following characteristics:
 *   - The clock specified by "clocks" uses the first-level clock (clkid = 0)
 *     as its parent.
 *   - The parent-child relationship among sub-clocks follows this order:
 *       - hw_data->hws[0] -> ... -> hw_data->hws[n].
 */
static int of_aml_clk_misc_init_register(struct device *dev,
					 struct clk_hw_onecell_data *hw_data)
{
	struct device_node *np = dev_of_node(dev);
	struct aml_clk *clk;
	struct clk_init_data init;
	struct clk_parent_data *pdata;
	u8 pnum;
	int ret, clkid;

	if (hw_data->num != of_aml_clk_get_count(np))
		return -EINVAL;

	ret = of_aml_clk_get_parent_num(dev, 0, -1);
	if (ret < 0)
		return ret;

	pnum = (u8)ret;
	pdata = devm_kcalloc(dev, pnum, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	for (clkid = 0; clkid < hw_data->num; clkid++) {
		/*
		 * By default, clocks specified in the DT clocks property act as
		 * the parent clocks for the first-level CCU clock (clkid == 0).
		 */
		if (clkid == 0) {
			ret = of_aml_clk_get_parent_data(dev, hw_data->hws,
							 0, -1, pdata, &pnum);
			if (ret)
				goto out;

			clk = to_aml_clk(hw_data->hws[clkid]);
			ret = of_aml_clk_misc_set_parent_table(dev, clk, 0, -1);
			if (ret)
				goto out;
		} else {
			pnum = 1;
			/*
			 *The parent-child relationship among sub-clocks follows
			 * this order:
			 *  - hw_data->hws[0] -> ... -> hw_data->hws[n].
			 */
			pdata[0].hw = hw_data->hws[clkid - 1];
		}

		memcpy(&init, hw_data->hws[clkid]->init, sizeof(init));
		init.name = of_aml_clk_get_name_index(np, clkid);
		if (!init.name) {
			ret = -EINVAL;
			goto out;
		}

		init.num_parents = pnum;
		init.parent_data = pdata;
		hw_data->hws[clkid]->init = &init;
		ret = of_aml_clk_register(dev, hw_data->hws[clkid], clkid);
		if (ret)
			goto out;
	}

out:
	devm_kfree(dev, pdata);

	return ret;
}

#define A9_ETH_CLK_NUM		2

static struct aml_clk a9_eth_125m = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_gate_ops,
	},
	.type = AML_CLKTYPE_GATE,
	.data = &(struct aml_clk_gate_data) {
		.bit_idx = 7,
	},
};

static struct aml_clk a9_eth_rmii = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_composite_ops,
	},
	.type = AML_CLKTYPE_COMPOSITE,
	.data = &(struct aml_clk_composite_data) {
		.div_width = 7,
	},
};

struct clk_hw_onecell_data a9_eth_clk_hw_data = {
	.hws = {
		[A9_CLK_ETH_125M]	= &a9_eth_125m.hw,
		[A9_CLK_ETH_RMII]	= &a9_eth_rmii.hw,
	},
	.num = A9_ETH_CLK_NUM,
};

/*
 * A diagram of the A9 eth-ccu is as follows:
 *         +------------------------------------------+
 *         |                             +------+     |
 * clk125m------------------------------>| gate |-------> eth_125m
 *         |                             +------+     |
 *         |     +---------------------------------+  |
 *         |     |  |\                             |  |
 * clkin0 --------->| |                            |  |
 *         |     |  | |      +-----+     +------+  |  |
 *         | ... |  | |----->| div |---->| gate |-------> eth_rmii
 *         |     |  | |      +-----+     +------+  |  |
 * clkin7 --------->| |                            |  |
 *         |     |  |/       composote-ccu         |  |
 *         |     +---------------------------------+  |
 *         +------------------------------------------+
 */
static int of_aml_clk_eth_init_register(struct device *dev,
					struct clk_hw_onecell_data *hw_data)
{
	struct device_node *np = dev_of_node(dev);
	struct aml_clk *clk;
	struct clk_parent_data pdata[9];
	u8 pnum;
	struct clk_init_data init;
	int ret, clkid;

	if (of_aml_clk_get_count(np) != A9_ETH_CLK_NUM)
		return -EINVAL;

	for (clkid = 0; clkid < A9_ETH_CLK_NUM; clkid++) {
		if (clkid == A9_CLK_ETH_125M) { /* eth_125m */
			ret = of_aml_clk_get_parent_data(dev, hw_data->hws,
							 0, 0, pdata, &pnum);
			if (ret)
				return ret;
		} else { /* eth_rmii */
			ret = of_aml_clk_get_parent_data(dev, hw_data->hws,
							 1, 8, pdata, &pnum);
			if (ret)
				return ret;

			clk = to_aml_clk(hw_data->hws[clkid]);
			ret = of_aml_clk_misc_set_parent_table(dev, clk, 1, 8);
			if (ret)
				return ret;
		}

		memcpy(&init, hw_data->hws[clkid]->init, sizeof(init));
		init.name = of_aml_clk_get_name_index(np, clkid);
		if (!init.name)
			return -EINVAL;

		init.num_parents = pnum;
		init.parent_data = pdata;
		hw_data->hws[clkid]->init = &init;
		ret = of_aml_clk_register(dev, hw_data->hws[clkid], clkid);
		if (ret)
			return ret;
	}

	return 0;
}

#define A9_DUALDIVMUX_CLK_NUM		2

static struct aml_clk_dualdiv_param a9_dualdiv_table[] = {
	{ 1, 732, 7, 731, 10 }, /* 32.768k for rtc/cec */
};

static struct aml_clk a9_dualdiv = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_dualdiv_ops,
	},
	.type = AML_CLKTYPE_DUALDIV,
	.data = &(struct aml_clk_dualdiv_data) {
		.reg_offset = 0,
		.table = a9_dualdiv_table,
		.table_count = ARRAY_SIZE(a9_dualdiv_table),
	},
};

static struct aml_clk a9_dualdiv_sel = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_mux_ops,
		/*
		 * NOTE: To ensure output clock continuity, the mux can switch
		 * correctly only when both the current path and the target path
		 * have valid clock inputs. Otherwise, the mux cannot complete
		 * the switch.
		 */
		.flags = CLK_OPS_PARENT_ENABLE,
	},
	.type = AML_CLKTYPE_MUX,
	.data = &(struct aml_clk_mux_data) {
		.reg_offset = 4,
		.mask = 0x3,
		.shift = 30,
	},
};

struct clk_hw_onecell_data a9_dualdiv_clk_hw_data = {
	.hws = {
		[A9_CLK_DUALDIV]	= &a9_dualdiv.hw,
		[A9_CLK_DUALDIV_SEL]	= &a9_dualdiv_sel.hw,
	},
	.num = A9_DUALDIVMUX_CLK_NUM,
};

/*
 * A diagram of the A9 dualdivmux-ccu is as follows:
 *       +--------------------------------------+
 *       |  +---------+                         |
 *       |  |         |                         |
 * divin--->| dualdiv |--+                      |
 *       |  |         |  |                      |
 *       |  +---------+  |   +---+         |\   |
 *       |               +-->| ~ |--mux0-->| |  |
 * clk0--------------------->| ~ |--mux1-->| |------> out
 * clk1--------------------->| ~ |--mux2-->| |  |
 * clk2--------------------->| ~ |--mux3-->|/   |
 *       |                   +---+              |
 *       +--------------------------------------+
 *
 * Its internal multiplexer selects from up to 4 parent clocks, one of which
 * must be 'dualdiv' output clock.
 */
static int of_aml_clk_dualdiv_init_register(struct device *dev,
					    struct clk_hw_onecell_data *hw_data)
{
	struct device_node *np = dev_of_node(dev);
	struct aml_clk *clk;
	struct clk_parent_data pdata[5];
	u8 pnum;
	struct clk_init_data init;
	int ret, clkid;

	if (of_aml_clk_get_count(np) != A9_DUALDIVMUX_CLK_NUM)
		return -EINVAL;

	for (clkid = 0; clkid < A9_DUALDIVMUX_CLK_NUM; clkid++) {
		if (clkid == A9_CLK_DUALDIV) { /* dualdiv */
			ret = of_aml_clk_get_parent_data(dev, hw_data->hws,
							 0, 0, pdata, &pnum);
			if (ret)
				return ret;
		} else { /* dualdiv_sel */
			ret = of_aml_clk_get_parent_data(dev, hw_data->hws,
							 1, 4, pdata, &pnum);
			if (ret)
				return ret;

			clk = to_aml_clk(hw_data->hws[clkid]);
			ret = of_aml_clk_misc_set_parent_table(dev, clk, 1, 4);
			if (ret)
				return ret;
		}

		memcpy(&init, hw_data->hws[clkid]->init, sizeof(init));
		init.name = of_aml_clk_get_name_index(np, clkid);
		if (!init.name)
			return -EINVAL;

		init.num_parents = pnum;
		init.parent_data = pdata;
		hw_data->hws[clkid]->init = &init;
		ret = of_aml_clk_register(dev, hw_data->hws[clkid], clkid);
		if (ret)
			return ret;
	}

	return 0;
}

static struct aml_clk a9_mclk0_pre_div = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_divider_ops,
		.num_parents = 1,
	},
	.type = AML_CLKTYPE_DIV,
	.data = &(struct aml_clk_divider_data) {
		.reg_offset = 0xc,
		.shift = 3,
		.width = 5,
		.flags = CLK_DIVIDER_MAX_AT_ZERO,
	},
};

static struct aml_clk a9_mclk0_sel = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_mux_ops,
	},
	.type = AML_CLKTYPE_MUX,
	.data = &(struct aml_clk_mux_data) {
		.reg_offset = 0xc,
		.mask = 0x3,
		.shift = 12,
	},
};

static struct aml_clk a9_mclk0_div = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_mclk0_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
	.type = AML_CLKTYPE_DIV,
	.data = &(struct aml_clk_divider_data) {
		.reg_offset = 0xc,
		.shift = 9,
		.width = 1,
	},
};

static struct aml_clk a9_mclk0 = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_mclk0_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
	.type = AML_CLKTYPE_GATE,
	.data = &(struct aml_clk_gate_data) {
		.reg_offset = 0xc,
		.bit_idx = 8,
	},
};

static struct aml_clk a9_mclk1_pre_div = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_divider_ops,
		.num_parents = 1,
	},
	.type = AML_CLKTYPE_DIV,
	.data = &(struct aml_clk_divider_data) {
		.reg_offset = 0xc,
		.shift = 19,
		.width = 5,
		.flags = CLK_DIVIDER_MAX_AT_ZERO,
	},
};

static struct aml_clk a9_mclk1_sel = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_mux_ops,
	},
	.type = AML_CLKTYPE_MUX,
	.data = &(struct aml_clk_mux_data) {
		.reg_offset = 0xc,
		.mask = 0x3,
		.shift = 28,
	},
};

static struct aml_clk a9_mclk1_div = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_mclk1_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
	.type = AML_CLKTYPE_DIV,
	.data = &(struct aml_clk_divider_data) {
		.reg_offset = 0xc,
		.shift = 25,
		.width = 1,
	},
};

static struct aml_clk a9_mclk1 = {
	.hw.init = &(const struct clk_init_data) {
		.ops = &aml_clk_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_mclk1_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
	.type = AML_CLKTYPE_GATE,
	.data = &(struct aml_clk_gate_data) {
		.reg_offset = 0xc,
		.bit_idx = 24,
	},
};

#define A9_MCLK_CLK_NUM		8

struct clk_hw_onecell_data a9_mclk_clk_hw_data = {
	.hws = {
		[A9_CLK_MCLK_0_PRE_DIV]	= &a9_mclk0_pre_div.hw,
		[A9_CLK_MCLK_0_SEL]	= &a9_mclk0_sel.hw,
		[A9_CLK_MCLK_0_DIV]	= &a9_mclk0_div.hw,
		[A9_CLK_MCLK_0]		= &a9_mclk0.hw,
		[A9_CLK_MCLK_1_PRE_DIV]	= &a9_mclk1_pre_div.hw,
		[A9_CLK_MCLK_1_SEL]	= &a9_mclk1_sel.hw,
		[A9_CLK_MCLK_1_DIV]	= &a9_mclk1_div.hw,
		[A9_CLK_MCLK_1]		= &a9_mclk1.hw,
	},
	.num = A9_MCLK_CLK_NUM,
};

/*
 * A diagram of the A9 mclk-ccu is as follows:
 *        +--------------------------------------------------+
 *        |              +-----+                             |
 * divin--------------+->| div |-->|\                        |
 *        |           |  +-----+   | |                       |
 * clk0 -----------+-------------->| |   +-----+   +------+  |
 *        |        |  |            | |-->| div |-->| gate |-----> mclk0
 * clk1 --------+----------------->| |   +-----+   +------+  |
 *        |     |  |  |            | |                       |
 * clk2 -----+-------------------->|/                        |
 *        |  |  |  |  |  +-----+                             |
 *        |  |  |  |  +->| div |-->|\                        |
 *        |  |  |  |     +-----+   | |                       |
 *        |  |  |  +-------------->| |   +-----+   +------+  |
 *        |  |  |                  | |-->| div |-->| gate |-----> mclk1
 *        |  |  +----------------->| |   +-----+   +------+  |
 *        |  |                     | |                       |
 *        |  +-------------------->|/                        |
 *        +--------------------------------------------------+
 *
 * The A9 mclk-ccu contains two identical mclk sub-modules. For each sub-module,
 * channel 0 of the mux (mclk0/1_sel) has an independent pre-divider in front
 * of it, while the clock sources for channels 1 to 3 are shared and identical.
 */
static int of_aml_clk_mclk_init_register(struct device *dev,
					 struct clk_hw_onecell_data *hw_data)
{
	struct device_node *np = dev_of_node(dev);
	struct aml_clk *clk;
	struct clk_parent_data pdata[5];
	u8 pnum;
	struct clk_init_data init;
	int ret, clkid;

	if (of_aml_clk_get_count(np) != A9_MCLK_CLK_NUM)
		return -EINVAL;

	for (clkid = 0; clkid < A9_MCLK_CLK_NUM; clkid++) {
		if (clkid == A9_CLK_MCLK_0_PRE_DIV ||
		    clkid == A9_CLK_MCLK_1_PRE_DIV) {
			ret = of_aml_clk_get_parent_data(dev, hw_data->hws,
							 0, 0, pdata, &pnum);
			if (ret)
				return ret;
		} else if (clkid == A9_CLK_MCLK_0_SEL ||
			   clkid == A9_CLK_MCLK_1_SEL) {
			ret = of_aml_clk_get_parent_data(dev, hw_data->hws,
							 1, 4, &pdata[1], &pnum);
			if (ret)
				return ret;

			/* Add the clock source for channel 0 of mclk0/1_sel */
			pdata[0].hw = hw_data->hws[clkid - 1];
			pnum += 1;

			clk = to_aml_clk(hw_data->hws[clkid]);
			ret = of_aml_clk_misc_set_parent_table(dev, clk, 1, 4);
			if (ret)
				return ret;
		} else {
			pnum = 1;
			/*
			 *The parent-child relationship among sub-clocks follows
			 * this order:
			 *  - hw_data->hws[0] -> ... -> hw_data->hws[n].
			 */
			pdata[0].hw = hw_data->hws[clkid - 1];
		}

		memcpy(&init, hw_data->hws[clkid]->init, sizeof(init));
		init.name = of_aml_clk_get_name_index(np, clkid);
		if (!init.name)
			return -EINVAL;

		init.num_parents = pnum;
		init.parent_data = pdata;
		hw_data->hws[clkid]->init = &init;
		ret = of_aml_clk_register(dev, hw_data->hws[clkid], clkid);
		if (ret)
			return ret;
	}

	return 0;
}

static int aml_clk_get_clk_data_size(enum aml_clk_type type)
{
	switch (type) {
	case AML_CLKTYPE_MUX:
		return sizeof(struct aml_clk_mux_data);

	case AML_CLKTYPE_DIV:
		return sizeof(struct aml_clk_divider_data);

	case AML_CLKTYPE_GATE:
		return sizeof(struct aml_clk_gate_data);

	case AML_CLKTYPE_COMPOSITE:
		return sizeof(struct aml_clk_composite_data);

	case AML_CLKTYPE_NOGLITCH:
		return sizeof(struct aml_clk_noglitch_data);

	case AML_CLKTYPE_DUALDIV:
		return sizeof(struct aml_clk_dualdiv_data);

	case AML_CLKTYPE_PLL:
		return sizeof(struct aml_pll_data);

	default:
		return -EINVAL;
	}
}

static struct clk_hw_onecell_data *
aml_clk_misc_alloc_hw_data(struct device *dev, struct regmap *regmap,
			   struct clk_hw_onecell_data *hw_data_tmp)
{
	struct device_node *np = dev_of_node(dev);
	int clk_num;
	int ret, clkid;
	struct clk_hw_onecell_data *hw_data;
	struct aml_clk *clk;
	struct aml_clk *clk_tmp;

	clk_num = of_aml_clk_get_count(np);
	if (clk_num != hw_data_tmp->num)
		return ERR_PTR(-EINVAL);

	hw_data = devm_kzalloc(dev, struct_size(hw_data, hws, clk_num),
			       GFP_KERNEL);
	if (!hw_data)
		return ERR_PTR(-ENOMEM);

	hw_data->num = clk_num;
	for (clkid = 0; clkid < clk_num; clkid++) {
		clk_tmp = to_aml_clk(hw_data_tmp->hws[clkid]);
		clk = devm_kmemdup(dev, clk_tmp, sizeof(*clk), GFP_KERNEL);
		if (!clk)
			return ERR_PTR(-ENOMEM);

		clk->map = regmap;
		hw_data->hws[clkid] = &clk->hw;
		ret = aml_clk_get_clk_data_size(clk_tmp->type);
		if (ret < 0)
			return ERR_PTR(ret);

		clk->data = devm_kmemdup(dev, clk_tmp->data, ret, GFP_KERNEL);
		if (!clk->data)
			return ERR_PTR(-ENOMEM);
	}

	return hw_data;
}

static int of_aml_clk_misc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	int ret;
	struct clk_hw_onecell_data *hw_data, *hw_data_tmp;

	/*
	 * NOTE: Here, the pointer returned by of_device_get_match_data() points
	 * to non-const data, and the hw_data parameter passed to
	 * devm_of_clk_add_hw_provider() is also of type (void *).
	 */
	hw_data_tmp = (void *)of_device_get_match_data(dev);
	if (!hw_data_tmp)
		return -EFAULT;

	regmap = aml_clk_regmap_init(pdev);
	if (IS_ERR_OR_NULL(regmap))
		return -EIO;

	hw_data = aml_clk_misc_alloc_hw_data(dev, regmap, hw_data_tmp);
	if (IS_ERR(hw_data))
		return PTR_ERR(hw_data);

	of_aml_clk_regs_init(dev);
	if (hw_data_tmp == &a9_eth_clk_hw_data)
		ret = of_aml_clk_eth_init_register(dev, hw_data);
	else if (hw_data_tmp == &a9_dualdiv_clk_hw_data)
		ret = of_aml_clk_dualdiv_init_register(dev, hw_data);
	else if (hw_data_tmp == &a9_mclk_clk_hw_data)
		ret = of_aml_clk_mclk_init_register(dev, hw_data);
	else
		ret = of_aml_clk_misc_init_register(dev, hw_data);
	if (ret)
		return ret;

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get,
					   hw_data);
}

static const struct of_device_id of_aml_clk_misc_match_table[] = {
	{
		.compatible = "amlogic,a9-sc-ccu",
		.data = &a9_sc_clk_hw_data,
	},
	{
		.compatible = "amlogic,a9-ts-ccu",
		.data = &a9_ts_clk_hw_data,
	},
	{
		.compatible = "amlogic,a9-genout-ccu",
		.data = &a9_genout_clk_hw_data,
	},
	{
		.compatible = "amlogic,a9-clk12_24m-ccu",
		.data = &a9_clk12_24m_clk_hw_data,
	},
	{
		.compatible = "amlogic,a9-vapb_ge2d-ccu",
		.data = &a9_vapbge2d_clk_hw_data,
	},
	{
		.compatible = "amlogic,a9-di-ccu",
		.data = &a9_di_clk_hw_data,
	},
	{
		.compatible = "amlogic,a9-eth-ccu",
		.data = &a9_eth_clk_hw_data,
	},
	{
		.compatible = "amlogic,a9-dualdivmux-ccu",
		.data = &a9_dualdiv_clk_hw_data,
	},
	{
		.compatible = "amlogic,a9-mclk-ccu",
		.data = &a9_mclk_clk_hw_data,
	},
	{}
};
MODULE_DEVICE_TABLE(of, of_aml_clk_misc_match_table);

static struct platform_driver of_aml_clk_misc_driver = {
	.probe		= of_aml_clk_misc_probe,
	.driver		= {
		.name	= "aml-misc-clk",
		.of_match_table = of_aml_clk_misc_match_table,
	},
};

int __init aml_clk_misc_driver_init(void)
{
	return platform_driver_register(&of_aml_clk_misc_driver);
}

void __exit aml_clk_misc_driver_exit(void)
{
	platform_driver_unregister(&of_aml_clk_misc_driver);
}

MODULE_DESCRIPTION("Amlogic A9 Misc Clock Control Units Driver");
MODULE_AUTHOR("Chuan Liu <chuan.liu@amlogic.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("CLK_AMLOGIC");
