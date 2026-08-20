// SPDX-License-Identifier: GPL-2.0-only
/*
 * ms5637.c - Support for Measurement-Specialties MS5637, MS5805
 *            MS5837 and MS8607 pressure & temperature sensor
 *
 * Copyright (c) 2015 Measurement-Specialties
 *
 * (7-bit I2C slave address 0x76)
 *
 * Datasheet:
 *  http://www.meas-spec.com/downloads/MS5637-02BA03.pdf
 * Datasheet:
 *  http://www.meas-spec.com/downloads/MS5805-02BA01.pdf
 * Datasheet:
 *  http://www.meas-spec.com/downloads/MS5837-30BA.pdf
 * Datasheet:
 *  http://www.meas-spec.com/downloads/MS8607-02BA01.pdf
 */

#include <linux/init.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/stat.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/mutex.h>

#include "../common/ms_sensors/ms_sensors_i2c.h"

static const int ms5637_samp_freq[6] = { 960, 480, 240, 120, 60, 30 };

static ssize_t ms5637_show_samp_freq(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct ms_tp_dev *dev_data = iio_priv(indio_dev);
	int i, len = 0;

	for (i = 0; i <= dev_data->data->hw->max_res_index; i++)
		len += sysfs_emit_at(buf, len, "%u ", ms5637_samp_freq[i]);
	sysfs_emit_at(buf, len - 1, "\n");

	return len;
}

static int ms5637_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *channel, int *val,
			   int *val2, long mask)
{
	int ret;
	int temperature;
	unsigned int pressure;
	struct ms_tp_dev *dev_data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_PROCESSED:
		ret = ms_sensors_read_temp_and_pressure(dev_data,
							&temperature,
							&pressure);
		if (ret)
			return ret;

		switch (channel->type) {
		case IIO_TEMP:	/* in milli °C */
			*val = temperature;

			return IIO_VAL_INT;
		case IIO_PRESSURE:	/* in kPa */
			*val = pressure;
			*val2 = dev_data->data->comp_consts->press_scale;
			return IIO_VAL_FRACTIONAL;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = ms5637_samp_freq[dev_data->res_index];

		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int ms5637_write_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int val, int val2, long mask)
{
	struct ms_tp_dev *dev_data = iio_priv(indio_dev);
	int i;

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		i = ARRAY_SIZE(ms5637_samp_freq);
		while (i-- > 0)
			if (val == ms5637_samp_freq[i])
				break;
		if (i < 0)
			return -EINVAL;
		dev_data->res_index = i;

		return 0;
	default:
		return -EINVAL;
	}
}

static const struct iio_chan_spec ms5637_channels[] = {
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
		.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),
	},
	{
		.type = IIO_PRESSURE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
		.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),
	}
};

static IIO_DEV_ATTR_SAMP_FREQ_AVAIL(ms5637_show_samp_freq);

static struct attribute *ms5637_attributes[] = {
	&iio_dev_attr_sampling_frequency_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group ms5637_attribute_group = {
	.attrs = ms5637_attributes,
};

static const struct iio_info ms5637_info = {
	.read_raw = ms5637_read_raw,
	.write_raw = ms5637_write_raw,
	.attrs = &ms5637_attribute_group,
};

static int ms5637_probe(struct i2c_client *client)
{
	const struct ms_tp_data *data;
	struct ms_tp_dev *dev_data;
	struct iio_dev *indio_dev;
	int ret;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_READ_WORD_DATA |
				     I2C_FUNC_SMBUS_WRITE_BYTE |
				     I2C_FUNC_SMBUS_READ_I2C_BLOCK)) {
		dev_err(&client->dev,
			"Adapter does not support some i2c transaction\n");
		return -EOPNOTSUPP;
	}

	data = i2c_get_match_data(client);
	if (!data)
		return -EINVAL;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*dev_data));
	if (!indio_dev)
		return -ENOMEM;

	dev_data = iio_priv(indio_dev);
	dev_data->client = client;
	dev_data->res_index = data->hw->max_res_index;
	dev_data->data = data;
	mutex_init(&dev_data->lock);

	indio_dev->info = &ms5637_info;
	indio_dev->name = data->name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = ms5637_channels;
	indio_dev->num_channels = ARRAY_SIZE(ms5637_channels);

	i2c_set_clientdata(client, indio_dev);

	ret = ms_sensors_reset(client, 0x1E, 3000);
	if (ret)
		return ret;

	ret = ms_sensors_tp_read_prom(dev_data);
	if (ret)
		return ret;

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct ms_tp_hw_data ms5637_hw_data  = {
	.prom_len = 7,
	.max_res_index = 5
};

