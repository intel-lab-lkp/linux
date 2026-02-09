// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (c) 2026 Amlogic, Inc. All rights reserved
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/math64.h>
#include <linux/module.h>

#include "clk.h"
#include "clk-pll.h"

/*
 * Amlogic PLL module:
 *
 *           +------------------------------------------------------+
 *           |      +-------+      +-----+                          |
 * osc_in --------->| div N |----->|     |   +-----+                |
 *           |      +-------+      |     |   |     |   +--------+   |
 *           |                     |     |-->| VCO |-->| div OD |------>pll_out
 *           |      +----------+   |     |   |     |   +--------+   |
 *           |  +-->| M & frac |-->|     |   +-----+                |
 *           |  |   +----------+   +-----+      |                   |
 *           |  |                               |                   |
 *           |  +-------------------------------+                   |
 *           |                                                      |
 *           +------------------------------------------------------+
 *
 * PLL output frequency calculation formula:
 *
 * pll_out = ((osc_in * (M + (frac / frac_max))) >> N) >> OD
 *
 * NOTE: Some PLLs support fractional multiplication. 'frac_max' is the counter
 * used for fractional multiplication. Currently, there are two design values of
 * 'frac_max' in Amlogic PLLs:
 *   - frac_max = 2^17: Mainly used for Amlogic general-purpose PLLs, such as
 *     gp_pll.
 *   - frac_max = 100000: The PLL step is of integer type (which helps eliminate
 *     accumulated errors in the driver), such as hifi_pll.
 *
 * Configuring 'N' for pre-division may affect the PLL bandwidth, phase margin,
 * etc., resulting in increased PLL output jitter. Therefore, it is not
 * recommended to arbitrarily configure 'N' for pre-division, and by default our
 * driver does not enable 'N' pre-division.
 *
 * If a special PLL output frequency is required and 'N' pre-division must be
 * used, and the resulting PLL output jitter is within an acceptable range, the
 * PLL configuration parameters can be specified via 'pll_table' (refer to the
 * definition of 'struct aml_pll_parms_table').
 */

#define AML_PLL_REG0_OFFSET			(0)
#define AML_PLL_REG1_OFFSET			(4)

struct aml_pll_reg_parms {
	union {
		struct {
			u32 m		:9;  /* bit0 - bit8 */
			u32 reserved	:3;  /* bit9 - bit11 */
			u32 n		:3;  /* bit12 - bit14 */
			u32 reserved1	:5;  /* bit15 - bit19 */
			u32 od		:3;  /* bit20 - bit22 */
			u32 reserved2	:3;  /* bit23 - bit25 */
			u32 force_lock	:1;  /* bit26 */
			u32 div0p5	:1;  /* bit27 */
			u32 en		:1;  /* bit28 */
			u32 rstn	:1;  /* bit29 */
			u32 l_detect_en	:1;  /* bit30 */
			u32 lock	:1;  /* bit31 */
		} bits;
		u32 val;
	} reg0;
	union {
		struct {
			u32 frac	:17;  /* bit0 - bit16 */
			u32 reserved	:15;  /* bit17 - bit31 */
		} bits;
		u32 val;
	} reg1;
};

static unsigned long __aml_pll_params_to_rate(unsigned long parent_rate,
					      unsigned int m, unsigned int n,
					      unsigned int frac,
					      unsigned int od,
					      struct aml_pll_data *pll)
{
	u64 rate = (u64)parent_rate * m;

	if (pll->flags & AML_PLL_M_EN0P5)
		rate = rate >> 1;

	if (frac && pll->frac_max) {
		u64 frac_rate = DIV_ROUND_UP_ULL((u64)parent_rate * frac,
						 pll->frac_max);
		if (pll->flags & AML_PLL_M_EN0P5)
			frac_rate = frac_rate >> 1;

		rate += frac_rate;
	}

	/* The 'n' divider has fixed power-of-two property */
	rate = rate >> n;

	rate = rate >> od;

	/*
	 * FIXME: CCF uses 'unsigned long' for rate values, which may overflow
	 * on 32-bit systems.
	 */
	return (unsigned long)rate;
}

static unsigned long aml_pll_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_pll_data *pll = clk->data;
	struct aml_pll_reg_parms regs;

	regmap_read(clk->map, AML_PLL_REG0_OFFSET, &regs.reg0.val);
	regmap_read(clk->map, AML_PLL_REG1_OFFSET, &regs.reg1.val);

	return __aml_pll_params_to_rate(parent_rate, regs.reg0.bits.m,
					regs.reg0.bits.n, regs.reg1.bits.frac,
					regs.reg0.bits.od, pll);
}

