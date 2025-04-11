// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Invensense, Inc.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/iio/buffer.h>
#include <linux/iio/common/inv_sensors_timestamp.h>
#include <linux/iio/iio.h>
#include <linux/iio/kfifo_buf.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include "inv_icm45600_buffer.h"
#include "inv_icm45600_temp.h"
#include "inv_icm45600.h"


#define INV_ICM45600_GYRO_CHAN(_modifier, _index, _ext_info)		\
	{								\
		.type = IIO_ANGL_VEL,					\
		.modified = 1,						\
		.channel2 = _modifier,					\
		.info_mask_separate =					\
			BIT(IIO_CHAN_INFO_RAW) |			\
			BIT(IIO_CHAN_INFO_CALIBBIAS),			\
		.info_mask_shared_by_type =				\
			BIT(IIO_CHAN_INFO_SCALE),			\
		.info_mask_shared_by_type_available =			\
			BIT(IIO_CHAN_INFO_SCALE) |			\
			BIT(IIO_CHAN_INFO_CALIBBIAS),			\
		.info_mask_shared_by_all =				\
			BIT(IIO_CHAN_INFO_SAMP_FREQ),			\
		.info_mask_shared_by_all_available =			\
			BIT(IIO_CHAN_INFO_SAMP_FREQ),			\
		.scan_index = _index,					\
		.scan_type = {						\
			.sign = 's',					\
			.realbits = 16,					\
			.storagebits = 16,				\
			.endianness = IIO_LE,				\
		},							\
		.ext_info = _ext_info,					\
	}

enum inv_icm45600_gyro_scan {
	INV_ICM45600_GYRO_SCAN_X,
	INV_ICM45600_GYRO_SCAN_Y,
	INV_ICM45600_GYRO_SCAN_Z,
	INV_ICM45600_GYRO_SCAN_TEMP,
	INV_ICM45600_GYRO_SCAN_TIMESTAMP,
};


static const char * const inv_icm45600_gyro_power_mode_items[] = {
	"low-noise",
	"low-power",
};
static const int inv_icm45600_gyro_power_mode_values[] = {
	INV_ICM45600_SENSOR_MODE_LOW_NOISE,
	INV_ICM45600_SENSOR_MODE_LOW_POWER,
};

static int inv_icm45600_gyro_power_mode_set(struct iio_dev *indio_dev,
					     const struct iio_chan_spec *chan,
					     unsigned int idx)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	struct inv_icm45600_sensor_state *gyro_st = iio_priv(indio_dev);
	int power_mode;

	if (chan->type != IIO_ANGL_VEL)
		return -EINVAL;

	if (idx >= ARRAY_SIZE(inv_icm45600_gyro_power_mode_values))
		return -EINVAL;

	if (iio_buffer_enabled(indio_dev))
		return -EBUSY;

	power_mode = inv_icm45600_gyro_power_mode_values[idx];

	guard(mutex)(&st->lock);

	/* prevent change if power mode is not supported by the ODR */
	switch (power_mode) {
	case INV_ICM45600_SENSOR_MODE_LOW_NOISE:
		if (st->conf.gyro.odr >= INV_ICM45600_ODR_6_25HZ_LP &&
		    st->conf.gyro.odr <= INV_ICM45600_ODR_1_5625HZ_LP)
			return -EPERM;
		break;
	case INV_ICM45600_SENSOR_MODE_LOW_POWER:
	default:
		if (st->conf.gyro.odr <= INV_ICM45600_ODR_800HZ_LN)
			return -EPERM;
		break;
	}

	gyro_st->power_mode = power_mode;

	return 0;
}

static int inv_icm45600_gyro_power_mode_get(struct iio_dev *indio_dev,
					     const struct iio_chan_spec *chan)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	struct inv_icm45600_sensor_state *gyro_st = iio_priv(indio_dev);
	unsigned int idx;
	int power_mode;

	if (chan->type != IIO_ANGL_VEL)
		return -EINVAL;

	guard(mutex)(&st->lock);

	/* if sensor is on, returns actual power mode and not configured one */
	switch (st->conf.gyro.mode) {
	case INV_ICM45600_SENSOR_MODE_LOW_POWER:
	case INV_ICM45600_SENSOR_MODE_LOW_NOISE:
		power_mode = st->conf.gyro.mode;
		break;
	default:
		power_mode = gyro_st->power_mode;
		break;
	}

	for (idx = 0; idx < ARRAY_SIZE(inv_icm45600_gyro_power_mode_values); ++idx) {
		if (power_mode == inv_icm45600_gyro_power_mode_values[idx])
			break;
	}
	if (idx >= ARRAY_SIZE(inv_icm45600_gyro_power_mode_values))
		return -EINVAL;

	return idx;
}

