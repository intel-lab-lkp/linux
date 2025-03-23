// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright 2020 Google Inc
// Copyright 2025 Linaro Ltd.
//
// Samsung S2MPG1x ACPM driver

#include <linux/array_size.h>
#include <linux/device.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/mfd/samsung/core.h>
#include <linux/mfd/samsung/s2mpg10.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include "sec-core.h"

#define ACPM_MAX_BULK_DATA   8

struct sec_pmic_acpm_platform_data {
	int device_type;

	unsigned int acpm_chan_id;
	u8 speedy_channel;

	const struct regmap_config *regmap_cfg_common;
	const struct regmap_config *regmap_cfg_pmic;
	const struct regmap_config *regmap_cfg_rtc;
	const struct regmap_config *regmap_cfg_meter;
};

static const struct regmap_range s2mpg10_common_registers[] = {
	regmap_reg_range(0x00, 0x02),
	regmap_reg_range(0x0a, 0x0c),
	regmap_reg_range(0x1a, 0x2a),
	regmap_reg_range(0x33, 0x3f),
	regmap_reg_range(0x57, 0x7f),
};

static const struct regmap_range s2mpg10_common_ro_registers[] = {
	regmap_reg_range(0x00, 0x01),
	regmap_reg_range(0x28, 0x2a),
	regmap_reg_range(0x33, 0x3f),
	regmap_reg_range(0x57, 0x7f),
};

static const struct regmap_range s2mpg10_common_nonvolatile_registers[] = {
	regmap_reg_range(0x00, 0x00), /* CHIP_ID_M */
	regmap_reg_range(0x02, 0x02), /* INT_MASK */
	regmap_reg_range(0x0a, 0x0c), /* speedy control */
};

static const struct regmap_range s2mpg10_common_precious_registers[] = {
	regmap_reg_range(0x01, 0x01), /* INT */
};

static const struct regmap_access_table s2mpg10_common_wr_table = {
	.yes_ranges = s2mpg10_common_registers,
	.n_yes_ranges = ARRAY_SIZE(s2mpg10_common_registers),
	.no_ranges = s2mpg10_common_ro_registers,
	.n_no_ranges = ARRAY_SIZE(s2mpg10_common_ro_registers),
};

static const struct regmap_access_table s2mpg10_common_rd_table = {
	.yes_ranges = s2mpg10_common_registers,
	.n_yes_ranges = ARRAY_SIZE(s2mpg10_common_registers),
};

static const struct regmap_access_table s2mpg10_common_volatile_table = {
	.no_ranges = s2mpg10_common_nonvolatile_registers,
	.n_no_ranges = ARRAY_SIZE(s2mpg10_common_nonvolatile_registers),
};

static const struct regmap_access_table s2mpg10_common_precious_table = {
	.yes_ranges = s2mpg10_common_precious_registers,
	.n_yes_ranges = ARRAY_SIZE(s2mpg10_common_precious_registers),
};

static const struct regmap_config s2mpg10_regmap_config_common = {
	.name = "common",
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x7f,
	.wr_table = &s2mpg10_common_wr_table,
	.rd_table = &s2mpg10_common_rd_table,
	.volatile_table = &s2mpg10_common_volatile_table,
	.precious_table = &s2mpg10_common_precious_table,
	.num_reg_defaults_raw = 0x7f + 1,
	.cache_type = REGCACHE_FLAT,
};

static const struct regmap_range s2mpg10_pmic_registers[] = {
	regmap_reg_range(0x00, 0xf6),
};

static const struct regmap_range s2mpg10_pmic_ro_registers[] = {
	regmap_reg_range(0x00, 0x05), /* INTx */
	regmap_reg_range(0x0c, 0x0f), /* STATUSx PWRONSRC OFFSRC */
	regmap_reg_range(0xc7, 0xc7), /* GPIO input */
};

static const struct regmap_range s2mpg10_pmic_nonvolatile_registers[] = {
	regmap_reg_range(0x06, 0x0b), /* INTxM */
};

static const struct regmap_range s2mpg10_pmic_precious_registers[] = {
	regmap_reg_range(0x00, 0x05), /* INTx */
};

static const struct regmap_access_table s2mpg10_pmic_wr_table = {
	.yes_ranges = s2mpg10_pmic_registers,
	.n_yes_ranges = ARRAY_SIZE(s2mpg10_pmic_registers),
	.no_ranges = s2mpg10_pmic_ro_registers,
	.n_no_ranges = ARRAY_SIZE(s2mpg10_pmic_ro_registers),
};

