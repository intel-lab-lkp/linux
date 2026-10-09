// SPDX-License-Identifier: GPL-2.0+
/*
 * Author: zhanghongchen <zhanghongchen@loongson.cn>
 *         Yinbo Zhu <zhuyinbo@loongson.cn>
 * Copyright (C) 2022-2023 Loongson Technology Corporation Limited
 */

#include <linux/bitfield.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/thermal.h>
#include <linux/units.h>

#include "thermal_hwmon.h"

#define LOONGSON2_THSENS_CTRL_HI_REG	0x0
#define LOONGSON2_THSENS_CTRL_LOW_REG	0x8
#define LOONGSON2_THSENS_STATUS_REG	0x10
#define LOONGSON2_THSENS_OUT_REG	0x14

#define LOONGSON2_THSENS_INT_LO		BIT(0)
#define LOONGSON2_THSENS_INT_HIGH	BIT(1)
#define LOONGSON2_THSENS_INT_EN		(LOONGSON2_THSENS_INT_LO | \
					 LOONGSON2_THSENS_INT_HIGH)

#define LOONGSON2_THSENS_CTRL_GATE_MASK	GENMASK(7, 0)
#define LOONGSON2_THSENS_CTRL_INTEN	BIT(8)
#define LOONGSON2_THSENS_CTRL_SEL_MASK	GENMASK(11, 10)

#define LOONGSON2_THSENS_OUT_8B_MASK	GENMASK(7, 0)
#define LOONGSON2_THSENS_OUT_11B_MASK	GENMASK(10, 0)
#define LOONGSON2_THSENS_OUT_16B_MASK	GENMASK(15, 0)

#define LOONGSON2_LOWEST_VALID_TEMP     (-40000)
#define LOONGSON2_HIGHEST_VALID_TEMP    (125000)

/* Loongson-2K0300 CHIP ID syscon registers */
#define LS2K0300_SYSCON_CHIP_ID0	0x10
#define LS2K0300_SYSCON_CHIP_ID1	0x14

/* Loongson-2K0300 thermal sensor registers */
#define LS2K0300_THSENS_CFG_REG		0x18

#define LS2K0300_THSENS_INIT_MASK	0xff03

#define LS2K0300_EXTERN_ID		BIT(4)
/* Internal ID: 12-bit unsigned compensation value */
#define LS2K0300_ID0_VAL_MASK		GENMASK(31, 20)
/* External ID: 16-bit signed compensation value */
#define LS2K0300_ID1_VAL_MASK		GENMASK(15, 0)

#define LS2K0300_COMP_VAL_MASK		GENMASK(14, 0)
#define LS2K0300_COMP_SIGN_BIT		BIT(15)

#define LS2K0300_TEMP_SLOPE		(570)	/* calibrated */
#define LS2K0300_TEMP_SLOPE_OLD		(569)	/* old fuse */
#define LS2K0300_TEMP_OFFSET		(394700)

/*
 * Set when the on-chip calibration data is unusable (early LS2K0300
 * fuse revision). The driver then uses LS2K0300_TEMP_SLOPE_OLD with
 * calib_offset = 0 instead of the calibrated formula.
 */
#define LS2K0300_UNCALIBRATED_FLAG	BIT(0)

/* Loongson-2K2000-Specific Registers */
#define LS2K2000_THSENS_HI_GATE_HI_REG	0x18
#define LS2K2000_THSENS_HI_GATE_LOW_REG	0x1c

#define LS2K2000_TEMP_MUL		(820)
#define LS2K2000_TEMP_DIV		(0x4000)
#define LS2K2000_TEMP_OFFSET		(311)

struct loongson2_thermal_data;

struct loongson2_thermal_chip_data {
	unsigned int thermal_sensor_sel;
	const struct thermal_zone_device_ops *thermal_ops;
	void (*set_trips_regs)(struct loongson2_thermal_data *data,
			       int temp, bool low, bool enable);
	int (*hw_init)(struct platform_device *pdev, struct loongson2_thermal_data *data);
};

struct loongson2_thermal_data {
	void __iomem *ctrl_reg;
	void __iomem *extend_reg;
	struct regmap *regmap_cfg;
	unsigned int flags;
	int calib_offset;
	const struct loongson2_thermal_chip_data *chip_data;
};

static void loongson2_thermal_set_trip_temp(struct loongson2_thermal_data *data,
					    int low, int high, bool enable)
{
	low = clamp(low, LOONGSON2_LOWEST_VALID_TEMP, LOONGSON2_HIGHEST_VALID_TEMP);
	high = clamp(high, LOONGSON2_LOWEST_VALID_TEMP, LOONGSON2_HIGHEST_VALID_TEMP);

	/* Set low temperature threshold */
	data->chip_data->set_trips_regs(data, low / (int)MILLI, true, enable);

	/* Set high temperature threshold */
	data->chip_data->set_trips_regs(data, high / (int)MILLI, false, enable);
}