static bool aml_pll_is_better(unsigned long rate, unsigned long best,
			      unsigned long now, struct aml_pll_data *pll)
{
	if (pll->flags & AML_PLL_ROUND_CLOSEST) {
		if (abs(now - rate) < abs(best - rate))
			return true;
	} else {
		/* Round down */
		if (now <= rate && best < now)
			return true;
	}

	return false;
}

static int aml_pll_get_table(unsigned long rate, unsigned long parent_rate,
			     struct aml_pll_parms_table *parm,
			     struct aml_pll_data *pll, unsigned long *out_rate)
{
	unsigned int idx, best_idx;
	unsigned long now, best = 0;

	for (idx = 0; idx < pll->table_count; idx++) {
		now = __aml_pll_params_to_rate(parent_rate, pll->table[idx].m,
					       pll->table[idx].n,
					       pll->table[idx].frac,
					       pll->table[idx].od, pll);
		if (aml_pll_is_better(rate, best, now, pll)) {
			best = now;
			best_idx = idx;

			if (now == rate)
				break;
		}
	}

	if (idx >= pll->table_count)
		return -EINVAL;

	parm->m = pll->table[best_idx].m;
	parm->n = pll->table[best_idx].n;
	parm->frac = pll->table[best_idx].frac;
	parm->od = pll->table[best_idx].od;

	*out_rate = best;

	return 0;
}

static int aml_pll_get_range(unsigned long rate, unsigned long parent_rate,
			     struct aml_pll_parms_table *parm,
			     struct aml_pll_data *pll, unsigned long *out_rate)
{
	unsigned int idx, t_m;
	u64 vco_rate, req_vco_rate;
	u64 val;
	unsigned int frac = 0;
	unsigned long frac_step;
	unsigned long now_rate, best_rate = 0;
	unsigned int best_m, best_frac, best_od;

	if (pll->flags & AML_PLL_M_EN0P5)
		parent_rate = parent_rate >> 1;

	/*
	 * NOTE: Configuring the 'n' divider may increase the PLL output
	 * jitter. Here fix 'n = 0' to disable pre-division.
	 *
	 * If absolutely required (The resulting PLL output jitter is within an
	 * acceptable range), ONLY implement via 'pll->table' configuration.
	 */
	parm->n = 0;

	for (idx = 0; idx <= pll->od_max; idx++) {
		req_vco_rate = (u64)rate << idx;
		if (req_vco_rate < pll->range.min)
			continue;

		if (req_vco_rate > pll->range.max)
			goto out;

		/*
		 * Ensure that the calculated vco_rate does not exceed
		 * pll->range.max.
		 */
		if ((pll->flags & AML_PLL_ROUND_CLOSEST) &&
		    !(pll->frac_max) &&
		    (req_vco_rate + (parent_rate >> 1)) <= pll->range.max)
			t_m = DIV_ROUND_CLOSEST_ULL(req_vco_rate, parent_rate);
		else
			t_m = div_u64(req_vco_rate,  parent_rate);

		vco_rate = (u64)parent_rate * t_m;
		if (pll->frac_max) {
			val = div_u64(req_vco_rate * pll->frac_max,
				      parent_rate);
			val -= t_m * pll->frac_max;
			frac = min((unsigned int)val, (pll->frac_max - 1));

			frac_step = parent_rate / pll->frac_max;
			vco_rate += frac_step * frac;

			/*
			 * With AML_PLL_ROUND_CLOSEST configured, the condition
			 * req_vco_rate >= vco_rate is guaranteed to be true.
			 */
			val = req_vco_rate - vco_rate;
			if (pll->flags & AML_PLL_ROUND_CLOSEST &&
			    (abs(val - frac_step) < val) &&
			    (vco_rate + frac_step <= pll->range.max)) {
				frac += 1;
				vco_rate += frac_step;
			}
		}

		if (vco_rate < pll->range.min)
			continue;

		now_rate = vco_rate >> idx;
		if (aml_pll_is_better(rate, best_rate, now_rate, pll)) {
			best_rate = now_rate;

			best_m = t_m;
			best_frac = frac;
			best_od = idx;

			if (now_rate == rate)
				break;
		}
	}

out:
	if (!best_rate)
		return -EINVAL;

	parm->m = best_m;
	parm->frac = best_frac;
	parm->od = best_od;

	*out_rate = best_rate;

