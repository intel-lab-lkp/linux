// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*
 * Copyright (c) 2025 Robert Bosch GmbH.
 */
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/units.h>

#include <linux/iio/buffer.h>
#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#include "smi330.h"

static struct iio_event_spec smi330_accel_events[] = {
	/* Any-Motion */
	{
		.type = IIO_EV_TYPE_ROC,
		.dir = IIO_EV_DIR_RISING,
		.mask_shared_by_all =
			BIT(IIO_EV_INFO_VALUE) | BIT(IIO_EV_INFO_PERIOD) |
			BIT(IIO_EV_INFO_HYSTERESIS) | BIT(IIO_EV_INFO_TIMEOUT),
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
	/* No-Motion */
	{
		.type = IIO_EV_TYPE_ROC,
		.dir = IIO_EV_DIR_FALLING,
		.mask_shared_by_all =
			BIT(IIO_EV_INFO_VALUE) | BIT(IIO_EV_INFO_PERIOD) |
			BIT(IIO_EV_INFO_HYSTERESIS) | BIT(IIO_EV_INFO_TIMEOUT),
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
	/* Tilt-Detection */
	{
		.type = IIO_EV_TYPE_CHANGE,
		.dir = IIO_EV_DIR_EITHER,
		.mask_shared_by_all = BIT(IIO_EV_INFO_VALUE) |
				      BIT(IIO_EV_INFO_PERIOD) |
				      BIT(IIO_EV_INFO_LOW_PASS_FILTER_3DB),
		.mask_shared_by_type = BIT(IIO_EV_INFO_ENABLE),
	},
};

// clang-format off
#define SMI330_ACCEL_CHANNEL(_type, _axis, _index) {			\
	.type = _type,							\
	.modified = 1,							\
	.channel2 = IIO_MOD_##_axis,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type =					\
		BIT(IIO_CHAN_INFO_ENABLE) |				\
		BIT(IIO_CHAN_INFO_SCALE) |				\
		BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO) |			\
		BIT(IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY),	\
	.info_mask_shared_by_type_available =				\
		BIT(IIO_CHAN_INFO_ENABLE) |				\
		BIT(IIO_CHAN_INFO_SCALE) |				\
		BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO) |			\
		BIT(IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY),	\
	.info_mask_shared_by_dir =					\
		BIT(IIO_CHAN_INFO_SAMP_FREQ),				\
	.info_mask_shared_by_dir_available =				\
		BIT(IIO_CHAN_INFO_SAMP_FREQ),				\
	.scan_index = _index,						\
	.scan_type = {							\
		.sign = 's',						\
		.realbits = 16,						\
		.storagebits = 16,					\
		.endianness = IIO_LE,					\
	},								\
	.event_spec = smi330_accel_events,				\
	.num_event_specs = ARRAY_SIZE(smi330_accel_events),		\
}

#define SMI330_GYRO_CHANNEL(_type, _axis, _index) {			\
	.type = _type,							\
	.modified = 1,							\
	.channel2 = IIO_MOD_##_axis,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type =					\
		BIT(IIO_CHAN_INFO_ENABLE) |				\
		BIT(IIO_CHAN_INFO_SCALE) |				\
		BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO) |			\
		BIT(IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY),	\
	.info_mask_shared_by_type_available =				\
		BIT(IIO_CHAN_INFO_ENABLE) |				\
		BIT(IIO_CHAN_INFO_SCALE) |				\
		BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO) |			\
		BIT(IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY),	\
	.info_mask_shared_by_dir =					\
		BIT(IIO_CHAN_INFO_SAMP_FREQ),				\
	.info_mask_shared_by_dir_available =				\
		BIT(IIO_CHAN_INFO_SAMP_FREQ),				\
	.scan_index = _index,						\
	.scan_type = {							\
		.sign = 's',						\
		.realbits = 16,						\
		.storagebits = 16,					\
		.endianness = IIO_LE,					\
	},								\
}

#define SMI330_TEMP_CHANNEL(_index) {			\
	.type = IIO_TEMP,				\
	.modified = 1,					\
	.channel2 = IIO_MOD_TEMP_OBJECT,		\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |	\
		BIT(IIO_CHAN_INFO_OFFSET) |		\
		BIT(IIO_CHAN_INFO_SCALE),		\
	.scan_index = _index,				\
	.scan_type = {					\
		.sign = 's',				\
		.realbits = 16,				\
		.storagebits = 16,			\
		.endianness = IIO_LE,			\
	},						\
}

// clang-format on

static const struct iio_chan_spec smi330_channels[] = {
	SMI330_ACCEL_CHANNEL(IIO_ACCEL, X, SMI330_SCAN_ACCEL_X),
	SMI330_ACCEL_CHANNEL(IIO_ACCEL, Y, SMI330_SCAN_ACCEL_Y),
	SMI330_ACCEL_CHANNEL(IIO_ACCEL, Z, SMI330_SCAN_ACCEL_Z),
	SMI330_GYRO_CHANNEL(IIO_ANGL_VEL, X, SMI330_SCAN_GYRO_X),
	SMI330_GYRO_CHANNEL(IIO_ANGL_VEL, Y, SMI330_SCAN_GYRO_Y),
	SMI330_GYRO_CHANNEL(IIO_ANGL_VEL, Z, SMI330_SCAN_GYRO_Z),
	SMI330_TEMP_CHANNEL(SMI330_TEMP_OBJECT),
	IIO_CHAN_SOFT_TIMESTAMP(SMI330_SCAN_TIMESTAMP),
};

static const struct smi330_sysfs_attr smi330_mode_attr = {
	.reg_vals = (int[]){ SMI330_MODE_SUSPEND, SMI330_MODE_LOW_POWER,
			     SMI330_MODE_NORMAL, SMI330_MODE_HIGH_PERF },
	.vals = (int[]){ 0, 3, 4, 7 },
	.len = 4,
	.type = IIO_VAL_INT
};

static const struct smi330_sysfs_attr smi330_mode_alt_gyro_attr = {
	.reg_vals = (int[]){ SMI330_MODE_SUSPEND, SMI330_MODE_GYRO_DRIVE,
			     SMI330_MODE_LOW_POWER, SMI330_MODE_NORMAL,
			     SMI330_MODE_HIGH_PERF },
	.vals = (int[]){ 0, 1, 3, 4, 7 },
	.len = 5,
	.type = IIO_VAL_INT
};

static const struct smi330_sysfs_attr smi330_accel_scale_attr = {
	.reg_vals = (int[]){ SMI330_ACCEL_RANGE_2G, SMI330_ACCEL_RANGE_4G,
			     SMI330_ACCEL_RANGE_8G, SMI330_ACCEL_RANGE_16G },
	.vals = (int[]){ 0, 61035, 0, 122070, 0, 244140, 0, 488281 },
	.len = 8,
	.type = IIO_VAL_INT_PLUS_NANO
};

static const struct smi330_sysfs_attr smi330_gyro_scale_attr = {
	.reg_vals = (int[]){ SMI330_GYRO_RANGE_125, SMI330_GYRO_RANGE_250,
			     SMI330_GYRO_RANGE_500 },
	.vals = (int[]){ 0, 3814697, 0, 7629395, 0, 15258789 },
	.len = 6,
	.type = IIO_VAL_INT_PLUS_NANO
};

static const struct smi330_sysfs_attr smi330_average_attr = {
	.reg_vals = (int[]){ SMI330_AVG_NUM_1, SMI330_AVG_NUM_2,
			     SMI330_AVG_NUM_4, SMI330_AVG_NUM_8,
			     SMI330_AVG_NUM_16, SMI330_AVG_NUM_32,
			     SMI330_AVG_NUM_64 },
	.vals = (int[]){ 1, 2, 4, 8, 16, 32, 64 },
	.len = 7,
	.type = IIO_VAL_INT
};

static const struct smi330_sysfs_attr smi330_bandwidth_attr = {
	.reg_vals = (int[]){ SMI330_BW_2, SMI330_BW_4 },
	.vals = (int[]){ 2, 4 },
	.len = 2,
	.type = IIO_VAL_INT
};

static const struct smi330_sysfs_attr smi330_odr_attr = {
	.reg_vals = (int[]){ SMI330_ODR_0_78125_HZ, SMI330_ODR_1_5625_HZ,
			     SMI330_ODR_3_125_HZ, SMI330_ODR_6_25_HZ,
			     SMI330_ODR_12_5_HZ, SMI330_ODR_25_HZ,
			     SMI330_ODR_50_HZ, SMI330_ODR_100_HZ,
			     SMI330_ODR_200_HZ, SMI330_ODR_400_HZ,
			     SMI330_ODR_800_HZ, SMI330_ODR_1600_HZ,
			     SMI330_ODR_3200_HZ, SMI330_ODR_6400_HZ },
	.vals = (int[]){ 0, 1, 3, 6, 12, 25, 50, 100, 200, 400, 800, 1600, 3200,
			 6400 },
	.len = 14,
	.type = IIO_VAL_INT
};

static int smi330_dev_init(struct smi330_data *data);

static int smi330_get_regs_dma(u8 reg_addr, int *reg_data,
			       struct smi330_data *data)
{
	int ret, status;

	ret = regmap_read(data->regmap, SMI330_FEATURE_DATA_STATUS_REG,
			  &status);
	if (ret)
		return ret;

	if (!FIELD_GET(SMI330_FEATURE_DATA_STATUS_TX_READY_MASK, status))
		return -EBUSY;

	ret = regmap_write(data->regmap, SMI330_FEATURE_DATA_ADDR_REG,
			   reg_addr);
	if (ret)
		return ret;

	return regmap_read(data->regmap, SMI330_FEATURE_DATA_TX_REG, reg_data);
}

static int smi330_set_regs_dma(u8 reg_addr, int reg_data,
			       struct smi330_data *data)
{
	int ret, status;

	ret = regmap_read(data->regmap, SMI330_FEATURE_DATA_STATUS_REG,
			  &status);
	if (ret)
		return ret;

	if (!FIELD_GET(SMI330_FEATURE_DATA_STATUS_TX_READY_MASK, status))
		return -EBUSY;

	ret = regmap_write(data->regmap, SMI330_FEATURE_DATA_ADDR_REG,
			   reg_addr);
	if (ret)
		return ret;

	return regmap_write(data->regmap, SMI330_FEATURE_DATA_TX_REG, reg_data);
}

