// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 InvenSense, Inc.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>

#include "inv_icm42607.h"
#include "inv_icm42607_temp.h"

static int inv_icm42607_temp_read(struct inv_icm42607_state *st, s16 *temp)
{
	struct device *dev = regmap_get_device(st->map);
	__be16 *raw;
	int ret;

	PM_RUNTIME_ACQUIRE_AUTOSUSPEND(dev, pm);
	if (PM_RUNTIME_ACQUIRE_ERR(&pm))
		return -ENXIO;

	guard(mutex)(&st->lock);

	ret = inv_icm42607_set_temp_conf(st, true, NULL);
	if (ret)
		return ret;

	raw = &st->buffer[0];
	ret = regmap_bulk_read(st->map, INV_ICM42607_REG_TEMP_DATA1, raw, sizeof(*raw));
	if (ret)
		return ret;

	*temp = be16_to_cpup(raw);
	if (*temp == INV_ICM42607_DATA_INVALID)
		ret = -EINVAL;

	return ret;
}

int inv_icm42607_temp_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int *val, int *val2, long mask)
{
	struct inv_icm42607_state *st = iio_device_get_drvdata(indio_dev);
	s16 temp;
	int ret;

	if (chan->type != IIO_TEMP)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = inv_icm42607_temp_read(st, &temp);
		iio_device_release_direct(indio_dev);
		if (ret)
			return ret;
		*val = temp;
		return IIO_VAL_INT;
	/*
	 * T°C = (temp / 128) + 25
	 * Tm°C = 1000 * ((temp * 100 / 12800) + 25)
	 * scale: 100000 / 12800 ~= 7.8125
	 * offset: 25000
	 */
	case IIO_CHAN_INFO_SCALE:
		*val = 7;
		*val2 = 812500;
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_OFFSET:
		*val = 25000;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}
