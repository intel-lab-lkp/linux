// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas SH7750/51 CPG driver
 *
 * Copyright 2023 Yoshinori Sato <ysato@users.sourceforge.jp>
 */

#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <dt-bindings/clock/sh7750.h>

/* PCLK divide rate selector */
static const struct clk_div_table pdiv_table[] = {
	{ .val = 0, .div = 2, },
	{ .val = 1, .div = 3, },
	{ .val = 2, .div = 4, },
	{ .val = 3, .div = 6, },
	{ .val = 4, .div = 8, },
	{ }
};

/* ICLK and BCLK divide rate selector */
static const struct clk_div_table div_table[] = {
	{ .val = 0, .div = 1, },
	{ .val = 1, .div = 2, },
	{ .val = 2, .div = 3, },
	{ .val = 3, .div = 4, },
	{ .val = 4, .div = 6, },
	{ .val = 5, .div = 8, },
	{ }
};

struct cpg_priv {
	struct clk_hw hw;
	spinlock_t clklock;
	void __iomem *frqcr;
	void __iomem *clkstp00;
	u32 mode;
	bool have_div1;
};

enum clk_type {CLK_DIV, CLK_GATE};

struct sh_clk_reg_arg {
	enum clk_type type;
	const char *name;
	const char *parent;
	void __iomem *reg;
	const struct clk_ops *ops;
	union {
		struct {
			/* divider */
			u8 shift;
			u8 width;
			const struct clk_div_table *table;
		} div;
		struct {
			/* gate */
			u8 bit_idx;
			u8 flags;
		} gate;
	};
};

enum {
	FRQCR = 0,
	STBCR = 4,
	WTCNT = 8,
	WTCSR = 12,
	STBCR2 = 16,
	CLKSTP00 = 0,
	CLKSTPCLR00 = 8,
};

static struct cpg_priv *cpg_data;

#define to_priv(_hw) container_of(_hw, struct cpg_priv, hw)

#define div_mask(width)	((1U << (width)) - 1)

static unsigned int _get_maxdiv(const struct clk_div_table *table,
				u8 width)
{
	unsigned int maxdiv = 0, limit = div_mask(width);
	const struct clk_div_table *clkt;

	if (table) {
		for (clkt = table; clkt->div; clkt++)
			if (maxdiv < clkt->div && clkt->div <= limit)
				maxdiv = clkt->div;

		return maxdiv;
	} else
		return limit + 1;
}

static unsigned int _get_div(const struct clk_div_table *table,
			     unsigned int val)
{
	const struct clk_div_table *clkt;

	if (table) {
		for (clkt = table; clkt->div; clkt++)
			if (clkt->val == val)
				return clkt->div;
		return 0;
	} else
		return val + 1;
}

static unsigned int _get_val(const struct clk_div_table *table,
			     unsigned int div)
{
	const struct clk_div_table *clkt;

	if (table) {
		for (clkt = table; clkt->div; clkt++)
			if (clkt->div == div)
				return clkt->val;
		return 0;
	} else
		return div - 1;
}

static unsigned long sh_clk_divider_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct clk_divider *divider = to_clk_divider(hw);
	unsigned int val;
	unsigned int div;

	val = __raw_readw(divider->reg) >> divider->shift;
	val &= div_mask(divider->width);

	div = _get_div(divider->table, val);

	return DIV_ROUND_UP_ULL((u64)parent_rate, div);
}

static bool _is_valid_div(const struct clk_div_table *table, unsigned int div)
{
	const struct clk_div_table *clkt;

	if (!table)
		return true;

	for (clkt = table; clkt->div; clkt++)
		if (clkt->div == div)
			return true;
	return false;
}

static int _round_up_table(const struct clk_div_table *table, int div)
{
	const struct clk_div_table *clkt;
	int up = INT_MAX;

	for (clkt = table; clkt->div; clkt++) {
		if (clkt->div == div)
			return clkt->div;
		else if (clkt->div < div)
			continue;

		if ((clkt->div - div) < (up - div))
			up = clkt->div;
	}

	return up;
}

static inline int _div_round(const struct clk_div_table *table,
			     unsigned long parent_rate, unsigned long rate)
{
	int div = DIV_ROUND_UP_ULL((u64)parent_rate, rate);

	if (table)
		div = _round_up_table(table, div);

	return div;
}

static inline bool _is_best_div(unsigned long rate, unsigned long now,
				unsigned long best)
{
	return now <= rate && now > best;
}

