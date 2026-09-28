// SPDX-License-Identifier: GPL-2.0+
/*
 * Author: zhanghongchen <zhanghongchen@loongson.cn>
 *         Yinbo Zhu <zhuyinbo@loongson.cn>
 * Copyright (C) 2022-2023 Loongson Technology Corporation Limited
 */

#include <linux/bitfield.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/thermal.h>
#include <linux/units.h>

#include "thermal_hwmon.h"

#define LOONGSON2_MAX_SENSOR_SEL_NUM	3

#define LOONGSON2_THSENS_CTRL_HI_REG	0x0
#define LOONGSON2_THSENS_CTRL_LOW_REG	0x8
#define LOONGSON2_THSENS_STATUS_REG	0x10
#define LOONGSON2_THSENS_OUT_REG	0x14

#define LOONGSON2_THSENS_INT_LO		BIT(0)
#define LOONGSON2_THSENS_INT_HIGH	BIT(1)
#define LOONGSON2_THSENS_INT_EN		(LOONGSON2_THSENS_INT_LO | \
					 LOONGSON2_THSENS_INT_HIGH)
#define LOONGSON2_THSENS_CTRL_INTEN	BIT(8)

#define LOONGSON2_THSENS_OUT_8B_MASK	GENMASK(7, 0)
#define LOONGSON2_THSENS_OUT_16B_MASK	GENMASK(15, 0)

#define LOONGSON2_LOWEST_VALID_TEMP     (-40000)
#define LOONGSON2_HIGHEST_VALID_TEMP    (125000)

/* Loongson-2K2000-Specific Registers */
#define LS2K2000_THSENS_HI_GATE_HI_REG	0x18
#define LS2K2000_THSENS_HI_GATE_LOW_REG	0x1c
#define LS2K2000_GATE0_VAL_MASK		GENMASK(7, 0)
#define LS2K2000_THSENS_SEL_MASK	GENMASK(11, 10)

#define LS2K2000_TEMP_MUL		(820)
#define LS2K2000_TEMP_DIV		(0x4000)
#define LS2K2000_TEMP_OFFSET		(311)

struct loongson2_thermal_data;

struct loongson2_thermal_chip_data {
	unsigned int thermal_sensor_sel;
	const struct thermal_zone_device_ops *thermal_ops;
	void (*set_trips_regs)(struct loongson2_thermal_data *data,
			       int temp, bool low, bool enable);
	int (*init_extend_resource)(struct platform_device *pdev,
				    struct loongson2_thermal_data *data);
};

struct loongson2_thermal_data {
	void __iomem *ctrl_reg;
	void __iomem *extend_reg;
	const struct loongson2_thermal_chip_data *chip_data;
};

static void loongson2_thermal_set_trip_temp(struct loongson2_thermal_data *data,
					    int low, int high, bool enable)
{
	/* Set low temperature threshold */
	data->chip_data->set_trips_regs(data, low, true, enable);

	/* Set high temperature threshold */
	data->chip_data->set_trips_regs(data, high, false, enable);
}

static int loognson2_thermal_set_trips(struct thermal_zone_device *tz, int low, int high)
{
	struct loongson2_thermal_data *data = thermal_zone_device_priv(tz);

	low = clamp(low, LOONGSON2_LOWEST_VALID_TEMP, LOONGSON2_HIGHEST_VALID_TEMP);
	high = clamp(high, LOONGSON2_LOWEST_VALID_TEMP, LOONGSON2_HIGHEST_VALID_TEMP);

	loongson2_thermal_set_trip_temp(data, low / MILLI, high / MILLI, true);

	return 0;
}

static int ls2k1000_thermal_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct loongson2_thermal_data *data = thermal_zone_device_priv(tz);
	int val;

	val = FIELD_GET(LOONGSON2_THSENS_OUT_8B_MASK,
			readl(data->ctrl_reg + LOONGSON2_THSENS_OUT_REG));
	*temp = (val - HECTO) * KILO;

	return 0;
}

static void ls2k1000_thermal_set_trips_regs(struct loongson2_thermal_data *data,
					    int temp, bool low, bool enable)
{
	int ctrl_reg = low ? LOONGSON2_THSENS_CTRL_LOW_REG : LOONGSON2_THSENS_CTRL_HI_REG;
	int reg_off  = data->chip_data->thermal_sensor_sel * 2;
	int reg_ctrl = 0;

	reg_ctrl = temp + HECTO;
	if (enable)
		reg_ctrl |= LOONGSON2_THSENS_CTRL_INTEN;

	writew(reg_ctrl, data->ctrl_reg + ctrl_reg + reg_off);
}

static int ls2k2000_thermal_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct loongson2_thermal_data *data = thermal_zone_device_priv(tz);
	int val;

	val = FIELD_GET(LOONGSON2_THSENS_OUT_16B_MASK, readl(data->extend_reg));
	*temp = (val * LS2K2000_TEMP_MUL / LS2K2000_TEMP_DIV - LS2K2000_TEMP_OFFSET) * KILO;

	return 0;
}

