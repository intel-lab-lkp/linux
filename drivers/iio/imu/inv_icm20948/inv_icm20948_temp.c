// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Bharadwaj Raju <bharadwaj.raju777@gmail.com>
 */

#include <linux/bits.h>

#include <linux/iio/iio.h>

#include "inv_icm20948.h"

static const struct iio_chan_spec
	inv_icm20948_temp_chan = { .type = IIO_TEMP,
				   .info_mask_separate =
					   BIT(IIO_CHAN_INFO_RAW) |
					   BIT(IIO_CHAN_INFO_OFFSET) |
					   BIT(IIO_CHAN_INFO_SCALE),
				   .scan_index = 0,
				   .scan_type = {
					   .sign = 's',
					   .realbits = 16,
				   } };

static int inv_icm20948_temp_read_sensor(struct inv_icm20948_state *state,
					 s16 *temp)
{
	int ret;

	pm_runtime_get_sync(state->dev);
	mutex_lock(&state->lock);

	__be16 raw;
	ret = regmap_bulk_read(state->regmap, INV_ICM20948_REG_TEMP_DATA,
				   &raw, sizeof(raw));
	if (ret)
		goto out;

	*temp = __be16_to_cpu(raw);
	ret = 0;

out:
	mutex_unlock(&state->lock);
	pm_runtime_put_autosuspend(state->dev);
	return ret;
}

static int inv_icm20948_temp_read_raw(struct iio_dev *temp_dev,
				      struct iio_chan_spec const *chan,
				      int *val, int *val2, long mask)
{
	struct inv_icm20948_state *state = iio_device_get_drvdata(temp_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(temp_dev))
			return -EBUSY;
		s16 temp;
		int ret = inv_icm20948_temp_read_sensor(state, &temp);

		if (ret)
			return ret;
		iio_device_release_direct(temp_dev);
		*val = temp;
		return IIO_VAL_INT;
	/*
	 * Sensitivity = 333.87
	 * RoomTempOff = 21
	 * T_degC = ((T_raw - RoomTempOff) / Sensitivity) + RoomTempOff
	 * T_degC = ((T_raw - 21) / 333.87) + 21
	 * T_milliDegC = 1000 * (((T_raw - 21) / 333.87) + 21)
	 * T_milliDegC = (1000 / 333.87) * (T_raw - 21 + (21 * 333.87))
	 * T_milliDegC = (T_raw + 6990.27) * 2.995177

	 * scale = 2.995177
	 * offset = 6990.27
	 */
	case IIO_CHAN_INFO_SCALE:
		*val = 2;
		*val2 = 995177;
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_OFFSET:
		*val = 6990;
		*val2 = 270000;
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static const struct iio_info inv_icm20948_temp_info = {
	.read_raw = inv_icm20948_temp_read_raw,
};

struct iio_dev *inv_icm20948_temp_init(struct inv_icm20948_state *state)
{
	struct iio_dev *temp_dev = devm_iio_device_alloc(state->dev, 0);

	if (!temp_dev)
		return ERR_PTR(-ENOMEM);

	iio_device_set_drvdata(temp_dev, state);

	temp_dev->name = "icm20948-temp";
	temp_dev->info = &inv_icm20948_temp_info;
	temp_dev->modes = INDIO_DIRECT_MODE;
	temp_dev->channels = &inv_icm20948_temp_chan;
	temp_dev->num_channels = 1;

	int ret = devm_iio_device_register(state->dev, temp_dev);

	if (ret)
		return ERR_PTR(ret);

	return temp_dev;
}
