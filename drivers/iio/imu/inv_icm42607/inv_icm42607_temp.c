// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 InvenSense, Inc.
 */

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/iio/iio.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#include "inv_icm42607.h"
#include "inv_icm42607_temp.h"

static int inv_icm42607_temp_read(struct inv_icm42607_state *st, s16 *temp)
{
	struct device *dev = regmap_get_device(st->map);
	u8 raw[2];
	int ret;

	PM_RUNTIME_ACQUIRE_AUTOSUSPEND(dev, pm);
	ret = PM_RUNTIME_ACQUIRE_ERR(&pm);
	if (ret)
		return ret;

	guard(mutex)(&st->lock);

	st->conf.temp_en = true;
	ret = inv_icm42607_set_temp_conf(st, st->conf.temp_en, NULL);
	st->conf.temp_en = false;
	if (ret)
		return ret;

	ret = regmap_bulk_read(st->map, INV_ICM42607_REG_TEMP_DATA1,
			       raw, sizeof(raw));
	if (ret)
		return ret;

	*temp = get_unaligned_be16(raw);
	if (*temp == INV_ICM42607_DATA_INVALID)
		return -EINVAL;

	return 0;
}

int inv_icm42607_temp_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int *val, int *val2, long mask)
{
	struct inv_icm42607_state *st = iio_device_get_drvdata(indio_dev);
	s16 temp;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = inv_icm42607_temp_read(st, &temp);
		if (ret)
			return ret;
		*val = temp;
		return IIO_VAL_INT;
	/*
	 * T°C = (temp / 128) + 25
	 * Tm°C = 1000 * ((temp * 100 / 12800) + 25)
	 * scale: 100000 / 12800 ~= 7.8125
	 * offset: 3200
	 */
	case IIO_CHAN_INFO_SCALE:
		*val = 7;
		*val2 = 812500000;
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_OFFSET:
		*val = 3200;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}