static const struct iio_enum inv_icm45600_gyro_power_mode_enum = {
	.items = inv_icm45600_gyro_power_mode_items,
	.num_items = ARRAY_SIZE(inv_icm45600_gyro_power_mode_items),
	.set = inv_icm45600_gyro_power_mode_set,
	.get = inv_icm45600_gyro_power_mode_get,
};

static const struct iio_chan_spec_ext_info inv_icm45600_gyro_ext_infos[] = {
	IIO_MOUNT_MATRIX(IIO_SHARED_BY_ALL, inv_icm45600_get_mount_matrix),
	IIO_ENUM_AVAILABLE("power_mode", IIO_SHARED_BY_TYPE,
			   &inv_icm45600_gyro_power_mode_enum),
	IIO_ENUM("power_mode", IIO_SHARED_BY_TYPE,
		 &inv_icm45600_gyro_power_mode_enum),
	{},
};

static const struct iio_chan_spec inv_icm45600_gyro_channels[] = {
	INV_ICM45600_GYRO_CHAN(IIO_MOD_X, INV_ICM45600_GYRO_SCAN_X,
			       inv_icm45600_gyro_ext_infos),
	INV_ICM45600_GYRO_CHAN(IIO_MOD_Y, INV_ICM45600_GYRO_SCAN_Y,
			       inv_icm45600_gyro_ext_infos),
	INV_ICM45600_GYRO_CHAN(IIO_MOD_Z, INV_ICM45600_GYRO_SCAN_Z,
			       inv_icm45600_gyro_ext_infos),
	INV_ICM45600_TEMP_CHAN(INV_ICM45600_GYRO_SCAN_TEMP),
	IIO_CHAN_SOFT_TIMESTAMP(INV_ICM45600_GYRO_SCAN_TIMESTAMP),
};

/*
 * IIO buffer data: size must be a power of 2 and timestamp aligned
 * 16 bytes: 6 bytes angular velocity, 2 bytes temperature, 8 bytes timestamp
 */
struct inv_icm45600_gyro_buffer {
	struct inv_icm45600_fifo_sensor_data gyro;
	int16_t temp;
	aligned_s64 timestamp;
};

#define INV_ICM45600_SCAN_MASK_GYRO_3AXIS				\
	(BIT(INV_ICM45600_GYRO_SCAN_X) |				\
	BIT(INV_ICM45600_GYRO_SCAN_Y) |					\
	BIT(INV_ICM45600_GYRO_SCAN_Z))

#define INV_ICM45600_SCAN_MASK_TEMP	BIT(INV_ICM45600_GYRO_SCAN_TEMP)

static const unsigned long inv_icm45600_gyro_scan_masks[] = {
	/* 3-axis gyro + temperature */
	INV_ICM45600_SCAN_MASK_GYRO_3AXIS | INV_ICM45600_SCAN_MASK_TEMP,
	0,
};

/* enable gyroscope sensor and FIFO write */
static int inv_icm45600_gyro_update_scan_mode(struct iio_dev *indio_dev,
					      const unsigned long *scan_mask)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	struct inv_icm45600_sensor_state *gyro_st = iio_priv(indio_dev);
	struct inv_icm45600_sensor_conf conf = INV_ICM45600_SENSOR_CONF_INIT;
	unsigned int fifo_en = 0;
	unsigned int sleep_gyro = 0;
	unsigned int sleep_temp = 0;
	unsigned int sleep;
	int ret;

	scoped_guard(mutex, &st->lock) {
		if (*scan_mask & INV_ICM45600_SCAN_MASK_TEMP)
			fifo_en |= INV_ICM45600_SENSOR_TEMP;

		if (*scan_mask & INV_ICM45600_SCAN_MASK_GYRO_3AXIS) {
			/* enable gyro sensor */
			conf.mode = gyro_st->power_mode;
			ret = inv_icm45600_set_gyro_conf(st, &conf, &sleep_gyro);
			if (ret)
				break;
			fifo_en |= INV_ICM45600_SENSOR_GYRO;
		}
		/* update data FIFO write */
		ret = inv_icm45600_buffer_set_fifo_en(st, fifo_en | st->fifo.en);
	}
	/* sleep maximum required time */
	sleep = max(sleep_gyro, sleep_temp);
	if (sleep)
		msleep(sleep);
	return ret;
}

