// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2019 Samsung Electronics Co., Ltd.
 * Copyright 2025 Google LLC.
 * Copyright 2026 Linaro Ltd.
 */

#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/device/devres.h>
#include <linux/err.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/thermal.h>
#include <linux/units.h>

#include "../thermal_hwmon.h"

#define EXYNOS_TMU_SENSOR(i)		BIT(i)
#define EXYNOS_TMU_SENSORS_MAX_COUNT	16

#define GS101_CPUCL2_SENSOR_MASK (EXYNOS_TMU_SENSOR(0) |	\
				  EXYNOS_TMU_SENSOR(6) |	\
				  EXYNOS_TMU_SENSOR(7) |	\
				  EXYNOS_TMU_SENSOR(8) |	\
				  EXYNOS_TMU_SENSOR(9))
#define GS101_CPUCL1_SENSOR_MASK (EXYNOS_TMU_SENSOR(4) |	\
				  EXYNOS_TMU_SENSOR(5))
#define GS101_CPUCL0_SENSOR_MASK (EXYNOS_TMU_SENSOR(1) |	\
				  EXYNOS_TMU_SENSOR(2))

#define GS101_REG_INTPEND(i)		((i) * 0x50 + 0xf8)

enum {
	P0_INTPEND,
	P1_INTPEND,
	P2_INTPEND,
	P3_INTPEND,
	P4_INTPEND,
	P5_INTPEND,
	P6_INTPEND,
	P7_INTPEND,
	P8_INTPEND,
	P9_INTPEND,
	P10_INTPEND,
	P11_INTPEND,
	P12_INTPEND,
	P13_INTPEND,
	P14_INTPEND,
	P15_INTPEND,
	REG_INTPEND_COUNT,
};

struct acpm_tmu_sensor_group {
	u16 mask;
	u8 id;
};

struct acpm_tmu_sensor {
	const struct acpm_tmu_sensor_group *group;
	struct thermal_zone_device *tzd;
	struct acpm_tmu_priv *priv;
	struct mutex lock; /* protects sensor state */
	bool enabled;
};

struct acpm_tmu_priv {
	struct regmap_field *regmap_fields[REG_INTPEND_COUNT];
	const struct acpm_handle *handle;
	struct device *dev;
	struct clk *clk;
	unsigned int mbox_chan_id;
	unsigned int num_sensors;
	int irq;
	struct acpm_tmu_sensor sensors[] __counted_by(num_sensors);
};

struct acpm_tmu_driver_data {
	const struct reg_field *reg_fields;
	const struct acpm_tmu_sensor_group *sensor_groups;
	unsigned int num_sensor_groups;
	unsigned int mbox_chan_id;
};

struct acpm_tmu_initialize_tripwalkdata {
	unsigned char threshold[8];
	unsigned char hysteresis[8];
	unsigned char inten;
	int i;
};

struct acpm_tmu_trip_temp_tripwalkdata {
	const struct thermal_trip *trip;
	unsigned char threshold[8];
	int temperature;
	int i;
};

#define ACPM_TMU_SENSOR_GROUP(_mask, _id)		\
	{					\
		.mask	= _mask,		\
		.id	= _id,			\
	}

static const struct acpm_tmu_sensor_group gs101_sensor_groups[] = {
	ACPM_TMU_SENSOR_GROUP(GS101_CPUCL2_SENSOR_MASK, 0),
	ACPM_TMU_SENSOR_GROUP(GS101_CPUCL1_SENSOR_MASK, 1),
	ACPM_TMU_SENSOR_GROUP(GS101_CPUCL0_SENSOR_MASK, 2),
};

