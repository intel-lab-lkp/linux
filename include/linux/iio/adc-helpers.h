/* SPDX-License-Identifier: GPL-2.0-only */

/* The industrial I/O ADC helpers
 *
 * Copyright (c) 2025 Matti Vaittinen <mazziesaccount@gmail.com>
 */

#ifndef _INDUSTRIAL_IO_ADC_HELPERS_H_
#define _INDUSTRIAL_IO_ADC_HELPERS_H_

#include <linux/iio/iio.h>

struct device;
struct fwnode_handle;

int iio_adc_fwnode_num_channels(struct fwnode_handle *fwnode);
int devm_iio_adc_device_alloc_chaninfo(struct device *dev,
				       const struct iio_chan_spec *template,
				       struct iio_chan_spec **cs);
int iio_adc_device_get_channels(struct device *dev, int *channels,
				int max_channels);
#endif /* _INDUSTRIAL_IO_ADC_HELPERS_H_ */
