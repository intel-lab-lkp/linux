// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Linaro Ltd.
 *
 * Samsung Exynos OTP driver.
 */

#include <linux/array_size.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/device/devres.h>
#include <linux/err.h>
#include <linux/ioport.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sys_soc.h>

#define EXYNOS_OTP_PRODUCT_ID			0
#define EXYNOS_OTP_PRODUCT_ID_MASK		GENMASK(31, 12)
#define EXYNOS_OTP_PRODUCT_ID_MAIN_REV		GENMASK(3, 0)

#define EXYNOS_OTP_CHIPID(i)			(0x4 + (i) * 4)
#define EXYNOS_OTP_CHIPID3_SUB_REV		GENMASK(19, 16)

#define EXYNOS_OTP_PRODUCT_ID_MAIN_REV_SHIFT	4

struct exynos_otp {
	struct clk *pclk;
	struct device *dev;
	struct regmap *regmap;
};

static const struct exynos_otp_soc_id {
	const char *name;
	u32 id;
} eotp_soc_ids[] = {
	{ "GS101", 0x9845 },
};

static const char *exynos_otp_xlate_soc_name(u32 id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(eotp_soc_ids); i++)
		if (id == eotp_soc_ids[i].id)
			return eotp_soc_ids[i].name;
	return NULL;
}

static void exynos_otp_unregister_soc(void *data)
{
	soc_device_unregister(data);
}

static int exynos_otp_soc_device_register(struct exynos_otp *eotp)
{
	struct soc_device_attribute *soc_dev_attr;
	struct regmap *regmap = eotp->regmap;
	struct device *dev = eotp->dev;
	struct soc_device *soc_dev;
	u32 val, main_rev, sub_rev;
	u32 product_id, revision;
	int ret;

	soc_dev_attr = devm_kzalloc(dev, sizeof(*soc_dev_attr), GFP_KERNEL);
	if (!soc_dev_attr)
		return -ENOMEM;

	ret = regmap_read(regmap, EXYNOS_OTP_PRODUCT_ID, &val);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read product id\n");

	product_id = FIELD_GET(EXYNOS_OTP_PRODUCT_ID_MASK, val);
	main_rev = FIELD_GET(EXYNOS_OTP_PRODUCT_ID_MAIN_REV, val);

	ret = regmap_read(regmap, EXYNOS_OTP_CHIPID(3), &val);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read chip id\n");

	sub_rev = FIELD_GET(EXYNOS_OTP_CHIPID3_SUB_REV, val);
	revision = main_rev << EXYNOS_OTP_PRODUCT_ID_MAIN_REV_SHIFT | sub_rev;

	soc_dev_attr->family = "Samsung Exynos";
	soc_dev_attr->soc_id = exynos_otp_xlate_soc_name(product_id);
	if (!soc_dev_attr->soc_id)
		return dev_err_probe(dev, -ENODEV, "failed to translate chip id to name\n");

	soc_dev_attr->revision = devm_kasprintf(dev, GFP_KERNEL, "%x",
						revision);
	if (!soc_dev_attr->revision)
		return -ENOMEM;

	soc_dev = soc_device_register(soc_dev_attr);
	if (IS_ERR(soc_dev))
		return dev_err_probe(dev, PTR_ERR(soc_dev),
				     "failed to register to the SoC interface\n");

	ret = devm_add_action_or_reset(dev, exynos_otp_unregister_soc, soc_dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add devm action\n");

	return 0;
}

static int exynos_otp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos_otp *eotp;
	struct resource *res;
	void __iomem *base;

	eotp = devm_kzalloc(dev, sizeof(*eotp), GFP_KERNEL);
	if (!eotp)
		return -ENOMEM;

	base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	const struct regmap_config reg_config = {
		.reg_bits = 32,
		.reg_stride = 4,
		.val_bits = 32,
		.use_relaxed_mmio = true,
		.max_register = (resource_size(res) - reg_config.reg_stride),
	};

	eotp->regmap = devm_regmap_init_mmio(dev, base, &reg_config);
	if (IS_ERR(eotp->regmap))
		return PTR_ERR(eotp->regmap);

	eotp->pclk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(eotp->pclk))
		return dev_err_probe(dev, PTR_ERR(eotp->pclk), "failed to get clock\n");

	eotp->dev = dev;

	return exynos_otp_soc_device_register(eotp);
}

static const struct of_device_id exynos_otp_dt_ids[] = {
	{ .compatible = "google,gs101-otp" },
	{},
};
MODULE_DEVICE_TABLE(of, exynos_otp_dt_ids);

static struct platform_driver exynos_otp_driver = {
	.probe	= exynos_otp_probe,
	.driver = {
		.name	= "exynos-otp",
		.of_match_table = exynos_otp_dt_ids,
	},
};
module_platform_driver(exynos_otp_driver);

MODULE_AUTHOR("Tudor Ambarus <tudor.ambarus@linaro.org>");
MODULE_DESCRIPTION("Samsung Exynos OTP driver");
MODULE_LICENSE("GPL");