	return 0;
}

static int aml_pll_get_best_parms(unsigned long rate, unsigned long parent_rate,
				  struct aml_pll_parms_table *parm,
				  struct aml_pll_data *pll,
				  unsigned long *out_rate)
{
	unsigned long range_rate = 0, table_rate = 0;
	struct aml_pll_parms_table range_parm, table_parm;

	aml_pll_get_range(rate, parent_rate, &range_parm, pll, &range_rate);
	aml_pll_get_table(rate, parent_rate, &table_parm, pll, &table_rate);
	if (!range_rate && !table_rate)
		return -EINVAL;

	if (aml_pll_is_better(rate, range_rate, table_rate, pll)) {
		if (parm) {
			parm->m = table_parm.m;
			parm->n = table_parm.n;
			parm->frac = table_parm.frac;
			parm->od = table_parm.od;
		}

		if (out_rate)
			*out_rate = table_rate;
	} else {
		if (parm) {
			parm->m = range_parm.m;
			parm->n = range_parm.n;
			parm->frac = range_parm.frac;
			parm->od = range_parm.od;
		}

		if (out_rate)
			*out_rate = range_rate;
	}

	return 0;
}

static int aml_pll_determine_rate(struct clk_hw *hw,
				  struct clk_rate_request *req)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_pll_data *pll = clk->data;
	int ret;

	if (pll->flags & AML_PLL_READ_ONLY) {
		req->rate = clk_hw_get_rate(hw);
		return 0;
	}

	ret = aml_pll_get_best_parms(req->rate, req->best_parent_rate, NULL,
				     pll, &req->rate);
	if (ret)
		return ret;

	return 0;
}

static int aml_pll_wait_lock(struct clk_hw *hw)
{
	struct aml_clk *clk = to_aml_clk(hw);
	int delay = 1000;
	struct aml_pll_reg_parms regs;

	do {
		regmap_read(clk->map, AML_PLL_REG0_OFFSET, &regs.reg0.val);
		/* Wait for the PLL to lock */
		if (regs.reg0.bits.lock)
			return 0;

		udelay(1);
	} while (delay--);

	return -ETIMEDOUT;
}

static int aml_pll_is_enabled(struct clk_hw *hw)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_pll_reg_parms regs;

	regmap_read(clk->map, AML_PLL_REG0_OFFSET, &regs.reg0.val);
	/* Enable and lock bit equal 1, it locks */
	if (regs.reg0.bits.en && regs.reg0.bits.lock)
		return 1;

	return 0;
}

static void aml_pll_disable(struct clk_hw *hw)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_pll_reg_parms regs;

	regmap_read(clk->map, AML_PLL_REG0_OFFSET, &regs.reg0.val);

	/* Put the pll is in reset */
	regs.reg0.bits.rstn = 0;
	regmap_write(clk->map, AML_PLL_REG0_OFFSET, regs.reg0.val);

	/* Disable lock detect module */
	regs.reg0.bits.l_detect_en = 0;
	regmap_write(clk->map, AML_PLL_REG0_OFFSET, regs.reg0.val);

	/* Disable the pll */
	regs.reg0.bits.en = 0;
	regmap_write(clk->map, AML_PLL_REG0_OFFSET, regs.reg0.val);
}

/*
 * NOTE: Under extreme conditions (such as low temperatures), PLL lock may fail.
 *
 * Although we proactively address this by optimizing the PLL enable timing, a
 * retry mechanism is added here to minimize the probability of PLL lock
 * failure.
 */
#define PLL_LOCK_RETRY_MAX		10

static int aml_pll_enable(struct clk_hw *hw)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_pll_reg_parms regs;
	int retry = 0;

	/* Do nothing if the PLL is already enabled */
	if (clk_hw_is_enabled(hw))
		return 0;

	do {
		/* Make sure the pll is disabled */
		aml_pll_disable(hw);

		regmap_read(clk->map, AML_PLL_REG0_OFFSET, &regs.reg0.val);

		/* Powers up PLL supply */
		regs.reg0.bits.en = 1;
		regmap_write(clk->map, AML_PLL_REG0_OFFSET, regs.reg0.val);

		/*
		 * Wait for Bandgap and LDO to power up and stabilize.
		 *
		 * The spinlock is held during the execution of clk_enable(),
		 * so usleep() cannot be used here.
		 */
		udelay(20);

		/* Take the pll out reset */
		regs.reg0.bits.rstn = 1;
		regmap_write(clk->map, AML_PLL_REG0_OFFSET, regs.reg0.val);

		/* Wait for PLL loop stabilization */
		udelay(20);

		/* Take the pll out lock reset */
		regs.reg0.bits.l_detect_en = 1;
		regmap_write(clk->map, AML_PLL_REG0_OFFSET, regs.reg0.val);

		if (!aml_pll_wait_lock(hw))
			return 0;
	} while (retry > PLL_LOCK_RETRY_MAX);

	/* disable PLL when PLL lock failed. */
	aml_pll_disable(hw);
	pr_warn("%s: PLL lock failed\n", clk_hw_get_name(hw));

	return -EIO;
}

