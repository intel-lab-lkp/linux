// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 InvenSense, Inc.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include <linux/math64.h>

#include <linux/iio/buffer.h>
#include <linux/iio/common/inv_sensors_timestamp.h>
#include <linux/iio/iio.h>
#include <linux/iio/kfifo_buf.h>

#include "inv_icm42607.h"
#include "inv_icm42607_temp.h"
#include "inv_icm42607_buffer.h"

#define INV_ICM42607_GYRO_CHAN(_modifier, _index, _ext_info)	\
{								\
	.type = IIO_ANGL_VEL,					\
	.modified = 1,						\
	.channel2 = _modifier,					\
	.info_mask_separate =					\
		BIT(IIO_CHAN_INFO_RAW),				\
	.info_mask_shared_by_type =				\
		BIT(IIO_CHAN_INFO_SCALE),			\
	.info_mask_shared_by_all =				\
		BIT(IIO_CHAN_INFO_SAMP_FREQ),			\
	.scan_index = _index,					\
	.scan_type = {						\
		.sign = 's',					\
		.realbits = 16,					\
		.storagebits = 16,				\
		.endianness = IIO_BE,				\
	},							\
	.ext_info = _ext_info,					\
}

enum inv_icm42607_gyro_scan {
	INV_ICM42607_GYRO_SCAN_X,
	INV_ICM42607_GYRO_SCAN_Y,
	INV_ICM42607_GYRO_SCAN_Z,
	INV_ICM42607_GYRO_SCAN_TEMP,
	INV_ICM42607_GYRO_SCAN_TIMESTAMP,
};

static const struct iio_chan_spec_ext_info inv_icm42607_gyro_ext_infos[] = {
	IIO_MOUNT_MATRIX(IIO_SHARED_BY_ALL, inv_icm42607_get_mount_matrix),
	{ },
};

static const struct iio_chan_spec inv_icm42607_gyro_channels[] = {
	INV_ICM42607_GYRO_CHAN(IIO_MOD_X, INV_ICM42607_GYRO_SCAN_X,
			       inv_icm42607_gyro_ext_infos),
	INV_ICM42607_GYRO_CHAN(IIO_MOD_Y, INV_ICM42607_GYRO_SCAN_Y,
			       inv_icm42607_gyro_ext_infos),
	INV_ICM42607_GYRO_CHAN(IIO_MOD_Z, INV_ICM42607_GYRO_SCAN_Z,
			       inv_icm42607_gyro_ext_infos),
	INV_ICM42607_TEMP_CHAN(INV_ICM42607_GYRO_SCAN_TEMP),
	IIO_CHAN_SOFT_TIMESTAMP(INV_ICM42607_GYRO_SCAN_TIMESTAMP),
};

/*
 * IIO buffer data: size must be a power of 2 and timestamp aligned
 * 16 bytes: 6 bytes angular velocity, 2 bytes temperature, 8 bytes timestamp
 */
struct inv_icm42607_gyro_buffer {
	struct inv_icm42607_fifo_sensor_data gyro;
	s16 temp;
	aligned_s64 timestamp;
};

#define INV_ICM42607_SCAN_MASK_GYRO_3AXIS				\
	(BIT(INV_ICM42607_GYRO_SCAN_X) |				\
	 BIT(INV_ICM42607_GYRO_SCAN_Y) |				\
	 BIT(INV_ICM42607_GYRO_SCAN_Z))

#define INV_ICM42607_SCAN_MASK_TEMP	BIT(INV_ICM42607_GYRO_SCAN_TEMP)

static const unsigned long inv_icm42607_gyro_scan_masks[] = {
	INV_ICM42607_SCAN_MASK_GYRO_3AXIS,
	INV_ICM42607_SCAN_MASK_TEMP,
	INV_ICM42607_SCAN_MASK_GYRO_3AXIS | INV_ICM42607_SCAN_MASK_TEMP,
	0,
};