static int loongson2_thermal_set_trips(struct thermal_zone_device *tz, int low, int high)
{
	struct loongson2_thermal_data *data = thermal_zone_device_priv(tz);

	loongson2_thermal_set_trip_temp(data, low, high, true);

	return 0;
}

static int ls2k0300_thermal_adc_to_temp_mc(struct loongson2_thermal_data *data, int slope)
{
	int raw_adc;

	raw_adc = FIELD_GET(LOONGSON2_THSENS_OUT_11B_MASK,
			    readl(data->ctrl_reg + LOONGSON2_THSENS_OUT_REG));

	return (raw_adc + data->calib_offset) * slope - LS2K0300_TEMP_OFFSET;
}

static int ls2k0300_thermal_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct loongson2_thermal_data *data = thermal_zone_device_priv(tz);
	int slope = LS2K0300_TEMP_SLOPE;

	if (data->flags & LS2K0300_UNCALIBRATED_FLAG)
		slope = LS2K0300_TEMP_SLOPE_OLD;

	*temp = ls2k0300_thermal_adc_to_temp_mc(data, slope);

	return 0;
}

static void ls2k0300_thermal_detect_old_fuse(struct device *dev,
					     struct loongson2_thermal_data *data)
{
	int temp;

	temp = ls2k0300_thermal_adc_to_temp_mc(data, LS2K0300_TEMP_SLOPE);

	if (temp < LOONGSON2_LOWEST_VALID_TEMP ||
	    temp > LOONGSON2_HIGHEST_VALID_TEMP) {
		dev_warn(dev, "calibrated temperature %d mC out of range [%d, %d], using old fuse formula",
			 temp, LOONGSON2_LOWEST_VALID_TEMP, LOONGSON2_HIGHEST_VALID_TEMP);
		data->flags = LS2K0300_UNCALIBRATED_FLAG;
		data->calib_offset = 0;
	}
}

static int ls2k0300_thermal_read_calib(struct loongson2_thermal_data *data)
{
	u32 chip_id0, chip_id1;
	int calib_data, calib_offset, ret;

	ret = regmap_read(data->regmap_cfg, LS2K0300_SYSCON_CHIP_ID0, &chip_id0);
	if (ret)
		return ret;

	if (chip_id0 & LS2K0300_EXTERN_ID) {
		ret = regmap_read(data->regmap_cfg, LS2K0300_SYSCON_CHIP_ID1, &chip_id1);
		if (ret)
			return ret;

		/*
		 * External ID: the signed compensation value is stored in
		 * CHIP_ID1, with bit 15 as the sign and bits 14:0 as the
		 * magnitude.
		 */
		calib_data = FIELD_GET(LS2K0300_ID1_VAL_MASK, chip_id1);
		calib_offset = FIELD_GET(LS2K0300_COMP_VAL_MASK, calib_data);
		if (calib_data & LS2K0300_COMP_SIGN_BIT)
			calib_offset = -calib_offset;
	} else {
		/*
		 * Internal ID: the compensation value is unsigned and is
		 * stored in CHIP_ID0[31:20].
		 */
		calib_offset = FIELD_GET(LS2K0300_ID0_VAL_MASK, chip_id0);
	}

	data->calib_offset = calib_offset;
	return 0;
}

static int ls2k0300_thermal_hw_init(struct platform_device *pdev,
				    struct loongson2_thermal_data *data)
{
	struct device *dev = &pdev->dev;
	int ret;

	data->regmap_cfg = syscon_regmap_lookup_by_phandle(dev->of_node, "loongson,chipid");
	if (IS_ERR(data->regmap_cfg))
		return PTR_ERR(data->regmap_cfg);

	ret = ls2k0300_thermal_read_calib(data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read calibration data");

	writel(LS2K0300_THSENS_INIT_MASK, data->ctrl_reg + LS2K0300_THSENS_CFG_REG);

	ls2k0300_thermal_detect_old_fuse(dev, data);

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
	u32 sel = data->chip_data->thermal_sensor_sel;
	int ctrl_reg;
	u16 val;

	ctrl_reg = low ? LOONGSON2_THSENS_CTRL_LOW_REG : LOONGSON2_THSENS_CTRL_HI_REG;
	val = FIELD_PREP(LOONGSON2_THSENS_CTRL_GATE_MASK, temp + HECTO) |
	      FIELD_PREP(LOONGSON2_THSENS_CTRL_INTEN, enable) |
	      FIELD_PREP(LOONGSON2_THSENS_CTRL_SEL_MASK, sel);
	writew(val, data->ctrl_reg + ctrl_reg + sel * 2);
}

static int ls2k1000_thermal_hw_init(struct platform_device *pdev,
				    struct loongson2_thermal_data *data)
{
	writeb(LOONGSON2_THSENS_INT_EN, data->ctrl_reg + LOONGSON2_THSENS_STATUS_REG);
	loongson2_thermal_set_trip_temp(data, 0, 0, false);