static const struct ms_tp_hw_data ms5803_hw_data  = {
	.prom_len = 8,
	.max_res_index = 4
};

/*
 * MS5637-02BA03 compensation constants
 * Datasheet: https://www.te.com/commerce/DocumentDelivery/DDEController?Action=showdoc&DocId=Data+Sheet%7FMS5637-02BA03%7FB1%7Fpdf%7FEnglish%7FENG_DS_MS5637-02BA03_B1.pdf
 * Pages: 7-8
 * Sections: Pressure and Temperature Calculation, Second Order Temperature Compensation
 */
static const struct ms_tp_comp_consts ms5637_02_consts = {
	.press_scale = 1000,
	.high_t2_multiplier = 5,
	.high_t2_shift = 38,
	.high_off2_multiplier = 0,
	.high_off2_shift = 0,
	.low_t2_multiplier = 3,
	.low_t2_shift = 33,
	.low_off2_multiplier = 61,
	.low_off2_shift = 4,
	.low_sens2_multiplier = 29,
	.low_sens2_shift = 4,
	.vlow_off2_multiplier = 17,
	.vlow_sens2_multiplier = 9,
	.has_vhigh_temp = false,
	.off_t1_shift = 17,
	.off_shift = 6,
	.sens_t1_shift = 16,
	.sens_shift = 7,
	.press_sens_shift = 21,
	.press_shift = 15,
};

static const struct ms_tp_data ms5637_data = {
	.name = "ms5637",
	.hw = &ms5637_hw_data,
	.comp_consts = &ms5637_02_consts,
};

static const struct ms_tp_data ms5803_data = {
	.name = "ms5803",
	.hw = &ms5803_hw_data,
	.comp_consts = &ms5637_02_consts,
};

static const struct ms_tp_data ms5805_data = {
	.name = "ms5805",
	.hw = &ms5637_hw_data,
	.comp_consts = &ms5637_02_consts,
};

static const struct ms_tp_data ms5837_data = {
	.name = "ms5837",
	.hw = &ms5637_hw_data,
	.comp_consts = &ms5637_02_consts,
};

static const struct ms_tp_data ms8607_data = {
	.name = "ms8607-temppressure",
	.hw = &ms5637_hw_data,
	.comp_consts = &ms5637_02_consts,
};

static const struct i2c_device_id ms5637_id[] = {
	{ .name = "ms5637", .driver_data = (kernel_ulong_t)&ms5637_data },
	{ .name = "ms5803", .driver_data = (kernel_ulong_t)&ms5803_data },
	{ .name = "ms5805", .driver_data = (kernel_ulong_t)&ms5805_data },
	{ .name = "ms5837", .driver_data = (kernel_ulong_t)&ms5837_data },
	{ .name = "ms8607-temppressure", .driver_data = (kernel_ulong_t)&ms8607_data },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ms5637_id);

static const struct of_device_id ms5637_of_match[] = {
	{ .compatible = "meas,ms5637", .data = &ms5637_data },
	{ .compatible = "meas,ms5803", .data = &ms5803_data },
	{ .compatible = "meas,ms5805", .data = &ms5805_data },
	{ .compatible = "meas,ms5837", .data = &ms5837_data },
	{ .compatible = "meas,ms8607-temppressure", .data = &ms8607_data },
	{ }
};
MODULE_DEVICE_TABLE(of, ms5637_of_match);

static struct i2c_driver ms5637_driver = {
	.probe = ms5637_probe,
	.id_table = ms5637_id,
	.driver = {
		   .name = "ms5637",
		   .of_match_table = ms5637_of_match,
		   },
};

module_i2c_driver(ms5637_driver);

MODULE_DESCRIPTION("Measurement-Specialties ms5637 temperature & pressure driver");
MODULE_AUTHOR("William Markezana <william.markezana@meas-spec.com>");
MODULE_AUTHOR("Ludovic Tancerel <ludovic.tancerel@maplehightech.com>");
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS("IIO_MEAS_SPEC_SENSORS");
