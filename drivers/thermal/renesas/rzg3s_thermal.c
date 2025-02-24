// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas RZ/G3S TSU Thermal Sensor Driver
 *
 * Copyright (C) 2024 Renesas Electronics Corporation
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/iio/consumer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/thermal.h>
#include <linux/units.h>

#include "../thermal_hwmon.h"

#define TSU_SM			0x0
#define TSU_SM_EN		BIT(0)
#define TSU_SM_OE		BIT(1)
#define OTPTSUTRIM_REG(n)	(0x18 + (n) * 0x4)
#define OTPTSUTRIM_EN_MASK	BIT(31)
#define OTPTSUTRIM_MASK		GENMASK(11, 0)

#define TSU_READ_STEPS		8

/* Default calibration values, if FUSE values are missing. */
#define SW_CALIB0_VAL		1297
#define SW_CALIB1_VAL		751

#define MCELSIUS(temp)		((temp) * MILLIDEGREE_PER_DEGREE)

/**
 * struct rzg3s_thermal_priv - RZ/G3S thermal private data structure
 * @base: TSU base address
 * @dev: device pointer
 * @tz: thermal zone pointer
 * @rstc: reset control
 * @channel: IIO channel to read the TSU
 * @devres_group_id: devres group for the driver devres resources
 *		      obtained in probe
 * @mode: current device mode
 * @calib0: calibration value
 * @calib1: calibration value
 */
struct rzg3s_thermal_priv {
	void __iomem *base;
	struct device *dev;
	struct thermal_zone_device *tz;
	struct reset_control *rstc;
	struct iio_channel *channel;
	void *devres_group_id;
	enum thermal_device_mode mode;
	u16 calib0;
	u16 calib1;
};

static int rzg3s_thermal_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct rzg3s_thermal_priv *priv = thermal_zone_device_priv(tz);
	struct device *dev = priv->dev;
	int ts_code_ave = 0;
	int ret, val;

	if (priv->mode != THERMAL_DEVICE_ENABLED)
		return -EAGAIN;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	for (u8 i = 0; i < TSU_READ_STEPS; i++) {
		ret = iio_read_channel_raw(priv->channel, &val);
		if (ret < 0)
			goto rpm_put;

		ts_code_ave += val;
		/*
		 * According to the HW manual (section 40.4.4 Procedure for Measuring the
		 * Temperature) we need to wait here at leat 3us.
		 */
		usleep_range(5, 10);
	}

	ret = 0;
	ts_code_ave = DIV_ROUND_CLOSEST(MCELSIUS(ts_code_ave), TSU_READ_STEPS);

	/*
	 * According to the HW manual (section 40.4.4 Procedure for Measuring the Temperature)
	 * the computation formula is as follows:
	 *
	 * Tj = (ts_code_ave - priv->calib1) * 165 / (priv->calib0 - priv->calib1) - 40
	 *
	 * Convert everything to mili Celsius before applying the formula to avoid
	 * losing precision.
	 */

	*temp = DIV_ROUND_CLOSEST((s64)(ts_code_ave - MCELSIUS(priv->calib1)) * MCELSIUS(165),
				  MCELSIUS(priv->calib0 - priv->calib1)) - MCELSIUS(40);

	/* Report it in mili degrees Celsius and round it up to 0.5 degrees Celsius. */
	*temp = roundup(*temp, 500);

rpm_put:
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

static void rzg3s_thermal_set_mode(struct rzg3s_thermal_priv *priv,
				   enum thermal_device_mode mode)
{
	struct device *dev = priv->dev;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return;

	if (mode == THERMAL_DEVICE_DISABLED) {
		writel(0, priv->base + TSU_SM);
	} else {
		writel(TSU_SM_EN, priv->base + TSU_SM);
		/*
		 * According to the HW manual (section 40.4.1 Procedure for
		 * Starting the TSU) we need to wait here 30us or more.
		 */
		usleep_range(30, 40);

		writel(TSU_SM_OE | TSU_SM_EN, priv->base + TSU_SM);
		/*
		 * According to the HW manual (section 40.4.1 Procedure for
		 * Starting the TSU) we need to wait here 50us or more.
		 */
		usleep_range(50, 60);
	}

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
}

static int rzg3s_thermal_change_mode(struct thermal_zone_device *tz,
				     enum thermal_device_mode mode)
{
	struct rzg3s_thermal_priv *priv = thermal_zone_device_priv(tz);

	if (priv->mode == mode)
		return 0;

	rzg3s_thermal_set_mode(priv, mode);
	priv->mode = mode;

	return 0;
}

static const struct thermal_zone_device_ops rzg3s_tz_of_ops = {
	.get_temp = rzg3s_thermal_get_temp,
	.change_mode = rzg3s_thermal_change_mode,
};

