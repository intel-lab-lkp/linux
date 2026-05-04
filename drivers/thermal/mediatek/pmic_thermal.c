// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Roman Vivchar <rva333@protonmail.com>
 *
 * Based on drivers/thermal/mediatek/auxadc_thermal.c
 */

#include <linux/err.h>
#include <linux/iio/consumer.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/thermal.h>

#include <linux/mfd/mt6323/registers.h>

#define MAX_SENSORS			1

#define MT6323_TEMP_MIN			-20000
#define MT6323_TEMP_MAX			150000

/* Layout of the fuses providing the calibration data */
#define CALIB_BUF0_VTS(x)		(((x) >> 8) & 0xff)
#define CALIB_BUF0_DEGC_CALI(x)		(((x) >> 2) & 0x3f)
#define CALIB_BUF0_ADC_CALI_EN(x)	(((x) >> 1) & 0x1)

#define CALIB_BUF1_ID_20(x)		(((x) >> 14) & 0x1)
#define CALIB_BUF1_ID_10(x)		(((x) >> 12) & 0x1)
#define CALIB_BUF1_O_SLOPE_20(x)	(((((x) >> 11) & 0x7) << 3) + (((x) >> 6) & 0x7))
#define CALIB_BUF1_O_SLOPE_10(x)	(((x) >> 6) & 0x3f)
#define CALIB_BUF1_O_SLOPE_SIGN(x)	(((x) >> 5) & 0x1)
#define CALIB_BUF1_VTS(x)		((((x) >> 0) & 0x1f) << 8)

#define MT6323_CALIBRATION		171

struct mtk_pmic_thermal;

struct mtk_thermal_data {
	const char *const *sensors;
	s32 num_sensors;
	const int cali_val;

	int (*extract_efuse)(struct mtk_pmic_thermal *mt, u16 *buf);
	void (*precalc)(struct mtk_pmic_thermal *mt, s32 vts, s32 degc_cali,
			s32 o_slope, s32 o_slope_sign);
};

struct mtk_pmic_sensor {
	struct mtk_pmic_thermal *mt;
	int id;
	struct iio_channel *adc_channel;
	struct thermal_zone_device *tzdev;
};

struct mtk_pmic_thermal {
	struct device *dev;
	struct regmap *regmap;
	struct mtk_pmic_sensor sensors[MAX_SENSORS];

	s32 t_slope1;
	s32 t_slope2;
	s32 t_intercept;

	const struct mtk_thermal_data *data;
};

static bool mtk_pmic_thermal_temp_is_valid(int temp)
{
	return (temp >= MT6323_TEMP_MIN) && (temp <= MT6323_TEMP_MAX);
}

static int mtk_pmic_read_temp(struct thermal_zone_device *tz, int *temperature)
{
	struct mtk_pmic_sensor *sensor = thermal_zone_device_priv(tz);
	int ret, raw, temp;

	ret = iio_read_channel_processed(sensor->adc_channel, &raw);
	if (ret < 0) {
		dev_err(sensor->mt->dev, "failed to read iio channel: %d\n",
			ret);
		return ret;
	}

	temp = sensor->mt->t_intercept +
	       ((sensor->mt->t_slope1 * raw) / sensor->mt->t_slope2);

	if (!mtk_pmic_thermal_temp_is_valid(temp))
		return -EINVAL;

	*temperature = temp;
	return 0;
}

static const struct thermal_zone_device_ops mtk_pmic_thermal_ops = {
	.get_temp = mtk_pmic_read_temp,
};

static void mtk_pmic_thermal_precalc_mt6323(struct mtk_pmic_thermal *mt,
					    s32 vts, s32 degc_cali, s32 o_slope,
					    s32 o_slope_sign)
{
	s32 vbe_t;

	mt->t_slope1 = 100 * 1000;

	if (o_slope_sign == 0)
		mt->t_slope2 = -(mt->data->cali_val + o_slope);
	else
		mt->t_slope2 = -(mt->data->cali_val - o_slope);

	vbe_t = -1 * (((vts + 9102) * 1800) / 32768) * 1000;

	if (o_slope_sign == 0)
		mt->t_intercept =
			(vbe_t * 100) / -(mt->data->cali_val + o_slope);
	else
		mt->t_intercept =
			(vbe_t * 100) / -(mt->data->cali_val - o_slope);

	mt->t_intercept += (degc_cali * (1000 / 2));
}

