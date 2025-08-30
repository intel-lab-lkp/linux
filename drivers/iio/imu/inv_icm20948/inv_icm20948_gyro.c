// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Bharadwaj Raju <bharadwaj.raju777@gmail.com>
 */

#include <linux/bits.h>

#include <linux/iio/iio.h>

#include "inv_icm20948.h"

/* IIO int + nano format */
static const int inv_icm20948_gyro_scale[] = {
	/* 250 dps == 0.000133158 rad/s per LSB */
	[2 * INV_ICM20948_GYRO_FS_250] = 0,
	[2 * INV_ICM20948_GYRO_FS_250 + 1] = 133158,
	/* 500 dps == 0.000266316 rad/s per LSB */
	[2 * INV_ICM20948_GYRO_FS_500] = 0,
	[2 * INV_ICM20948_GYRO_FS_500 + 1] = 266316,
	/* 1000 dps == 0.000532632 rad/s per LSB */
	[2 * INV_ICM20948_GYRO_FS_1000] = 0,
	[2 * INV_ICM20948_GYRO_FS_1000 + 1] = 532632,
	/* 2000 dps == 0.001065264 rad/s per LSB */
	[2 * INV_ICM20948_GYRO_FS_1000] = 0,
	[2 * INV_ICM20948_GYRO_FS_1000 + 1] = 1065264,
};

/* Calibration bias, IIO range format int + nano */
/* raw value -2**15 to +2**15, 0.0305 dps per LSB step */
static const int inv_icm20948_gyro_calibbias_range[] = {
	-17, 443239423, /* min */
	0,   532325, /* step */
	+17, 443239423, /* max */
};

#define INV_ICM20948_GYRO_CHAN(_dir) \
	{		\
		.type = IIO_ANGL_VEL,		\
		.modified = 1,		\
		.channel2 = IIO_MOD_##_dir,		\
		.info_mask_separate =		\
		  BIT(IIO_CHAN_INFO_RAW) |		\
		  BIT(IIO_CHAN_INFO_CALIBBIAS),		\
		.info_mask_shared_by_type =		\
		  BIT(IIO_CHAN_INFO_SCALE),		\
		.info_mask_shared_by_type_available =		\
		  BIT(IIO_CHAN_INFO_SCALE) |		\
		  BIT(IIO_CHAN_INFO_CALIBBIAS),		\
		.info_mask_shared_by_all =		\
		  BIT(IIO_CHAN_INFO_SAMP_FREQ),		\
		.info_mask_shared_by_all_available =		\
		  BIT(IIO_CHAN_INFO_SAMP_FREQ),		\
		.scan_index = INV_ICM20948_GYRO_SCAN_##_dir,		\
		.scan_type = {		\
			.sign = 's',		\
			.realbits = 16,		\
			.endianness = IIO_BE,		\
		},		\
	}

enum inv_icm20948_gyro_scan {
	INV_ICM20948_GYRO_SCAN_X,
	INV_ICM20948_GYRO_SCAN_Y,
	INV_ICM20948_GYRO_SCAN_Z,
};

static const struct iio_chan_spec inv_icm20948_gyro_channels[] = {
	INV_ICM20948_GYRO_CHAN(X),
	INV_ICM20948_GYRO_CHAN(Y),
	INV_ICM20948_GYRO_CHAN(Z),
};

static int inv_icm20948_gyro_apply_config(struct inv_icm20948_state *state)
{
	guard(mutex)(&state->lock);
	pm_runtime_get_sync(state->dev);

	int ret = regmap_write_bits(state->regmap, INV_ICM20948_REG_GYRO_CONFIG_1,
				 INV_ICM20948_GYRO_CONFIG_FULLSCALE,
				 state->gyro_conf->fsr << 1);

	pm_runtime_put_autosuspend(state->dev);
	return ret;
}

