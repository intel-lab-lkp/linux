// SPDX-License-Identifier: GPL-2.0+
/*
 * Analog Devices LTC2378 ADC series driver
 *
 * Copyright (C) 2026 Analog Devices Inc.
 * Author: Marcelo Schmitt <marcelo.schmitt@analog.com>
 */

#include <linux/err.h>
#include <linux/iio/iio.h>

#include "ltc2378.h"

int ltc2378_lib_buffer_setup(struct iio_dev *indio_dev, struct ltc2378_state *st)
{
	struct device *dev = &st->spi->dev;
	int ret;

	ret = __ltc2378_set_offload_ops(st);
	if (ret == -EOPNOTSUPP)
		return 0; /* Let device setup complete without buffer support */

	if (!ret)
		ret = st->ops->buffer_setup(indio_dev, st);

	if (ret)
		return dev_err_probe(dev, ret, "error on SPI offload setup\n");

	return 0;
}
EXPORT_SYMBOL_NS_GPL(ltc2378_lib_buffer_setup, "IIO_LTC2378");

MODULE_AUTHOR("Marcelo Schmitt <marcelo.schmitt@analog.com>");
MODULE_DESCRIPTION("Analog Devices LTC2378 ADC series driver");
MODULE_LICENSE("GPL");