static inline int _next_div(const struct clk_div_table *table, int div)
{
	div++;

	if (table)
		return _round_up_table(table, div);

	return div;
}

static int clk_divider_bestdiv(struct clk_hw *hw, unsigned long rate,
			       unsigned long *best_parent_rate,
			       const struct clk_div_table *table, u8 width)
{
	int bestdiv = 0;
	unsigned int i;
	unsigned long parent_rate, best = 0, now, maxdiv;
	unsigned long parent_rate_saved = *best_parent_rate;

	if (!rate)
		rate = 1;

	maxdiv = _get_maxdiv(table, width);

	if (!(clk_hw_get_flags(hw) & CLK_SET_RATE_PARENT)) {
		parent_rate = *best_parent_rate;
		bestdiv = _div_round(table, parent_rate, rate);
		bestdiv = bestdiv == 0 ? 1 : bestdiv;
		bestdiv = bestdiv > maxdiv ? maxdiv : bestdiv;
		return bestdiv;
	}

	/*
	 * The maximum divider we can use without overflowing
	 * unsigned long in rate * i below
	 */
	maxdiv = min(ULONG_MAX / rate, maxdiv);

	for (i = _next_div(table, 0); i <= maxdiv;
	     i = _next_div(table, i)) {
		if (rate * i == parent_rate_saved) {
			/*
			 * It's the most ideal case if the requested rate can be
			 * divided from parent clock without needing to change
			 * parent rate, so return the divider immediately.
			 */
			*best_parent_rate = parent_rate_saved;
			return i;
		}
		parent_rate = clk_hw_round_rate(clk_hw_get_parent(hw),
					       rate * i);
		now = DIV_ROUND_UP_ULL((u64)parent_rate, i);
		if (_is_best_div(rate, now, best)) {
			bestdiv = i;
			best = now;
			*best_parent_rate = parent_rate;
		}
	}

	if (!bestdiv) {
		bestdiv = _get_maxdiv(table, width);
		*best_parent_rate = clk_hw_round_rate(clk_hw_get_parent(hw), 1);
	}

	return bestdiv;
}

static int sh_clk_divider_determine_rate(struct clk_hw *hw,
					  struct clk_rate_request *req)
{
	struct clk_divider *divider = to_clk_divider(hw);
	unsigned long prate;
	int div;

	prate = req->best_parent_rate;

	div = clk_divider_bestdiv(hw, req->rate, &prate,
				  divider->table, divider->width);
	req->rate = DIV_ROUND_UP_ULL(prate, div);
	req->best_parent_rate = prate;
	return 0;
}

static int sh_divider_get_val(unsigned long rate, unsigned long parent_rate,
			   const struct clk_div_table *table, u8 width)
{
	unsigned int div, value;

	div = DIV_ROUND_UP_ULL((u64)parent_rate, rate);

	if (!_is_valid_div(table, div))
		return -EINVAL;

	value = _get_val(table, div);

	return min(value, div_mask(width));
}

static int sh_clk_divider_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct clk_divider *divider = to_clk_divider(hw);
	int value;
	unsigned long flags = 0;
	u16 val;

	value = sh_divider_get_val(rate, parent_rate, divider->table,
				   divider->width);
	if (value < 0)
		return value;

	if (divider->lock)
		spin_lock_irqsave(divider->lock, flags);
	else
		__acquire(divider->lock);

	val = __raw_readw(divider->reg);
	val &= ~(div_mask(divider->width) << divider->shift);
	val |= value << divider->shift;
	__raw_writew(val, divider->reg);

	if (divider->lock)
		spin_unlock_irqrestore(divider->lock, flags);
	else
		__release(divider->lock);

	return 0;
}

static const struct clk_ops sh_clk_divider_ops = {
	.recalc_rate = sh_clk_divider_recalc_rate,
	.determine_rate = sh_clk_divider_determine_rate,
	.set_rate = sh_clk_divider_set_rate,
};

#define FRQCR_PLL1EN BIT(10)
static const unsigned int pll1mult[] = { 12, 12, 6, 12, 6, 12, 1};

static unsigned long pll_recalc_rate(struct clk_hw *hw,
				      unsigned long parent_rate)
{
	struct cpg_priv *cpg = to_priv(hw);
	unsigned long rate = parent_rate;
	u16 frqcr;

	frqcr = ioread16(cpg->frqcr);
	if (frqcr & FRQCR_PLL1EN) {
		rate *= pll1mult[cpg->mode];
		if (cpg->mode < 6 && cpg->have_div1)
			rate /= 2;
	}
	return rate;
}