static const struct reg_field gs101_reg_fields[REG_INTPEND_COUNT] = {
	[P0_INTPEND] = REG_FIELD(GS101_REG_INTPEND(0), 0, 31),
	[P1_INTPEND] = REG_FIELD(GS101_REG_INTPEND(1), 0, 31),
	[P2_INTPEND] = REG_FIELD(GS101_REG_INTPEND(2), 0, 31),
	[P3_INTPEND] = REG_FIELD(GS101_REG_INTPEND(3), 0, 31),
	[P4_INTPEND] = REG_FIELD(GS101_REG_INTPEND(4), 0, 31),
	[P5_INTPEND] = REG_FIELD(GS101_REG_INTPEND(5), 0, 31),
	[P6_INTPEND] = REG_FIELD(GS101_REG_INTPEND(6), 0, 31),
	[P7_INTPEND] = REG_FIELD(GS101_REG_INTPEND(7), 0, 31),
	[P8_INTPEND] = REG_FIELD(GS101_REG_INTPEND(8), 0, 31),
	[P9_INTPEND] = REG_FIELD(GS101_REG_INTPEND(9), 0, 31),
	[P10_INTPEND] = REG_FIELD(GS101_REG_INTPEND(10), 0, 31),
	[P11_INTPEND] = REG_FIELD(GS101_REG_INTPEND(11), 0, 31),
	[P12_INTPEND] = REG_FIELD(GS101_REG_INTPEND(12), 0, 31),
	[P13_INTPEND] = REG_FIELD(GS101_REG_INTPEND(13), 0, 31),
	[P14_INTPEND] = REG_FIELD(GS101_REG_INTPEND(14), 0, 31),
	[P15_INTPEND] = REG_FIELD(GS101_REG_INTPEND(15), 0, 31),
};

static const struct regmap_config gs101_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.use_relaxed_mmio = true,
	.max_register = GS101_REG_INTPEND(15),
};

static const struct acpm_tmu_driver_data acpm_tmu_gs101 = {
	.reg_fields = gs101_reg_fields,
	.sensor_groups = gs101_sensor_groups,
	.num_sensor_groups = ARRAY_SIZE(gs101_sensor_groups),
	.mbox_chan_id = 9,
};

static int acpm_tmu_op_tz_control(struct acpm_tmu_sensor *sensor, bool on)
{
	struct acpm_tmu_priv *priv = sensor->priv;
	const struct acpm_handle *handle = priv->handle;
	const struct acpm_tmu_ops *ops = &handle->ops.tmu;
	int ret;

	ret = ops->tz_control(handle, priv->mbox_chan_id, sensor->group->id,
			      on);
	if (ret)
		return ret;

	sensor->enabled = on;

	return 0;
}

static int acpm_tmu_control(struct acpm_tmu_priv *priv, bool on)
{
	struct device *dev = priv->dev;
	int i, ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	for (i = 0; i < priv->num_sensors; i++) {
		struct acpm_tmu_sensor *sensor = &priv->sensors[i];

		/* Skip sensors that weren't found in DT */
		if (!sensor->tzd)
			continue;

		scoped_guard(mutex, &sensor->lock) {
			ret = acpm_tmu_op_tz_control(sensor, on);
		}

		if (ret)
			goto out;
	}

out:
	pm_runtime_put_autosuspend(dev);
	return ret;
}

static int acpm_tmu_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct acpm_tmu_sensor *sensor = thermal_zone_device_priv(tz);
	struct acpm_tmu_priv *priv = sensor->priv;
	const struct acpm_handle *handle = priv->handle;
	const struct acpm_tmu_ops *ops = &handle->ops.tmu;
	struct device *dev = priv->dev;
	int acpm_temp, ret;

	if (!sensor->enabled)
		return -EAGAIN;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	scoped_guard(mutex, &sensor->lock) {
		ret = ops->read_temp(handle, priv->mbox_chan_id,
				     sensor->group->id, &acpm_temp);
	}

	pm_runtime_put_autosuspend(dev);

	if (ret)
		return ret;

	*temp = acpm_temp * MILLIDEGREE_PER_DEGREE;

	return 0;
}

static int acpm_tmu_trip_temp_walk_cb(struct thermal_trip *trip, void *data)
{
	struct acpm_tmu_trip_temp_tripwalkdata * const twd = data;
	int temperature;

	if (twd->i >= ARRAY_SIZE(twd->threshold))
		return -ERANGE;

	if (trip->type == THERMAL_TRIP_PASSIVE)
		goto out;

	temperature = (trip == twd->trip) ? twd->temperature : trip->temperature;

	twd->threshold[twd->i] = temperature / MILLIDEGREE_PER_DEGREE;

out:
	++twd->i;
	return 0;
}