static int aml_pll_set_rate(struct clk_hw *hw, unsigned long rate,
			    unsigned long parent_rate)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_pll_data *pll = clk->data;
	struct aml_pll_reg_parms regs;
	struct aml_pll_parms_table parm;
	int enabled, ret;

	if (parent_rate == 0 || rate == 0)
		return -EINVAL;

	ret = aml_pll_get_best_parms(rate, parent_rate, &parm, pll, NULL);
	if (ret)
		return ret;

	enabled = aml_pll_is_enabled(hw);

	regmap_read(clk->map, AML_PLL_REG0_OFFSET, &regs.reg0.val);
	/* If neither m nor n is changed, there is no need to disable the PLL */
	if ((regs.reg0.bits.m != parm.m || regs.reg0.bits.n != parm.n) &&
	    enabled)
		aml_pll_disable(hw);

	regs.reg0.bits.m = parm.m;
	regs.reg0.bits.n = parm.n;
	regmap_write(clk->map, AML_PLL_REG0_OFFSET, regs.reg0.val);

	if (pll->frac_max) {
		regmap_read(clk->map, AML_PLL_REG1_OFFSET, &regs.reg1.val);
		regs.reg1.bits.frac = parm.frac;
		regmap_write(clk->map, AML_PLL_REG1_OFFSET, regs.reg1.val);
	}

	if (pll->od_max) {
		regs.reg0.bits.od = parm.od;
		regmap_write(clk->map, AML_PLL_REG0_OFFSET, regs.reg0.val);
	}

	if (!enabled)
		return 0;

	return aml_pll_enable(hw);
}

static int aml_pll_save_context(struct clk_hw *hw)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_pll_data *pll = clk->data;
	unsigned long p_rate = clk_hw_get_rate(clk_hw_get_parent(hw));

	pll->context_is_enabled = aml_pll_is_enabled(hw);
	pll->context_rate = aml_pll_recalc_rate(hw, p_rate);

	return 0;
}

static void aml_pll_restore_context(struct clk_hw *hw)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_pll_data *pll = clk->data;
	unsigned long p_rate = clk_hw_get_rate(clk_hw_get_parent(hw));

	aml_pll_set_rate(hw, pll->context_rate, p_rate);
	if (pll->context_is_enabled)
		aml_pll_enable(hw);
	else
		aml_pll_disable(hw);
}

/*
 * If debugfs is enabled, two nodes "clk_available_rates" and "clk_type" will be
 * created under the corresponding debugfs directory to assist with debugging or
 * testing.
 */
#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>

static unsigned long aml_pll_get_rate_step(struct aml_pll_data *pll,
					   unsigned long parent_rate)
{
	if (pll->flags & AML_PLL_M_EN0P5)
		parent_rate = parent_rate >> 1;

	if (pll->frac_max)
		return parent_rate / pll->frac_max;
	else
		return parent_rate;
}

enum round_type {
	ROUND_DOWN	= 0,
	ROUND_UP
};

static int aml_pll_get_best_rate(unsigned long rate, unsigned long step_rate,
				 u64 min_vco_rate, u64 max_vco_rate,
				 u8 od_max, enum round_type round,
				 unsigned long *out_rate)
{
	int i;
	u64 vco_rate;
	unsigned long now_rate, best_rate = 0;

	for (i = 0; i <= od_max; i++) {
		vco_rate = rate << i;
		if (vco_rate < min_vco_rate)
			continue;

		if (vco_rate > max_vco_rate)
			break;

		if (vco_rate % step_rate == 0) {
			best_rate = rate;

			break;
		}

		if (round == ROUND_DOWN) {
			vco_rate = vco_rate - (vco_rate % step_rate);
			now_rate = vco_rate >> i;
			if ((rate - now_rate) < (rate - best_rate))
				best_rate = now_rate;
		} else {
			vco_rate = vco_rate + step_rate;
			vco_rate = vco_rate - (vco_rate % step_rate);
			now_rate = vco_rate >> i;
			if ((now_rate - rate) < (best_rate - rate))
				best_rate = now_rate;
		}
	}

	if (!best_rate)
		return -EINVAL;

	*out_rate = best_rate;

	return 0;
}