static int smi330_get_sysfs_attr(enum smi330_sensor_conf_select config,
				 enum smi330_sensor sensor,
				 const struct smi330_sysfs_attr **attr)
{
	switch (config) {
	case SMI330_ODR:
		*attr = &smi330_odr_attr;
		return 0;
	case SMI330_RANGE:
		if (sensor == SMI330_ACCEL)
			*attr = &smi330_accel_scale_attr;
		else
			*attr = &smi330_gyro_scale_attr;
		return 0;
	case SMI330_BW:
		*attr = &smi330_bandwidth_attr;
		return 0;
	case SMI330_AVG_NUM:
		*attr = &smi330_average_attr;
		return 0;
	case SMI330_MODE:
		if (sensor == SMI330_ALT_GYRO)
			*attr = &smi330_mode_alt_gyro_attr;
		else
			*attr = &smi330_mode_attr;
		return 0;
	default:
		return -EINVAL;
	}
}

static int smi330_get_odr_ns(enum smi330_odr odr, s64 *odr_ns)
{
	switch (odr) {
	case SMI330_ODR_0_78125_HZ:
		*odr_ns = 1280000000;
		break;
	case SMI330_ODR_1_5625_HZ:
		*odr_ns = 640000000;
		break;
	case SMI330_ODR_3_125_HZ:
		*odr_ns = 320000000;
		break;
	case SMI330_ODR_6_25_HZ:
		*odr_ns = 160000000;
		break;
	case SMI330_ODR_12_5_HZ:
		*odr_ns = 80000000;
		break;
	case SMI330_ODR_25_HZ:
		*odr_ns = 40000000;
		break;
	case SMI330_ODR_50_HZ:
		*odr_ns = 20000000;
		break;
	case SMI330_ODR_100_HZ:
		*odr_ns = 10000000;
		break;
	case SMI330_ODR_200_HZ:
		*odr_ns = 5000000;
		break;
	case SMI330_ODR_400_HZ:
		*odr_ns = 2500000;
		break;
	case SMI330_ODR_800_HZ:
		*odr_ns = 1250000;
		break;
	case SMI330_ODR_1600_HZ:
		*odr_ns = 625000;
		break;
	case SMI330_ODR_3200_HZ:
		*odr_ns = 312500;
		break;
	case SMI330_ODR_6400_HZ:
		*odr_ns = 156250;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int smi330_get_avg_num(enum smi330_avg_num avg, int *avg_num)
{
	int i;

	for (i = 0; i < smi330_average_attr.len; i++) {
		if (smi330_average_attr.reg_vals[i] == avg) {
			*avg_num = smi330_average_attr.vals[i];
			return 0;
		}
	}

	return -EINVAL;
}

static int smi330_get_config_reg(enum smi330_sensor sensor, int *reg)
{
	switch (sensor) {
	case SMI330_ACCEL:
		*reg = SMI330_ACCEL_CFG_REG;
		return 0;
	case SMI330_GYRO:
		*reg = SMI330_GYRO_CFG_REG;
		return 0;
	case SMI330_ALT_ACCEL:
		*reg = SMI330_ALT_ACCEL_CFG_REG;
		return 0;
	case SMI330_ALT_GYRO:
		*reg = SMI330_ALT_GYRO_CFG_REG;
		return 0;
	default:
		return -EINVAL;
	}
}

static int smi330_validate_sensor_config(enum smi330_mode mode,
					 enum smi330_odr odr,
					 enum smi330_avg_num avg)
{
	int ret, avg_num;
	s64 odr_ns, min_odr_ns, skipped_samples;

	switch (mode) {
	case SMI330_MODE_LOW_POWER:
		if (odr > SMI330_ODR_400_HZ)
			return -EINVAL;

		ret = smi330_get_odr_ns(SMI330_ODR_6400_HZ, &min_odr_ns);
		if (ret)
			return ret;

		ret = smi330_get_odr_ns(odr, &odr_ns);
		if (ret)
			return ret;

		ret = smi330_get_avg_num(avg, &avg_num);
		if (ret)
			return ret;

		skipped_samples = div_s64(odr_ns, min_odr_ns) - avg_num;

		if (skipped_samples <= 0)
			return -EINVAL;
		return 0;

	case SMI330_MODE_NORMAL:
	case SMI330_MODE_HIGH_PERF:
		if (odr <= SMI330_ODR_6_25_HZ)
			return -EINVAL;
		return 0;

	default:
		return 0;
	}
}

static int smi330_get_sensor_config_reg(struct smi330_data *data,
					enum smi330_sensor sensor, int *cfg)
{
	int ret, reg;

	ret = smi330_get_config_reg(sensor, &reg);
	if (ret)
		return ret;

	return regmap_read(data->regmap, reg, cfg);
}

static int smi330_get_sensor_config(struct smi330_data *data,
				    enum smi330_sensor sensor,
				    enum smi330_sensor_conf_select config,
				    int *value)

{
	int ret, reg_val, i;
	const struct smi330_sysfs_attr *attr;

	ret = smi330_get_sensor_config_reg(data, sensor, &reg_val);
	if (ret)
		return ret;

	switch (config) {
	case SMI330_ODR:
		reg_val = FIELD_GET(SMI330_CFG_ODR_MASK, reg_val);
		break;
	case SMI330_RANGE:
		reg_val = FIELD_GET(SMI330_CFG_RANGE_MASK, reg_val);
		break;
	case SMI330_BW:
		reg_val = FIELD_GET(SMI330_CFG_BW_MASK, reg_val);
		break;
	case SMI330_AVG_NUM:
		reg_val = FIELD_GET(SMI330_CFG_AVG_NUM_MASK, reg_val);
		break;
	case SMI330_MODE:
		reg_val = FIELD_GET(SMI330_CFG_MODE_MASK, reg_val);
		break;
	default:
		return -EINVAL;
	}

	ret = smi330_get_sysfs_attr(config, sensor, &attr);
	if (ret)
		return ret;

	if (attr->type == IIO_VAL_INT) {
		for (i = 0; i < attr->len; i++) {
			if (attr->reg_vals[i] == reg_val) {
				*value = attr->vals[i];
				return 0;
			}
		}
	} else {
		for (i = 0; i < attr->len / 2; i++) {
			if (attr->reg_vals[i] == reg_val) {
				*value = attr->vals[2 * i + 1];
				return 0;
			}
		}
	}

	return -EINVAL;
}

static int smi330_set_sensor_config_reg(struct smi330_data *data,
					enum smi330_sensor sensor, int mask,
					int val)
{
	int ret, reg, cfg, error;
	enum smi330_mode mode;
	enum smi330_odr odr;
	enum smi330_avg_num avg;
	struct device *dev = regmap_get_device(data->regmap);

	ret = smi330_get_config_reg(sensor, &reg);
	if (ret)
		return ret;

	ret = regmap_read(data->regmap, reg, &cfg);
	if (ret)
		return ret;

	cfg = (val & mask) | (cfg & ~mask);

	mode = FIELD_GET(SMI330_CFG_MODE_MASK, cfg);
	odr = FIELD_GET(SMI330_CFG_ODR_MASK, cfg);
	avg = FIELD_GET(SMI330_CFG_AVG_NUM_MASK, cfg);

	ret = smi330_validate_sensor_config(mode, odr, avg);
	if (ret) {
		dev_err(dev, "Invalid sensor configuration\n");
		return ret;
	}

	ret = regmap_write(data->regmap, reg, cfg);
	if (ret)
		return ret;

	ret = regmap_read(data->regmap, SMI330_ERR_REG, &error);
	if (ret)
		return ret;

	if (FIELD_GET(SMI330_ERR_ACC_CONF_MASK, error) ||
	    FIELD_GET(SMI330_ERR_GYR_CONF_MASK, error))
		return -EIO;

	return smi330_get_odr_ns(odr, &data->cfg.odr_ns);
}

static int smi330_set_sensor_config(struct smi330_data *data,
				    enum smi330_sensor sensor,
				    enum smi330_sensor_conf_select config,
				    int value, bool is_reg_value)
{
	int ret, i, reg_value, mask;
	const struct smi330_sysfs_attr *attr;

	if (is_reg_value) {
		reg_value = value;
	} else {
		ret = smi330_get_sysfs_attr(config, sensor, &attr);
		if (ret)
			return ret;

		ret = -EINVAL;
		for (i = 0; i < attr->len; i++) {
			if (attr->vals[i] == value) {
				if (attr->type == IIO_VAL_INT)
					reg_value = attr->reg_vals[i];
				else
					reg_value = attr->reg_vals[i / 2];
				ret = 0;
			}
		}
		if (ret)
			return ret;
	}

	switch (config) {
	case SMI330_ODR:
		mask = SMI330_CFG_ODR_MASK;
		reg_value = FIELD_PREP(mask, reg_value);
		break;
	case SMI330_RANGE:
		mask = SMI330_CFG_RANGE_MASK;
		reg_value = FIELD_PREP(mask, reg_value);
		break;
	case SMI330_BW:
		mask = SMI330_CFG_BW_MASK;
		reg_value = FIELD_PREP(mask, reg_value);
		break;
	case SMI330_AVG_NUM:
		mask = SMI330_CFG_AVG_NUM_MASK;
		reg_value = FIELD_PREP(mask, reg_value);
		break;
	case SMI330_MODE:
		mask = SMI330_CFG_MODE_MASK;
		reg_value = FIELD_PREP(mask, reg_value);
		break;
	default:
		return -EINVAL;
	}

	return smi330_set_sensor_config_reg(data, sensor, mask, reg_value);
}

static int smi330_get_data(struct smi330_data *data, int chan_type, int axis,
			   int *val)
{
	u8 reg;
	int ret, sample;

	switch (chan_type) {
	case IIO_ACCEL:
		reg = SMI330_ACCEL_X_REG + (axis - IIO_MOD_X);
		break;
	case IIO_ANGL_VEL:
		reg = SMI330_GYRO_X_REG + (axis - IIO_MOD_X);
		break;
	case IIO_TEMP:
		reg = SMI330_TEMP_REG;
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_read(data->regmap, reg, &sample);
	if (ret)
		return ret;

	*val = sign_extend32(sample, 15);

	return 0;
}

static int smi330_read_avail(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan, const int **vals,
			     int *type, int *length, long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_ENABLE:
		*vals = smi330_mode_attr.vals;
		*length = smi330_mode_attr.len;
		*type = smi330_mode_attr.type;
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_SCALE:
		if (chan->type == IIO_ACCEL) {
			*vals = smi330_accel_scale_attr.vals;
			*length = smi330_accel_scale_attr.len;
			*type = smi330_accel_scale_attr.type;
		} else {
			*vals = smi330_gyro_scale_attr.vals;
			*length = smi330_gyro_scale_attr.len;
			*type = smi330_gyro_scale_attr.type;
		}
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		*vals = smi330_average_attr.vals;
		*length = smi330_average_attr.len;
		*type = smi330_average_attr.type;
		*type = IIO_VAL_INT;
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY:
		*vals = smi330_bandwidth_attr.vals;
		*length = smi330_bandwidth_attr.len;
		*type = smi330_bandwidth_attr.type;
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*vals = smi330_odr_attr.vals;
		*length = smi330_odr_attr.len;
		*type = smi330_odr_attr.type;
		return IIO_AVAIL_LIST;
	default:
		return -EINVAL;
	}

	return -EINVAL;
}

static int smi330_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan, int *val,
			   int *val2, long mask)
{
	int ret;
	struct smi330_data *data = iio_priv(indio_dev);
	enum smi330_sensor sensor;

	/* valid for all channel types */
	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = smi330_get_data(data, chan->type, chan->channel2, val);
		iio_device_release_direct(indio_dev);
		return ret ? ret : IIO_VAL_INT;
	default:
		break;
	}

	switch (chan->type) {
	case IIO_ACCEL:
		sensor = SMI330_ACCEL;
		break;
	case IIO_ANGL_VEL:
		sensor = SMI330_GYRO;
		break;
	case IIO_TEMP:
		switch (mask) {
		case IIO_CHAN_INFO_SCALE:
			*val = SMI330_TEMP_SCALE / GIGA;
			*val2 = SMI330_TEMP_SCALE % GIGA;
			return IIO_VAL_INT_PLUS_NANO;
		case IIO_CHAN_INFO_OFFSET:
			*val = SMI330_TEMP_OFFSET;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}

	/* valid for acc and gyro channels */
	switch (mask) {
	case IIO_CHAN_INFO_ENABLE:
		ret = smi330_get_sensor_config(data, sensor, SMI330_MODE, val);
		return ret ? ret : IIO_VAL_INT;

	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		ret = smi330_get_sensor_config(data, sensor, SMI330_AVG_NUM,
					       val);
		return ret ? ret : IIO_VAL_INT;

	case IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY:
		ret = smi330_get_sensor_config(data, sensor, SMI330_BW, val);
		return ret ? ret : IIO_VAL_INT;

	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = smi330_get_sensor_config(data, sensor, SMI330_ODR, val);
		return ret ? ret : IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		ret = smi330_get_sensor_config(data, sensor, SMI330_RANGE,
					       val2);
		return ret ? ret : IIO_VAL_INT_PLUS_NANO;

	default:
		return -EINVAL;
	}
}

static int smi330_write_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int val, int val2,
			    long mask)
{
	int ret;
	struct smi330_data *data = iio_priv(indio_dev);
	enum smi330_sensor sensor;

	switch (chan->type) {
	case IIO_ACCEL:
		sensor = SMI330_ACCEL;
		break;
	case IIO_ANGL_VEL:
		sensor = SMI330_GYRO;
		break;
	default:
		return -EINVAL;
	}

	switch (mask) {
	case IIO_CHAN_INFO_ENABLE:
		ret = smi330_set_sensor_config(data, sensor, SMI330_MODE, val,
					       false);
		if (ret)
			return ret;

		if (sensor == SMI330_ACCEL)
			data->savestate.acc_pwr = val;
		else
			data->savestate.gyr_pwr = val;
		return 0;
	case IIO_CHAN_INFO_SCALE:
		return smi330_set_sensor_config(data, sensor, SMI330_RANGE,
						val2, false);
	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		return smi330_set_sensor_config(data, sensor, SMI330_AVG_NUM,
						val, false);
	case IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY:
		return smi330_set_sensor_config(data, sensor, SMI330_BW, val,
						false);
	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = smi330_set_sensor_config(data, SMI330_ACCEL, SMI330_ODR,
					       val, false);
		if (ret)
			return ret;

		return smi330_set_sensor_config(data, SMI330_GYRO, SMI330_ODR,
						val, false);
	default:
		return -EINVAL;
	}
}

