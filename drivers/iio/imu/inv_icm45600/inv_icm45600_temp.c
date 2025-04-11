// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Invensense, Inc.
 */

#include <linux/device.h>
#include <linux/iio/iio.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include "inv_icm45600_temp.h"
#include "inv_icm45600.h"

static int inv_icm45600_temp_read(struct inv_icm45600_state *st, int16_t *temp)
{
	struct device *dev = regmap_get_device(st->map);
	__le16 *raw;
	int ret;

	pm_runtime_get_sync(dev);
	scoped_guard(mutex, &st->lock) {
		raw = (__le16 *)&st->buffer[0];
		ret = regmap_bulk_read(st->map, INV_ICM45600_REG_TEMP_DATA, raw, sizeof(*raw));
		if (ret)
			break;

		*temp = (int16_t)le16_to_cpup(raw);
		if (*temp == INV_ICM45600_DATA_INVALID)
			ret = -EINVAL;
	}
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

int inv_icm45600_temp_read_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan,
			       int *val, int *val2, long mask)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	int16_t temp;
	int ret;

	if (chan->type != IIO_TEMP)
		return -EINVAL;

	/* temperature sensor work only with accel and/or gyro */
	if (st->conf.accel.mode <= INV_ICM45600_SENSOR_MODE_STANDBY &&
		st->conf.gyro.mode  <= INV_ICM45600_SENSOR_MODE_STANDBY) {
		return -ENODATA;
	}

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = inv_icm45600_temp_read(st, &temp);
		iio_device_release_direct(indio_dev);
		if (ret)
			return ret;
		*val = temp;
		return IIO_VAL_INT;
	/*
	 * T°C = (temp / 128) + 25
	 * Tm°C = 1000 * ((temp * 100 / 12800) + 25)
	 * scale: 100000 / 13248 = 7.8125
	 * offset: 25000
	 */
	case IIO_CHAN_INFO_SCALE:
		*val = 7;
		*val2 = 812500;
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_OFFSET:
		*val = 25000;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}