static int inv_icm45600_gyro_read_sensor(struct iio_dev *indio_dev,
					 struct iio_chan_spec const *chan,
					 int16_t *val)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	struct inv_icm45600_sensor_state *gyro_st = iio_priv(indio_dev);
	struct device *dev = regmap_get_device(st->map);
	struct inv_icm45600_sensor_conf conf = INV_ICM45600_SENSOR_CONF_INIT;
	unsigned int reg;
	__le16 *data;
	int ret;

	if (chan->type != IIO_ANGL_VEL)
		return -EINVAL;

	switch (chan->channel2) {
	case IIO_MOD_X:
		reg = INV_ICM45600_REG_GYRO_DATA_X;
		break;
	case IIO_MOD_Y:
		reg = INV_ICM45600_REG_GYRO_DATA_Y;
		break;
	case IIO_MOD_Z:
		reg = INV_ICM45600_REG_GYRO_DATA_Z;
		break;
	default:
		return -EINVAL;
	}

	pm_runtime_get_sync(dev);
	scoped_guard(mutex, &st->lock) {
		/* enable gyro sensor */
		conf.mode = gyro_st->power_mode;
		ret = inv_icm45600_set_gyro_conf(st, &conf, NULL);
		if (ret)
			break;

		/* read gyro register data */
		data = (__le16 *)&st->buffer[0];
		ret = regmap_bulk_read(st->map, reg, data, sizeof(*data));
		if (ret)
			break;

		*val = (int16_t)le16_to_cpup(data);
		if (*val == INV_ICM45600_DATA_INVALID)
			ret = -EINVAL;
	}
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return ret;
}

/* IIO format int + nano */
static const int inv_icm45600_gyro_scale[] = {
	/* +/- 2000dps => 0.001065264 rad/s */
	[2 * INV_ICM45600_GYRO_FS_2000DPS] = 0,
	[2 * INV_ICM45600_GYRO_FS_2000DPS + 1] = 1065264,
	/* +/- 1000dps => 0.000532632 rad/s */
	[2 * INV_ICM45600_GYRO_FS_1000DPS] = 0,
	[2 * INV_ICM45600_GYRO_FS_1000DPS + 1] = 532632,
	/* +/- 500dps => 0.000266316 rad/s */
	[2 * INV_ICM45600_GYRO_FS_500DPS] = 0,
	[2 * INV_ICM45600_GYRO_FS_500DPS + 1] = 266316,
	/* +/- 250dps => 0.000133158 rad/s */
	[2 * INV_ICM45600_GYRO_FS_250DPS] = 0,
	[2 * INV_ICM45600_GYRO_FS_250DPS + 1] = 133158,
	/* +/- 125dps => 0.000066579 rad/s */
	[2 * INV_ICM45600_GYRO_FS_125DPS] = 0,
	[2 * INV_ICM45600_GYRO_FS_125DPS + 1] = 66579,
	/* +/- 62.5dps => 0.000033290 rad/s */
	[2 * INV_ICM45600_GYRO_FS_62_5DPS] = 0,
	[2 * INV_ICM45600_GYRO_FS_62_5DPS + 1] = 33290,
	/* +/- 31.25dps => 0.000016645 rad/s */
	[2 * INV_ICM45600_GYRO_FS_31_25DPS] = 0,
	[2 * INV_ICM45600_GYRO_FS_31_25DPS + 1] = 16645,
	/* +/- 15.625dps => 0.000008322 rad/s */
	[2 * INV_ICM45600_GYRO_FS_15_625DPS] = 0,
	[2 * INV_ICM45600_GYRO_FS_15_625DPS + 1] = 8322,
};