static int smi330_write_raw_get_fmt(struct iio_dev *indio_dev,
				    struct iio_chan_spec const *chan, long info)
{
	switch (info) {
	case IIO_CHAN_INFO_SCALE:
		return IIO_VAL_INT_PLUS_NANO;
	default:
		return IIO_VAL_INT_PLUS_MICRO;
	}
}

static u16 smi330_get_fifo_length(struct smi330_data *data)
{
	int ret, fill_level;

	ret = regmap_read(data->regmap, SMI330_FIFO_FILL_LEVEL_REG,
			  &fill_level);
	if (ret)
		return 0;

	return fill_level & SMI330_FIFO_FILL_LEVEL_MASK;
}

static int smi330_get_fifo_frame_length(struct iio_dev *indio_dev)
{
	int chan;
	int fifo_frame_length = 0;

	iio_for_each_active_channel(indio_dev, chan) {
		fifo_frame_length++;
	}

	return fifo_frame_length;
}

static int smi330_config_auto_op_mode(struct smi330_data *data,
				      enum smi330_auto_op_mode auto_op_mode)
{
	switch (auto_op_mode) {
	case SMI330_AUTO_OP_EN_ALL:
		return regmap_update_bits(data->regmap, SMI330_ALT_CONF_REG,
					  SMI330_ALT_CONF_EN_MASK,
					  SMI330_ALT_CONF_EN_MASK);
	case SMI330_AUTO_OP_EN_ACC:
		return regmap_update_bits(data->regmap, SMI330_ALT_CONF_REG,
					  SMI330_ALT_CONF_EN_MASK,
					  SMI330_ALT_CONF_ACC_EN_MASK);
	case SMI330_AUTO_OP_EN_GYR:
		return regmap_update_bits(data->regmap, SMI330_ALT_CONF_REG,
					  SMI330_ALT_CONF_EN_MASK,
					  SMI330_ALT_CONF_GYR_EN_MASK);
	case SMI330_AUTO_OP_DISABLE:
		return regmap_update_bits(data->regmap, SMI330_ALT_CONF_REG,
					  SMI330_ALT_CONF_EN_MASK, 0);
	default:
		return -EINVAL;
	}
}

static int smi330_auto_op_cond(struct smi330_data *data,
			       enum smi330_auto_op_config auto_op_config,
			       enum smi330_auto_op_adv_feat auto_op_adv_feat)
{
	int ret, val, mask;

	ret = smi330_get_regs_dma(SMI330_ALT_CONF_CHG_EX_REG, &val, data);
	if (ret)
		return ret;

	switch (auto_op_config) {
	case SMI330_AUTO_OP_CONFIG_ALT:
		if (FIELD_GET(SMI330_ALT_CONF_CHG_USER_MASK, val) !=
		    auto_op_adv_feat) {
			mask = SMI330_ALT_CONF_CHG_ALT_MASK;
			break;
		}
		return -EIO;
	case SMI330_AUTO_OP_CONFIG_USER:
		if (FIELD_GET(SMI330_ALT_CONF_CHG_ALT_MASK, val) !=
		    auto_op_adv_feat) {
			mask = SMI330_ALT_CONF_CHG_USER_MASK;
			break;
		}
		return -EIO;
	default:
		return -EINVAL;
	}

	/* FIELD_PREP is not possible with non-const mask */
	val = ((auto_op_adv_feat << (__builtin_ffs(mask) - 1)) & mask) |
	      (val & ~mask);

	return smi330_set_regs_dma(SMI330_ALT_CONF_CHG_EX_REG, val, data);
}

static int smi330_get_st_result(struct smi330_data *data)
{
	int ret, st_result, io1_data;

	ret = regmap_read_poll_timeout(
		data->regmap, SMI330_FEATURE_IO1_REG, io1_data,
		FIELD_GET(SMI330_FEATURE_IO1_SC_COMPLETE_MASK, io1_data) == 1,
		SMI330_FEAT_ENG_POLL, SMI330_FEAT_ENG_TIMEOUT);
	if (ret)
		return ret;

	if (FIELD_GET(SMI330_FEATURE_IO1_ST_RESULT_MASK, io1_data)) {
		ret = regmap_write(data->regmap, SMI330_FEATURE_DATA_ADDR_REG,
				   SMI330_ST_RESULT_EX_REG);
		if (ret)
			return ret;

		ret = regmap_read(data->regmap, SMI330_FEATURE_DATA_TX_REG,
				  &st_result);
		if (ret)
			return ret;

		if (st_result != SMI330_ST_SUCCESS_MASK)
			return -EIO;
	} else {
		return -EIO;
	}

	return 0;
}

static int smi330_disable_alt_conf_acc_gyr_mode(struct smi330_data *data)
{
	int ret;

	ret = smi330_set_sensor_config(data, SMI330_ALT_ACCEL, SMI330_MODE,
				       SMI330_MODE_SUSPEND, true);
	if (ret)
		return ret;

	return smi330_set_sensor_config(data, SMI330_ALT_GYRO, SMI330_MODE,
					SMI330_MODE_SUSPEND, true);
}

static int smi330_st_precondition(struct smi330_data *data)
{
	int mask, acc_cfg;

	mask = SMI330_CFG_MODE_MASK | SMI330_CFG_ODR_MASK;

	acc_cfg = FIELD_PREP(SMI330_CFG_MODE_MASK, SMI330_MODE_NORMAL) |
		  FIELD_PREP(SMI330_CFG_ODR_MASK, SMI330_ODR_100_HZ);

	return smi330_set_sensor_config_reg(data, SMI330_ACCEL, mask, acc_cfg);
}

static int smi330_trigger_self_test(struct smi330_data *data)
{
	int ret;

	ret = smi330_st_precondition(data);
	if (ret)
		return ret;

	ret = smi330_disable_alt_conf_acc_gyr_mode(data);
	if (ret)
		return ret;

	return regmap_write(data->regmap, SMI330_CMD_REG, SMI330_CMD_SELF_TEST);
}

