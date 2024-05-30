// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 CVITEK Inc.
 * Copyright (C) 2024 Haylen Chu <heylenay@outlook.com>
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/thermal.h>

#define TEMPSEN_VERSION					0x0
#define TEMPSEN_CTRL					0x4
#define TEMPSEN_CTRL_EN					BIT(0)
#define TEMPSEN_CTRL_SEL_MASK				GENMASK(7, 4)
#define TEMPSEN_CTRL_SEL_OFFSET				4
#define TEMPSEN_STATUS					0x8
#define TEMPSEN_SET					0xc
#define TEMPSEN_SET_CHOPSEL_MASK			GENMASK(5, 4)
#define TEMPSEN_SET_CHOPSEL_OFFSET			4
#define TEMPSEN_SET_CHOPSEL_128T			0
#define TEMPSEN_SET_CHOPSEL_256T			1
#define TEMPSEN_SET_CHOPSEL_512T			2
#define TEMPSEN_SET_CHOPSEL_1024T			3
#define TEMPSEN_SET_ACCSEL_MASK				GENMASK(7, 6)
#define TEMPSEN_SET_ACCSEL_OFFSET			6
#define TEMPSEN_SET_ACCSEL_512T				0
#define TEMPSEN_SET_ACCSEL_1024T			1
#define TEMPSEN_SET_ACCSEL_2048T			2
#define TEMPSEN_SET_ACCSEL_4096T			3
#define TEMPSEN_SET_CYC_CLKDIV_MASK			GENMASK(15, 8)
#define TEMPSEN_SET_CYC_CLKDIV_OFFSET			8
#define TEMPSEN_INTR_EN					0x10
#define TEMPSEN_INTR_CLR				0x14
#define TEMPSEN_INTR_STA				0x18
#define TEMPSEN_INTR_RAW				0x1c
#define TEMPSEN_RESULT(n)				(0x20 + (n) * 4)
#define TEMPSEN_RESULT_RESULT_MASK			GENMASK(12, 0)
#define TEMPSEN_RESULT_MAX_RESULT_MASK			GENMASK(28, 16)
#define TEMPSEN_RESULT_CLR_MAX_RESULT			BIT(31)
#define TEMPSEN_AUTO_PERIOD				0x64
#define TEMPSEN_AUTO_PERIOD_AUTO_CYCLE_MASK		GENMASK(23, 0)
#define TEMPSEN_AUTO_PERIOD_AUTO_CYCLE_OFFSET		0

struct cv180x_thermal_zone {
	struct device *dev;
	void __iomem *base;
	struct clk *clk_tempsen;
};

static void cv180x_thermal_init(struct cv180x_thermal_zone *ctz)
{
	void __iomem *base = ctz->base;
	u32 regval;

	writel(readl(base + TEMPSEN_INTR_RAW), base + TEMPSEN_INTR_CLR);
	writel(TEMPSEN_RESULT_CLR_MAX_RESULT, base + TEMPSEN_RESULT(0));

	regval = readl(base + TEMPSEN_SET);
	regval &= ~TEMPSEN_SET_CHOPSEL_MASK;
	regval &= ~TEMPSEN_SET_ACCSEL_MASK;
	regval &= ~TEMPSEN_SET_CYC_CLKDIV_MASK;
	regval |= TEMPSEN_SET_CHOPSEL_1024T << TEMPSEN_SET_CHOPSEL_OFFSET;
	regval |= TEMPSEN_SET_ACCSEL_2048T  << TEMPSEN_SET_ACCSEL_OFFSET;
	regval |= 0x31 << TEMPSEN_SET_CYC_CLKDIV_OFFSET;
	writel(regval, base + TEMPSEN_SET);

	regval = readl(base + TEMPSEN_AUTO_PERIOD);
	regval &= ~TEMPSEN_AUTO_PERIOD_AUTO_CYCLE_MASK;
	regval |= 0x100000 << TEMPSEN_AUTO_PERIOD_AUTO_CYCLE_OFFSET;
	writel(regval, base + TEMPSEN_AUTO_PERIOD);

	regval = readl(base + TEMPSEN_CTRL);
	regval &= ~TEMPSEN_CTRL_SEL_MASK;
	regval |= 1 << TEMPSEN_CTRL_SEL_OFFSET;
	regval |= TEMPSEN_CTRL_EN;
	writel(regval, base + TEMPSEN_CTRL);
}

