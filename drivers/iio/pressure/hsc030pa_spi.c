// SPDX-License-Identifier: GPL-2.0-only
/*
 * Honeywell TruStability HSC Series pressure/temperature sensor
 *
 * Copyright (c) 2023 Petre Rodan <petre.rodan@subdimension.ro>
 *
 * Datasheet: https://prod-edam.honeywell.com/content/dam/honeywell-edam/sps/siot/en-us/products/sensors/pressure-sensors/board-mount-pressure-sensors/trustability-hsc-series/documents/sps-siot-trustability-hsc-series-high-accuracy-board-mount-pressure-sensors-50099148-a-en-ciid-151133.pdf
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/iio/iio.h>
#include <linux/regulator/consumer.h>
#include "hsc030pa.h"

static int hsc_spi_xfer(struct hsc_data *data)
{
	struct spi_transfer xfer = {
		.tx_buf = NULL,
		.rx_buf = (char *)&data->buffer,
		.len = HSC_REG_MEASUREMENT_RD_SIZE,
	};
	int ret;

	ret = spi_sync_transfer(data->client, &xfer, 1);

	return ret;
}

static int hsc_spi_probe(struct spi_device *spi)
{
	struct iio_dev *indio_dev;
	struct hsc_data *hsc;
	const char *range_nom;
	int ret;
	struct device *dev = &spi->dev;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*hsc));
	if (!indio_dev)
		return -ENOMEM;

	spi_set_drvdata(spi, indio_dev);

	spi->mode = SPI_MODE_0;
	spi->max_speed_hz = min(spi->max_speed_hz, 800000U);
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret < 0)
		return ret;

	hsc = iio_priv(indio_dev);
	hsc->xfer = hsc_spi_xfer;
	hsc->client = spi;

	ret = devm_regulator_get_enable_optional(dev, "vdd");
	if (ret == -EPROBE_DEFER)
		return -EPROBE_DEFER;

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

	ret =
	    device_property_read_string(dev, "honeywell,range_str", &range_nom);
	if (ret)
		return dev_err_probe(dev, ret,
				     "honeywell,range_str not defined\n");

	// minimal input sanitization
	memcpy(hsc->range_str, range_nom, HSC_RANGE_STR_LEN - 1);
	hsc->range_str[HSC_RANGE_STR_LEN - 1] = 0;

	if (strcasecmp(hsc->range_str, "na") == 0) {
		// range string "not available"
		// we got a custom chip not covered by the nomenclature with a custom range
		ret = device_property_read_u32(dev, "honeywell,pmin-pascal",
					       &hsc->pmin);
		if (ret)
			return dev_err_probe(dev, ret,
					     "honeywell,pmin-pascal could not be read\n");
		ret = device_property_read_u32(dev, "honeywell,pmax-pascal",
					       &hsc->pmax);
		if (ret)
			return dev_err_probe(dev, ret,
					     "honeywell,pmax-pascal could not be read\n");
	}

	return hsc_probe(indio_dev, &spi->dev, spi_get_device_id(spi)->name,
			 spi_get_device_id(spi)->driver_data);
}

static const struct of_device_id hsc_spi_match[] = {
	{.compatible = "honeywell,hsc",},
	{.compatible = "honeywell,ssc",},
	{},
};

MODULE_DEVICE_TABLE(of, hsc_spi_match);

static const struct spi_device_id hsc_spi_id[] = {
	{"hsc", HSC},
	{"ssc", SSC},
	{}
};

MODULE_DEVICE_TABLE(spi, hsc_spi_id);

static struct spi_driver hsc_spi_driver = {
	.driver = {
		   .name = "honeywell_hsc",
		   .of_match_table = hsc_spi_match,
		   },
	.probe = hsc_spi_probe,
	.id_table = hsc_spi_id,
};

module_spi_driver(hsc_spi_driver);

MODULE_AUTHOR("Petre Rodan <petre.rodan@subdimension.ro>");
MODULE_DESCRIPTION("Honeywell HSC and SSC pressure sensor spi driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(IIO_HONEYWELL_HSC);