static int smi330_set_gyro_filter_coefficients(struct smi330_data *data)
{
	int ret;
	s16 data_array[9];

	data_array[0] = SMI330_SC_ST_VALUE_0;
	data_array[1] = SMI330_SC_ST_VALUE_1;
	data_array[2] = SMI330_SC_ST_VALUE_2;
	data_array[3] = SMI330_SC_ST_VALUE_3;
	data_array[4] = SMI330_SC_ST_VALUE_4;
	data_array[5] = SMI330_SC_ST_VALUE_5;
	data_array[6] = SMI330_SC_ST_VALUE_6;
	data_array[7] = SMI330_SC_ST_VALUE_7;
	data_array[8] = SMI330_SC_ST_VALUE_8;

	ret = regmap_write(data->regmap, SMI330_FEATURE_DATA_ADDR_REG,
			   SMI330_GYRO_SC_ST_VALUES_EX_REG);
	if (ret)
		return ret;

	return regmap_bulk_write(data->regmap, SMI330_FEATURE_DATA_TX_REG,
				 data_array, ARRAY_SIZE(data_array));
}

static int smi330_set_self_test_mode(struct smi330_data *data)
{
	int ret;

	ret = regmap_write(data->regmap, SMI330_FEATURE_DATA_ADDR_REG,
			   SMI330_ST_SELECT_EX_REG);
	if (ret)
		return ret;

	return regmap_write(data->regmap, SMI330_FEATURE_DATA_TX_REG,
			    SMI330_ST_SELECT_ACC_GYR_MASK);
}

static int smi330_perform_self_test(struct smi330_data *data)
{
	int ret, acc_cfg, cfg_restore;
	s16 data_array[9] = { 0 };

	ret = regmap_write(data->regmap, SMI330_FEATURE_DATA_ADDR_REG,
			   SMI330_GYRO_SC_ST_VALUES_EX_REG);
	if (ret == 0)
		ret = regmap_bulk_write(data->regmap,
					SMI330_FEATURE_DATA_TX_REG, data_array,
					ARRAY_SIZE(data_array));
	if (ret == 0)
		ret = regmap_write(data->regmap, SMI330_FEATURE_DATA_ADDR_REG,
				   SMI330_GYRO_SC_ST_VALUES_EX_REG);
	if (ret == 0)
		ret = regmap_bulk_read(data->regmap, SMI330_FEATURE_DATA_TX_REG,
				       data_array, ARRAY_SIZE(data_array));

	if (ret == 0 && (data_array[3] != SMI330_SC_ST_VALUE_3 &&
			 (data_array[0] != SMI330_SC_ST_VALUE_0 ||
			  data_array[1] != SMI330_SC_ST_VALUE_1 ||
			  data_array[2] != SMI330_SC_ST_VALUE_2 ||
			  data_array[4] != SMI330_SC_ST_VALUE_4 ||
			  data_array[5] != SMI330_SC_ST_VALUE_5 ||
			  data_array[6] != SMI330_SC_ST_VALUE_6 ||
			  data_array[7] != SMI330_SC_ST_VALUE_7 ||
			  data_array[8] != SMI330_SC_ST_VALUE_8))) {
		ret = smi330_set_gyro_filter_coefficients(data);
	}

	if (ret == 0)
		ret = smi330_set_self_test_mode(data);

	/*
	 * Save accel configurations in separate variable to
	 * restore configuration independent from other errors.
	 */
	cfg_restore =
		smi330_get_sensor_config_reg(data, SMI330_ACCEL, &acc_cfg);
	ret = cfg_restore;

	if (ret == 0)
		ret = smi330_trigger_self_test(data);

	if (ret == 0)
		ret = smi330_get_st_result(data);

	if (cfg_restore == 0)
		/* Restore accel configurations */
		ret = smi330_set_sensor_config_reg(data, SMI330_ACCEL,
						   SMI330_CFG_MASK, acc_cfg);

	return ret;
}

static int smi330_self_calib_select(struct smi330_data *data)
{
	int ret;

	ret = regmap_write(data->regmap, SMI330_FEATURE_DATA_ADDR_REG,
			   SMI330_GYRO_SC_SELECT_EX_REG);
	if (ret)
		return ret;

	return regmap_write(data->regmap, SMI330_FEATURE_DATA_TX_REG,
			    SMI330_GYRO_SC_SELECT_ALL_MASK);
}

static int smi330_self_calib_preconfig(struct smi330_data *data)
{
	int ret, acc_cfg;
	int mask = SMI330_CFG_MODE_MASK | SMI330_CFG_ODR_MASK;

	acc_cfg = FIELD_PREP(SMI330_CFG_MODE_MASK, SMI330_MODE_HIGH_PERF) |
		  FIELD_PREP(SMI330_CFG_ODR_MASK, SMI330_ODR_100_HZ);

	ret = smi330_set_sensor_config_reg(data, SMI330_ACCEL, mask, acc_cfg);
	if (ret)
		return ret;

	ret = smi330_set_sensor_config(data, SMI330_ALT_ACCEL, SMI330_MODE,
				       SMI330_MODE_SUSPEND, true);
	if (ret)
		return ret;

	return smi330_set_sensor_config(data, SMI330_ALT_GYRO, SMI330_MODE,
					SMI330_MODE_SUSPEND, true);
}

static void smi330_self_calib_restore_config(struct smi330_data *data,
					     int accel_config,
					     int accel_alt_config,
					     int gyro_alt_config)
{
	smi330_set_sensor_config_reg(data, SMI330_ACCEL, SMI330_CFG_MASK,
				     accel_config);

	smi330_set_sensor_config_reg(data, SMI330_ALT_ACCEL, SMI330_CFG_MASK,
				     accel_alt_config);

	smi330_set_sensor_config_reg(data, SMI330_ALT_GYRO, SMI330_CFG_MASK,
				     gyro_alt_config);
}

static int smi330_self_calibration(struct smi330_data *data,
				   struct iio_dev *indio_dev)
{
	int ret, io1_data, acc_cfg, acc_alt_cfg, gyr_alt_cfg;

	ret = regmap_read(data->regmap, SMI330_FEATURE_IO1_REG, &io1_data);
	if (ret)
		return ret;

	if (FIELD_GET(SMI330_FEATURE_IO1_STATE_MASK, io1_data))
		return -EBUSY;

	ret = smi330_self_calib_select(data);
	if (ret)
		return ret;

	ret = smi330_get_sensor_config_reg(data, SMI330_ACCEL, &acc_cfg);
	if (ret)
		return ret;

	ret = smi330_get_sensor_config_reg(data, SMI330_ALT_ACCEL,
					   &acc_alt_cfg);
	if (ret)
		return ret;

	ret = smi330_get_sensor_config_reg(data, SMI330_ALT_GYRO, &gyr_alt_cfg);
	if (ret)
		return ret;

	ret = smi330_self_calib_preconfig(data);
	if (ret)
		goto out;

	ret = regmap_write(data->regmap, SMI330_CMD_REG,
			   SMI330_CMD_SELF_CALIBRATION);
	if (ret)
		goto out;

	ret = regmap_read_poll_timeout(
		data->regmap, SMI330_FEATURE_IO1_REG, io1_data,
		FIELD_GET(SMI330_FEATURE_IO1_SC_COMPLETE_MASK, io1_data) == 1,
		SMI330_FEAT_ENG_POLL, SMI330_FEAT_ENG_TIMEOUT);

out:
	smi330_self_calib_restore_config(data, acc_cfg, acc_alt_cfg,
					 gyr_alt_cfg);
	return ret;
}

static int smi330_enable_feature_engine(struct smi330_data *data)
{
	int ret, io1;

	ret = regmap_write(data->regmap, SMI330_FEATURE_IO2_REG,
			   SMI330_FEATURE_IO2_STARTUP_CONFIG);
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, SMI330_FEATURE_IO_STATUS_REG,
			   SMI330_FEATURE_IO_STATUS_MASK);
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, SMI330_FEATURE_CTRL_REG,
			   SMI330_FEATURE_CTRL_ENABLE);
	if (ret)
		return ret;

	return regmap_read_poll_timeout(data->regmap, SMI330_FEATURE_IO1_REG,
					io1,
					FIELD_GET(SMI330_FEATURE_IO1_ERROR_MASK,
						  io1) == 1,
					SMI330_FEAT_ENG_POLL,
					SMI330_FEAT_ENG_TIMEOUT);
}

static int smi330_enable_adv_feat(struct smi330_data *data, int mask, int val)
{
	int ret, io_status;

	/* update advanced feature config */
	ret = regmap_update_bits(data->regmap, SMI330_FEATURE_IO0_REG, mask,
				 val);
	if (ret)
		return ret;

	/* sync settings */
	io_status = FIELD_PREP(SMI330_FEATURE_IO_STATUS_MASK, 1);
	ret = regmap_write(data->regmap, SMI330_FEATURE_IO_STATUS_REG,
			   io_status);
	return ret;
}

static int smi330_soft_reset(struct smi330_data *data)
{
	int ret, dummy_byte;

	ret = regmap_write(data->regmap, SMI330_CMD_REG, SMI330_CMD_SOFT_RESET);
	if (ret)
		return ret;
	fsleep(SMI330_SOFT_RESET_DELAY);

	/* Performing a dummy read after a soft-reset */
	regmap_read(data->regmap, SMI330_CHIP_ID_REG, &dummy_byte);
	if (ret)
		return ret;

	if (data->cfg.feat_irq != SMI330_INT_DISABLED)
		ret = smi330_enable_feature_engine(data);

	return ret;
}

static int smi330_fifo_handler(struct iio_dev *indio_dev)
{
	int ret, index, chan, i, fifo_frame_length, fifo_length, frame_count;
	struct smi330_data *data = iio_priv(indio_dev);
	s16 *iio_buffer_iter = data->buf;
	s16 *fifo_iter = data->fifo;
	s64 tsamp = data->cfg.odr_ns;
	s64 timestamp;

	/* Ignore if buffer disabled */
	if (!iio_buffer_enabled(indio_dev))
		return 0;

	fifo_frame_length = smi330_get_fifo_frame_length(indio_dev);
	fifo_length = smi330_get_fifo_length(data);
	frame_count = fifo_length / fifo_frame_length;

	if (data->last_timestamp != 0 && frame_count != 0)
		tsamp = div_s64(data->current_timestamp - data->last_timestamp,
				frame_count);

	ret = regmap_noinc_read(data->regmap, SMI330_FIFO_DATA_REG, data->fifo,
				fifo_length * sizeof(s16));
	if (ret)
		return ret;

	for (i = 0; i < frame_count; i++) {
		index = 0;
		fifo_iter = &data->fifo[i * fifo_frame_length];

		iio_for_each_active_channel(indio_dev, chan) {
			iio_buffer_iter[index++] = fifo_iter[chan];
		}
		timestamp =
			data->current_timestamp - tsamp * (frame_count - i - 1);
		iio_push_to_buffers_with_timestamp(indio_dev, data->buf,
						   timestamp);
	}

	data->last_timestamp = data->current_timestamp;

	return 0;
}

