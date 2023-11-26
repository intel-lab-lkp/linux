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

#include "hsc030pa.h"

static int hsc_spi_xfer(struct hsc_data *data)
{
	struct spi_transfer xfer = {
		.tx_buf = NULL,
		.rx_buf = (char *)&data->buffer,
		.len = HSC_REG_MEASUREMENT_RD_SIZE,
	};

	return spi_sync_transfer(data->client, &xfer, 1);
}

static int hsc_spi_probe(struct spi_device *spi)
{
	struct iio_dev *indio_dev;
	struct hsc_data *hsc;
	struct device *dev = &spi->dev;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*hsc));
	if (!indio_dev)
		return -ENOMEM;

	hsc = iio_priv(indio_dev);
	hsc->xfer = hsc_spi_xfer;
	hsc->client = spi;

	return hsc_probe(indio_dev, &spi->dev, spi_get_device_id(spi)->name,
			 spi_get_device_id(spi)->driver_data);
}

static const struct of_device_id hsc_spi_match[] = {
	{.compatible = "honeywell,hsc030pa",},
	{}
};
MODULE_DEVICE_TABLE(of, hsc_spi_match);

static const struct spi_device_id hsc_spi_id[] = {
	{"hsc030pa"},
	{}
};
MODULE_DEVICE_TABLE(spi, hsc_spi_id);

static struct spi_driver hsc_spi_driver = {
	.driver = {
		.name = "hsc030pa",
		.of_match_table = hsc_spi_match,
		},
	.probe = hsc_spi_probe,
	.id_table = hsc_spi_id,
};
module_spi_driver(hsc_spi_driver);

MODULE_AUTHOR("Petre Rodan <petre.rodan@subdimension.ro>");
MODULE_DESCRIPTION("Honeywell HSC and SSC pressure sensor spi driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(IIO_HONEYWELL_HSC030PA);
