// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Ivaylo Ivanov <ivo.ivanov.ivanov1@gmail.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/soc/samsung/exynos-regs-pmu.h>

#define EXYNOS2200_USBCON_LINKCTRL		0x4
#define LINKCTRL_FORCE_QACT			BIT(8)

#define EXYNOS2200_USBCON_UTMI_CTRL		0x10
#define UTMI_CTRL_FORCESLEEP			BIT(13)
#define UTMI_CTRL_FORCESUSPEND			BIT(12)
#define UTMI_CTRL_FORCE_VBUSVALID		BIT(1)
#define UTMI_CTRL_FORCE_BVALID			BIT(0)

#define EXYNOS2200_USBCON_LINK_CLKRST		0xc
#define LINK_CLKRST_SW_RST			BIT(0)

struct exynos2200_usbcon_phy_drvdata {
	const char * const *clk_names;
	int num_clks;
};

struct exynos2200_usbcon_phy {
	struct phy *phy;
	void __iomem *base;
	struct regmap *reg_pmu;
	struct clk_bulk_data *clks;
	const struct exynos2200_usbcon_phy_drvdata *drv_data;
	u32 pmu_offset;
	struct phy *hs_phy;
};

static void exynos2200_usbcon_phy_isol(struct exynos2200_usbcon_phy *inst,
				       bool isolate)
{
	unsigned int val;

	if (!inst->reg_pmu)
		return;

	val = isolate ? 0 : EXYNOS4_PHY_ENABLE;

	regmap_update_bits(inst->reg_pmu, inst->pmu_offset,
			   EXYNOS4_PHY_ENABLE, val);
}

static void exynos2200_usbcon_phy_write_mask(void __iomem *base, u32 offset,
					     u32 mask, u32 val)
{
	u32 reg;

	reg = readl(base + offset);
	reg &= ~mask;
	reg |= val & mask;
	writel(reg, base + offset);

	/* Ensure above write is completed */
	readl(base + offset);
}

static int exynos2200_usbcon_phy_init(struct phy *p)
{
	int ret;
	struct exynos2200_usbcon_phy *phy = phy_get_drvdata(p);

	/* Power-on PHY ... */
	ret = clk_bulk_prepare_enable(phy->drv_data->num_clks, phy->clks);
	if (ret)
		return ret;

	/*
	 * ... and ungate power via PMU. Without this here, we can't access
	 * registers
	 */
	exynos2200_usbcon_phy_isol(phy, false);

	/*
	 * Disable HWACG (hardware auto clock gating control). This will force
	 * QACTIVE signal in Q-Channel interface to HIGH level, to make sure
	 * the PHY clock is not gated by the hardware.
	 */
	exynos2200_usbcon_phy_write_mask(phy->base, EXYNOS2200_USBCON_LINKCTRL,
					 LINKCTRL_FORCE_QACT,
					 LINKCTRL_FORCE_QACT);

	/* Reset Link */
	exynos2200_usbcon_phy_write_mask(phy->base,
					 EXYNOS2200_USBCON_LINK_CLKRST,
					 LINK_CLKRST_SW_RST,
					 LINK_CLKRST_SW_RST);

	fsleep(10); /* required after POR high */
	exynos2200_usbcon_phy_write_mask(phy->base,
					 EXYNOS2200_USBCON_LINK_CLKRST,
					 LINK_CLKRST_SW_RST, 0);

	exynos2200_usbcon_phy_write_mask(phy->base,
					 EXYNOS2200_USBCON_UTMI_CTRL,
					 UTMI_CTRL_FORCESLEEP |
					 UTMI_CTRL_FORCESUSPEND,
					 0);

	exynos2200_usbcon_phy_write_mask(phy->base,
					 EXYNOS2200_USBCON_UTMI_CTRL,
					 UTMI_CTRL_FORCE_BVALID |
					 UTMI_CTRL_FORCE_VBUSVALID,
					 UTMI_CTRL_FORCE_BVALID |
					 UTMI_CTRL_FORCE_VBUSVALID);

	return phy_init(phy->hs_phy);
}