static int acpm_tmu_set_trip_temp(struct thermal_zone_device *tz,
			const struct thermal_trip *trip, int temperature)
{
	struct acpm_tmu_sensor *sensor = thermal_zone_device_priv(tz);
	struct acpm_tmu_priv *priv = sensor->priv;
	const struct acpm_handle *handle = priv->handle;
	const struct acpm_tmu_ops *ops = &handle->ops.tmu;
	struct device *dev = priv->dev;
	struct acpm_tmu_trip_temp_tripwalkdata twd = {
		.trip = trip,
		.temperature = temperature,
	};
	int ret;

	if (trip->type == THERMAL_TRIP_PASSIVE)
		return 0;

	for_each_thermal_trip(tz, acpm_tmu_trip_temp_walk_cb, &twd);

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	guard(mutex)(&sensor->lock);

	if (!sensor->enabled) {
		ret = ops->set_threshold(handle, priv->mbox_chan_id,
					 sensor->group->id, twd.threshold,
					 ARRAY_SIZE(twd.threshold));
		goto out;
	}

	ret = acpm_tmu_op_tz_control(sensor, false);
	if (ret)
		goto out;

	ret = ops->set_threshold(handle, priv->mbox_chan_id, sensor->group->id,
				 twd.threshold, ARRAY_SIZE(twd.threshold));
	if (ret)
		goto out;

	ret = acpm_tmu_op_tz_control(sensor, true);

out:
	pm_runtime_put_autosuspend(dev);
	return ret;
}

static const struct thermal_zone_device_ops acpm_tmu_sensor_ops = {
	.get_temp = acpm_tmu_get_temp,
	.set_trip_temp = acpm_tmu_set_trip_temp,
};

static int acpm_tmu_has_pending_irq(struct acpm_tmu_sensor *sensor,
				    bool *pending_irq)
{
	struct acpm_tmu_priv *priv = sensor->priv;
	unsigned long mask = sensor->group->mask;
	int i, ret;
	u32 val;

	guard(mutex)(&sensor->lock);

	for_each_set_bit(i, &mask, EXYNOS_TMU_SENSORS_MAX_COUNT) {
		ret = regmap_field_read(priv->regmap_fields[i], &val);
		if (ret)
			return ret;

		if (val) {
			*pending_irq = true;
			break;
		}
	}

	return 0;
}

static irqreturn_t acpm_tmu_thread_fn(int irq, void *id)
{
	struct acpm_tmu_priv *priv = id;
	const struct acpm_handle *handle = priv->handle;
	const struct acpm_tmu_ops *ops = &handle->ops.tmu;
	struct device *dev = priv->dev;
	int i, ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret) {
		dev_err(dev, "Failed to resume: %d\n", ret);
		return IRQ_NONE;
	}

	for (i = 0; i < priv->num_sensors; i++) {
		struct acpm_tmu_sensor *sensor = &priv->sensors[i];
		bool pending_irq = false;

		if (!sensor->tzd)
			continue;

		ret = acpm_tmu_has_pending_irq(sensor, &pending_irq);
		if (ret || !pending_irq)
			continue;

		thermal_zone_device_update(sensor->tzd,
					   THERMAL_EVENT_UNSPECIFIED);

		scoped_guard(mutex, &sensor->lock) {
			ret = ops->clear_tz_irq(handle, priv->mbox_chan_id,
						sensor->group->id);
			if (ret)
				dev_err(priv->dev, "Sensor %d: failed to clear IRQ (%d)\n",
					i, ret);
		}
	}

	pm_runtime_put_autosuspend(dev);

	return IRQ_HANDLED;
}