static int mtk_pmic_thermal_extract_efuse_mt6323(struct mtk_pmic_thermal *mt,
						 u16 *buf)
{
	u32 reg;
	s32 vts, degc_cali, o_slope, o_slope_sign, id;
	int ret;

	if (!CALIB_BUF0_ADC_CALI_EN(buf[0]))
		return -EINVAL;

	vts = CALIB_BUF1_VTS(buf[1]) + CALIB_BUF0_VTS(buf[0]);
	degc_cali = CALIB_BUF0_DEGC_CALI(buf[0]);
	o_slope_sign = CALIB_BUF1_O_SLOPE_SIGN(buf[1]);

	ret = regmap_read(mt->regmap, MT6323_CID, &reg);
	if (ret) {
		dev_err(mt->dev, "failed to read chip id\n");
		return ret;
	}

	if (reg == 0x1023) {
		o_slope = CALIB_BUF1_O_SLOPE_10(buf[1]);
		id = CALIB_BUF1_ID_10(buf[1]);
	} else if (reg == 0x2023) {
		o_slope = CALIB_BUF1_O_SLOPE_20(buf[1]);
		id = CALIB_BUF1_ID_20(buf[1]);
	} else {
		dev_err(mt->dev, "invalid chip id: 0x%x\n", reg);
		return -EINVAL;
	}

	if (id == 0)
		o_slope = 0;

	mt->data->precalc(mt, vts, degc_cali, o_slope, o_slope_sign);

	return 0;
}

static int mtk_pmic_thermal_get_calib_data(struct device *dev,
					   struct mtk_pmic_thermal *mt)
{
	struct nvmem_cell *cell;
	void *buf;
	size_t len;
	int ret;

	cell = nvmem_cell_get(dev, "calibration-data");
	if (IS_ERR(cell))
		return PTR_ERR(cell);

	buf = nvmem_cell_read(cell, &len);
	nvmem_cell_put(cell);

	if (IS_ERR(buf))
		return PTR_ERR(buf);

	if (len < 2 * sizeof(u16)) {
		dev_warn(dev, "invalid calibration data length\n");
		ret = -EINVAL;
		goto out;
	}

	ret = mt->data->extract_efuse(mt, buf);
	if (ret) {
		dev_info(dev, "device not calibrated, using default values\n");
		mt->data->precalc(mt, 3698, 50, 0, 0);
		ret = 0;
	}

out:
	kfree(buf);
	return ret;
}

static int mtk_pmic_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_pmic_thermal *mt;
	int i, ret;

	mt = devm_kzalloc(dev, sizeof(*mt), GFP_KERNEL);
	if (!mt)
		return -ENOMEM;

	mt->regmap = dev_get_regmap(dev->parent->parent, NULL);
	if (!mt->regmap)
		return dev_err_probe(dev, -ENODEV, "failed to get regmap");

	mt->dev = dev;
	mt->data = of_device_get_match_data(dev);

	ret = mtk_pmic_thermal_get_calib_data(dev, mt);
	if (ret)
		return ret;

	for (i = 0; i < mt->data->num_sensors; i++) {
		struct mtk_pmic_sensor *sensor = &mt->sensors[i];

		sensor->id = i;
		sensor->mt = mt;

		sensor->adc_channel =
			devm_iio_channel_get(dev, mt->data->sensors[i]);
		if (IS_ERR(sensor->adc_channel))
			return dev_err_probe(dev, PTR_ERR(sensor->adc_channel),
					     "failed to get channel %s\n",
					     mt->data->sensors[i]);

		sensor->tzdev = devm_thermal_of_zone_register(
			dev, i, sensor, &mtk_pmic_thermal_ops);
		if (IS_ERR(sensor->tzdev))
			return dev_err_probe(
				dev, PTR_ERR(sensor->tzdev),
				"failed to register thermal zone %d\n", i);
	}

	return 0;
}

static const char *const mt6323_adc_channels[] = { "vts" };

static const struct mtk_thermal_data mt6323_thermal_data = {
	.sensors = mt6323_adc_channels,
	.num_sensors = ARRAY_SIZE(mt6323_adc_channels),
	.cali_val = MT6323_CALIBRATION,
	.extract_efuse = mtk_pmic_thermal_extract_efuse_mt6323,
	.precalc = mtk_pmic_thermal_precalc_mt6323,
};

static const struct of_device_id mtk_pmic_thermal_of_match[] = {
	{ .compatible = "mediatek,mt6323-thermal",
	  .data = &mt6323_thermal_data },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mtk_pmic_thermal_of_match);

static struct platform_driver mtk_pmic_thermal_driver = {
	.probe = mtk_pmic_thermal_probe,
	.driver = {
		.name = "mtk-pmic-thermal",
		.of_match_table = mtk_pmic_thermal_of_match,
	},
};
module_platform_driver(mtk_pmic_thermal_driver);

MODULE_DESCRIPTION("MediaTek PMIC thermal driver");
MODULE_LICENSE("GPL");