static int rzg3s_thermal_read_calib(struct rzg3s_thermal_priv *priv)
{
	struct device *dev = priv->dev;
	u32 val;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	val = readl(priv->base + OTPTSUTRIM_REG(0));
	if (val & OTPTSUTRIM_EN_MASK)
		priv->calib0 = FIELD_GET(OTPTSUTRIM_MASK, val);
	else
		priv->calib0 = SW_CALIB0_VAL;

	val = readl(priv->base + OTPTSUTRIM_REG(1));
	if (val & OTPTSUTRIM_EN_MASK)
		priv->calib1 = FIELD_GET(OTPTSUTRIM_MASK, val);
	else
		priv->calib1 = SW_CALIB1_VAL;

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return 0;
}

static int rzg3s_thermal_probe(struct platform_device *pdev)
{
	struct rzg3s_thermal_priv *priv;
	struct device *dev = &pdev->dev;
	void *devres_group_id;
	int ret;

	/*
	 * Open a devres group to allow using devm_pm_runtime_enable()
	 * w/o interfeering with dev_pm_genpd_detach() in the platform bus
	 * remove. Otherwise, durring repeated unbind/bind operations,
	 * the TSU may be runtime resumed when it is not part of its power
	 * domain, leading to accessing TSU registers (through
	 * rzg3s_thermal_change_mode()) without its clocks being enabled
	 * and its PM domain being turned on.
	 */
	devres_group_id = devres_open_group(dev, NULL, GFP_KERNEL);
	if (!devres_group_id)
		return -ENOMEM;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto release_group;
	}
	priv->devres_group_id = devres_group_id;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base)) {
		ret = PTR_ERR(priv->base);
		goto release_group;
	}

	priv->channel = devm_iio_channel_get(dev, "tsu");
	if (IS_ERR(priv->channel)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->channel), "Failed to get IIO channel!\n");
		goto release_group;
	}

	priv->rstc = devm_reset_control_get_exclusive_deasserted(dev, NULL);
	if (IS_ERR(priv->rstc)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->rstc), "Failed to get reset!\n");
		goto release_group;
	}

	priv->dev = dev;
	priv->mode = THERMAL_DEVICE_DISABLED;
	platform_set_drvdata(pdev, priv);

	pm_runtime_set_autosuspend_delay(dev, 300);
	pm_runtime_use_autosuspend(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to enable runtime PM!\n");
		goto release_group;
	}

	ret = rzg3s_thermal_read_calib(priv);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to read calibration data!\n");
		goto release_group;
	}

	priv->tz = devm_thermal_of_zone_register(dev, 0, priv, &rzg3s_tz_of_ops);
	if (IS_ERR(priv->tz)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->tz), "Failed to register thermal zone!\n");
		goto release_group;
	}

	ret = devm_thermal_add_hwmon_sysfs(dev, priv->tz);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to add hwmon sysfs!\n");
		goto release_group;
	}

	return 0;

release_group:
	devres_release_group(dev, devres_group_id);
	return ret;
}

static void rzg3s_thermal_remove(struct platform_device *pdev)
{
	struct rzg3s_thermal_priv *priv = dev_get_drvdata(&pdev->dev);

	devres_release_group(priv->dev, priv->devres_group_id);
}

static int rzg3s_thermal_suspend(struct device *dev)
{
	struct rzg3s_thermal_priv *priv = dev_get_drvdata(dev);

	rzg3s_thermal_set_mode(priv, THERMAL_DEVICE_DISABLED);

	return reset_control_assert(priv->rstc);
}

static int rzg3s_thermal_resume(struct device *dev)
{
	struct rzg3s_thermal_priv *priv = dev_get_drvdata(dev);
	int ret;

	ret = reset_control_deassert(priv->rstc);
	if (ret)
		return ret;

	if (priv->mode != THERMAL_DEVICE_DISABLED)
		rzg3s_thermal_set_mode(priv, priv->mode);

	return 0;
}

static const struct dev_pm_ops rzg3s_thermal_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(rzg3s_thermal_suspend, rzg3s_thermal_resume)
};

static const struct of_device_id rzg3s_thermal_dt_ids[] = {
	{ .compatible = "renesas,r9a08g045-tsu" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rzg3s_thermal_dt_ids);

static struct platform_driver rzg3s_thermal_driver = {
	.driver = {
		.name = "rzg3s_thermal",
		.of_match_table = rzg3s_thermal_dt_ids,
		.pm = pm_ptr(&rzg3s_thermal_pm_ops),
	},
	.probe = rzg3s_thermal_probe,
	.remove = rzg3s_thermal_remove,
};
module_platform_driver(rzg3s_thermal_driver);

MODULE_DESCRIPTION("Renesas RZ/G3S Thermal Sensor Unit Driver");
MODULE_AUTHOR("Claudiu Beznea <claudiu.beznea.uj@bp.renesas.com>");
MODULE_LICENSE("GPL");