/* IIO format int + nano */
static const int inv_icm45686_gyro_scale[] = {
	/* +/- 4000dps => 0.002130529 rad/s */
	[2 * INV_ICM45686_GYRO_FS_4000DPS] = 0,
	[2 * INV_ICM45686_GYRO_FS_4000DPS + 1] = 2130529,
	/* +/- 2000dps => 0.001065264 rad/s */
	[2 * INV_ICM45686_GYRO_FS_2000DPS] = 0,
	[2 * INV_ICM45686_GYRO_FS_2000DPS + 1] = 1065264,
	/* +/- 1000dps => 0.000532632 rad/s */
	[2 * INV_ICM45686_GYRO_FS_1000DPS] = 0,
	[2 * INV_ICM45686_GYRO_FS_1000DPS + 1] = 532632,
	/* +/- 500dps => 0.000266316 rad/s */
	[2 * INV_ICM45686_GYRO_FS_500DPS] = 0,
	[2 * INV_ICM45686_GYRO_FS_500DPS + 1] = 266316,
	/* +/- 250dps => 0.000133158 rad/s */
	[2 * INV_ICM45686_GYRO_FS_250DPS] = 0,
	[2 * INV_ICM45686_GYRO_FS_250DPS + 1] = 133158,
	/* +/- 125dps => 0.000066579 rad/s */
	[2 * INV_ICM45686_GYRO_FS_125DPS] = 0,
	[2 * INV_ICM45686_GYRO_FS_125DPS + 1] = 66579,
	/* +/- 62.5dps => 0.000033290 rad/s */
	[2 * INV_ICM45686_GYRO_FS_62_5DPS] = 0,
	[2 * INV_ICM45686_GYRO_FS_62_5DPS + 1] = 33290,
	/* +/- 31.25dps => 0.000016645 rad/s */
	[2 * INV_ICM45686_GYRO_FS_31_25DPS] = 0,
	[2 * INV_ICM45686_GYRO_FS_31_25DPS + 1] = 16645,
	/* +/- 15.625dps => 0.000008322 rad/s */
	[2 * INV_ICM45686_GYRO_FS_15_625DPS] = 0,
	[2 * INV_ICM45686_GYRO_FS_15_625DPS + 1] = 8322,
};

static int inv_icm45600_gyro_read_scale(struct iio_dev *indio_dev,
					int *val, int *val2)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	struct inv_icm45600_sensor_state *gyro_st = iio_priv(indio_dev);
	unsigned int idx;

	idx = st->conf.gyro.fs;

	/* Full scale register starts at 1 for not High FSR parts */
	if (gyro_st->scales == inv_icm45600_gyro_scale)
		idx--;

	*val = gyro_st->scales[2 * idx];
	*val2 = gyro_st->scales[2 * idx + 1];
	return IIO_VAL_INT_PLUS_NANO;
}

static int inv_icm45600_gyro_write_scale(struct iio_dev *indio_dev,
					 int val, int val2)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	struct inv_icm45600_sensor_state *gyro_st = iio_priv(indio_dev);
	struct device *dev = regmap_get_device(st->map);
	unsigned int idx;
	struct inv_icm45600_sensor_conf conf = INV_ICM45600_SENSOR_CONF_INIT;
	int ret;

	for (idx = 0; idx < gyro_st->scales_len; idx += 2) {
		if (val == gyro_st->scales[idx] &&
		    val2 == gyro_st->scales[idx + 1])
			break;
	}
	if (idx >= gyro_st->scales_len)
		return -EINVAL;

	conf.fs = idx / 2;

	/* Full scale register starts at 1 for not High FSR parts */
	if (gyro_st->scales == inv_icm45600_gyro_scale)
		conf.fs++;

	pm_runtime_get_sync(dev);
	scoped_guard(mutex, &st->lock) {
		ret = inv_icm45600_set_gyro_conf(st, &conf, NULL);
	}
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

/* IIO format int + micro */
static const int inv_icm45600_gyro_odr[] = {
	/* 1.5625Hz */
	1, 562500,
	/* 3.125Hz */
	3, 125000,
	/* 6.25Hz */
	6, 250000,
	/* 12.5Hz */
	12, 500000,
	/* 25Hz */
	25, 0,
	/* 50Hz */
	50, 0,
	/* 100Hz */
	100, 0,
	/* 200Hz */
	200, 0,
	/* 400Hz */
	400, 0,
	/* 800Hz */
	800, 0,
	/* 1.6kHz */
	1600, 0,
	/* 3.2kHz */
	3200, 0,
	/* 6.4kHz */
	6400, 0,
};

