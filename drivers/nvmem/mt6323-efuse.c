// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2026 Roman Vivchar <rva333@protonmail.com>
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/mfd/mt6323/registers.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define MT6323_EFUSE_DOUT_BASE MT6323_EFUSE_DOUT_0_15
#define MT6323_EFUSE_SIZE 24

struct mt6323_efuse {
	struct regmap *regmap;
};

static int mt6323_efuse_read(void *context, unsigned int offset, void *val,
			     size_t bytes)
{
	struct mt6323_efuse *efuse = context;
	u32 tmp;
	u16 *buf = val;
	int i, ret;

	for (i = 0; i < bytes; i += 2) {
		ret = regmap_read(efuse->regmap,
				  MT6323_EFUSE_DOUT_BASE + offset + i, &tmp);
		if (ret)
			return ret;
		buf[i / 2] = tmp;
	}
	return 0;
}

static int mt6323_efuse_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6323_efuse *priv;
	struct nvmem_config config = {
		.name = "mt6323-efuse",
		.stride = 2,
		.word_size = 2,
		.size = MT6323_EFUSE_SIZE,
		.reg_read = mt6323_efuse_read,
	};
	struct nvmem_device *nvmem;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* efuse -> mfd -> pwrap */
	priv->regmap = dev_get_regmap(dev->parent->parent, NULL);
	if (!priv->regmap)
		return -ENODEV;

	config.dev = dev;
	config.priv = priv;

	nvmem = devm_nvmem_register(dev, &config);
	return PTR_ERR_OR_ZERO(nvmem);
}

static const struct of_device_id mt6323_efuse_of_match[] = {
	{ .compatible = "mediatek,mt6323-efuse" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mt6323_efuse_of_match);

static struct platform_driver mt6323_efuse_driver = {
	.probe = mt6323_efuse_probe,
	.driver = {
		.name = "mt6323-efuse",
		.of_match_table = mt6323_efuse_of_match,
	},
};
module_platform_driver(mt6323_efuse_driver);

MODULE_DESCRIPTION("Mediatek MT6323 PMIC EFUSE driver");
MODULE_LICENSE("GPL");