static void cv180x_thermal_deinit(struct cv180x_thermal_zone *ct)
{
	void __iomem *base = ct->base;
	u32 regval;

	regval = readl(base + TEMPSEN_CTRL);
	regval &= ~(TEMPSEN_CTRL_SEL_MASK | TEMPSEN_CTRL_EN);
	writel(regval, base + TEMPSEN_CTRL);

	writel(readl(base + TEMPSEN_INTR_RAW), base + TEMPSEN_INTR_CLR);
}

static int calc_temp(uint32_t result)
{
	return ((result * 1000) * 716 / 2048 - 273000);
}

static int cv180x_get_temp(struct thermal_zone_device *tdev, int *temperature)
{
	struct cv180x_thermal_zone *ctz = thermal_zone_device_priv(tdev);
	void __iomem *base = ctz->base;
	u32 result;

	result = readl(base + TEMPSEN_RESULT(0)) & TEMPSEN_RESULT_RESULT_MASK;
	*temperature = calc_temp(result);

	return 0;
}

static const struct thermal_zone_device_ops cv180x_thermal_ops = {
	.get_temp = cv180x_get_temp,
};

static const struct of_device_id cv180x_thermal_of_match[] = {
	{
		.compatible = "sophgo,cv180x-thermal",
	},
	{},
};
MODULE_DEVICE_TABLE(of, cv180x_thermal_of_match);

static int cv180x_thermal_probe(struct platform_device *pdev)
{
	struct cv180x_thermal_zone *ctz;
	struct thermal_zone_device *tz;
	struct resource *res;

	ctz = devm_kzalloc(&pdev->dev, sizeof(*ctz), GFP_KERNEL);
	if (!ctz)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ctz->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(ctz->base))
		return dev_err_probe(&pdev->dev, PTR_ERR(ctz->base),
				     "failed to map tempsen registers\n");

	ctz->clk_tempsen = devm_clk_get(&pdev->dev, "clk_tempsen");
	if (IS_ERR(ctz->clk_tempsen))
		return dev_err_probe(&pdev->dev, PTR_ERR(ctz->clk_tempsen),
				     "failed to get clk_tempsen\n");

	clk_prepare_enable(ctz->clk_tempsen);

	ctz->dev = &pdev->dev;

	cv180x_thermal_init(ctz);

	platform_set_drvdata(pdev, ctz);

	tz = devm_thermal_of_zone_register(&pdev->dev, 0, ctz,
					   &cv180x_thermal_ops);
	if (IS_ERR(tz))
		return dev_err_probe(&pdev->dev, PTR_ERR(tz),
				     "failed to register thermal zone\n");

	return 0;
}

static int cv180x_thermal_remove(struct platform_device *pdev)
{
	struct cv180x_thermal_zone *ctz = platform_get_drvdata(pdev);

	cv180x_thermal_deinit(ctz);
	clk_disable_unprepare(ctz->clk_tempsen);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int cv180x_thermal_suspend(struct device *dev)
{
	struct cv180x_thermal_zone *ctz = dev_get_drvdata(dev);

	cv180x_thermal_deinit(ctz);
	clk_disable_unprepare(ctz->clk_tempsen);

	return 0;
}

static int cv180x_thermal_resume(struct device *dev)
{
	struct cv180x_thermal_zone *ctz = dev_get_drvdata(dev);

	clk_prepare_enable(ctz->clk_tempsen);
	cv180x_thermal_init(ctz);

	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static SIMPLE_DEV_PM_OPS(cv180x_thermal_pm_ops,
			 cv180x_thermal_suspend, cv180x_thermal_resume);

static struct platform_driver cv180x_thermal_driver = {
	.probe = cv180x_thermal_probe,
	.remove = cv180x_thermal_remove,
	.driver = {
		.name = "cv180x-thermal",
		.pm = &cv180x_thermal_pm_ops,
		.of_match_table = cv180x_thermal_of_match,
	},
};

module_platform_driver(cv180x_thermal_driver);

MODULE_AUTHOR("Haylen Chu <heylenay@outlook.com>");
MODULE_LICENSE("GPL");