static irqreturn_t smi330_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct smi330_data *data = iio_priv(indio_dev);
	struct device *dev = regmap_get_device(data->regmap);
	int ret, sample, chan;
	int i = 0;

	/* Ignore if buffer disabled */
	if (!iio_buffer_enabled(indio_dev))
		return 0;

	if (data->cfg.op_mode == SMI330_FIFO) {
		dev_warn(dev, "Can't use trigger when FIFO enabled\n");
		return -EBUSY;
	}

	if (data->cfg.op_mode == SMI330_IDLE)
		data->current_timestamp = iio_get_time_ns(indio_dev);

	if (*indio_dev->active_scan_mask == SMI330_ALL_CHAN_MSK) {
		ret = regmap_bulk_read(data->regmap, SMI330_ACCEL_X_REG,
				       data->buf, ARRAY_SIZE(smi330_channels));
		if (ret)
			goto out;
	} else {
		iio_for_each_active_channel(indio_dev, chan) {
			ret = regmap_read(data->regmap,
					  SMI330_ACCEL_X_REG + chan, &sample);
			if (ret)
				goto out;
			data->buf[i++] = sample;
		}
	}

	iio_push_to_buffers_with_timestamp(indio_dev, data->buf,
					   data->current_timestamp);

out:
	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static irqreturn_t smi330_irq_thread_handler(int irq, void *indio_dev_)
{
	int ret, int_stat;
	s16 int_status[2] = { 0 };
	struct iio_dev *indio_dev = indio_dev_;
	struct smi330_data *data = iio_priv(indio_dev);

	guard(mutex)(&data->lock);

	data->current_timestamp = atomic64_read(&data->irq_timestamp);

	ret = regmap_bulk_read(data->regmap, SMI330_INT1_STATUS_REG, int_status,
			       2);
	if (ret)
		return IRQ_NONE;

	int_stat = int_status[0] | int_status[1];

	if (FIELD_GET(SMI330_INT_STATUS_FWM_MASK, int_stat) ||
	    FIELD_GET(SMI330_INT_STATUS_FFULL_MASK, int_stat)) {
		ret = smi330_fifo_handler(indio_dev);
		if (ret)
			return IRQ_NONE;
	}

	if (FIELD_GET(SMI330_INT_STATUS_ACC_DRDY_MASK, int_stat) ||
	    FIELD_GET(SMI330_INT_STATUS_GYR_DRDY_MASK, int_stat)) {
		iio_trigger_poll_nested(data->trig);
	}

	if (FIELD_GET(SMI330_INT_STATUS_ANYMO_MASK, int_stat)) {
		iio_push_event(indio_dev,
			       IIO_MOD_EVENT_CODE(IIO_ACCEL, 0,
						  IIO_MOD_X_OR_Y_OR_Z,
						  IIO_EV_TYPE_ROC,
						  IIO_EV_DIR_RISING),
			       data->current_timestamp);
	}

	if (FIELD_GET(SMI330_INT_STATUS_NOMO_MASK, int_stat)) {
		iio_push_event(indio_dev,
			       IIO_MOD_EVENT_CODE(IIO_ACCEL, 0,
						  IIO_MOD_X_OR_Y_OR_Z,
						  IIO_EV_TYPE_ROC,
						  IIO_EV_DIR_FALLING),
			       data->current_timestamp);
	}

	if (FIELD_GET(SMI330_INT_STATUS_TILT_MASK, int_stat)) {
		iio_push_event(indio_dev,
			       IIO_MOD_EVENT_CODE(IIO_ACCEL, 0,
						  IIO_MOD_X_OR_Y_OR_Z,
						  IIO_EV_TYPE_CHANGE,
						  IIO_EV_DIR_EITHER),
			       data->current_timestamp);
	}

	return IRQ_HANDLED;
}

static irqreturn_t smi330_irq_handler(int irq, void *p)
{
	struct iio_dev *indio_dev = p;
	struct smi330_data *data = iio_priv(indio_dev);

	atomic64_set(&data->irq_timestamp, iio_get_time_ns(indio_dev));

	return IRQ_WAKE_THREAD;
}

static int smi330_set_int_pin_config(struct smi330_data *data,
				     enum smi330_int_out irq_num,
				     bool active_high, bool open_drain,
				     bool latch)
{
	int ret, mask, val;

	val = active_high ? SMI330_IO_INT_CTRL_LVL : 0;
	val |= open_drain ? SMI330_IO_INT_CTRL_OD : 0;
	val |= SMI330_IO_INT_CTRL_EN;

	switch (irq_num) {
	case SMI330_INT_1:
		mask = SMI330_IO_INT_CTRL_INT1_MASK;
		val = FIELD_PREP(mask, val);
		break;
	case SMI330_INT_2:
		mask = SMI330_IO_INT_CTRL_INT2_MASK;
		val = FIELD_PREP(mask, val);
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_update_bits(data->regmap, SMI330_IO_INT_CTRL_REG, mask,
				 val);
	if (ret)
		return ret;

	return regmap_update_bits(data->regmap, SMI330_INT_CONF_REG,
				  SMI330_INT_CONF_LATCH_MASK,
				  FIELD_PREP(SMI330_INT_CONF_LATCH_MASK,
					     latch));
}

static int smi330_setup_irq(struct device *dev, struct iio_dev *indio_dev,
			    int irq, enum smi330_int_out irq_num)
{
	int ret, irq_type;
	bool open_drain, active_high, latch;
	struct smi330_data *data = iio_priv(indio_dev);
	struct irq_data *desc;
	struct fwnode_handle *fwnode;

	desc = irq_get_irq_data(irq);
	if (!desc)
		return -EINVAL;

	irq_type = irqd_get_trigger_type(desc);
	switch (irq_type) {
	case IRQF_TRIGGER_RISING:
		latch = false;
		active_high = true;
		break;
	case IRQF_TRIGGER_HIGH:
		latch = true;
		active_high = true;
		break;
	case IRQF_TRIGGER_FALLING:
		latch = false;
		active_high = false;
		break;
	case IRQF_TRIGGER_LOW:
		latch = true;
		active_high = false;
		break;
	default:
		return -EINVAL;
	}

	fwnode = dev_fwnode(dev);
	if (!fwnode)
		return -ENODEV;

	open_drain = fwnode_property_read_bool(fwnode, "drive-open-drain");

	ret = smi330_set_int_pin_config(data, irq_num, active_high, open_drain,
					latch);
	if (ret)
		return ret;

	return devm_request_threaded_irq(dev, irq, smi330_irq_handler,
					 smi330_irq_thread_handler, irq_type,
					 indio_dev->name, indio_dev);
}

static int smi330_register_irq(struct device *dev, struct iio_dev *indio_dev)
{
	int ret, irq;
	struct smi330_data *data = iio_priv(indio_dev);
	struct smi330_cfg *cfg = &data->cfg;
	struct fwnode_handle *fwnode;

	fwnode = dev_fwnode(dev);
	if (!fwnode)
		return -ENODEV;

	irq = fwnode_irq_get_byname(fwnode, "INT1");
	if (irq > 0) {
		ret = smi330_setup_irq(dev, indio_dev, irq, SMI330_INT_1);
		if (ret)
			return ret;
	} else if (cfg->data_irq == SMI330_INT_1 ||
		   cfg->feat_irq == SMI330_INT_1) {
		return -ENODEV;
	}

	irq = fwnode_irq_get_byname(fwnode, "INT2");
	if (irq > 0) {
		ret = smi330_setup_irq(dev, indio_dev, irq, SMI330_INT_2);
		if (ret)
			return ret;
	} else if (cfg->data_irq == SMI330_INT_2 ||
		   cfg->feat_irq == SMI330_INT_2) {
		return -ENODEV;
	}

	return 0;
}

static int smi330_hw_fifo_enable(struct iio_dev *indio_dev)
{
	int ret, cfg, chan;
	int val = 0;
	enum smi330_mode acc_mode = SMI330_MODE_SUSPEND;
	enum smi330_mode gyr_mode = SMI330_MODE_SUSPEND;
	struct smi330_data *data = iio_priv(indio_dev);
	struct device *dev = regmap_get_device(data->regmap);

	data->last_timestamp = 0;

	ret = smi330_get_sensor_config_reg(data, SMI330_ACCEL, &cfg);
	if (ret)
		return ret;

	acc_mode = FIELD_GET(SMI330_CFG_MODE_MASK, cfg);
	ret = smi330_get_sensor_config_reg(data, SMI330_GYRO, &cfg);
	if (ret)
		return ret;

	gyr_mode = FIELD_GET(SMI330_CFG_MODE_MASK, cfg);

	if (acc_mode == SMI330_MODE_LOW_POWER ||
	    gyr_mode == SMI330_MODE_LOW_POWER) {
		dev_warn(dev, "Fifo can't be enabled in low power mode");
		return -EINVAL;
	}

	iio_for_each_active_channel(indio_dev, chan) {
		switch (chan) {
		case SMI330_SCAN_ACCEL_X:
		case SMI330_SCAN_ACCEL_Y:
		case SMI330_SCAN_ACCEL_Z:
			val |= FIELD_PREP(SMI330_FIFO_CONF_ACC_MASK, 1);
			break;
		case SMI330_SCAN_GYRO_X:
		case SMI330_SCAN_GYRO_Y:
		case SMI330_SCAN_GYRO_Z:
			val |= FIELD_PREP(SMI330_FIFO_CONF_GYR_MASK, 1);
			break;
		case SMI330_TEMP_OBJECT:
			val |= FIELD_PREP(SMI330_FIFO_CONF_TEMP_MASK, 1);
			break;
		default:
			break;
		}
	}

	ret = regmap_update_bits(data->regmap, SMI330_FIFO_CONF_REG,
				 SMI330_FIFO_CONF_MASK, val);
	if (ret)
		return ret;

	val = FIELD_PREP(SMI330_INT_MAP2_FIFO_WM_MASK, data->cfg.data_irq) |
	      FIELD_PREP(SMI330_INT_MAP2_FIFO_FULL_MASK, data->cfg.data_irq);
	ret = regmap_update_bits(data->regmap, SMI330_INT_MAP2_REG,
				 SMI330_INT_MAP2_FIFO_MASK, val);
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, SMI330_FIFO_CTRL_REG,
			   SMI330_FIFO_CTRL_FLUSH_MASK);
	if (ret)
		return ret;

	data->cfg.op_mode = SMI330_FIFO;

	return 0;
}