static int inv_icm20948_gyro_read_sensor(struct inv_icm20948_state *state,
					 struct iio_chan_spec const *chan,
					 s16 *val)
{
	unsigned int reg;

	switch (chan->channel2) {
	case IIO_MOD_X:
		reg = INV_ICM20948_REG_GYRO_DATA_X;
		break;
	case IIO_MOD_Y:
		reg = INV_ICM20948_REG_GYRO_DATA_Y;
		break;
	case IIO_MOD_Z:
		reg = INV_ICM20948_REG_GYRO_DATA_Z;
		break;
	default:
		return -EINVAL;
	}

	pm_runtime_get_sync(state->dev);

	__be16 raw;
	int ret = regmap_bulk_read(state->regmap, reg, &raw, sizeof(raw));

	if (ret)
		goto out;

	*val = (s16)be16_to_cpu(raw);

out:
	pm_runtime_put_autosuspend(state->dev);
	return ret;
}

static int inv_icm20948_gyro_read_calibbias(struct inv_icm20948_state *state,
					    struct iio_chan_spec const *chan,
					    int *val, int *val2)
{
	unsigned int reg;

	switch (chan->channel2) {
	case IIO_MOD_X:
		reg = INV_ICM20948_REG_GYRO_USER_OFFSET_X;
		break;
	case IIO_MOD_Y:
		reg = INV_ICM20948_REG_GYRO_USER_OFFSET_Y;
		break;
	case IIO_MOD_Z:
		reg = INV_ICM20948_REG_GYRO_USER_OFFSET_Z;
		break;
	default:
		return -EINVAL;
	}

	__be16 offset_raw;

	pm_runtime_get_sync(state->dev);
	int ret = regmap_bulk_read(state->regmap, reg, &offset_raw,
				   sizeof(offset_raw));
	pm_runtime_put_autosuspend(state->dev);

	if (ret)
		return ret;
	int offset = be16_to_cpu(offset_raw);

	/* step size = 0.0305 dps/LSB => +/- 999.24 dps over 16-bit range */
	/* offset * 0.0305 * Pi * 10**9 (for nano) / 180 */
	/* offset * 95818575.93448868 / 180 */
	/* offset * 95818576 / 180 */
	s64 val64 = (s64)offset * 95818576;
	/* for rounding, add or subtract divisor/2 */
	if (val64 >= 0)
		val64 += 180 / 2;
	else
		val64 -= 180 / 2;

	s64 bias = div_s64(val64, 180);

	*val = bias / 1000000000L;
	*val2 = bias % 1000000000L;

	return IIO_VAL_INT_PLUS_NANO;
}

static int inv_icm20948_gyro_read_raw(struct iio_dev *gyro_dev,
				      struct iio_chan_spec const *chan,
				      int *val, int *val2, long mask)
{
	struct inv_icm20948_state *state = iio_device_get_drvdata(gyro_dev);

	if (chan->type != IIO_ANGL_VEL)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(gyro_dev))
			return -EBUSY;
		s16 raw;
		int ret = inv_icm20948_gyro_read_sensor(state, chan, &raw);

		iio_device_release_direct(gyro_dev);
		if (ret)
			return ret;
		*val = raw;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = inv_icm20948_gyro_scale[2 * state->gyro_conf->fsr + 1];
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_CALIBBIAS:
		return inv_icm20948_gyro_read_calibbias(state, chan, val, val2);
	default:
		return -EINVAL;
	}
}

static int inv_icm20948_gyro_write_scale(struct inv_icm20948_state *state,
					 int val, int val2)
{
	/* all supported scales are < 1.0 and > 0.0 */
	if (val != 0)
		return -EINVAL;

	int idx;

	for (idx = 0; idx < ARRAY_SIZE(inv_icm20948_gyro_scale); idx += 2) {
		if (val2 == inv_icm20948_gyro_scale[idx + 1])
			break;
	}

	if (idx >= ARRAY_SIZE(inv_icm20948_gyro_scale))
		return -EINVAL;

	state->gyro_conf->fsr = idx / 2;
	return inv_icm20948_gyro_apply_config(state);
}