static const int inv_icm45600_gyro_odr_conv[] = {
	INV_ICM45600_ODR_1_5625HZ_LP,
	INV_ICM45600_ODR_3_125HZ_LP,
	INV_ICM45600_ODR_6_25HZ_LP,
	INV_ICM45600_ODR_12_5HZ,
	INV_ICM45600_ODR_25HZ,
	INV_ICM45600_ODR_50HZ,
	INV_ICM45600_ODR_100HZ,
	INV_ICM45600_ODR_200HZ,
	INV_ICM45600_ODR_400HZ,
	INV_ICM45600_ODR_800HZ_LN,
	INV_ICM45600_ODR_1600HZ_LN,
	INV_ICM45600_ODR_3200HZ_LN,
	INV_ICM45600_ODR_6400HZ_LN,
};

static int inv_icm45600_gyro_read_odr(struct inv_icm45600_state *st,
				      int *val, int *val2)
{
	unsigned int odr;
	unsigned int i;

	odr = st->conf.gyro.odr;

	for (i = 0; i < ARRAY_SIZE(inv_icm45600_gyro_odr_conv); ++i) {
		if (inv_icm45600_gyro_odr_conv[i] == odr)
			break;
	}
	if (i >= ARRAY_SIZE(inv_icm45600_gyro_odr_conv))
		return -EINVAL;

	*val = inv_icm45600_gyro_odr[2 * i];
	*val2 = inv_icm45600_gyro_odr[2 * i + 1];

	return IIO_VAL_INT_PLUS_MICRO;
}

static int inv_icm45600_gyro_write_odr(struct iio_dev *indio_dev,
				       int val, int val2)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	struct inv_icm45600_sensor_state *gyro_st = iio_priv(indio_dev);
	struct inv_sensors_timestamp *ts = &gyro_st->ts;
	struct device *dev = regmap_get_device(st->map);
	unsigned int idx;
	struct inv_icm45600_sensor_conf conf = INV_ICM45600_SENSOR_CONF_INIT;
	int ret;

	for (idx = 0; idx < ARRAY_SIZE(inv_icm45600_gyro_odr); idx += 2) {
		if (val == inv_icm45600_gyro_odr[idx] &&
		    val2 == inv_icm45600_gyro_odr[idx + 1])
			break;
	}
	if (idx >= ARRAY_SIZE(inv_icm45600_gyro_odr))
		return -EINVAL;

	conf.odr = inv_icm45600_gyro_odr_conv[idx / 2];

	pm_runtime_get_sync(dev);
	scoped_guard(mutex, &st->lock) {
		ret = inv_sensors_timestamp_update_odr(ts, inv_icm45600_odr_to_period(conf.odr),
						       iio_buffer_enabled(indio_dev));
		if (ret)
			break;

		if (st->conf.gyro.mode != INV_ICM45600_SENSOR_MODE_OFF)
			conf.mode = gyro_st->power_mode;
		ret = inv_icm45600_set_gyro_conf(st, &conf, NULL);
		if (ret)
			break;
		inv_icm45600_buffer_update_fifo_period(st);
		inv_icm45600_buffer_update_watermark(st);
	}
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

/*
 * Calibration bias values, IIO range format int + nano.
 * Value is limited to +/-64dps coded on 12 bits signed. Step is 1/32 dps.
 */
static int inv_icm45600_gyro_calibbias[] = {
	-1, 117010721,		/* min: -1.117010721 rad/s */
	0, 545415,		/* step: 0.000545415 rad/s */
	1, 116465306,		/* max: 1.116465306 rad/s */
};

