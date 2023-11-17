// SPDX-License-Identifier: GPL-2.0-only
/*
 * Honeywell TruStability HSC Series pressure/temperature sensor
 *
 * Copyright (c) 2023 Petre Rodan <petre.rodan@subdimension.ro>
 *
 * Datasheet: https://prod-edam.honeywell.com/content/dam/honeywell-edam/sps/siot/en-us/products/sensors/pressure-sensors/board-mount-pressure-sensors/trustability-hsc-series/documents/sps-siot-trustability-hsc-series-high-accuracy-board-mount-pressure-sensors-50099148-a-en-ciid-151133.pdf
 * i2c-related datasheet: https://prod-edam.honeywell.com/content/dam/honeywell-edam/sps/siot/en-us/products/sensors/pressure-sensors/board-mount-pressure-sensors/common/documents/sps-siot-i2c-comms-digital-output-pressure-sensors-tn-008201-3-en-ciid-45841.pdf
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/regulator/consumer.h>
#include <linux/iio/iio.h>
#include "hsc030pa.h"

static int hsc_i2c_xfer(struct hsc_data *data)
{
	struct i2c_client *client = data->client;
	struct i2c_msg msg;
	int ret;

	msg.addr = client->addr;
	msg.flags = client->flags | I2C_M_RD;
	msg.len = HSC_REG_MEASUREMENT_RD_SIZE;
	msg.buf = (char *)&data->buffer;

	ret = i2c_transfer(client->adapter, &msg, 1);

	return (ret == 2) ? 0 : ret;
}

static int hsc_i2c_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct hsc_data *hsc;
	const char *range_nom;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*hsc));
	if (!indio_dev) {
		dev_err(&client->dev, "Failed to allocate device\n");
		return -ENOMEM;
	}

	hsc = iio_priv(indio_dev);

	if (i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		hsc->xfer = hsc_i2c_xfer;
	else
		return -EOPNOTSUPP;

	ret = devm_regulator_get_enable_optional(dev, "vdd");
	if (ret == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	if (!dev_fwnode(dev))
		return -EOPNOTSUPP;

	ret = device_property_read_u32(dev,
				       "honeywell,transfer-function",
				       &hsc->function);
	if (ret)
		return dev_err_probe(dev, ret,
				     "honeywell,transfer-function could not be read\n");
	if (hsc->function > HSC_FUNCTION_F)
		return dev_err_probe(dev, -EINVAL,
				     "honeywell,transfer-function %d invalid\n",
				     hsc->function);

	ret = device_property_read_string(dev,
					  "honeywell,range_str", &range_nom);
	if (ret)
		return dev_err_probe(dev, ret,
				     "honeywell,range_str not defined\n");

	memcpy(hsc->range_str, range_nom, HSC_RANGE_STR_LEN - 1);
	hsc->range_str[HSC_RANGE_STR_LEN - 1] = 0;

	if (strcasecmp(hsc->range_str, "na") == 0) {
		// "not available"
		// we got a custom-range chip not covered by the nomenclature
		ret = device_property_read_u32(dev,
					     "honeywell,pmin-pascal",
					     &hsc->pmin);
		if (ret)
			return dev_err_probe(dev, ret,
					     "honeywell,pmin-pascal could not be read\n");
		ret = device_property_read_u32(dev,
					     "honeywell,pmax-pascal",
					     &hsc->pmax);
		if (ret)
			return dev_err_probe(dev, ret,
					     "honeywell,pmax-pascal could not be read\n");
	}

	i2c_set_clientdata(client, indio_dev);
	hsc->client = client;

	return hsc_probe(indio_dev, &client->dev, id->name, id->driver_data);
}

static const struct of_device_id hsc_i2c_match[] = {
	{.compatible = "honeywell,hsc",},
	{.compatible = "honeywell,ssc",},
	{},
};

MODULE_DEVICE_TABLE(of, hsc_i2c_match);

static const struct i2c_device_id hsc_i2c_id[] = {
	{"hsc", HSC},
	{"ssc", SSC},
	{}
};

MODULE_DEVICE_TABLE(i2c, hsc_i2c_id);

static struct i2c_driver hsc_i2c_driver = {
	.driver = {
		   .name = "honeywell_hsc",
		   .of_match_table = hsc_i2c_match,
		   },
	.probe = hsc_i2c_probe,
	.id_table = hsc_i2c_id,
};

module_i2c_driver(hsc_i2c_driver);

MODULE_AUTHOR("Petre Rodan <petre.rodan@subdimension.ro>");
MODULE_DESCRIPTION("Honeywell HSC and SSC pressure sensor i2c driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(IIO_HONEYWELL_HSC);
