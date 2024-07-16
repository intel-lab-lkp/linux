// SPDX-License-Identifier: GPL-2.0
/*
 * VBATTB clock driver
 *
 * Copyright (C) 2024 Renesas Electronics Corp.
 */

#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#define VBATTB_BKSCCR			0x0
#define VBATTB_BKSCCR_SOSEL		BIT(6)
#define VBATTB_SOSCCR2			0x8
#define VBATTB_SOSCCR2_SOSTP2		BIT(0)
#define VBATTB_XOSCCR			0x14
#define VBATTB_XOSCCR_OUTEN		BIT(16)
#define VBATTB_XOSCCR_XSEL		GENMASK(1, 0)
#define VBATTB_XOSCCR_XSEL_4_PF		0x0
#define VBATTB_XOSCCR_XSEL_7_PF		0x1
#define VBATTB_XOSCCR_XSEL_9_PF		0x2
#define VBATTB_XOSCCR_XSEL_12_5_PF	0x3

/**
 * struct vbattb_clk - VBATTB clock data structure
 * @base: base address
 * @hw: clk hw
 * @lock: lock
 * @load_capacitance: load capacitance
 */
struct vbattb_clk {
	void __iomem *base;
	struct clk_hw hw;
	spinlock_t lock;
	u8 load_capacitance;
};

#define to_vbattb_clk(_hw) container_of(_hw, struct vbattb_clk, hw)

static void vbattb_clk_update_bits(void __iomem *base, u32 offset, u32 mask, u32 val)
{
	u32 tmp;

	tmp = readl_relaxed(base + offset);
	tmp &= ~mask;
	tmp |= (val & mask);
	writel_relaxed(tmp, base + offset);
}

static int vbattb_clk_enable(struct clk_hw *hw)
{
	struct vbattb_clk *vbclk = to_vbattb_clk(hw);
	void __iomem *base = vbclk->base;

	guard(spinlock)(&vbclk->lock);

	vbattb_clk_update_bits(base, VBATTB_SOSCCR2, VBATTB_SOSCCR2_SOSTP2, 0);
	vbattb_clk_update_bits(base, VBATTB_XOSCCR, VBATTB_XOSCCR_OUTEN | VBATTB_XOSCCR_XSEL,
			       VBATTB_XOSCCR_OUTEN | vbclk->load_capacitance);

	return 0;
}

static void vbattb_clk_disable(struct clk_hw *hw)
{
	struct vbattb_clk *vbclk = to_vbattb_clk(hw);
	void __iomem *base = vbclk->base;

	guard(spinlock)(&vbclk->lock);

	vbattb_clk_update_bits(base, VBATTB_XOSCCR, VBATTB_XOSCCR_OUTEN, 0);
	vbattb_clk_update_bits(base, VBATTB_SOSCCR2, VBATTB_SOSCCR2_SOSTP2, VBATTB_SOSCCR2_SOSTP2);
}

static int vbattb_clk_is_enabled(struct clk_hw *hw)
{
	struct vbattb_clk *vbclk = to_vbattb_clk(hw);
	void __iomem *base = vbclk->base;
	unsigned int xosccr, sosccr2;

	guard(spinlock)(&vbclk->lock);

	xosccr = readl_relaxed(base + VBATTB_XOSCCR);
	sosccr2 = readl_relaxed(base + VBATTB_SOSCCR2);

	return ((xosccr & VBATTB_XOSCCR_OUTEN) && !(sosccr2 & VBATTB_SOSCCR2_SOSTP2));
}

static const struct clk_ops vbattb_clk_ops = {
	.enable = vbattb_clk_enable,
	.disable = vbattb_clk_disable,
	.is_enabled = vbattb_clk_is_enabled,
};

static int vbattb_clk_validate_load_capacitance(struct vbattb_clk *vbclk, u32 load_capacitance)
{
	switch (load_capacitance) {
	case 4000:
		vbclk->load_capacitance = VBATTB_XOSCCR_XSEL_4_PF;
		break;
	case 7000:
		vbclk->load_capacitance = VBATTB_XOSCCR_XSEL_7_PF;
		break;
	case 9000:
		vbclk->load_capacitance = VBATTB_XOSCCR_XSEL_9_PF;
		break;
	case 12500:
		vbclk->load_capacitance = VBATTB_XOSCCR_XSEL_12_5_PF;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int vbattb_clk_need_bypass(struct device *dev)
{
	struct clk *clkin, *xin;

	clkin = devm_clk_get_optional(dev, "clkin");
	xin = devm_clk_get_optional(dev, "xin");

	if (!IS_ERR_OR_NULL(clkin) && !IS_ERR_OR_NULL(xin))
		return -EINVAL;
	else if (!clkin && !IS_ERR_OR_NULL(xin))
		return 0;
	else if (!IS_ERR_OR_NULL(clkin) && !xin)
		return 1;

	return -EINVAL;
}

static int vbattb_clk_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct clk_parent_data parent_data = {};
	struct device *dev = &pdev->dev;
	struct clk_init_data init = {};
	struct vbattb_clk *vbclk;
	u32 load_capacitance;
	struct clk_hw *hw;
	int ret, bypass;

	vbclk = devm_kzalloc(dev, sizeof(*vbclk), GFP_KERNEL);
	if (!vbclk)
		return -ENOMEM;

	vbclk->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vbclk->base))
		return PTR_ERR(vbclk->base);

	bypass = vbattb_clk_need_bypass(dev);
	if (bypass < 0) {
		return bypass;
	} else if (bypass) {
		parent_data.fw_name = "clkin";
		bypass = VBATTB_BKSCCR_SOSEL;
	} else {
		parent_data.fw_name = "xin";
	}

	ret = of_property_read_u32(np, "renesas,vbattb-load-nanofarads", &load_capacitance);
	if (ret)
		return ret;

	ret = vbattb_clk_validate_load_capacitance(vbclk, load_capacitance);
	if (ret)
		return ret;

	vbattb_clk_update_bits(vbclk->base, VBATTB_BKSCCR, VBATTB_BKSCCR_SOSEL, bypass);

	spin_lock_init(&vbclk->lock);

	init.name = "vbattclk";
	init.ops = &vbattb_clk_ops;
	init.parent_data = &parent_data;
	init.num_parents = 1;
	init.flags = 0;

	vbclk->hw.init = &init;
	hw = &vbclk->hw;

	ret = devm_clk_hw_register(dev, hw);
	if (ret)
		return ret;

	return of_clk_add_hw_provider(np, of_clk_hw_simple_get, hw);
}

static const struct of_device_id vbattb_clk_match[] = {
	{ .compatible = "renesas,r9a08g045-vbattb-clk" },
	{ /* sentinel */ }
};

static struct platform_driver vbattb_clk_driver = {
	.driver		= {
		.name	= "renesas-vbattb-clk",
		.of_match_table = vbattb_clk_match,
	},
	.probe = vbattb_clk_probe,
};
module_platform_driver(vbattb_clk_driver);

MODULE_DESCRIPTION("Renesas VBATTB Clock Driver");
MODULE_AUTHOR("Claudiu Beznea <claudiu.beznea.uj@bp.renesas.com>");
MODULE_LICENSE("GPL");