static int inv_icm45600_gyro_read_offset(struct inv_icm45600_state *st,
					 struct iio_chan_spec const *chan,
					 int *val, int *val2)
{
	struct device *dev = regmap_get_device(st->map);
	int64_t val64;
	int32_t bias;
	unsigned int reg;
	int16_t offset;
	uint8_t data[2];
	int ret;

	if (chan->type != IIO_ANGL_VEL)
		return -EINVAL;

	switch (chan->channel2) {
	case IIO_MOD_X:
		reg = INV_ICM45600_IPREG_SYS1_REG_42;
		break;
	case IIO_MOD_Y:
		reg = INV_ICM45600_IPREG_SYS1_REG_56;
		break;
	case IIO_MOD_Z:
		reg = INV_ICM45600_IPREG_SYS1_REG_70;
		break;
	default:
		return -EINVAL;
	}

	pm_runtime_get_sync(dev);
	scoped_guard(mutex, &st->lock) {
		ret = regmap_bulk_read(st->map, reg, st->buffer, sizeof(data));
		memcpy(data, st->buffer, sizeof(data));
	}
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	if (ret)
		return ret;

	/* 12 bits signed value */
	switch (chan->channel2) {
	case IIO_MOD_X:
		offset = sign_extend32(((data[1] & 0x0F) << 8) | data[0], 11);
		break;
	case IIO_MOD_Y:
		offset = sign_extend32(((data[0] & 0xF0) << 4) | data[1], 11);
		break;
	case IIO_MOD_Z:
		offset = sign_extend32(((data[1] & 0x0F) << 8) | data[0], 11);
		break;
	default:
		return -EINVAL;
	}

	/*
	 * convert raw offset to dps then to rad/s
	 * 12 bits signed raw max 64 to dps: 64 / 2048
	 * dps to rad: Pi / 180
	 * result in nano (1000000000)
	 * (offset * 64 * Pi * 1000000000) / (2048 * 180)
	 */
	val64 = (int64_t)offset * 64LL * 3141592653LL;
	/* for rounding, add + or - divisor (2048 * 180) divided by 2 */
	if (val64 >= 0)
		val64 += 2048 * 180 / 2;
	else
		val64 -= 2048 * 180 / 2;
	bias = div_s64(val64, 2048 * 180);
	*val = bias / 1000000000L;
	*val2 = bias % 1000000000L;

	return IIO_VAL_INT_PLUS_NANO;
}

static int inv_icm45600_gyro_write_offset(struct inv_icm45600_state *st,
					  struct iio_chan_spec const *chan,
					  int val, int val2)
{
	struct device *dev = regmap_get_device(st->map);
	int64_t val64, min, max;
	unsigned int reg;
	int16_t offset;
	int ret;

	if (chan->type != IIO_ANGL_VEL)
		return -EINVAL;

	switch (chan->channel2) {
	case IIO_MOD_X:
		reg = INV_ICM45600_IPREG_SYS1_REG_42;
		break;
	case IIO_MOD_Y:
		reg = INV_ICM45600_IPREG_SYS1_REG_56;
		break;
	case IIO_MOD_Z:
		reg = INV_ICM45600_IPREG_SYS1_REG_70;
		break;
	default:
		return -EINVAL;
	}

	/* inv_icm45600_gyro_calibbias: min - step - max in nano */
	min = (int64_t)inv_icm45600_gyro_calibbias[0] * 1000000000LL +
	      (int64_t)inv_icm45600_gyro_calibbias[1];
	max = (int64_t)inv_icm45600_gyro_calibbias[4] * 1000000000LL +
	      (int64_t)inv_icm45600_gyro_calibbias[5];
	val64 = (int64_t)val * 1000000000LL + (int64_t)val2;
	if (val64 < min || val64 > max)
		return -EINVAL;

	/*
	 * convert rad/s to dps then to raw value
	 * rad to dps: 180 / Pi
	 * dps to raw 14 bits signed, max 62.5: 8192 / 62.5
	 * val in nano (1000000000)
	 * val * 180 * 8192 / (Pi * 1000000000 * 62.5)
	 */
	val64 = val64 * 180LL * 8192;
	/* for rounding, add + or - divisor (314159265 * 625) divided by 2 */
	if (val64 >= 0)
		val64 += 314159265LL * 625LL / 2LL;
	else
		val64 -= 314159265LL * 625LL / 2LL;
	offset = div64_s64(val64, 314159265LL * 625LL);

	/* clamp value limited to 14 bits signed */
	if (offset < -8192)
		offset = -8192;
	else if (offset > 8191)
		offset = 8191;

	pm_runtime_get_sync(dev);
	scoped_guard(mutex, &st->lock) {
		ret = regmap_bulk_write(st->map, reg, st->buffer, 2);
	}
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return ret;
}

