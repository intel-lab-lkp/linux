// SPDX-License-Identifier: GPL-2.0
/*
 * VBATTB clock driver
 *
 * Copyright (C) 2024 Renesas Electronics Corp.
 */

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#define VBATTB_BKSCCR			0x1c
#define VBATTB_BKSCCR_SOSEL		BIT(6)
#define VBATTB_SOSCCR2			0x24
#define VBATTB_SOSCCR2_SOSTP2		BIT(0)
#define VBATTB_XOSCCR			0x30
#define VBATTB_XOSCCR_OUTEN		BIT(16)
#define VBATTB_XOSCCR_XSEL		GENMASK(1, 0)
#define VBATTB_XOSCCR_XSEL_4_PF		0x0
#define VBATTB_XOSCCR_XSEL_7_PF		0x1
#define VBATTB_XOSCCR_XSEL_9_PF		0x2
#define VBATTB_XOSCCR_XSEL_12_5_PF	0x3

/**
 * struct vbattb_clk - VBATTB clock data structure
 * @regmap: regmap
 * @hw: clk hw
 * @lock: device lock
 * @load_capacitance: load capacitance
 */
struct vbattb_clk {
	struct regmap *regmap;
	struct clk_hw hw;
	spinlock_t lock;
	u8 load_capacitance;
};

#define to_vbattb_clk(_hw) container_of(_hw, struct vbattb_clk, hw)

static int vbattb_clk_enable(struct clk_hw *hw)
{
	struct vbattb_clk *vbclk = to_vbattb_clk(hw);
	struct regmap *regmap = vbclk->regmap;

	spin_lock(&vbclk->lock);
	regmap_update_bits(regmap, VBATTB_SOSCCR2, VBATTB_SOSCCR2_SOSTP2, 0);
	regmap_update_bits(regmap, VBATTB_XOSCCR, VBATTB_XOSCCR_OUTEN | VBATTB_XOSCCR_XSEL,
			   VBATTB_XOSCCR_OUTEN | vbclk->load_capacitance);
	spin_unlock(&vbclk->lock);

	return 0;
}

static void vbattb_clk_disable(struct clk_hw *hw)
{
	struct vbattb_clk *vbclk = to_vbattb_clk(hw);
	struct regmap *regmap = vbclk->regmap;

	spin_lock(&vbclk->lock);
	regmap_update_bits(regmap, VBATTB_XOSCCR, VBATTB_XOSCCR_OUTEN, 0);
	regmap_update_bits(regmap, VBATTB_SOSCCR2, VBATTB_SOSCCR2_SOSTP2, VBATTB_SOSCCR2_SOSTP2);
	spin_unlock(&vbclk->lock);
}

static int vbattb_clk_is_enabled(struct clk_hw *hw)
{
	struct vbattb_clk *vbclk = to_vbattb_clk(hw);
	struct regmap *regmap = vbclk->regmap;
	unsigned int xosccr, sosccr2;
	int ret;

	spin_lock(&vbclk->lock);
	ret = regmap_read(regmap, VBATTB_XOSCCR, &xosccr);
	if (ret)
		goto unlock;

	ret = regmap_read(regmap, VBATTB_SOSCCR2, &sosccr2);
unlock:
	spin_unlock(&vbclk->lock);

	if (ret)
		return 0;

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

static int vbattb_clk_probe(struct platform_device *pdev)
{
	struct clk_parent_data parent_data = { .fw_name = "vbattb_xtal" };
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct clk_init_data init = {};
	struct vbattb_clk *vbclk;
	u32 load_capacitance;
	struct clk_hw *hw;
	bool bypass;
	int ret;

	vbclk = devm_kzalloc(dev, GFP_KERNEL, sizeof(*vbclk));
	if (!vbclk)
		return -ENOMEM;

	vbclk->regmap = syscon_node_to_regmap(np->parent);
	if (IS_ERR(vbclk->regmap))
		return PTR_ERR(vbclk->regmap);

	bypass = of_property_read_bool(np, "renesas,vbattb-osc-bypass");
	ret = of_property_read_u32(np, "renesas,vbattb-load-nanofarads", &load_capacitance);
	if (ret)
		return ret;

	ret = vbattb_clk_validate_load_capacitance(vbclk, load_capacitance);
	if (ret)
		return ret;

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	regmap_update_bits(vbclk->regmap, VBATTB_BKSCCR, VBATTB_BKSCCR_SOSEL,
			   bypass ? VBATTB_BKSCCR_SOSEL : 0);

	init.name = "vbattclk";
	init.ops = &vbattb_clk_ops;
	init.parent_data = &parent_data;
	init.num_parents = 1;
	init.flags = 0;

	vbclk->hw.init = &init;
	hw = &vbclk->hw;

	spin_lock_init(&vbclk->lock);

	ret = devm_clk_hw_register(dev, hw);
	if (ret)
		goto rpm_put;

	ret = of_clk_add_hw_provider(np, of_clk_hw_simple_get, hw);
	if (ret)
		goto rpm_put;

	return 0;

rpm_put:
	pm_runtime_put(dev);
	return ret;
}

static const struct of_device_id vbattb_clk_match[] = {
	{ .compatible = "renesas,rzg3s-vbattb-clk" },
	{ /* sentinel */ }
};

static struct platform_driver vbattb_clk_driver = {
	.driver		= {
		.name	= "vbattb-clk",
		.of_match_table = vbattb_clk_match,
	},
	.probe = vbattb_clk_probe,
};
module_platform_driver(vbattb_clk_driver);

MODULE_DESCRIPTION("Renesas VBATTB Clock Driver");
MODULE_AUTHOR("Claudiu Beznea <claudiu.beznea.uj@bp.renesas.com>");
MODULE_LICENSE("GPL");