static const struct regmap_access_table s2mpg10_pmic_rd_table = {
	.yes_ranges = s2mpg10_pmic_registers,
	.n_yes_ranges = ARRAY_SIZE(s2mpg10_pmic_registers),
};

static const struct regmap_access_table s2mpg10_pmic_volatile_table = {
	.no_ranges = s2mpg10_pmic_nonvolatile_registers,
	.n_no_ranges = ARRAY_SIZE(s2mpg10_pmic_nonvolatile_registers),
};

static const struct regmap_access_table s2mpg10_pmic_precious_table = {
	.yes_ranges = s2mpg10_pmic_precious_registers,
	.n_yes_ranges = ARRAY_SIZE(s2mpg10_pmic_precious_registers),
};

static const struct regmap_config s2mpg10_regmap_config_pmic = {
	.name = "pmic",
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xf6,
	.wr_table = &s2mpg10_pmic_wr_table,
	.rd_table = &s2mpg10_pmic_rd_table,
	.volatile_table = &s2mpg10_pmic_volatile_table,
	.precious_table = &s2mpg10_pmic_precious_table,
	.num_reg_defaults_raw = 0xf6 + 1,
	.cache_type = REGCACHE_FLAT,
};

static const struct regmap_range s2mpg10_rtc_registers[] = {
	regmap_reg_range(0x00, 0x2b),
};

static const struct regmap_range s2mpg10_volatile_registers[] = {
	regmap_reg_range(0x01, 0x01), /* RTC_UPDATE */
	regmap_reg_range(0x05, 0x0c), /* time / date */
};

static const struct regmap_access_table s2mpg10_rtc_wr_table = {
	.yes_ranges = s2mpg10_rtc_registers,
	.n_yes_ranges = ARRAY_SIZE(s2mpg10_rtc_registers),
	/* no r/o registers in RTC block  */
};

static const struct regmap_access_table s2mpg10_rtc_rd_table = {
	.yes_ranges = s2mpg10_rtc_registers,
	.n_yes_ranges = ARRAY_SIZE(s2mpg10_rtc_registers),
};

static const struct regmap_access_table s2mpg10_rtc_volatile_table = {
	.yes_ranges = s2mpg10_volatile_registers,
	.n_yes_ranges = ARRAY_SIZE(s2mpg10_volatile_registers),
};

static const struct regmap_config s2mpg10_regmap_config_rtc = {
	.name = "rtc",
	.reg_bits = 8,
	.val_bits = 8,
	.reg_stride = 1,
	.max_register = 0x2b,
	.wr_table = &s2mpg10_rtc_wr_table,
	.rd_table = &s2mpg10_rtc_rd_table,
	.volatile_table = &s2mpg10_rtc_volatile_table,
	.num_reg_defaults_raw = 0x2b + 1,
	.cache_type = REGCACHE_FLAT,
};

static const struct regmap_range s2mpg10_meter_registers[] = {
	regmap_reg_range(0x00, 0x21), /* meter config */
	regmap_reg_range(0x40, 0x8a), /* meter data */
	regmap_reg_range(0xee, 0xee),
	regmap_reg_range(0xf1, 0xf1),
};

static const struct regmap_range s2mpg10_meter_ro_registers[] = {
	regmap_reg_range(0x40, 0x8a),
};

static const struct regmap_access_table s2mpg10_meter_wr_table = {
	.yes_ranges = s2mpg10_meter_registers,
	.n_yes_ranges = ARRAY_SIZE(s2mpg10_meter_registers),
	.no_ranges = s2mpg10_meter_ro_registers,
	.n_no_ranges = ARRAY_SIZE(s2mpg10_meter_ro_registers),
};

static const struct regmap_access_table s2mpg10_meter_rd_table = {
	.yes_ranges = s2mpg10_meter_registers,
	.n_yes_ranges = ARRAY_SIZE(s2mpg10_meter_registers),
};

static const struct regmap_access_table s2mpg10_meter_volatile_table = {
	.yes_ranges = s2mpg10_meter_ro_registers,
	.n_yes_ranges = ARRAY_SIZE(s2mpg10_meter_ro_registers),
};

static const struct regmap_config s2mpg10_regmap_config_meter = {
	.name = "meter",
	.reg_bits = 8,
	.val_bits = 8,
	.reg_stride = 1,
	.max_register = 0xf1,
	.wr_table = &s2mpg10_meter_wr_table,
	.rd_table = &s2mpg10_meter_rd_table,
	.volatile_table = &s2mpg10_meter_volatile_table,
	.num_reg_defaults_raw = 0xf1 + 1,
	.cache_type = REGCACHE_FLAT,
};