static int smi330_hw_fifo_disable(struct iio_dev *indio_dev)
{
	int ret;
	struct smi330_data *data = iio_priv(indio_dev);

	ret = regmap_update_bits(data->regmap, SMI330_FIFO_CONF_REG,
				 SMI330_FIFO_CONF_MASK, 0);
	if (ret)
		return ret;

	ret = regmap_update_bits(data->regmap, SMI330_INT_MAP2_REG,
				 SMI330_INT_MAP2_FIFO_MASK, 0);
	if (ret)
		return ret;

	data->cfg.op_mode = SMI330_IDLE;

	return 0;
}

static int smi330_hwfifo_set_watermark(struct iio_dev *indio_dev,
				       unsigned int val)
{
	struct smi330_data *data = iio_priv(indio_dev);

	unsigned int fifo_frame_length =
		smi330_get_fifo_frame_length(indio_dev);
	unsigned int abs_max_wm = (SMI330_FIFO_MAX_LENGTH - 1);
	unsigned int max_wm = abs_max_wm - 3 * max(fifo_frame_length, 3u);
	unsigned int watermark = min(fifo_frame_length * val, max_wm);

	return regmap_write(data->regmap, SMI330_FIFO_WATERMARK_REG, watermark);
}

static int smi330_buffer_postenable(struct iio_dev *indio_dev)
{
	struct smi330_data *data = iio_priv(indio_dev);

	if (iio_device_get_current_mode(indio_dev) == INDIO_BUFFER_TRIGGERED)
		return 0;

	if (data->cfg.data_irq != SMI330_INT_DISABLED)
		return smi330_hw_fifo_enable(indio_dev);

	return 0;
}

static int smi330_buffer_predisable(struct iio_dev *indio_dev)
{
	struct smi330_data *data = iio_priv(indio_dev);

	/* Don't break the current interrupt thread handler */
	mutex_lock(&data->lock);

	if (iio_device_get_current_mode(indio_dev) == INDIO_BUFFER_TRIGGERED)
		return 0;

	if (data->cfg.data_irq != SMI330_INT_DISABLED)
		return smi330_hw_fifo_disable(indio_dev);

	return 0;
}

static int smi330_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct smi330_data *data = iio_priv(indio_dev);

	mutex_unlock(&data->lock);

	return 0;
}

static int smi330_set_drdy_trigger_state(struct iio_trigger *trig, bool enable)
{
	int val;
	struct smi330_data *data = iio_trigger_get_drvdata(trig);
	struct device *dev = regmap_get_device(data->regmap);

	if (data->cfg.op_mode == SMI330_FIFO) {
		dev_warn(dev, "Can't set trigger when FIFO enabled\n");
		return -EBUSY;
	}

	if (enable)
		data->cfg.op_mode = SMI330_DATA_READY;
	else
		data->cfg.op_mode = SMI330_IDLE;

	val = FIELD_PREP(SMI330_INT_MAP2_ACC_DRDY_MASK,
			 enable ? data->cfg.data_irq : 0);
	val |= FIELD_PREP(SMI330_INT_MAP2_GYR_DRDY_MASK,
			  enable ? data->cfg.data_irq : 0);
	return regmap_update_bits(data->regmap, SMI330_INT_MAP2_REG,
				  SMI330_INT_MAP2_DRDY_MASK, val);
}

static int smi330_read_odr_reg_value(struct smi330_data *data,
				     enum smi330_odr *odr)
{
	int ret, cfg, acc_odr, gyr_odr;

	ret = smi330_get_sensor_config_reg(data, SMI330_ACCEL, &cfg);
	if (ret)
		return ret;
	acc_odr = FIELD_GET(SMI330_CFG_ODR_MASK, cfg);

	ret = smi330_get_sensor_config_reg(data, SMI330_GYRO, &cfg);
	if (ret)
		return ret;
	gyr_odr = FIELD_GET(SMI330_CFG_ODR_MASK, cfg);

	if (acc_odr == gyr_odr)
		*odr = acc_odr;
	else
		return -EINVAL;

	return 0;
}

static ssize_t alt_odr_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	int ret, acc_odr, gyr_odr;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);

	ret = smi330_get_sensor_config(data, SMI330_ALT_ACCEL, SMI330_ODR,
				       &acc_odr);
	if (ret)
		return ret;

	ret = smi330_get_sensor_config(data, SMI330_ALT_GYRO, SMI330_ODR,
				       &gyr_odr);
	if (ret)
		return ret;

	if (acc_odr == gyr_odr)
		return snprintf(buf, PAGE_SIZE, "%d\n", acc_odr);

	return snprintf(buf, PAGE_SIZE,
			"ODR read failed, potential ACC and GYRO mismatch\n");
}

static ssize_t alt_status_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	int ret, status;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);

	ret = regmap_read(data->regmap, SMI330_ALT_STATUS_REG, &status);
	if (ret)
		return ret;

	return snprintf(buf, PAGE_SIZE, "0x%x\n", status);
}

static ssize_t alt_acc_mode_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	int ret, mode;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);

	ret = smi330_get_sensor_config(data, SMI330_ALT_ACCEL, SMI330_MODE,
				       &mode);
	if (ret)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", mode);
}

static ssize_t alt_acc_avg_num_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	int ret, avg_num;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);

	ret = smi330_get_sensor_config(data, SMI330_ALT_ACCEL, SMI330_AVG_NUM,
				       &avg_num);
	if (ret)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", avg_num);
}

static ssize_t alt_gyr_mode_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	int ret, mode;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);

	ret = smi330_get_sensor_config(data, SMI330_ALT_GYRO, SMI330_MODE,
				       &mode);
	if (ret)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", mode);
}

static ssize_t alt_gyr_avg_num_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	int ret, avg_num;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);

	ret = smi330_get_sensor_config(data, SMI330_ALT_GYRO, SMI330_AVG_NUM,
				       &avg_num);
	if (ret)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", avg_num);
}

static ssize_t hwfifo_watermark_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	int ret, watermark;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);

	ret = regmap_read(data->regmap, SMI330_FIFO_WATERMARK_REG, &watermark);
	if (ret)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%ld\n",
			watermark & SMI330_FIFO_WATERMARK_MASK);
}

static ssize_t hwfifo_watermark_min_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", 1);
}

static ssize_t hwfifo_watermark_max_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", SMI330_FIFO_MAX_LENGTH - 1);
}

static ssize_t hwfifo_fill_level_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);
	int fill_level = smi330_get_fifo_length(data);

	return snprintf(buf, PAGE_SIZE, "%d\n", fill_level);
}

static ssize_t hwfifo_enabled_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);

	if (data->cfg.op_mode == SMI330_FIFO)
		return sprintf(buf, "1\n");
	else
		return sprintf(buf, "0\n");
}

static ssize_t control_auto_op_mode_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	int ret;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);
	u16 op_mode;

	ret = kstrtou16(buf, 10, &op_mode);
	if (ret)
		return ret;

	if (op_mode != SMI330_AUTO_OP_EN_ACC &&
	    op_mode != SMI330_AUTO_OP_EN_GYR &&
	    op_mode != SMI330_AUTO_OP_EN_ALL &&
	    op_mode != SMI330_AUTO_OP_DISABLE) {
		return -EINVAL;
	}

	ret = smi330_config_auto_op_mode(data, op_mode);
	if (ret)
		return ret;

	return count;
}

static ssize_t config_user_overwrite_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	int ret;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);
	u16 user_overwrite;

	ret = kstrtou16(buf, 10, &user_overwrite);
	if (ret)
		return ret;

	if (user_overwrite != SMI330_AUTO_OP_RESET &&
	    user_overwrite != SMI330_AUTO_OP_SET) {
		return -EINVAL;
	}

	ret = regmap_update_bits(data->regmap, SMI330_ALT_CONF_REG,
				 SMI330_ALT_CONF_RST_CONF_EN_MASK,
				 FIELD_PREP(SMI330_ALT_CONF_RST_CONF_EN_MASK,
					    user_overwrite));
	if (ret)
		return ret;

	return count;
}

static ssize_t set_auto_op_mode_cond_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	int ret;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);
	u16 option;

	ret = kstrtou16(buf, 10, &option);
	if (ret)
		return ret;

	switch (option) {
	case SMI330_AUTO_OP_USER_A:
		ret = smi330_auto_op_cond(data, SMI330_AUTO_OP_CONFIG_USER,
					  A_NO_MOTION);
		break;

	case SMI330_AUTO_OP_USER_B:
		ret = smi330_auto_op_cond(data, SMI330_AUTO_OP_CONFIG_USER,
					  B_ANY_MOTION);
		break;

	case SMI330_AUTO_OP_USER_H:
		ret = smi330_auto_op_cond(data, SMI330_AUTO_OP_CONFIG_USER,
					  H_TILT_DETECTION);
		break;

	case SMI330_AUTO_OP_ALT_A:
		ret = smi330_auto_op_cond(data, SMI330_AUTO_OP_CONFIG_ALT,
					  A_NO_MOTION);
		break;

	case SMI330_AUTO_OP_ALT_B:
		ret = smi330_auto_op_cond(data, SMI330_AUTO_OP_CONFIG_ALT,
					  B_ANY_MOTION);
		break;

	case SMI330_AUTO_OP_ALT_H:
		ret = smi330_auto_op_cond(data, SMI330_AUTO_OP_CONFIG_ALT,
					  H_TILT_DETECTION);
		break;

	default:
		return -EINVAL;
	}

	if (ret)
		return ret;

	return count;
}

static ssize_t alt_odr_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	int ret;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);
	u16 odr;

	ret = kstrtou16(buf, 10, &odr);
	if (ret)
		return ret;

	ret = smi330_set_sensor_config(data, SMI330_ALT_ACCEL, SMI330_ODR, odr,
				       false);
	if (ret)
		return ret;

	ret = smi330_set_sensor_config(data, SMI330_ALT_GYRO, SMI330_ODR, odr,
				       false);
	if (ret)
		return ret;

	return count;
}

static ssize_t alt_acc_mode_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	int ret;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);
	u16 mode;

	ret = kstrtou16(buf, 10, &mode);
	if (ret)
		return ret;

	ret = smi330_set_sensor_config(data, SMI330_ALT_ACCEL, SMI330_MODE,
				       mode, false);
	if (ret)
		return ret;

	data->savestate.alt_acc_pwr = mode;

	return count;
}