static int aml_pll_get_rate_range(struct clk_hw *hw, unsigned long parent_rate,
				  unsigned long *min, unsigned long *max)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_pll_data *pll = clk->data;
	unsigned long step = aml_pll_get_rate_step(pll, parent_rate);
	unsigned long min_rate, max_rate;
	unsigned long core_min_rate, core_max_rate;
	int ret;

	min_rate = pll->range.min >> pll->od_max;
	max_rate = pll->range.max;

	clk_hw_get_rate_range(hw, &core_min_rate, &core_max_rate);
	if (min_rate < core_min_rate)
		min_rate = core_min_rate;

	ret = aml_pll_get_best_rate(min_rate, step, pll->range.min,
				    pll->range.max, pll->od_max, ROUND_UP, min);
	if (ret)
		return ret;

	if (max_rate > core_max_rate)
		max_rate = core_max_rate;

	ret = aml_pll_get_best_rate(max_rate, step, pll->range.min,
				    pll->range.max, pll->od_max,
				    ROUND_DOWN, max);
	if (ret)
		return ret;

	return 0;
}

static int aml_pll_available_rates_show(struct seq_file *s, void *data)
{
	struct clk_hw *hw = s->private;
	struct clk_hw *phw = clk_hw_get_parent(hw);
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_pll_data *pll = clk->data;
	u64 rate, prate = 0;
	unsigned long min, max;
	int i, ret;

	if (!phw) {
		pr_err("%s: can't get parent\n", clk_hw_get_name(hw));

		return -ENOENT;
	}

	prate = clk_hw_get_rate(phw);
	if (pll->flags & AML_PLL_READ_ONLY) {
		seq_printf(s, "%ld\n", clk_hw_get_rate(hw));

		return 0;
	}

	if (pll->range.min || pll->range.max) {
		ret = aml_pll_get_rate_range(hw, prate, &min, &max);
		if (ret)
			return ret;

		seq_printf(s, "min_rate:%ld\n", min);
		seq_printf(s, "max_rate:%ld\n", max);
	} else if (pll->table) {
		if (pll->flags & AML_PLL_M_EN0P5)
			prate >>= 1;

		clk_hw_get_rate_range(hw, &min, &max);

		for (i = 0; pll->table[i].m != 0; i++) {
			rate = (prate * pll->table[i].m) >> pll->table[i].n;

			rate = rate >> pll->table[i].od;
			if (rate < min || rate > max)
				continue;

			seq_printf(s, "%ld\n", (unsigned long)rate);
		}
	} else {
		seq_printf(s, "%ld\n", clk_hw_get_rate(hw));
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(aml_pll_available_rates);

static void aml_pll_debug_init(struct clk_hw *hw, struct dentry *dentry)
{
	debugfs_create_file("clk_type", 0444, dentry, hw, &aml_clk_type_fops);
	debugfs_create_file("clk_available_rates", 0444, dentry, hw,
			    &aml_pll_available_rates_fops);
}
#endif /* CONFIG_DEBUG_FS */

const struct clk_ops aml_pll_ops = {
	.recalc_rate	= aml_pll_recalc_rate,
	.determine_rate	= aml_pll_determine_rate,
	.set_rate	= aml_pll_set_rate,
	.is_enabled	= aml_pll_is_enabled,
	.save_context	= aml_pll_save_context,
	.restore_context = aml_pll_restore_context,
	.enable		= aml_pll_enable,
	.disable	= aml_pll_disable,
#ifdef CONFIG_DEBUG_FS
	.debug_init	= aml_pll_debug_init,
#endif /* CONFIG_DEBUG_FS */
};
EXPORT_SYMBOL_NS_GPL(aml_pll_ops, "CLK_AMLOGIC");

const struct clk_ops aml_pll_ro_ops = {
	.recalc_rate	= aml_pll_recalc_rate,
	.is_enabled	= aml_pll_is_enabled,
};
EXPORT_SYMBOL_NS_GPL(aml_pll_ro_ops, "CLK_AMLOGIC");

MODULE_DESCRIPTION("Amlogic PLL Driver");
MODULE_AUTHOR("Chuan Liu <chuan.liu@amlogic.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("CLK_AMLOGIC");