struct sec_acpm_shared_bus_context {
	const struct acpm_handle *acpm;
	unsigned int acpm_chan_id;
	u8 speedy_channel;
};

struct sec_acpm_bus_context {
	struct sec_acpm_shared_bus_context *shared;
	u8 type;
#define SEC_ACPM_TYPE_COMMON  0x00
#define SEC_ACPM_TYPE_PMIC    0x01
#define SEC_ACPM_TYPE_RTC     0x02
#define SEC_ACPM_TYPE_METER   0x0a
#define SEC_ACPM_TYPE_WLWP    0x0b
#define SEC_ACPM_TYPE_TRIM    0x0f
};

static int sec_acpm_bus_write(void *context, const void *data, size_t count)
{
	struct sec_acpm_bus_context *ctx = context;
	const struct acpm_handle *acpm = ctx->shared->acpm;
	const struct acpm_pmic_ops *pmic_ops = &acpm->ops.pmic_ops;
	u8 reg;

	if (count < 2 || count > (ACPM_MAX_BULK_DATA + 1))
		return -EINVAL;

	reg = *(u8 *)data;
	++data;
	--count;

	return pmic_ops->bulk_write(acpm, ctx->shared->acpm_chan_id,
				    ctx->type, reg,
				    ctx->shared->speedy_channel, count, data);
}

static int sec_acpm_bus_read(void *context, const void *reg_buf,
			     size_t reg_size, void *val_buf,
			     size_t val_size)
{
	struct sec_acpm_bus_context *ctx = context;
	const struct acpm_handle *acpm = ctx->shared->acpm;
	const struct acpm_pmic_ops *pmic_ops = &acpm->ops.pmic_ops;
	u8 reg;

	if (reg_size != 1 || !val_size || val_size > ACPM_MAX_BULK_DATA)
		return -EINVAL;

	reg = *(u8 *)reg_buf;

	return pmic_ops->bulk_read(acpm, ctx->shared->acpm_chan_id,
				   ctx->type, reg,
				   ctx->shared->speedy_channel,
				   val_size, val_buf);
}

static int sec_acpm_bus_reg_update_bits(void *context, unsigned int reg,
					unsigned int mask, unsigned int val)
{
	struct sec_acpm_bus_context *ctx = context;
	const struct acpm_handle *acpm = ctx->shared->acpm;
	const struct acpm_pmic_ops *pmic_ops = &acpm->ops.pmic_ops;

	return pmic_ops->update_reg(acpm, ctx->shared->acpm_chan_id,
				    ctx->type, reg & 0xff,
				    ctx->shared->speedy_channel,
				    val, mask);
}

static const struct regmap_bus sec_pmic_acpm_regmap_bus = {
	.write = sec_acpm_bus_write,
	.read = sec_acpm_bus_read,
	.reg_update_bits = sec_acpm_bus_reg_update_bits,
	.max_raw_read = ACPM_MAX_BULK_DATA,
	.max_raw_write = ACPM_MAX_BULK_DATA,
};

static struct regmap *
sec_pmic_acpm_regmap_init(struct device *dev,
			  struct sec_acpm_shared_bus_context *shared_ctx,
			  u8 type, const struct regmap_config *cfg,
			  bool do_attach)
{
	struct sec_acpm_bus_context *ctx;
	struct regmap *regmap;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	ctx->shared = shared_ctx;
	ctx->type = type;

	regmap = devm_regmap_init(dev, &sec_pmic_acpm_regmap_bus, ctx, cfg);
	if (IS_ERR(regmap))
		return ERR_PTR(dev_err_probe(dev, PTR_ERR(regmap),
					     "regmap init (%s) failed\n",
					     cfg->name));

	if (do_attach) {
		int ret;

		ret = regmap_attach_dev(dev, regmap, cfg);
		if (ret)
			return ERR_PTR(dev_err_probe(dev, ret,
						     "regmap attach (%s) failed\n",
						     cfg->name));
	}

	return regmap;
}

static void sec_acpm_mask_common_irqs(void *regmap_common)
{
	/* Mask all interrupts from 'common' block on shutdown */
	regmap_write(regmap_common, S2MPG10_COMMON_INT_MASK,
		     S2MPG10_COMMON_INT_SRC_MASK);
}