static int inv_icm45600_gyro_read_raw(struct iio_dev *indio_dev,
				      struct iio_chan_spec const *chan,
				      int *val, int *val2, long mask)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	int16_t data;
	int ret;

	switch (chan->type) {
	case IIO_ANGL_VEL:
		break;
	case IIO_TEMP:
		return inv_icm45600_temp_read_raw(indio_dev, chan, val, val2, mask);
	default:
		return -EINVAL;
	}

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = inv_icm45600_gyro_read_sensor(indio_dev, chan, &data);
		iio_device_release_direct(indio_dev);
		if (ret)
			return ret;
		*val = data;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		return inv_icm45600_gyro_read_scale(indio_dev, val, val2);
	case IIO_CHAN_INFO_SAMP_FREQ:
		return inv_icm45600_gyro_read_odr(st, val, val2);
	case IIO_CHAN_INFO_CALIBBIAS:
		return inv_icm45600_gyro_read_offset(st, chan, val, val2);
	default:
		return -EINVAL;
	}
}

static int inv_icm45600_gyro_read_avail(struct iio_dev *indio_dev,
					struct iio_chan_spec const *chan,
					const int **vals,
					int *type, int *length, long mask)
{
	struct inv_icm45600_sensor_state *gyro_st = iio_priv(indio_dev);

	if (chan->type != IIO_ANGL_VEL)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		*vals = gyro_st->scales;
		*type = IIO_VAL_INT_PLUS_NANO;
		*length = gyro_st->scales_len;
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*vals = inv_icm45600_gyro_odr;
		*type = IIO_VAL_INT_PLUS_MICRO;
		*length = ARRAY_SIZE(inv_icm45600_gyro_odr);
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_CALIBBIAS:
		*vals = inv_icm45600_gyro_calibbias;
		*type = IIO_VAL_INT_PLUS_NANO;
		return IIO_AVAIL_RANGE;
	default:
		return -EINVAL;
	}
}

static int inv_icm45600_gyro_write_raw(struct iio_dev *indio_dev,
				       struct iio_chan_spec const *chan,
				       int val, int val2, long mask)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	int ret;

	if (chan->type != IIO_ANGL_VEL)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = inv_icm45600_gyro_write_scale(indio_dev, val, val2);
		iio_device_release_direct(indio_dev);
		return ret;
	case IIO_CHAN_INFO_SAMP_FREQ:
		return inv_icm45600_gyro_write_odr(indio_dev, val, val2);
	case IIO_CHAN_INFO_CALIBBIAS:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = inv_icm45600_gyro_write_offset(st, chan, val, val2);
		iio_device_release_direct(indio_dev);
		return ret;
	default:
		return -EINVAL;
	}
}

static int inv_icm45600_gyro_write_raw_get_fmt(struct iio_dev *indio_dev,
					       struct iio_chan_spec const *chan,
					       long mask)
{
	if (chan->type != IIO_ANGL_VEL)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_SAMP_FREQ:
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_CALIBBIAS:
		return IIO_VAL_INT_PLUS_NANO;
	default:
		return -EINVAL;
	}
}

static int inv_icm45600_gyro_hwfifo_set_watermark(struct iio_dev *indio_dev,
						  unsigned int val)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	int ret;

	guard(mutex)(&st->lock);

	st->fifo.watermark.gyro = val;
	ret = inv_icm45600_buffer_update_watermark(st);

	return ret;
}

static int inv_icm45600_gyro_hwfifo_flush(struct iio_dev *indio_dev,
					  unsigned int count)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	int ret;

	if (count == 0)
		return 0;

	guard(mutex)(&st->lock);

	ret = inv_icm45600_buffer_hwfifo_flush(st, count);
	if (!ret)
		ret = st->fifo.nb.gyro;

	return ret;
}

static const struct iio_info inv_icm45600_gyro_info = {
	.read_raw = inv_icm45600_gyro_read_raw,
	.read_avail = inv_icm45600_gyro_read_avail,
	.write_raw = inv_icm45600_gyro_write_raw,
	.write_raw_get_fmt = inv_icm45600_gyro_write_raw_get_fmt,
	.debugfs_reg_access = inv_icm45600_debugfs_reg,
	.update_scan_mode = inv_icm45600_gyro_update_scan_mode,
	.hwfifo_set_watermark = inv_icm45600_gyro_hwfifo_set_watermark,
	.hwfifo_flush_to_buffer = inv_icm45600_gyro_hwfifo_flush,
};

struct iio_dev *inv_icm45600_gyro_init(struct inv_icm45600_state *st)
{
	struct device *dev = regmap_get_device(st->map);
	const char *name;
	struct inv_icm45600_sensor_state *gyro_st;
	struct inv_sensors_timestamp_chip ts_chip;
	struct iio_dev *indio_dev;
	int ret;