static void get_round_rate(struct cpg_priv *cpg,
			   unsigned long *out, bool *pllen,
			   unsigned long rate, unsigned long prate)
{
	long pllout, res;
	bool pll;

	if (cpg->mode < 6 && cpg->have_div1)
		prate /= 2;

	pllout = prate * pll1mult[cpg->mode];
	if (abs(pllout - rate) > abs(prate - rate)) {
		res = prate;
		pll = false;
	} else {
		res = pllout;
		pll = true;
	}
	if (out)
		*out = res;
	if (pllen)
		*pllen = pll;
}

static int pll_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	struct cpg_priv *cpg = to_priv(hw);

	get_round_rate(cpg, &req->rate, NULL, req->rate, req->best_parent_rate);
	return 0;
}

static int pll_set_rate(struct clk_hw *hw,
			unsigned long rate, unsigned long prate)
{
	struct cpg_priv *cpg = to_priv(hw);
	bool oldpll, newpll;
	u16 frqcr;

	frqcr = ioread16(cpg->frqcr);
	get_round_rate(cpg, NULL, &newpll, rate, prate);
	oldpll = frqcr & FRQCR_PLL1EN;
	frqcr &= ~FRQCR_PLL1EN;

	if (newpll) {
		frqcr |= FRQCR_PLL1EN;
		if (!oldpll) {
			/* set PLL wakeup delay time */
			iowrite16(0xa500, cpg->frqcr + WTCNT);
			iowrite16(0xa507, cpg->frqcr + WTCNT);
			iowrite16(0x5a00, cpg->frqcr + WTCSR);
		}
	}
	iowrite16(frqcr, cpg->frqcr);

	/* Test for new PLL state */
	frqcr = ioread16(cpg->frqcr);
	oldpll = frqcr & FRQCR_PLL1EN;
	return !(oldpll == newpll);
}

static const struct clk_ops pll_ops = {
	.recalc_rate = pll_recalc_rate,
	.determine_rate = pll_determine_rate,
	.set_rate = pll_set_rate,
};

#define PLLOUT "pllout"

static int register_pll(struct device_node *node, struct cpg_priv *cpg)
{
	const char *clk_name = node->name;
	const char *parent_name;
	struct clk_init_data init;
	int ret;

	parent_name = of_clk_get_parent_name(node, 0);
	init.name = PLLOUT;
	init.ops = &pll_ops;
	init.flags = 0;
	init.parent_names = &parent_name;
	init.num_parents = 1;
	cpg->hw.init = &init;

	ret = of_clk_hw_register(node, &cpg->hw);
	if (ret < 0) {
		pr_err("%s: failed to register %s pll clock (%d)\n",
		       __func__, clk_name, ret);
		return ret;
	}
	if (ret < 0)
		pr_err("%s: failed to add provider %s (%d)\n",
		       __func__, clk_name, ret);
	return ret;
}

static void sbcr_sw(struct clk_hw *hw, bool on)
{
	struct clk_gate *gate = to_clk_gate(hw);
	u8 msk = BIT(gate->bit_idx);
	u8 val;

	val = readb(gate->reg);
	if (on)
		val &= ~msk;
	else
		val |= msk;
	writeb(val, gate->reg);
}

static int sbcr_enable(struct clk_hw *hw)
{
	sbcr_sw(hw, true);
	return 0;
}

static void sbcr_disable(struct clk_hw *hw)
{
	sbcr_sw(hw, false);
}

static int sbcr_is_enabled(struct clk_hw *hw)
{
	u8 val;
	struct clk_gate *gate = to_clk_gate(hw);

	val = readb(gate->reg);
	val &= 1 << gate->bit_idx;
	return val != 0;
}

static const struct clk_ops gate_stbcr_ops = {
	.enable = sbcr_enable,
	.disable = sbcr_disable,
	.is_enabled = sbcr_is_enabled,
};

static void clkstp00_sw(struct clk_hw *hw, bool on)
{
	u32 val;
	struct clk_gate *gate = to_clk_gate(hw);

	val = BIT(gate->bit_idx);
	if (on)
		writel(val, gate->reg + CLKSTPCLR00);
	else
		writel(val, gate->reg);
}

static int clkstp00_enable(struct clk_hw *hw)
{
	clkstp00_sw(hw, true);
	return 0;
}

static void clkstp00_disable(struct clk_hw *hw)
{
	clkstp00_sw(hw, false);
}