/* enable gyroscope sensor and FIFO write */
static int inv_icm42607_gyro_update_scan_mode(struct iio_dev *indio_dev,
					      const unsigned long *scan_mask)
{
	struct inv_icm42607_state *st = iio_device_get_drvdata(indio_dev);
	struct inv_icm42607_sensor_conf conf = INV_ICM42607_SENSOR_CONF_INIT;
	unsigned int fifo_en = 0;
	unsigned int sleep_gyro = 0;
	unsigned int sleep_temp = 0;
	unsigned int sleep;
	int ret;

	mutex_lock(&st->lock);

	if (*scan_mask & INV_ICM42607_SCAN_MASK_TEMP) {
		/* enable temp sensor */
		ret = inv_icm42607_set_temp_conf(st, true, &sleep_temp);
		if (ret)
			goto out_unlock;
		fifo_en |= INV_ICM42607_SENSOR_TEMP;
	}

	if (*scan_mask & INV_ICM42607_SCAN_MASK_GYRO_3AXIS) {
		/* enable gyro sensor */
		conf.mode = INV_ICM42607_SENSOR_MODE_LOW_NOISE;
		ret = inv_icm42607_set_gyro_conf(st, &conf, &sleep_gyro);
		if (ret)
			goto out_unlock;
		fifo_en |= INV_ICM42607_SENSOR_GYRO;
	}

	/* update data FIFO write */
	ret = inv_icm42607_buffer_set_fifo_en(st, fifo_en | st->fifo.en);
	if (ret)
		goto out_unlock;

out_unlock:
	mutex_unlock(&st->lock);
	/* sleep maximum required time */
	sleep = max(sleep_gyro, sleep_temp);
	if (sleep)
		msleep(sleep);
	return ret;
}

static int inv_icm42607_gyro_read_sensor(struct inv_icm42607_state *st,
					 struct iio_chan_spec const *chan,
					 s16 *val)
{
	struct device *dev = regmap_get_device(st->map);
	struct inv_icm42607_sensor_conf conf = INV_ICM42607_SENSOR_CONF_INIT;
	unsigned int reg;
	__be16 *data;
	int ret;

	if (chan->type != IIO_ANGL_VEL)
		return -EINVAL;

	switch (chan->channel2) {
	case IIO_MOD_X:
		reg = INV_ICM42607_REG_GYRO_DATA_X1;
		break;
	case IIO_MOD_Y:
		reg = INV_ICM42607_REG_GYRO_DATA_Y1;
		break;
	case IIO_MOD_Z:
		reg = INV_ICM42607_REG_GYRO_DATA_Z1;
		break;
	default:
		return -EINVAL;
	}

	PM_RUNTIME_ACQUIRE_AUTOSUSPEND(dev, pm);
	if (PM_RUNTIME_ACQUIRE_ERR(&pm))
		return -ENXIO;

	guard(mutex)(&st->lock);

	/* enable gyro sensor */
	conf.mode = INV_ICM42607_SENSOR_MODE_LOW_NOISE;
	ret = inv_icm42607_set_gyro_conf(st, &conf, NULL);
	if (ret)
		return ret;

	/* read gyro register data */
	data = (__be16 *)&st->buffer[0];
	ret = regmap_bulk_read(st->map, reg, data, sizeof(*data));
	if (ret)
		return ret;

	*val = (s16)be16_to_cpup(data);
	if (*val == INV_ICM42607_DATA_INVALID)
		ret = -EINVAL;

	return ret;
}

static const int inv_icm42607_gyro_scale_nano[][2] = {
	[INV_ICM42607_GYRO_FS_2000DPS] = { 0, 1065264 },
	[INV_ICM42607_GYRO_FS_1000DPS] = { 0, 532632 },
	[INV_ICM42607_GYRO_FS_500DPS] = { 0, 266316 },
	[INV_ICM42607_GYRO_FS_250DPS] = { 0, 133158 },
};

static int inv_icm42607_gyro_read_scale(struct iio_dev *indio_dev,
					int *val, int *val2)
{
	struct inv_icm42607_state *st = iio_device_get_drvdata(indio_dev);
	unsigned int idx;

	idx = st->conf.gyro.fs;

	*val = inv_icm42607_gyro_scale_nano[idx][0];
	*val2 = inv_icm42607_gyro_scale_nano[idx][1];
	return IIO_VAL_INT_PLUS_NANO;
}

static int inv_icm42607_gyro_write_scale(struct iio_dev *indio_dev,
					 int val, int val2)
{
	struct inv_icm42607_state *st = iio_device_get_drvdata(indio_dev);
	struct device *dev = regmap_get_device(st->map);
	unsigned int idx;
	struct inv_icm42607_sensor_conf conf = INV_ICM42607_SENSOR_CONF_INIT;
	int ret;
	size_t scales_len = ARRAY_SIZE(inv_icm42607_gyro_scale_nano);

	for (idx = 0; idx < scales_len; idx++) {
		if (val == inv_icm42607_gyro_scale_nano[idx][0] &&
		    val2 == inv_icm42607_gyro_scale_nano[idx][1])
			break;
	}
	if (idx >= scales_len)
		return -EINVAL;

	conf.fs = idx;

	PM_RUNTIME_ACQUIRE_AUTOSUSPEND(dev, pm);
	if (PM_RUNTIME_ACQUIRE_ERR(&pm))
		return -ENXIO;

	guard(mutex)(&st->lock);

	ret = inv_icm42607_set_gyro_conf(st, &conf, NULL);

	return ret;
}