static int acpm_tmu_init_sensor(struct acpm_tmu_sensor *sensor,
			const struct acpm_tmu_initialize_tripwalkdata *twd)
{
	struct acpm_tmu_priv *priv = sensor->priv;
	const struct acpm_handle *handle = priv->handle;
	const struct acpm_tmu_ops *ops = &handle->ops.tmu;
	unsigned int mbox_chan_id = priv->mbox_chan_id;
	u8 acpm_sensor_id = sensor->group->id;
	int ret;

	guard(mutex)(&sensor->lock);

	ret = ops->set_threshold(handle, mbox_chan_id, acpm_sensor_id,
				 twd->threshold,
				 ARRAY_SIZE(twd->threshold));
	if (ret)
		return ret;

	ret = ops->set_hysteresis(handle, mbox_chan_id, acpm_sensor_id,
				  twd->hysteresis,
				  ARRAY_SIZE(twd->threshold));
	if (ret)
		return ret;

	ret = ops->set_interrupt_enable(handle, mbox_chan_id, acpm_sensor_id,
					twd->inten);
	return ret;
}

static int acpm_tmu_initialize_walk_cb(struct thermal_trip *trip, void *data)
{
	struct acpm_tmu_initialize_tripwalkdata * const twd = data;
	int i;

	if (twd->i >= ARRAY_SIZE(twd->threshold))
		return -ERANGE;

	if (trip->type == THERMAL_TRIP_PASSIVE)
		goto out;

	i = twd->i;

	twd->threshold[i] = trip->temperature / MILLIDEGREE_PER_DEGREE;
	twd->hysteresis[i] = trip->hysteresis / MILLIDEGREE_PER_DEGREE;

	twd->inten |= BIT(i);

out:
	++twd->i;
	return 0;
}