static int clkstp00_is_enabled(struct clk_hw *hw)
{
	u8 val;
	struct clk_gate *gate = to_clk_gate(hw);

	val = readb(gate->reg);
	val &= 1 << gate->bit_idx;
	return val == 0;
}

static const struct clk_ops gate_clkstp00_ops = {
	.enable = clkstp00_enable,
	.disable = clkstp00_disable,
	.is_enabled = clkstp00_is_enabled,
};


static void set_init_data(struct clk_init_data *init,
			  const char *name, const char **parent_name,
			  const struct clk_ops *ops)
{
	init->name = name;
	init->ops = ops;
	init->flags = 0;
	init->parent_names = (parent_name ? parent_name : NULL);
	init->num_parents = (parent_name ? 1 : 0);
}

static int _clk_register(struct device_node *node,
			 unsigned int num_clk, struct sh_clk_reg_arg *args,
			 spinlock_t *lock, struct clk_hw *pll_hw)
{
	struct clk_hw_onecell_data *data;
	struct sh_clk_reg_arg *arg;
	struct clk_divider *div = NULL, *divp;
	struct clk_gate *gate = NULL, *gatep;
	struct clk_init_data init;
	struct clk_hw *reg_hw;
	const char *parent;
	unsigned int num_div, num_gate;
	unsigned int i;
	int ret = 0;

	for (arg = args, num_div = 0, num_gate = 0, i = 0;
	     i < num_clk; arg++, i++) {
		switch (arg->type) {
		case CLK_DIV:
			num_div++;
			break;
		case CLK_GATE:
			num_gate++;
			break;
		}
	}

	data = kzalloc(struct_size(data, hws, num_clk + 1), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	if (num_div > 0) {
		div = kcalloc(num_div, sizeof(*div), GFP_KERNEL);
		if (!div) {
			ret = -ENOMEM;
			goto error;
		}
	}
	if (num_gate > 0) {
		gate = kcalloc(num_gate, sizeof(*gate), GFP_KERNEL);
		if (!gate) {
			ret = -ENOMEM;
			goto error;
		}
	}

	divp = div;
	gatep = gate;
	data->hws[0] = pll_hw;
	data->num = num_clk + 1;

	for (arg = args, i = 0; i < num_clk; arg++, i++) {
		parent = arg->parent;
		set_init_data(&init, arg->name, &parent, arg->ops);
		switch (arg->type) {
		case CLK_DIV:
			divp->reg = arg->reg;
			divp->shift = arg->div.shift;
			divp->width = arg->div.width;
			divp->lock = lock;
			divp->hw.init = &init;
			divp->table = arg->div.table;
			reg_hw = &divp->hw;
			divp++;
			break;
		case CLK_GATE:
			gatep->reg = arg->reg;
			gatep->bit_idx = arg->gate.bit_idx;
			gatep->flags = arg->gate.flags;
			gatep->lock = lock;
			gatep->hw.init = &init;
			reg_hw = &gatep->hw;
			gatep++;
			break;
		}
		ret = of_clk_hw_register(node, reg_hw);
		if (ret < 0)
			goto error;

		data->hws[i + 1] = reg_hw;
	}

	ret = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, data);
	if (ret < 0)
		goto error;
	return 0;
error:
	kfree(data);
	kfree(div);
	kfree(gate);
	return ret;
}

static int register_div(struct device_node *node, struct cpg_priv *cpg)
{
	static const char * const divout[] = {
		"fck", "bck", "ick",
	};
	static const char * const mstpout[] = {
		"sci_clk", "rtc_clk", "tmu012_clk",	/* STBCR */
		"scif_clk", "dmac_clk",			/* STBCR */
		"ubc_clk", "sq_clk",			/* STBCR2 */
		"intc_clk", "tmu34_clk", "pcic_clk",	/* CLKSTP00 */
	};
	struct sh_clk_reg_arg args[ARRAY_SIZE(divout) + ARRAY_SIZE(mstpout)];
	unsigned int i, j;
	int ret;

	for (i = 0; i < ARRAY_SIZE(divout); i++) {
		args[i].type = CLK_DIV;
		args[i].name  = divout[i];
		args[i].parent = PLLOUT;
		args[i].reg = cpg->frqcr;
		args[i].ops = &sh_clk_divider_ops;
		args[i].div.shift = i * 3;
		args[i].div.width = 3;
		args[i].div.table = (i == 0) ? pdiv_table : div_table;
	}
	for (i = 0, j = ARRAY_SIZE(divout); j < ARRAY_SIZE(args); i++, j++) {
		args[j].type = CLK_GATE;
		args[j].name  = mstpout[i];
		args[j].parent = divout[0];
		switch (i) {
		case 0 ... 4:
			args[j].reg = cpg->frqcr + STBCR;
			args[j].gate.bit_idx = i;
			args[j].ops = &gate_stbcr_ops;
			break;
		case 5 ... 6:
			args[j].reg = cpg->frqcr + STBCR2;
			args[j].gate.bit_idx = i - 5;
			args[j].ops = &gate_stbcr_ops;
			break;
		case 7 ... 9:
			args[j].reg = cpg->clkstp00;
			args[j].gate.bit_idx = i - 7;
			args[j].ops = &gate_clkstp00_ops;
			break;
		}
	}

	ret = _clk_register(node, ARRAY_SIZE(args), args,
			    &cpg->clklock, &cpg->hw);
	if (ret < 0)
		pr_err("%pOF: failed to register clock (%d)\n",
		       node, ret);
	return ret;
}