/* IIO format int + micro */
static const int inv_icm42607_gyro_odr[] = {
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
	/* 1600Hz */
	1600, 0,
};

static const int inv_icm42607_gyro_odr_conv[] = {
	INV_ICM42607_ODR_12_5HZ,
	INV_ICM42607_ODR_25HZ,
	INV_ICM42607_ODR_50HZ,
	INV_ICM42607_ODR_100HZ,
	INV_ICM42607_ODR_200HZ,
	INV_ICM42607_ODR_400HZ,
	INV_ICM42607_ODR_800HZ,
	INV_ICM42607_ODR_1600HZ,
};

static int inv_icm42607_gyro_read_odr(struct inv_icm42607_state *st,
				      int *val, int *val2)
{
	unsigned int odr;
	unsigned int i;

	odr = st->conf.gyro.odr;

	for (i = 0; i < ARRAY_SIZE(inv_icm42607_gyro_odr_conv); ++i) {
		if (inv_icm42607_gyro_odr_conv[i] == odr)
			break;
	}
	if (i >= ARRAY_SIZE(inv_icm42607_gyro_odr_conv))
		return -EINVAL;

	*val = inv_icm42607_gyro_odr[2 * i];
	*val2 = inv_icm42607_gyro_odr[2 * i + 1];

	return IIO_VAL_INT_PLUS_MICRO;
}

static int inv_icm42607_gyro_write_odr(struct iio_dev *indio_dev,
				       int val, int val2)
{
	struct inv_icm42607_state *st = iio_device_get_drvdata(indio_dev);
	struct inv_icm42607_sensor_state *gyro_st = iio_priv(indio_dev);
	struct inv_sensors_timestamp *ts = &gyro_st->ts;
	struct device *dev = regmap_get_device(st->map);
	unsigned int idx;
	struct inv_icm42607_sensor_conf conf = INV_ICM42607_SENSOR_CONF_INIT;
	int ret;

	for (idx = 0; idx < ARRAY_SIZE(inv_icm42607_gyro_odr); idx += 2) {
		if (val == inv_icm42607_gyro_odr[idx] &&
			val2 == inv_icm42607_gyro_odr[idx + 1])
			break;
	}
	if (idx >= ARRAY_SIZE(inv_icm42607_gyro_odr))
		return -EINVAL;

	conf.odr = inv_icm42607_gyro_odr_conv[idx / 2];

	PM_RUNTIME_ACQUIRE_AUTOSUSPEND(dev, pm);
	if (PM_RUNTIME_ACQUIRE_ERR(&pm))
		return -ENXIO;

	guard(mutex)(&st->lock);

	ret = inv_sensors_timestamp_update_odr(ts, inv_icm42607_odr_to_period(conf.odr),
					       iio_buffer_enabled(indio_dev));
	if (ret)
		return ret;

	ret = inv_icm42607_set_gyro_conf(st, &conf, NULL);
	if (ret)
		return ret;
	inv_icm42607_buffer_update_fifo_period(st);
	inv_icm42607_buffer_update_watermark(st);

	return ret;
}

static int inv_icm42607_gyro_read_raw(struct iio_dev *indio_dev,
				      struct iio_chan_spec const *chan,
				      int *val, int *val2, long mask)
{
	struct inv_icm42607_state *st = iio_device_get_drvdata(indio_dev);
	s16 data;
	int ret;

	switch (chan->type) {
	case IIO_ANGL_VEL:
		break;
	case IIO_TEMP:
		return inv_icm42607_temp_read_raw(indio_dev, chan, val, val2, mask);
	default:
		return -EINVAL;
	}

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = inv_icm42607_gyro_read_sensor(st, chan, &data);
		iio_device_release_direct(indio_dev);
		if (ret)
			return ret;
		*val = data;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		return inv_icm42607_gyro_read_scale(indio_dev, val, val2);
	case IIO_CHAN_INFO_SAMP_FREQ:
		return inv_icm42607_gyro_read_odr(st, val, val2);
	default:
		return -EINVAL;
	}
}

static int inv_icm42607_gyro_write_raw(struct iio_dev *indio_dev,
				       struct iio_chan_spec const *chan,
				       int val, int val2, long mask)
{
	int ret;

	if (chan->type != IIO_ANGL_VEL)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = inv_icm42607_gyro_write_scale(indio_dev, val, val2);
		iio_device_release_direct(indio_dev);
		return ret;
	case IIO_CHAN_INFO_SAMP_FREQ:
		return inv_icm42607_gyro_write_odr(indio_dev, val, val2);
	default:
		return -EINVAL;
	}
}