	return 0;
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
	unsigned int sel = data->chip_data->thermal_sensor_sel;
	int hi_reg, ctrl_reg, val, gate;

	gate = (temp + LS2K2000_TEMP_OFFSET) * LS2K2000_TEMP_DIV / LS2K2000_TEMP_MUL;

	/* Low byte of the gate value goes to the control register */
	ctrl_reg = low ? LOONGSON2_THSENS_CTRL_LOW_REG : LOONGSON2_THSENS_CTRL_HI_REG;
	val = FIELD_PREP(LOONGSON2_THSENS_CTRL_GATE_MASK, gate) |
	      FIELD_PREP(LOONGSON2_THSENS_CTRL_INTEN, enable) |
	      FIELD_PREP(LOONGSON2_THSENS_CTRL_SEL_MASK, sel);
	writew(val, data->ctrl_reg + ctrl_reg + sel * 2);

	/* High byte of the gate value goes to the low byte of the HI_GATE register */
	hi_reg = low ? LS2K2000_THSENS_HI_GATE_LOW_REG : LS2K2000_THSENS_HI_GATE_HI_REG;
	val = FIELD_GET(GENMASK(15, 8), gate);
	writeb(val, data->ctrl_reg + hi_reg + sel);
}

static int ls2k2000_thermal_hw_init(struct platform_device *pdev,
				    struct loongson2_thermal_data *data)
{
	/* The temperature output register is separate for Loongson-2K2000 */
	data->extend_reg = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(data->extend_reg))
		return PTR_ERR(data->extend_reg);

	writeb(LOONGSON2_THSENS_INT_EN, data->ctrl_reg + LOONGSON2_THSENS_STATUS_REG);
	loongson2_thermal_set_trip_temp(data, 0, 0, false);

	return 0;
}

static const struct thermal_zone_device_ops ls2k0300_thermal_zone_ops = {
	.get_temp = ls2k0300_thermal_get_temp,
};

static const struct thermal_zone_device_ops ls2k1000_thermal_zone_ops = {
	.get_temp = ls2k1000_thermal_get_temp,
	.set_trips = loongson2_thermal_set_trips,
};

static const struct thermal_zone_device_ops ls2k2000_thermal_zone_ops = {
	.get_temp = ls2k2000_thermal_get_temp,
	.set_trips = loongson2_thermal_set_trips,
};

static const struct loongson2_thermal_chip_data ls2k0300_thermal_chip_data = {
	.thermal_sensor_sel = 0,
	.thermal_ops = &ls2k0300_thermal_zone_ops,
	.hw_init = ls2k0300_thermal_hw_init,
};

static const struct loongson2_thermal_chip_data ls2k1000_thermal_chip_data = {
	.thermal_sensor_sel = 0,
	.thermal_ops = &ls2k1000_thermal_zone_ops,
	.set_trips_regs = ls2k1000_thermal_set_trips_regs,
	.hw_init = ls2k1000_thermal_hw_init,
};

static const struct loongson2_thermal_chip_data ls2k2000_thermal_chip_data = {
	.thermal_sensor_sel = 0,
	.thermal_ops = &ls2k2000_thermal_zone_ops,
	.set_trips_regs = ls2k2000_thermal_set_trips_regs,
	.hw_init = ls2k2000_thermal_hw_init,
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
	int ret, irq;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->chip_data = device_get_match_data(dev);
	if (!data->chip_data)
		return -EINVAL;

	data->ctrl_reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(data->ctrl_reg))
		return PTR_ERR(data->ctrl_reg);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = data->chip_data->hw_init(pdev, data);
	if (ret)
		return ret;

	tzd = devm_thermal_of_zone_register(dev, 0, data, data->chip_data->thermal_ops);
	if (IS_ERR(tzd))
		return dev_err_probe(dev, PTR_ERR(tzd), "failed to register sensor");

	ret = devm_request_threaded_irq(dev, irq, NULL, loongson2_thermal_irq_thread,
					IRQF_ONESHOT, "loongson2_thermal", tzd);
	if (ret < 0)
		return ret;

	devm_thermal_add_hwmon_sysfs(dev, tzd);

	return 0;
}

static const struct of_device_id of_loongson2_thermal_match[] = {
	{
		.compatible = "loongson,ls2k0300-thermal",
		.data = &ls2k0300_thermal_chip_data,
	},
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