static struct cpg_priv *sh7750_cpg_setup(struct device_node *node)
{
	unsigned int num_parents;
	u32 mode;
	struct cpg_priv *cpg;
	int ret = 0;

	num_parents = of_clk_get_parent_count(node);
	if (num_parents < 1) {
		pr_err("%s: no parent found", node->name);
		return ERR_PTR(-ENODEV);
	}

	of_property_read_u32_index(node, "renesas,mode", 0, &mode);
	if (mode >= 7) {
		pr_err("%s: Invalid clock mode setting (%u)\n",
		       node->name, mode);
		return ERR_PTR(-EINVAL);
	}

	cpg = kzalloc(sizeof(struct cpg_priv), GFP_KERNEL);
	if (!cpg) {
		pr_err("%pOF: failed to alloc memory", node);
		return ERR_PTR(-ENOMEM);
	}

	cpg->frqcr = of_iomap(node, 0);
	if (cpg->frqcr == NULL) {
		pr_err("%pOF: failed to map divide register", node);
		ret = -ENODEV;
		goto cpg_free;
	}

	cpg->clkstp00 = of_iomap(node, 1);
	if (cpg->clkstp00 == NULL) {
		pr_err("%pOF: failed to map clkstp00 register", node);
		ret = -ENODEV;
		goto unmap_frqcr;
	}
	cpg->mode = mode;

	ret = register_pll(node, cpg);
	if (ret < 0)
		goto unmap_clkstp00;

	ret = register_div(node, cpg);
	if (ret < 0)
		goto unmap_clkstp00;

	return cpg;

unmap_clkstp00:
	iounmap(cpg->clkstp00);
unmap_frqcr:
	iounmap(cpg->frqcr);
cpg_free:
	kfree(cpg);
	return ERR_PTR(ret);
}

static void __init sh7750_cpg_init(struct device_node *node)
{
	cpg_data = sh7750_cpg_setup(node);
	if (IS_ERR(cpg_data))
		cpg_data = NULL;
	else
		cpg_data->have_div1 = !of_device_is_compatible(node, "renesas,sh7750r");
}

CLK_OF_DECLARE_DRIVER(sh7750_cpg, "renesas,sh7750-cpg",
		      sh7750_cpg_init);

static int sh7750_cpg_probe(struct platform_device *pdev)
{
	if (cpg_data)
		return 0;
	cpg_data = sh7750_cpg_setup(pdev->dev.of_node);
	if (IS_ERR(cpg_data))
		return PTR_ERR(cpg_data);
	cpg_data->have_div1 = (of_device_get_match_data(&pdev->dev) != NULL);
	return 0;
}

static const struct of_device_id sh7750_cpg_of_match[] = {
	{ .compatible = "renesas,sh7750-cpg",
	  .data = (void *)1 },
	{ .compatible = "renesas,sh7750s-cpg",
	  .data = (void *)1 },
	{ .compatible = "renesas,sh7750r-cpg",
	  .data = 0 },
	{ .compatible = "renesas,sh7751-cpg",
	  .data = (void *)1 },
	{ .compatible = "renesas,sh7751r-cpg",
	  .data = 0 },
	{ }
};

static struct platform_driver sh7750_cpg_driver = {
	.probe  = sh7750_cpg_probe,
	.driver = {
		.name = "sh7750-cpg",
		.of_match_table = sh7750_cpg_of_match,
	},
};
builtin_platform_driver(sh7750_cpg_driver);