static int inv_icm42607_gyro_write_raw_get_fmt(struct iio_dev *indio_dev,
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
	default:
		return -EINVAL;
	}
}

static int inv_icm42607_gyro_hwfifo_set_watermark(struct iio_dev *indio_dev,
						  unsigned int val)
{
	struct inv_icm42607_state *st = iio_device_get_drvdata(indio_dev);

	guard(mutex)(&st->lock);

	st->fifo.watermark.gyro = val;
	return inv_icm42607_buffer_update_watermark(st);
}

static int inv_icm42607_gyro_hwfifo_flush(struct iio_dev *indio_dev,
					  unsigned int count)
{
	struct inv_icm42607_state *st = iio_device_get_drvdata(indio_dev);
	int ret;

	if (count == 0)
		return 0;

	guard(mutex)(&st->lock);

	ret = inv_icm42607_buffer_hwfifo_flush(st, count);
	if (ret)
		return ret;

	return st->fifo.nb.gyro;
}

static const struct iio_info inv_icm42607_gyro_info = {
	.read_raw = inv_icm42607_gyro_read_raw,
	.write_raw = inv_icm42607_gyro_write_raw,
	.write_raw_get_fmt = inv_icm42607_gyro_write_raw_get_fmt,
	.debugfs_reg_access = inv_icm42607_debugfs_reg,
	.update_scan_mode = inv_icm42607_gyro_update_scan_mode,
	.hwfifo_set_watermark = inv_icm42607_gyro_hwfifo_set_watermark,
	.hwfifo_flush_to_buffer = inv_icm42607_gyro_hwfifo_flush,
};

struct iio_dev *inv_icm42607_gyro_init(struct inv_icm42607_state *st)
{
	struct device *dev = regmap_get_device(st->map);
	const char *name;
	struct inv_icm42607_sensor_state *gyro_st;
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

	/*
	 * clock period is 32kHz (31250ns)
	 * jitter is +/- 2% (20 per mille)
	 */
	ts_chip.clock_period = 31250;
	ts_chip.jitter = 20;
	ts_chip.init_period = inv_icm42607_odr_to_period(st->conf.accel.odr);
	inv_sensors_timestamp_init(&gyro_st->ts, &ts_chip);

	iio_device_set_drvdata(indio_dev, st);
	indio_dev->name = name;
	indio_dev->info = &inv_icm42607_gyro_info;
	indio_dev->modes = INDIO_DIRECT_MODE | INDIO_BUFFER_SOFTWARE;
	indio_dev->channels = inv_icm42607_gyro_channels;
	indio_dev->num_channels = ARRAY_SIZE(inv_icm42607_gyro_channels);
	indio_dev->available_scan_masks = inv_icm42607_gyro_scan_masks;
	indio_dev->setup_ops = &inv_icm42607_buffer_ops;

	ret = devm_iio_kfifo_buffer_setup(dev, indio_dev,
					  &inv_icm42607_buffer_ops);
	if (ret)
		return ERR_PTR(ret);

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return ERR_PTR(ret);

	return indio_dev;
}

int inv_icm42607_gyro_parse_fifo(struct iio_dev *indio_dev)
{
	struct inv_icm42607_state *st = iio_device_get_drvdata(indio_dev);
	struct inv_icm42607_sensor_state *gyro_st = iio_priv(indio_dev);
	struct inv_sensors_timestamp *ts = &gyro_st->ts;
	ssize_t i, size;
	unsigned int no;
	const void *accel, *gyro, *timestamp;
	const s8 *temp;
	unsigned int odr;
	s64 ts_val;
	struct inv_icm42607_gyro_buffer buffer = { };

	/* parse all fifo packets */
	for (i = 0, no = 0; i < st->fifo.count; i += size, ++no) {
		size = inv_icm42607_fifo_decode_packet(&st->fifo.data[i],
				&accel, &gyro, &temp, &timestamp, &odr);
		/* quit if error or FIFO is empty */
		if (size <= 0)
			return size;

		/* skip packet if no gyro data or data is invalid */
		if (gyro == NULL || !inv_icm42607_fifo_is_data_valid(gyro))
			continue;

		/* update odr */
		if (odr & INV_ICM42607_SENSOR_GYRO) {
			inv_sensors_timestamp_apply_odr(ts, st->fifo.period,
							st->fifo.nb.total, no);
		}

		memcpy(&buffer.gyro, gyro, sizeof(buffer.gyro));
		/* convert 8 bits FIFO temperature in high resolution format */
		buffer.temp = temp ? (*temp * 64) : 0;
		ts_val = inv_sensors_timestamp_pop(ts);
		iio_push_to_buffers_with_ts(indio_dev, &buffer,
					    sizeof(buffer), ts_val);
	}

	return 0;
}