static void ls2k2000_thermal_set_trips_regs(struct loongson2_thermal_data *data,
					    int temp, bool low, bool enable)
{
	int hi_reg, ctrl_reg;
	int val = 0, gate;

	ctrl_reg = low ? LOONGSON2_THSENS_CTRL_LOW_REG : LOONGSON2_THSENS_CTRL_HI_REG;
	gate = (temp + LS2K2000_TEMP_OFFSET) * LS2K2000_TEMP_DIV / LS2K2000_TEMP_MUL;

	val = FIELD_PREP(LS2K2000_GATE0_VAL_MASK, FIELD_GET(GENMASK(7, 0), gate)) |
	      FIELD_PREP(LS2K2000_THSENS_SEL_MASK, 0);
	if (enable)
		val |= LOONGSON2_THSENS_CTRL_INTEN;

	writel(val, data->ctrl_reg + ctrl_reg);

	hi_reg = low ? LS2K2000_THSENS_HI_GATE_LOW_REG : LS2K2000_THSENS_HI_GATE_HI_REG;

	val = readl(data->ctrl_reg + hi_reg);
	val &= ~LS2K2000_GATE0_VAL_MASK;
	val |= FIELD_PREP(LS2K2000_GATE0_VAL_MASK, FIELD_GET(GENMASK(15, 8), gate));

	writel(val, data->ctrl_reg + hi_reg);
}

static int ls2k2000_thermal_init_extend_resource(struct platform_device *pdev,
						 struct loongson2_thermal_data *data)
{
	/* The temperature output register is separate for Loongson-2K2000 */
	data->extend_reg = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(data->extend_reg))
		return PTR_ERR(data->extend_reg);

	return 0;
}

static const struct thermal_zone_device_ops ls2k1000_thermal_zone_ops = {
	.get_temp = ls2k1000_thermal_get_temp,
	.set_trips = loognson2_thermal_set_trips,
};

static const struct thermal_zone_device_ops ls2k2000_thermal_zone_ops = {
	.get_temp = ls2k2000_thermal_get_temp,
	.set_trips = loognson2_thermal_set_trips,
};

static const struct loongson2_thermal_chip_data ls2k1000_thermal_chip_data = {
	.thermal_sensor_sel = 0,
	.thermal_ops = &ls2k1000_thermal_zone_ops,
	.set_trips_regs = ls2k1000_thermal_set_trips_regs,
};

static const struct loongson2_thermal_chip_data ls2k2000_thermal_chip_data = {
	.thermal_sensor_sel = 0,
	.thermal_ops = &ls2k2000_thermal_zone_ops,
	.set_trips_regs = ls2k2000_thermal_set_trips_regs,
	.init_extend_resource = ls2k2000_thermal_init_extend_resource,
};

static irqreturn_t loongson2_thermal_irq_thread(int irq, void *dev)
{
	struct thermal_zone_device *tzd = dev;
	struct loongson2_thermal_data *data = thermal_zone_device_priv(tzd);

	writeb(LOONGSON2_THSENS_INT_EN, data->ctrl_reg + LOONGSON2_THSENS_STATUS_REG);

	thermal_zone_device_update(tzd, THERMAL_EVENT_UNSPECIFIED);

	return IRQ_HANDLED;
}

static int loongson2_thermal_probe(struct platform_device *pdev)
{
	struct loongson2_thermal_data *data;
	struct device *dev = &pdev->dev;
	struct thermal_zone_device *tzd;
	int ret, irq, i;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->chip_data = device_get_match_data(dev);
	if (!data->chip_data)
		return -EINVAL;

	data->ctrl_reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(data->ctrl_reg))
		return PTR_ERR(data->ctrl_reg);

	if (data->chip_data->init_extend_resource) {
		ret = data->chip_data->init_extend_resource(pdev, data);
		if (ret)
			return ret;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	writeb(LOONGSON2_THSENS_INT_EN, data->ctrl_reg + LOONGSON2_THSENS_STATUS_REG);

	loongson2_thermal_set_trip_temp(data, 0, 0, false);

	for (i = 0; i <= LOONGSON2_MAX_SENSOR_SEL_NUM; i++) {
		tzd = devm_thermal_of_zone_register(dev, i, data, data->chip_data->thermal_ops);
		if (!IS_ERR(tzd))
			break;

		if (PTR_ERR(tzd) == -ENODEV)
			continue;

		return dev_err_probe(dev, PTR_ERR(tzd), "failed to register sensor %d\n", i);
	}

	if (IS_ERR(tzd))
		return dev_err_probe(dev, -ENODEV, "No thermal sensor registered\n");

	ret = devm_request_threaded_irq(dev, irq, NULL, loongson2_thermal_irq_thread,
					IRQF_ONESHOT, "loongson2_thermal", tzd);
	if (ret < 0)
		return ret;

	devm_thermal_add_hwmon_sysfs(dev, tzd);

	return 0;
}

static const struct of_device_id of_loongson2_thermal_match[] = {
	{
		.compatible = "loongson,ls2k1000-thermal",
		.data = &ls2k1000_thermal_chip_data,
	},
	{
		.compatible = "loongson,ls2k2000-thermal",
		.data = &ls2k2000_thermal_chip_data,
	},
	{ /* end */ }
};
MODULE_DEVICE_TABLE(of, of_loongson2_thermal_match);

static struct platform_driver loongson2_thermal_driver = {
	.driver = {
		.name		= "loongson2_thermal",
		.of_match_table = of_loongson2_thermal_match,
	},
	.probe	= loongson2_thermal_probe,
};
module_platform_driver(loongson2_thermal_driver);

MODULE_DESCRIPTION("Loongson2 thermal driver");
MODULE_AUTHOR("Loongson Technology Corporation Limited");
MODULE_LICENSE("GPL");
