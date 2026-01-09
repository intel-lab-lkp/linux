// SPDX-License-Identifier: GPL-2.0-only
/*
 * STMicroelectronics st_lsm6dsx IMU sensor fusion
 */

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/sprintf.h>
#include <linux/types.h>
#include <linux/units.h>

#include "st_lsm6dsx.h"

static int
st_lsm6dsx_sf_get_odr_val(const struct st_lsm6dsx_sf_settings *settings,
			  u32 odr, u8 *val)
{
	int i;

	for (i = 0; i < settings->odr_table.odr_len; i++) {
		if (settings->odr_table.odr_avl[i].milli_hz == odr)
			break;
	}
	if (i == settings->odr_table.odr_len)
		return -EINVAL;

	*val = settings->odr_table.odr_avl[i].val;
	return 0;
}

/**
 * st_lsm6dsx_sf_set_page - Enable or disable access to sensor fusion
 * configuration registers.
 * @hw: Sensor hardware instance.
 * @enable: True to enable access, false to disable access.
 *
 * The register page lock is acquired when enabling access, and released when
 * disabling access. Therefore, a function call with @enable set to true must be
 * followed by a call with @enable set to false (unless the first call returns
 * an error value).
 *
 * Return: 0 on success, negative value on error.
 */
static int st_lsm6dsx_sf_set_page(struct st_lsm6dsx_hw *hw, bool enable)
{
	const struct st_lsm6dsx_reg *mux;
	int err;

	mux = &hw->settings->sf_settings.page_mux;
	if (enable)
		mutex_lock(&hw->page_lock);
	err = regmap_assign_bits(hw->regmap, mux->addr, mux->mask, enable);
	if (!enable || err < 0)
		mutex_unlock(&hw->page_lock);

	return err;
}

int st_lsm6dsx_sf_set_enable(struct st_lsm6dsx_sensor *sensor, bool enable)
{
	struct st_lsm6dsx_hw *hw = sensor->hw;
	const struct st_lsm6dsx_reg *enable_reg;
	int err;

	enable_reg = &hw->settings->sf_settings.enable;
	err = st_lsm6dsx_sf_set_page(hw, true);
	if (err < 0)
		return err;

	err = regmap_assign_bits(hw->regmap, enable_reg->addr, enable_reg->mask,
				 enable);
	st_lsm6dsx_sf_set_page(hw, false);

	return err;
}

int st_lsm6dsx_sf_set_odr(struct st_lsm6dsx_sensor *sensor, bool enable)
{
	struct st_lsm6dsx_hw *hw = sensor->hw;
	const struct st_lsm6dsx_sf_settings *settings;
	u8 data;
	int err;

	err = st_lsm6dsx_sf_set_page(hw, true);
	if (err < 0)
		return err;

	settings = &hw->settings->sf_settings;
	if (enable) {
		u8 odr_val;
		const struct st_lsm6dsx_reg *reg = &settings->odr_table.reg;

		st_lsm6dsx_sf_get_odr_val(settings, sensor->hwfifo_odr_mHz,
					  &odr_val);
		data = ST_LSM6DSX_SHIFT_VAL(odr_val, reg->mask);
		err = regmap_update_bits(hw->regmap, reg->addr, reg->mask,
					 data);
		if (err < 0)
			goto out;
	}
	err = regmap_assign_bits(hw->regmap, settings->fifo_enable.addr,
				 settings->fifo_enable.mask, enable);

out:
	st_lsm6dsx_sf_set_page(hw, false);

	return err;
}

static int st_lsm6dsx_sf_read_raw(struct iio_dev *iio_dev,
				  struct iio_chan_spec const *ch,
				  int *val, int *val2, long mask)
{
	struct st_lsm6dsx_sensor *sensor = iio_priv(iio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = sensor->hwfifo_odr_mHz / MILLI;
		*val2 = (sensor->hwfifo_odr_mHz % MILLI) * MILLI;
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int st_lsm6dsx_sf_write_raw(struct iio_dev *iio_dev,
				   struct iio_chan_spec const *chan,
				   int val, int val2, long mask)
{
	struct st_lsm6dsx_sensor *sensor = iio_priv(iio_dev);
	const struct st_lsm6dsx_sf_settings *settings;
	int err;

	settings = &sensor->hw->settings->sf_settings;
	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ: {
		u32 odr_mHz;
		u8 odr_val;

		odr_mHz = val * MILLI + val2 / MILLI;
		err = st_lsm6dsx_sf_get_odr_val(settings, odr_mHz, &odr_val);
		if (err)
			return err;

		sensor->hwfifo_odr_mHz = odr_mHz;
		break;
	}
	default:
		return -EINVAL;
	}

	return 0;
}

static ssize_t st_lsm6dsx_sf_sampling_freq_avail(struct device *dev,
						 struct device_attribute *attr,
						 char *buf)
{
	struct st_lsm6dsx_sensor *sensor = iio_priv(dev_to_iio_dev(dev));
	const struct st_lsm6dsx_sf_settings *settings;
	int i, len = 0;

	settings = &sensor->hw->settings->sf_settings;
	for (i = 0; i < settings->odr_table.odr_len; i++) {
		u32 val = settings->odr_table.odr_avl[i].milli_hz;

		len += scnprintf(buf + len, PAGE_SIZE - len, "%lu.%03lu ",
				 val / MILLI, val % MILLI);
	}
	buf[len - 1] = '\n';

	return len;
}

static IIO_DEV_ATTR_SAMP_FREQ_AVAIL(st_lsm6dsx_sf_sampling_freq_avail);
static struct attribute *st_lsm6dsx_sf_attributes[] = {
	&iio_dev_attr_sampling_frequency_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group st_lsm6dsx_sf_attribute_group = {
	.attrs = st_lsm6dsx_sf_attributes,
};

static const struct iio_info st_lsm6dsx_sf_info = {
	.attrs = &st_lsm6dsx_sf_attribute_group,
	.read_raw = st_lsm6dsx_sf_read_raw,
	.write_raw = st_lsm6dsx_sf_write_raw,
	.hwfifo_set_watermark = st_lsm6dsx_set_watermark,
};

int st_lsm6dsx_sf_probe(struct st_lsm6dsx_hw *hw, const char *name)
{
	const struct st_lsm6dsx_sf_settings *settings;
	struct st_lsm6dsx_sensor *sensor;
	struct iio_dev *iio_dev;

	iio_dev = devm_iio_device_alloc(hw->dev, sizeof(*sensor));
	if (!iio_dev)
		return -ENOMEM;

	settings = &hw->settings->sf_settings;
	sensor = iio_priv(iio_dev);
	sensor->id = ST_LSM6DSX_ID_SF;
	sensor->hw = hw;
	sensor->hwfifo_odr_mHz = settings->odr_table.odr_avl[0].milli_hz;
	sensor->watermark = 1;
	iio_dev->modes = INDIO_DIRECT_MODE;
	iio_dev->info = &st_lsm6dsx_sf_info;
	iio_dev->channels = settings->chan;
	iio_dev->num_channels = settings->chan_len;
	scnprintf(sensor->name, sizeof(sensor->name), "%s_sf", name);
	iio_dev->name = sensor->name;

	/**
	 *  Put the IIO device pointer in the iio_devs array so that the caller
	 *  can set up a buffer and register this IIO device.
	 */
	hw->iio_devs[ST_LSM6DSX_ID_SF] = iio_dev;

	return 0;
}