static int inv_icm20948_write_calibbias(struct inv_icm20948_state *state,
					struct iio_chan_spec const *chan,
					int val, int val2)
{
	unsigned int reg;

	switch (chan->channel2) {
	case IIO_MOD_X:
		reg = INV_ICM20948_REG_GYRO_USER_OFFSET_X;
		break;
	case IIO_MOD_Y:
		reg = INV_ICM20948_REG_GYRO_USER_OFFSET_Y;
		break;
	case IIO_MOD_Z:
		reg = INV_ICM20948_REG_GYRO_USER_OFFSET_Z;
		break;
	default:
		return -EINVAL;
	}

	s64 bias = (s64)val * 100000000L + val2;
	s64 val64 = bias * 180;

	if (val64 >= 0)
		val64 -= 180 / 2;
	else
		val64 += 180 / 2;

	s64 offset64 = div_s64(val64, 95818576L);
	s16 offset = clamp(offset64, (s64)S16_MIN, (s64)S16_MAX);
	__be16 offset_write = cpu_to_be16(offset);

	pm_runtime_get_sync(state->dev);
	mutex_lock(&state->lock);
	int ret = regmap_bulk_write(state->regmap, reg, &offset_write,
				 sizeof(offset_write));
	mutex_unlock(&state->lock);
	pm_runtime_put_autosuspend(state->dev);
	return ret;
}

static int inv_icm20948_gyro_write_raw(struct iio_dev *gyro_dev,
				       struct iio_chan_spec const *chan,
				       int val, int val2, long mask)
{
	struct inv_icm20948_state *state = iio_device_get_drvdata(gyro_dev);

	int ret;

	if (chan->type != IIO_ANGL_VEL)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		if (!iio_device_claim_direct(gyro_dev))
			return -EBUSY;
		ret = inv_icm20948_gyro_write_scale(state, val, val2);
		iio_device_release_direct(gyro_dev);
		return ret;
	case IIO_CHAN_INFO_CALIBBIAS:
		if (!iio_device_claim_direct(gyro_dev))
			return -EBUSY;
		ret = inv_icm20948_write_calibbias(state, chan, val, val2);
		iio_device_release_direct(gyro_dev);
		return ret;
	default:
		return -EINVAL;
	}
}

static int inv_icm20948_gyro_read_avail(struct iio_dev *gyro_dev,
					struct iio_chan_spec const *chan,
					const int **vals, int *type,
					int *length, long mask)
{
	if (chan->type != IIO_ANGL_VEL)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		*vals = inv_icm20948_gyro_scale;
		*type = IIO_VAL_INT_PLUS_NANO;
		*length = ARRAY_SIZE(inv_icm20948_gyro_scale);
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_CALIBBIAS:
		*vals = inv_icm20948_gyro_calibbias_range;
		*type = IIO_VAL_INT_PLUS_NANO;
		*length = ARRAY_SIZE(inv_icm20948_gyro_calibbias_range);
		return IIO_AVAIL_RANGE;
	default:
		return -EINVAL;
	}
}

static const struct iio_info inv_icm20948_gyro_info = {
	.read_raw = inv_icm20948_gyro_read_raw,
	.write_raw = inv_icm20948_gyro_write_raw,
	.read_avail = inv_icm20948_gyro_read_avail,
};

struct iio_dev *inv_icm20948_gyro_init(struct inv_icm20948_state *state)
{
	struct iio_dev *gyro_dev = devm_iio_device_alloc(state->dev, 0);

	if (!gyro_dev)
		return ERR_PTR(-ENOMEM);

	iio_device_set_drvdata(gyro_dev, state);

	gyro_dev->name = "icm20948-gyro";
	gyro_dev->info = &inv_icm20948_gyro_info;
	gyro_dev->modes = INDIO_DIRECT_MODE;
	gyro_dev->channels = inv_icm20948_gyro_channels;
	gyro_dev->num_channels = ARRAY_SIZE(inv_icm20948_gyro_channels);

	int ret = devm_iio_device_register(state->dev, gyro_dev);

	if (ret)
		return ERR_PTR(ret);

	state->gyro_conf =
		devm_kzalloc(state->dev, sizeof(*state->gyro_conf), GFP_KERNEL);
	if (!state->gyro_conf)
		return ERR_PTR(-ENOMEM);

	state->gyro_conf->fsr = INV_ICM20948_GYRO_FS_250;
	ret = inv_icm20948_gyro_apply_config(state);
	if (ret)
		return ERR_PTR(ret);

	return gyro_dev;
}