static ssize_t alt_acc_avg_num_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	int ret;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);
	u16 avg_num;

	ret = kstrtou16(buf, 10, &avg_num);
	if (ret)
		return ret;

	ret = smi330_set_sensor_config(data, SMI330_ALT_ACCEL, SMI330_AVG_NUM,
				       avg_num, false);
	if (ret)
		return ret;

	return count;
}

static ssize_t alt_gyr_mode_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	int ret;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);
	u16 mode;

	ret = kstrtou16(buf, 10, &mode);
	if (ret)
		return ret;

	ret = smi330_set_sensor_config(data, SMI330_ALT_GYRO, SMI330_MODE, mode,
				       false);
	if (ret)
		return ret;

	data->savestate.alt_gyr_pwr = mode;

	return count;
}

static ssize_t alt_gyr_avg_num_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	int ret;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);
	u16 avg_num;

	ret = kstrtou16(buf, 10, &avg_num);
	if (ret)
		return ret;

	ret = smi330_set_sensor_config(data, SMI330_ALT_GYRO, SMI330_AVG_NUM,
				       avg_num, false);
	if (ret)
		return ret;

	return count;
}

static ssize_t self_test_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	int ret;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);

	ret = smi330_perform_self_test(data);
	if (ret)
		return ret;

	return count;
}

static ssize_t self_cal_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	int ret;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);

	ret = smi330_self_calibration(data, indio_dev);
	if (ret)
		return ret;

	return count;
}

static ssize_t soft_reset_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	int ret;
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct smi330_data *data = iio_priv(indio_dev);

	ret = smi330_soft_reset(data);
	if (ret)
		return ret;

	ret = smi330_dev_init(data);
	if (ret)
		return ret;

	return count;
}

static int smi330_read_event_config(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir)
{
	int ret, cfg;
	struct smi330_data *data = iio_priv(indio_dev);

	ret = regmap_read(data->regmap, SMI330_FEATURE_IO0_REG, &cfg);
	if (ret)
		return ret;

	switch (type) {
	case IIO_EV_TYPE_ROC:
		if (dir == IIO_EV_DIR_RISING) {
			switch (chan->channel2) {
			case IIO_MOD_X:
				return FIELD_GET(SMI330_FEATURE_IO0_ANYMO_X_EN_MASK,
						 cfg);
			case IIO_MOD_Y:
				return FIELD_GET(SMI330_FEATURE_IO0_ANYMO_Y_EN_MASK,
						 cfg);
			case IIO_MOD_Z:
				return FIELD_GET(SMI330_FEATURE_IO0_ANYMO_Z_EN_MASK,
						 cfg);
			default:
				return 0;
			}
		}
		if (dir == IIO_EV_DIR_FALLING) {
			switch (chan->channel2) {
			case IIO_MOD_X:
				return FIELD_GET(SMI330_FEATURE_IO0_NOMO_X_EN_MASK,
						 cfg);
			case IIO_MOD_Y:
				return FIELD_GET(SMI330_FEATURE_IO0_NOMO_Y_EN_MASK,
						 cfg);
			case IIO_MOD_Z:
				return FIELD_GET(SMI330_FEATURE_IO0_NOMO_Z_EN_MASK,
						 cfg);
			default:
				return 0;
			}
		}
		return 0;
	case IIO_EV_TYPE_CHANGE:
		if (dir == IIO_EV_DIR_EITHER)
			return FIELD_GET(SMI330_FEATURE_IO0_TILT_EN_MASK, cfg);
		return 0;
	default:
		return 0;
	}
}

static int smi330_write_event_config(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir, bool state)
{
	int ret, int_mask, int_val, en_mask, en_val;
	struct smi330_data *data = iio_priv(indio_dev);

	switch (type) {
	case IIO_EV_TYPE_ROC:
		if (dir == IIO_EV_DIR_RISING) {
			switch (chan->channel2) {
			case IIO_MOD_X:
				en_mask = SMI330_FEATURE_IO0_ANYMO_X_EN_MASK;
				en_val = FIELD_PREP(en_mask, state);
				break;
			case IIO_MOD_Y:
				en_mask = SMI330_FEATURE_IO0_ANYMO_Y_EN_MASK;
				en_val = FIELD_PREP(en_mask, state);
				break;
			case IIO_MOD_Z:
				en_mask = SMI330_FEATURE_IO0_ANYMO_Z_EN_MASK;
				en_val = FIELD_PREP(en_mask, state);
				break;
			default:
				break;
			}
			int_mask = SMI330_INT_MAP1_ANYMO_MASK;
			int_val = FIELD_PREP(int_mask, data->cfg.feat_irq);
			break;
		} else if (dir == IIO_EV_DIR_FALLING) {
			switch (chan->channel2) {
			case IIO_MOD_X:
				en_mask = SMI330_FEATURE_IO0_NOMO_X_EN_MASK;
				en_val = FIELD_PREP(en_mask, state);
				break;
			case IIO_MOD_Y:
				en_mask = SMI330_FEATURE_IO0_NOMO_Y_EN_MASK;
				en_val = FIELD_PREP(en_mask, state);
				break;
			case IIO_MOD_Z:
				en_mask = SMI330_FEATURE_IO0_NOMO_Z_EN_MASK;
				en_val = FIELD_PREP(en_mask, state);
				break;
			default:
				break;
			}
			int_mask = SMI330_INT_MAP1_NOMO_MASK;
			int_val = FIELD_PREP(int_mask, data->cfg.feat_irq);
			break;
		}
		return -EINVAL;
	case IIO_EV_TYPE_CHANGE:
		if (dir == IIO_EV_DIR_EITHER) {
			en_mask = SMI330_FEATURE_IO0_TILT_EN_MASK;
			en_val = FIELD_PREP(en_mask, state);
			int_mask = SMI330_INT_MAP1_TILT_MASK;
			int_val = FIELD_PREP(int_mask, data->cfg.feat_irq);
			break;
		}
		return -EINVAL;
	default:
		return -EINVAL;
	}

	ret = smi330_enable_adv_feat(data, en_mask, en_val);
	if (ret)
		return ret;

	return regmap_update_bits(data->regmap, SMI330_INT_MAP1_REG, int_mask,
				  int_val);
}