static int exynos2200_usbcon_phy_exit(struct phy *p)
{
	struct exynos2200_usbcon_phy *phy = phy_get_drvdata(p);
	int ret;

	ret = phy_exit(phy->hs_phy);
	if (ret)
		return ret;

	exynos2200_usbcon_phy_write_mask(phy->base,
					 EXYNOS2200_USBCON_UTMI_CTRL,
					 UTMI_CTRL_FORCESLEEP |
					 UTMI_CTRL_FORCESUSPEND,
					 UTMI_CTRL_FORCESLEEP |
					 UTMI_CTRL_FORCESUSPEND);

	exynos2200_usbcon_phy_write_mask(phy->base,
					 EXYNOS2200_USBCON_LINK_CLKRST,
					 LINK_CLKRST_SW_RST,
					 LINK_CLKRST_SW_RST);

	/* Gate power via PMU */
	exynos2200_usbcon_phy_isol(phy, true);

	clk_bulk_disable_unprepare(phy->drv_data->num_clks, phy->clks);

	return 0;
}

static const struct phy_ops exynos2200_usbcon_phy_ops = {
	.init		= exynos2200_usbcon_phy_init,
	.exit		= exynos2200_usbcon_phy_exit,
	.owner		= THIS_MODULE,
};

static int exynos2200_usbcon_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos2200_usbcon_phy *phy;
	const struct exynos2200_usbcon_phy_drvdata *drv_data;
	struct phy_provider *phy_provider;
	struct phy *generic_phy;
	int ret;

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	drv_data = of_device_get_match_data(dev);
	if (!drv_data)
		return -EINVAL;
	phy->drv_data = drv_data;

	phy->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(phy->base))
		return PTR_ERR(phy->base);

	phy->clks = devm_kcalloc(dev, drv_data->num_clks,
				 sizeof(*phy->clks), GFP_KERNEL);
	if (!phy->clks)
		return -ENOMEM;

	for (int i = 0; i < drv_data->num_clks; ++i)
		phy->clks[i].id = drv_data->clk_names[i];

	ret = devm_clk_bulk_get(dev, phy->drv_data->num_clks,
				phy->clks);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to get phy clock(s)\n");

	phy->reg_pmu = syscon_regmap_lookup_by_phandle_args(dev->of_node,
							    "samsung,pmu-syscon",
							    1, &phy->pmu_offset);
	if (IS_ERR(phy->reg_pmu)) {
		dev_err(dev, "Failed to lookup PMU regmap\n");
		return PTR_ERR(phy->reg_pmu);
	}

	phy->hs_phy = devm_of_phy_get_by_index(dev, dev->of_node, 0);
	if (IS_ERR(phy->hs_phy))
		return dev_err_probe(dev, PTR_ERR(phy->hs_phy),
				     "failed to get hs_phy\n");

	generic_phy = devm_phy_create(dev, NULL, &exynos2200_usbcon_phy_ops);
	if (IS_ERR(generic_phy))
		return dev_err_probe(dev, PTR_ERR(generic_phy),
				     "failed to create phy %d\n", ret);

	dev_set_drvdata(dev, phy);
	phy_set_drvdata(generic_phy, phy);

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(phy_provider))
		return dev_err_probe(dev, PTR_ERR(phy_provider),
				     "failed to register phy provider\n");

	return 0;
}

static const char * const exynos2200_clk_names[] = {
	"bus",
};

static const struct exynos2200_usbcon_phy_drvdata exynos2200_usbcon_phy = {
	.clk_names		= exynos2200_clk_names,
	.num_clks		= ARRAY_SIZE(exynos2200_clk_names),
};

static const struct of_device_id exynos2200_usbcon_phy_of_match_table[] = {
	{
		.compatible = "samsung,exynos2200-usbcon-phy",
		.data = &exynos2200_usbcon_phy,
	}, { },
};
MODULE_DEVICE_TABLE(of, exynos2200_usbcon_phy_of_match_table);

static struct platform_driver exynos2200_usbcon_phy_driver = {
	.probe		= exynos2200_usbcon_phy_probe,
	.driver = {
		.name	= "exynos2200-usbcon-phy",
		.of_match_table = exynos2200_usbcon_phy_of_match_table,
	},
};

module_platform_driver(exynos2200_usbcon_phy_driver);
MODULE_DESCRIPTION("Exynos2200 USBCON PHY driver");
MODULE_LICENSE("GPL");