static int sec_pmic_acpm_probe(struct platform_device *pdev)
{
	const struct sec_pmic_acpm_platform_data *data;
	struct sec_acpm_shared_bus_context *shared_ctx;
	const struct acpm_handle *acpm;
	struct regmap *regmap_common, *regmap_pmic, *regmap;
	struct device *dev;
	int ret, irq;

	dev = &pdev->dev;

	data = device_get_match_data(dev);
	if (!data)
		return dev_err_probe(dev, -ENODEV,
				     "Unsupported device type\n");

	acpm = devm_acpm_get_by_phandle(dev, "exynos,acpm-ipc");
	if (IS_ERR(acpm))
		return dev_err_probe(dev, PTR_ERR(acpm),
				     "Failed to get acpm\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	shared_ctx = devm_kzalloc(dev, sizeof(*shared_ctx), GFP_KERNEL);
	if (!shared_ctx)
		return -ENOMEM;

	shared_ctx->acpm = acpm;
	shared_ctx->acpm_chan_id = data->acpm_chan_id;
	shared_ctx->speedy_channel = data->speedy_channel;

	regmap_common = sec_pmic_acpm_regmap_init(dev, shared_ctx,
						  SEC_ACPM_TYPE_COMMON,
						  data->regmap_cfg_common,
						  false);
	if (IS_ERR(regmap_common))
		return PTR_ERR(regmap_common);

	/* Mask all interrupts from 'common' block, until successful init */
	ret = regmap_write(regmap_common, S2MPG10_COMMON_INT_MASK,
			   S2MPG10_COMMON_INT_SRC_MASK);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to mask common block interrupts\n");

	regmap_pmic = sec_pmic_acpm_regmap_init(dev, shared_ctx,
						SEC_ACPM_TYPE_PMIC,
						data->regmap_cfg_pmic,
						false);
	if (IS_ERR(regmap_pmic))
		return PTR_ERR(regmap_pmic);

	regmap = sec_pmic_acpm_regmap_init(dev, shared_ctx, SEC_ACPM_TYPE_RTC,
					   data->regmap_cfg_rtc, true);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	regmap = sec_pmic_acpm_regmap_init(dev, shared_ctx, SEC_ACPM_TYPE_METER,
					   data->regmap_cfg_meter, true);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ret = sec_pmic_probe(dev, data->device_type, irq, regmap_pmic,
			     NULL, NULL);
	if (ret)
		return ret;

	if (device_property_read_bool(dev, "wakeup-source"))
		devm_device_init_wakeup(dev);

	/*
	 * Unmask PMIC interrupt from 'common' block, now that everything is in
	 * place.
	 */
	ret = regmap_clear_bits(regmap_common, S2MPG10_COMMON_INT_MASK,
				S2MPG10_COMMON_INT_SRC_PMIC);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to unmask PMIC interrupt\n");

	ret = devm_add_action_or_reset(dev, sec_acpm_mask_common_irqs,
				       regmap_common);
	if (ret)
		return ret;

	return 0;
}

static void sec_pmic_acpm_shutdown(struct platform_device *pdev)
{
	sec_pmic_shutdown(&pdev->dev);
}

static const struct sec_pmic_acpm_platform_data s2mpg10_data = {
	.device_type = S2MPG10,
	.acpm_chan_id = 2,
	.speedy_channel = 0,
	.regmap_cfg_common = &s2mpg10_regmap_config_common,
	.regmap_cfg_pmic = &s2mpg10_regmap_config_pmic,
	.regmap_cfg_rtc = &s2mpg10_regmap_config_rtc,
	.regmap_cfg_meter = &s2mpg10_regmap_config_meter,
};

static const struct of_device_id sec_pmic_acpm_of_match[] = {
	{ .compatible = "samsung,s2mpg10-pmic", .data = &s2mpg10_data, },
	{ },
};
MODULE_DEVICE_TABLE(of, sec_pmic_acpm_of_match);

static struct platform_driver sec_pmic_acpm_driver = {
	.driver = {
		.name = "sec-pmic-acpm",
		.pm = pm_sleep_ptr(&sec_pmic_pm_ops),
		.of_match_table = sec_pmic_acpm_of_match,
	},
	.probe = sec_pmic_acpm_probe,
	.shutdown = sec_pmic_acpm_shutdown,
};
module_platform_driver(sec_pmic_acpm_driver);

MODULE_AUTHOR("André Draszik <andre.draszik@linaro.org>");
MODULE_DESCRIPTION("ACPM driver for the Samsung S2MPG1x");
MODULE_LICENSE("GPL");
