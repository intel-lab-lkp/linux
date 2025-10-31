// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Linaro Ltd.
 *
 * Samsung Exynos OTP driver.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mod_devicetable.h>

struct exynos_otp {
	struct clk *pclk;
	struct regmap *regmap;
};

static int exynos_otp_read(void *context, unsigned int offset, void *val,
			   size_t bytes)
{
	struct exynos_otp *eotp = context;

	return regmap_bulk_read(eotp->regmap, offset, val, bytes / 4);
}

static struct nvmem_config exynos_otp_nvmem_config = {
	.name = "exynos-otp-reg",
	.add_legacy_fixed_of_cells = true,
	.reg_read = exynos_otp_read,
	.word_size = 4,
	.stride = 4,
};

static int exynos_otp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nvmem_device *nvmem;
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
		.val_bits = 32,
		.reg_stride = 4,
		.use_relaxed_mmio = true,
		.max_register = (resource_size(res) - reg_config.reg_stride),
	};

	eotp->regmap = devm_regmap_init_mmio(dev, base, &reg_config);
	if (IS_ERR(eotp->regmap))
		return PTR_ERR(eotp->regmap);

	eotp->pclk = devm_clk_get_enabled(dev, "pclk");
	if (IS_ERR(eotp->pclk))
		return dev_err_probe(dev, PTR_ERR(eotp->pclk),
				     "Could not get pclk\n");

	exynos_otp_nvmem_config.size = resource_size(res);
	exynos_otp_nvmem_config.dev = dev;
	exynos_otp_nvmem_config.priv = eotp;

	nvmem = devm_nvmem_register(dev, &exynos_otp_nvmem_config);

	return PTR_ERR_OR_ZERO(nvmem);
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
