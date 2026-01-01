// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 Vladimir Zapolskiy <vz@mleia.com>
 */

#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sys_soc.h>

#define SERIAL_ID0	0x130

static int lpc32xx_soc_probe(struct platform_device *pdev)
{
	struct soc_device_attribute *soc_dev_attr;
	struct device *dev = &pdev->dev;
	struct soc_device *soc_dev;
	struct regmap *scb;
	u32 serial_id[4];
	int ret;

	soc_dev_attr = devm_kzalloc(dev, sizeof(*soc_dev_attr), GFP_KERNEL);
	if (!soc_dev_attr)
		return -ENOMEM;

	soc_dev_attr->family = "NXP LPC32xx";

	ret = of_property_read_string(of_root, "model", &soc_dev_attr->machine);
	if (ret)
		return ret;

	scb = syscon_regmap_lookup_by_compatible("nxp,lpc3220-scb");
	if (!IS_ERR(scb)) {
		/* Do not bail out on error, if SCB device tree node is found */
		ret = regmap_bulk_read(scb, SERIAL_ID0, serial_id, 4);
		if (ret)
			return ret;

		soc_dev_attr->serial_number = devm_kasprintf(dev, GFP_KERNEL,
							     "%08x%08x%08x%08x",
							     serial_id[3],
							     serial_id[2],
							     serial_id[1],
							     serial_id[0]);
		if (!soc_dev_attr->serial_number)
			return -ENOMEM;
	} else {
		dev_info(dev, "failed to get SCB regmap: %ld\n", PTR_ERR(scb));
	}

	soc_dev = soc_device_register(soc_dev_attr);
	if (IS_ERR(soc_dev))
		return PTR_ERR(soc_dev);

	platform_set_drvdata(pdev, soc_dev);

	return 0;
}

static void lpc32xx_soc_remove(struct platform_device *pdev)
{
	struct soc_device *soc_dev = platform_get_drvdata(pdev);

	soc_device_unregister(soc_dev);
}

static const struct of_device_id lpc32xx_soc[] = {
	{ .compatible = "nxp,lpc3220", },
	{ .compatible = "nxp,lpc3230", },
	{ .compatible = "nxp,lpc3240", },
	{ .compatible = "nxp,lpc3250", },
	{ }
};

static struct platform_driver lpc32xx_soc_driver = {
	.probe = lpc32xx_soc_probe,
	.remove = lpc32xx_soc_remove,
	.driver = {
		.name = "lpc32xx-soc",
		.of_match_table = lpc32xx_soc,
	},
};

static int __init lpc32xx_soc_device_init(void)
{
	struct platform_device *pdev;
	int ret;

	if (!of_match_node(lpc32xx_soc, of_root))
		return 0;

	ret = platform_driver_register(&lpc32xx_soc_driver);
	if (ret) {
		pr_info("Failed to register lpc32xx-soc platform driver: %d\n",
			ret);
		return ret;
	}

	pdev = platform_device_register_simple("lpc32xx-soc", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		pr_info("Failed to register lpc32xx-soc platform device: %ld\n",
			PTR_ERR(pdev));
		platform_driver_unregister(&lpc32xx_soc_driver);
		return PTR_ERR(pdev);
	};

	return 0;
}
device_initcall(lpc32xx_soc_device_init);

MODULE_AUTHOR("Vladimir Zapolskiy <vz@mleia.com>");
MODULE_DESCRIPTION("NXP LPC32xx SoC driver");
MODULE_LICENSE("GPL");