static int acpm_tmu_init_sensors(struct acpm_tmu_priv *priv)
{
	int i, ret;

	for (i = 0; i < priv->num_sensors; i++) {
		struct acpm_tmu_sensor *sensor = &priv->sensors[i];
		struct acpm_tmu_initialize_tripwalkdata twd = {};

		/* Skip sensors that weren't found in DT */
		if (!sensor->tzd)
			continue;

		thermal_zone_for_each_trip(sensor->tzd,
					   acpm_tmu_initialize_walk_cb, &twd);

		ret = acpm_tmu_init_sensor(sensor, &twd);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct of_device_id acpm_tmu_match[] = {
	{ .compatible = "google,gs101-tmu-top" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, acpm_tmu_match);

static int acpm_tmu_probe(struct platform_device *pdev)
{
	const struct acpm_tmu_driver_data *data = &acpm_tmu_gs101;
	const struct acpm_handle *acpm_handle;
	struct device *dev = &pdev->dev;
	struct acpm_tmu_priv *priv;
	struct regmap *regmap;
	void __iomem *base;
	int i, ret;

	acpm_handle = devm_acpm_get_by_phandle(dev);
	if (IS_ERR(acpm_handle))
		return dev_err_probe(dev, PTR_ERR(acpm_handle),
				     "Failed to get ACPM handle\n");

	priv = devm_kzalloc(dev,
			    struct_size(priv, sensors, data->num_sensor_groups),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->handle = acpm_handle;
	priv->mbox_chan_id = data->mbox_chan_id;
	priv->num_sensors = data->num_sensor_groups;

	platform_set_drvdata(pdev, priv);

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return dev_err_probe(dev, PTR_ERR(base), "Failed to ioremap resource\n");

	regmap = devm_regmap_init_mmio(dev, base, &gs101_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "Failed to init regmap\n");

	ret = devm_regmap_field_bulk_alloc(dev, regmap, priv->regmap_fields,
					   data->reg_fields, REG_INTPEND_COUNT);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Unable to map syscon registers\n");

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk))
		return dev_err_probe(dev, PTR_ERR(priv->clk),
				     "Failed to get the clock\n");

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0)
		return dev_err_probe(dev, priv->irq, "Failed to get irq\n");

	ret = devm_request_threaded_irq(dev, priv->irq, NULL,
					acpm_tmu_thread_fn, IRQF_ONESHOT,
					dev_name(dev), priv);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request irq\n");

	pm_runtime_set_autosuspend_delay(dev, 100);
	pm_runtime_use_autosuspend(dev);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable runtime PM\n");

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to resume device\n");

	ret = acpm_handle->ops.tmu.init(acpm_handle, priv->mbox_chan_id);
	if (ret) {
		ret = dev_err_probe(dev, ret, "Failed to init TMU\n");
		goto err_pm_put;
	}

	for (i = 0; i < priv->num_sensors; i++) {
		struct acpm_tmu_sensor *sensor = &priv->sensors[i];

		mutex_init(&sensor->lock);
		sensor->group = &data->sensor_groups[i];
		sensor->priv = priv;

		sensor->tzd = devm_thermal_of_zone_register(dev, i, sensor,
							    &acpm_tmu_sensor_ops);
		if (IS_ERR(sensor->tzd)) {
			ret = PTR_ERR(sensor->tzd);
			if (ret == -ENODEV) {
				sensor->tzd = NULL;
				dev_dbg(dev, "Sensor %d not used in DT, skipping\n", i);
				continue;
			}

			ret = dev_err_probe(dev, ret, "Failed to register sensor %d\n", i);
			goto err_pm_put;
		}

		ret = devm_thermal_add_hwmon_sysfs(dev, sensor->tzd);
		if (ret)
			dev_warn(dev, "Failed to add hwmon sysfs!\n");
	}

	ret = acpm_tmu_init_sensors(priv);
	if (ret) {
		ret = dev_err_probe(dev, ret, "Failed to init sensors\n");
		goto err_pm_put;
	}

	ret = acpm_tmu_control(priv, true);
	if (ret) {
		ret = dev_err_probe(dev, ret, "Failed to enable TMU\n");
		goto err_pm_put;
	}

	pm_runtime_put_autosuspend(&pdev->dev);

	return 0;

err_pm_put:
	pm_runtime_put_sync(dev);
	return ret;
}

static void acpm_tmu_remove(struct platform_device *pdev)
{
	struct acpm_tmu_priv *priv = platform_get_drvdata(pdev);

	/* Stop IRQ first to prevent race with thread_fn */
	disable_irq(priv->irq);

	acpm_tmu_control(priv, false);
}

static int acpm_tmu_suspend(struct device *dev)
{
	struct acpm_tmu_priv *priv = dev_get_drvdata(dev);
	const struct acpm_handle *handle = priv->handle;
	const struct acpm_tmu_ops *ops = &handle->ops.tmu;
	int ret;

	ret = acpm_tmu_control(priv, false);
	if (ret)
		return ret;

	/* APB clock not required for this specific msg */
	return ops->suspend(handle, priv->mbox_chan_id);
}

static int acpm_tmu_resume(struct device *dev)
{
	struct acpm_tmu_priv *priv = dev_get_drvdata(dev);
	const struct acpm_handle *handle = priv->handle;
	const struct acpm_tmu_ops *ops = &handle->ops.tmu;
	int ret;

	/* APB clock not required for this specific msg */
	ret = ops->resume(handle, priv->mbox_chan_id);
	if (ret)
		return ret;

	return acpm_tmu_control(priv, true);
}

static int acpm_tmu_runtime_suspend(struct device *dev)
{
	struct acpm_tmu_priv *priv = dev_get_drvdata(dev);

	clk_disable_unprepare(priv->clk);

	return 0;
}

static int acpm_tmu_runtime_resume(struct device *dev)
{
	struct acpm_tmu_priv *priv = dev_get_drvdata(dev);

	return clk_prepare_enable(priv->clk);
}

static const struct dev_pm_ops acpm_tmu_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(acpm_tmu_suspend, acpm_tmu_resume)
	RUNTIME_PM_OPS(acpm_tmu_runtime_suspend, acpm_tmu_runtime_resume, NULL)
};

static struct platform_driver acpm_tmu_driver = {
	.driver = {
		.name   = "gs-tmu",
		.pm     = pm_ptr(&acpm_tmu_pm_ops),
		.of_match_table = acpm_tmu_match,
	},
	.probe = acpm_tmu_probe,
	.remove = acpm_tmu_remove,
};
module_platform_driver(acpm_tmu_driver);

MODULE_AUTHOR("Tudor Ambarus <tudor.ambarus@linaro.org>");
MODULE_DESCRIPTION("Samsung Exynos ACPM TMU Driver");
MODULE_LICENSE("GPL");