static int smi330_get_event_reg_mask(enum iio_event_type type,
				     enum iio_event_direction dir,
				     enum iio_event_info info, int *reg,
				     int *mask)
{
	switch (type) {
	case IIO_EV_TYPE_ROC:
		if (dir == IIO_EV_DIR_RISING) {
			switch (info) {
			case IIO_EV_INFO_PERIOD:
				*reg = SMI330_ANYMO_3_EX_REG;
				*mask = SMI330_MOTION3_DURATION_MASK;
				return 0;
			case IIO_EV_INFO_VALUE:
				*reg = SMI330_ANYMO_1_EX_REG;
				*mask = SMI330_MOTION1_SLOPE_THRES_MASK;
				return 0;
			case IIO_EV_INFO_HYSTERESIS:
				*reg = SMI330_ANYMO_2_EX_REG;
				*mask = SMI330_MOTION2_HYSTERESIS_MASK;
				return 0;
			case IIO_EV_INFO_TIMEOUT:
				*reg = SMI330_ANYMO_3_EX_REG;
				*mask = SMI330_MOTION3_WAIT_TIME_MASK;
				return 0;
			default:
				return -EINVAL;
			}
		} else if (dir == IIO_EV_DIR_FALLING) {
			switch (info) {
			case IIO_EV_INFO_PERIOD:
				*reg = SMI330_NOMO_3_EX_REG;
				*mask = SMI330_MOTION3_DURATION_MASK;
				return 0;
			case IIO_EV_INFO_VALUE:
				*reg = SMI330_NOMO_1_EX_REG;
				*mask = SMI330_MOTION1_SLOPE_THRES_MASK;
				return 0;
			case IIO_EV_INFO_HYSTERESIS:
				*reg = SMI330_NOMO_2_EX_REG;
				*mask = SMI330_MOTION2_HYSTERESIS_MASK;
				return 0;
			case IIO_EV_INFO_TIMEOUT:
				*reg = SMI330_NOMO_3_EX_REG;
				*mask = SMI330_MOTION3_WAIT_TIME_MASK;
				return 0;
			default:
				return -EINVAL;
			}
		} else {
			return -EINVAL;
		}
	case IIO_EV_TYPE_CHANGE:
		if (dir == IIO_EV_DIR_EITHER) {
			switch (info) {
			case IIO_EV_INFO_VALUE:
				*reg = SMI330_TILT_1_EX_REG;
				*mask = SMI330_TILT1_MIN_ANGLE_MASK;
				return 0;
			case IIO_EV_INFO_PERIOD:
				*reg = SMI330_TILT_1_EX_REG;
				*mask = SMI330_TILT1_SEGMENT_SIZE_MASK;
				return 0;
			case IIO_EV_INFO_LOW_PASS_FILTER_3DB:
				*reg = SMI330_TILT_2_EX_REG;
				*mask = SMI330_TILT2_BETA_ACC_MEAN_MASK;
				return 0;
			default:
				return -EINVAL;
			}
		} else {
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}

	return 0;
}

static int smi330_read_event_value(struct iio_dev *indio_dev,
				   const struct iio_chan_spec *chan,
				   enum iio_event_type type,
				   enum iio_event_direction dir,
				   enum iio_event_info info, int *val,
				   int *val2)
{
	int ret, reg, mask, config;
	struct smi330_data *data = iio_priv(indio_dev);

	ret = smi330_get_event_reg_mask(type, dir, info, &reg, &mask);
	if (ret)
		return ret;

	ret = smi330_get_regs_dma(reg, &config, data);
	if (ret)
		return ret;

	/* FIELD_GET is not possible with non-const mask */
	*val = ((config) & (mask)) >> (__builtin_ffs(mask) - 1);

	return IIO_VAL_INT;
}

static int smi330_write_event_value(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir,
				    enum iio_event_info info, int val, int val2)
{
	int ret, reg, mask, config;
	struct smi330_data *data = iio_priv(indio_dev);

	ret = smi330_get_event_reg_mask(type, dir, info, &reg, &mask);
	if (ret)
		return ret;

	ret = smi330_get_regs_dma(reg, &config, data);
	if (ret)
		return ret;

	/* FIELD_PREP is not possible with non-const mask */
	config = ((val << (__builtin_ffs(mask) - 1)) & mask) | (config & ~mask);

	return smi330_set_regs_dma(reg, config, data);
}

static int smi330_suspend(struct device *dev)
{
	int ret;
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct smi330_data *data = iio_priv(indio_dev);

	ret = iio_device_suspend_triggering(indio_dev);
	if (ret)
		return ret;

	ret = smi330_set_sensor_config(data, SMI330_ACCEL, SMI330_MODE,
				       SMI330_MODE_SUSPEND, true);
	if (ret)
		return ret;

	ret = smi330_set_sensor_config(data, SMI330_GYRO, SMI330_MODE,
				       SMI330_MODE_SUSPEND, true);
	if (ret)
		return ret;

	ret = smi330_set_sensor_config(data, SMI330_ALT_ACCEL, SMI330_MODE,
				       SMI330_MODE_SUSPEND, true);
	if (ret)
		return ret;

	return smi330_set_sensor_config(data, SMI330_ALT_GYRO, SMI330_MODE,
					SMI330_MODE_SUSPEND, true);
}

static int smi330_resume(struct device *dev)
{
	int ret;
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct smi330_data *data = iio_priv(indio_dev);

	ret = smi330_set_sensor_config(data, SMI330_ACCEL, SMI330_MODE,
				       data->savestate.acc_pwr, true);
	if (ret)
		return ret;

	ret = smi330_set_sensor_config(data, SMI330_GYRO, SMI330_MODE,
				       data->savestate.gyr_pwr, true);
	if (ret)
		return ret;

	ret = smi330_set_sensor_config(data, SMI330_ALT_ACCEL, SMI330_MODE,
				       data->savestate.alt_acc_pwr, true);
	if (ret)
		return ret;

	ret = smi330_set_sensor_config(data, SMI330_ALT_GYRO, SMI330_MODE,
				       data->savestate.alt_gyr_pwr, true);
	if (ret)
		return ret;

	if (data->cfg.op_mode == SMI330_FIFO) {
		ret = regmap_write(data->regmap, SMI330_FIFO_CTRL_REG,
				   SMI330_FIFO_CTRL_FLUSH_MASK);
		if (ret)
			return ret;
	}

	return iio_device_resume_triggering(indio_dev);
}

DEFINE_SIMPLE_DEV_PM_OPS(smi330_pm_ops, smi330_suspend, smi330_resume);

static IIO_DEVICE_ATTR_WO(control_auto_op_mode, 0);
static IIO_DEVICE_ATTR_WO(set_auto_op_mode_cond, 0);
static IIO_DEVICE_ATTR_WO(config_user_overwrite, 0);

static IIO_DEVICE_ATTR_WO(self_test, 0);
static IIO_DEVICE_ATTR_WO(self_cal, 0);
static IIO_DEVICE_ATTR_WO(soft_reset, 0);

static IIO_DEVICE_ATTR_RW(alt_acc_mode, 0);
static IIO_DEVICE_ATTR_RW(alt_acc_avg_num, 0);
static IIO_DEVICE_ATTR_RW(alt_gyr_mode, 0);
static IIO_DEVICE_ATTR_RW(alt_gyr_avg_num, 0);
static IIO_DEVICE_ATTR_RW(alt_odr, 0);
static IIO_DEVICE_ATTR_RO(alt_status, 0);

static struct attribute *smi330_event_attributes[] = {
	&iio_dev_attr_control_auto_op_mode.dev_attr.attr,
	&iio_dev_attr_set_auto_op_mode_cond.dev_attr.attr,
	&iio_dev_attr_config_user_overwrite.dev_attr.attr,
	&iio_dev_attr_self_test.dev_attr.attr,
	&iio_dev_attr_self_cal.dev_attr.attr,
	&iio_dev_attr_soft_reset.dev_attr.attr,
	&iio_dev_attr_alt_acc_mode.dev_attr.attr,
	&iio_dev_attr_alt_acc_avg_num.dev_attr.attr,
	&iio_dev_attr_alt_gyr_mode.dev_attr.attr,
	&iio_dev_attr_alt_gyr_avg_num.dev_attr.attr,
	&iio_dev_attr_alt_odr.dev_attr.attr,
	&iio_dev_attr_alt_status.dev_attr.attr,
	NULL,
};

static const struct attribute_group smi330_event_attribute_group = {
	.attrs = smi330_event_attributes,
};

static IIO_DEVICE_ATTR_RO(hwfifo_watermark_min, 0);
static IIO_DEVICE_ATTR_RO(hwfifo_watermark_max, 0);
static IIO_DEVICE_ATTR_RO(hwfifo_watermark, 0);
static IIO_DEVICE_ATTR_RO(hwfifo_enabled, 0);
static IIO_DEVICE_ATTR_RO(hwfifo_fill_level, 0);

static const struct iio_dev_attr *smi330_fifo_attributes[] = {
	&iio_dev_attr_hwfifo_watermark_min, &iio_dev_attr_hwfifo_watermark_max,
	&iio_dev_attr_hwfifo_watermark,	    &iio_dev_attr_hwfifo_enabled,
	&iio_dev_attr_hwfifo_fill_level,    NULL,
};

static const struct iio_buffer_setup_ops smi330_buffer_ops = {
	.postenable = smi330_buffer_postenable,
	.predisable = smi330_buffer_predisable,
	.postdisable = smi330_buffer_postdisable,
};

static const struct iio_trigger_ops smi330_trigger_ops = {
	.set_trigger_state = &smi330_set_drdy_trigger_state,
};

static struct iio_info smi330_info = {
	.read_event_config = smi330_read_event_config,
	.read_event_value = smi330_read_event_value,
	.write_event_config = smi330_write_event_config,
	.write_event_value = smi330_write_event_value,
	.read_avail = smi330_read_avail,
	.read_raw = smi330_read_raw,
	.write_raw = smi330_write_raw,
	.write_raw_get_fmt = smi330_write_raw_get_fmt,
	.hwfifo_set_watermark = smi330_hwfifo_set_watermark,
};

static int smi330_dev_init(struct smi330_data *data)
{
	int ret, chip_id, val;
	enum smi330_odr odr;
	struct device *dev = regmap_get_device(data->regmap);
	struct smi330_cfg *cfg = &data->cfg;

	ret = regmap_read(data->regmap, SMI330_CHIP_ID_REG, &chip_id);
	if (ret)
		return ret;

	chip_id &= 0x00FF;

	if (chip_id != SMI330_CHIP_ID)
		dev_info(dev, "Unknown chip id: 0x%04x\n", chip_id);

	ret = regmap_read(data->regmap, SMI330_ERR_REG, &val);
	if (ret || FIELD_GET(SMI330_ERR_FATAL_MASK, val))
		return -ENODEV;

	ret = regmap_read(data->regmap, SMI330_STATUS_REG, &val);
	if (ret || FIELD_GET(SMI330_STATUS_POR_MASK, val) == 0)
		return -ENODEV;

	ret = smi330_read_odr_reg_value(data, &odr);
	if (ret)
		return ret;

	ret = smi330_get_odr_ns(odr, &cfg->odr_ns);
	return ret;
}

int smi330_core_probe(struct device *dev, struct regmap *regmap)
{
	int ret;
	struct iio_dev *indio_dev;
	struct smi330_data *data;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	dev_set_drvdata(dev, indio_dev);
	data->regmap = regmap;
	mutex_init(&data->lock);

	if (IS_ENABLED(CONFIG_SMI330_IRQ_DATA_INT1))
		data->cfg.data_irq = SMI330_INT_1;
	else if (IS_ENABLED(CONFIG_SMI330_IRQ_DATA_INT2))
		data->cfg.data_irq = SMI330_INT_2;
	else
		data->cfg.data_irq = SMI330_INT_DISABLED;

	if (IS_ENABLED(CONFIG_SMI330_IRQ_ADV_FEAT_INT1))
		data->cfg.feat_irq = SMI330_INT_1;
	else if (IS_ENABLED(CONFIG_SMI330_IRQ_ADV_FEAT_INT2))
		data->cfg.feat_irq = SMI330_INT_2;
	else
		data->cfg.feat_irq = SMI330_INT_DISABLED;

	ret = smi330_soft_reset(data);
	if (ret)
		return dev_err_probe(dev, ret, "Soft reset failed\n");

	indio_dev->channels = smi330_channels;
	indio_dev->num_channels = ARRAY_SIZE(smi330_channels);
	indio_dev->name = "smi330";
	indio_dev->modes = INDIO_DIRECT_MODE | INDIO_BUFFER_SOFTWARE;
	indio_dev->info = &smi330_info;

	data->cfg.op_mode = SMI330_IDLE;
	data->savestate.acc_pwr = SMI330_MODE_SUSPEND;
	data->savestate.gyr_pwr = SMI330_MODE_SUSPEND;
	data->savestate.alt_acc_pwr = SMI330_MODE_SUSPEND;
	data->savestate.alt_gyr_pwr = SMI330_MODE_SUSPEND;

	ret = smi330_dev_init(data);
	if (ret)
		return dev_err_probe(dev, ret, "Init failed\n");

	if (data->cfg.data_irq != SMI330_INT_DISABLED) {
		ret = smi330_register_irq(dev, indio_dev);
		if (ret)
			return dev_err_probe(
				dev, ret,
				"Register IRQ failed - check Kconfig and devicetree\n");

		data->trig = devm_iio_trigger_alloc(dev, "%s-drdy-trigger",
						    indio_dev->name);
		if (!data->trig)
			return -ENOMEM;

		data->trig->ops = &smi330_trigger_ops;
		iio_trigger_set_drvdata(data->trig, data);

		ret = devm_iio_trigger_register(dev, data->trig);
		if (ret)
			return dev_err_probe(dev, ret,
					     "IIO register trigger failed\n");

		/*
		 * Set default operation mode to data ready,
		 * remove the trigger if you want to use HW fifo.
		 */
		indio_dev->trig = iio_trigger_get(data->trig);
	}

	ret = devm_iio_triggered_buffer_setup_ext(dev, indio_dev,
						  iio_pollfunc_store_time,
						  smi330_trigger_handler,
						  IIO_BUFFER_DIRECTION_IN,
						  &smi330_buffer_ops,
						  smi330_fifo_attributes);
	if (ret)
		return dev_err_probe(dev, ret, "IIO buffer setup failed\n");

	if (data->cfg.feat_irq != SMI330_INT_DISABLED)
		smi330_info.event_attrs = &smi330_event_attribute_group;

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "Register IIO device failed\n");

	return 0;
}

MODULE_AUTHOR("Roman Huber <roman.huber@de.bosch.com>");
MODULE_AUTHOR("Stefan Gutmann <stefan.gutmann@de.bosch.com>");
MODULE_DESCRIPTION("Bosch SMI330 driver");
MODULE_LICENSE("Dual BSD/GPL");
