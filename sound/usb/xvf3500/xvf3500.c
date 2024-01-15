// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the XMOS XVF3500 VocalFusion Voice Processor.
 *
 * Copyright (C) 2023 WolfVision GmbH.
 *
 */

#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

static const char * const supply_names[] = {
	"vcc1v0",
	"vcc3v3",
};

#define NUM_SUPPLIES ARRAY_SIZE(supply_names)

struct xvf3500 {
	struct regulator_bulk_data supplies[NUM_SUPPLIES];
	struct device *dev;
	struct gpio_desc *reset;
};

static int xvf3500_power(struct xvf3500 *priv, bool on)
{
	int ret;

	if (on) {
		ret = regulator_bulk_enable(NUM_SUPPLIES, priv->supplies);
		if (ret) {
			dev_err(priv->dev, "failed to enable supplies: %d\n", ret);
			return ret;
		}
		/*
		 * A delay of >=100ns + regulator startup is needed before releasing
		 * the reset here. Wait for 10 ms to be on the safe side.
		 */
		fsleep(10000);
		gpiod_set_value_cansleep(priv->reset, 0);
	} else {
		gpiod_set_value_cansleep(priv->reset, 1);
		ret = regulator_bulk_disable(NUM_SUPPLIES, priv->supplies);
		if (ret) {
			dev_err(priv->dev, "failed to disable supplies: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static int xvf3500_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct xvf3500 *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	dev_set_drvdata(dev, priv);

	regulator_bulk_set_supply_names(priv->supplies, supply_names,
					NUM_SUPPLIES);

	ret = devm_regulator_bulk_get(dev, NUM_SUPPLIES, priv->supplies);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to get regulator supplies\n");
		return ret;
	}

	priv->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset))
		return dev_err_probe(priv->dev, PTR_ERR(priv->reset),
				     "failed to get reset GPIO\n");

	return xvf3500_power(priv, true);
}

static void xvf3500_remove(struct platform_device *pdev)
{
	struct xvf3500 *priv = dev_get_drvdata(&pdev->dev);

	xvf3500_power(priv, false);
}

#ifdef CONFIG_PM_SLEEP
static int xvf3500_suspend(struct device *dev)
{
	struct xvf3500 *priv = dev_get_drvdata(dev);

	xvf3500_power(priv, false);

	return 0;
}

static int xvf3500_resume(struct device *dev)
{
	struct xvf3500 *priv = dev_get_drvdata(dev);

	xvf3500_power(priv, true);

	return 0;
}

static SIMPLE_DEV_PM_OPS(xvf3500_pm, xvf3500_suspend, xvf3500_resume);
#define XVF3500_PM_OPS	(&xvf3500_pm)
#else
#define XVF3500_PM_OPS	NULL
#endif /* CONFIG_PM_SLEEP */

static const struct of_device_id xvf3500_of_table[] = {
	{
		.compatible = "xmos,xvf3500",
	},
	{},
};
MODULE_DEVICE_TABLE(of, xvf3500_of_table);

static struct platform_driver xvf3500_driver = {
	.driver = {
		.name = "xvf3500",
		.of_match_table = xvf3500_of_table,
		.pm = XVF3500_PM_OPS,
	},
	.probe = xvf3500_probe,
	.remove_new = xvf3500_remove,
};
module_platform_driver(xvf3500_driver);

MODULE_AUTHOR("Javier Carrasco <javier.carrasco@wolfvision.net>");
MODULE_DESCRIPTION("XMOS XVF3500 Voice Processor");
MODULE_LICENSE("GPL");