	name = devm_kasprintf(dev, GFP_KERNEL, "%s-gyro", st->name);
	if (!name)
		return ERR_PTR(-ENOMEM);

	indio_dev = devm_iio_device_alloc(dev, sizeof(*gyro_st));
	if (!indio_dev)
		return ERR_PTR(-ENOMEM);
	gyro_st = iio_priv(indio_dev);

	switch (st->chip) {
	case INV_CHIP_ICM45686:
	case INV_CHIP_ICM45688P:
	case INV_CHIP_ICM45689:
	case INV_CHIP_ICM45687:
		gyro_st->scales = inv_icm45686_gyro_scale;
		gyro_st->scales_len = ARRAY_SIZE(inv_icm45686_gyro_scale);
		break;
	default:
		gyro_st->scales = inv_icm45600_gyro_scale;
		gyro_st->scales_len = ARRAY_SIZE(inv_icm45600_gyro_scale);
		/* Set Gyro default FSR */
		ret = regmap_update_bits(st->map, INV_ICM45600_REG_GYRO_CONFIG0,
					INV_ICM45600_GYRO_CONFIG0_FS_MASK,
					INV_ICM45600_GYRO_CONFIG0_FS_2000DPS);
		if (ret)
			return ERR_PTR(ret);
		break;
	}
	/* low-noise by default at init */
	gyro_st->power_mode = INV_ICM45600_SENSOR_MODE_LOW_NOISE;

	/*
	 * clock period is 32kHz (31250ns)
	 * jitter is +/- 2% (20 per mille)
	 */
	ts_chip.clock_period = 31250;
	ts_chip.jitter = 20;
	ts_chip.init_period = inv_icm45600_odr_to_period(st->conf.gyro.odr);
	inv_sensors_timestamp_init(&gyro_st->ts, &ts_chip);

	iio_device_set_drvdata(indio_dev, st);
	indio_dev->name = name;
	indio_dev->info = &inv_icm45600_gyro_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = inv_icm45600_gyro_channels;
	indio_dev->num_channels = ARRAY_SIZE(inv_icm45600_gyro_channels);
	indio_dev->available_scan_masks = inv_icm45600_gyro_scan_masks;
	indio_dev->setup_ops = &inv_icm45600_buffer_ops;

	ret = devm_iio_kfifo_buffer_setup(dev, indio_dev,
					  &inv_icm45600_buffer_ops);
	if (ret)
		return ERR_PTR(ret);

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return ERR_PTR(ret);

	return indio_dev;
}

int inv_icm45600_gyro_parse_fifo(struct iio_dev *indio_dev)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	struct inv_icm45600_sensor_state *gyro_st = iio_priv(indio_dev);
	struct inv_sensors_timestamp *ts = &gyro_st->ts;
	ssize_t i, size;
	unsigned int no;
	const void *accel, *gyro, *timestamp;
	const int8_t *temp;
	unsigned int odr;
	int64_t ts_val;
	struct inv_icm45600_gyro_buffer buffer;

	/* parse all fifo packets */
	for (i = 0, no = 0; i < st->fifo.count; i += size, ++no) {
		size = inv_icm45600_fifo_decode_packet(&st->fifo.data[i],
				&accel, &gyro, &temp, &timestamp, &odr);
		/* quit if error or FIFO is empty */
		if (size <= 0)
			return size;

		/* skip packet if no gyro data or data is invalid */
		if (gyro == NULL || !inv_icm45600_fifo_is_data_valid(gyro))
			continue;

		/* update odr */
		if (odr & INV_ICM45600_SENSOR_GYRO)
			inv_sensors_timestamp_apply_odr(ts, st->fifo.period,
							st->fifo.nb.total, no);

		/* buffer is copied to userspace, zeroing it to avoid any data leak */
		memset(&buffer, 0, sizeof(buffer));
		memcpy(&buffer.gyro, gyro, sizeof(buffer.gyro));
		/* convert 8 bits FIFO temperature in high resolution format */
		buffer.temp = temp ? (*temp * 64) : 0;
		ts_val = inv_sensors_timestamp_pop(ts);
		iio_push_to_buffers_with_timestamp(indio_dev, &buffer, ts_val);
	}

	return 0;
}
