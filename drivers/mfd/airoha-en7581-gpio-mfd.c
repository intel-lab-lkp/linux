// SPDX-License-Identifier: GPL-2.0+
/*
 * MFD driver for Airoha EN7581
 */

#include <linux/io.h>
#include <linux/of.h>
#include <linux/mfd/airoha-en7581-mfd.h>
#include <linux/mfd/core.h>
#include <linux/module.h>

static struct resource airoha_mfd_pinctrl_intr[] = {
	{
		.flags = IORESOURCE_IRQ,
	},
};

static const struct mfd_cell airoha_mfd_devs[] = {
	{
		.name = "pinctrl-airoha",
		.resources = airoha_mfd_pinctrl_intr,
		.num_resources = ARRAY_SIZE(airoha_mfd_pinctrl_intr),
	}, {
		.name = "pwm-airoha",
	},
};

static int airoha_mfd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct airoha_mfd *mfd;
	int irq;

	mfd = devm_kzalloc(dev, sizeof(*mfd), GFP_KERNEL);
	if (!mfd)
		return -ENOMEM;

	platform_set_drvdata(pdev, mfd);

	mfd->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mfd->base))
		return PTR_ERR(mfd->base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	airoha_mfd_pinctrl_intr[0].start = irq;
	airoha_mfd_pinctrl_intr[0].end = irq;

	return devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO, airoha_mfd_devs,
				    ARRAY_SIZE(airoha_mfd_devs), NULL, 0,
				    NULL);
}

static const struct of_device_id airoha_mfd_of_match[] = {
	{ .compatible = "airoha,en7581-gpio-sysctl" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, airoha_mfd_of_match);

static struct platform_driver airoha_mfd_driver = {
	.probe = airoha_mfd_probe,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = airoha_mfd_of_match,
	},
};
module_platform_driver(airoha_mfd_driver);

MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
MODULE_DESCRIPTION("Driver for Airoha EN7581 MFD");
