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

enum {
	IIO_ADC_CHAN_PROP_REG,
	IIO_ADC_CHAN_PROP_SINGLE_ENDED,
	IIO_ADC_CHAN_PROP_DIFF,
	IIO_ADC_CHAN_PROP_COMMON,
	IIO_ADC_CHAN_NUM_PROP_TYPES
};

/*
 * Channel property types to be used with iio_adc_device_get_channels,
 * devm_iio_adc_device_alloc_chaninfo, ...
 */
#define IIO_ADC_CHAN_PROP_TYPE_REG BIT(IIO_ADC_CHAN_PROP_REG)
#define IIO_ADC_CHAN_PROP_TYPE_SINGLE_ENDED BIT(IIO_ADC_CHAN_PROP_SINGLE_ENDED)
#define IIO_ADC_CHAN_PROP_TYPE_SINGLE_COMMON					\
	(BIT(IIO_ADC_CHAN_PROP_SINGLE_ENDED) | BIT(IIO_ADC_CHAN_PROP_COMMON))
#define IIO_ADC_CHAN_PROP_TYPE_DIFF BIT(IIO_ADC_CHAN_PROP_DIFF)
#define IIO_ADC_CHAN_PROP_TYPE_ALL GENMASK(IIO_ADC_CHAN_NUM_PROP_TYPES - 1, 0)

/**
 * iio_adc_chan_props - information of expected device-tree channel properties
 *
 * @required:	Bitmask of property definitions of required channel properties
 * @allowed:	Bitmask of property definitions of optional channel properties.
 *		Listing of required properties is not needed here.
 */
struct iio_adc_props {
	unsigned long required;
	unsigned long allowed;
};

int iio_adc_device_num_channels(struct device *dev);
int devm_iio_adc_device_alloc_chaninfo(struct device *dev,
				const struct iio_chan_spec *template,
				struct iio_chan_spec **cs,
				const struct iio_adc_props *expected_props);

int iio_adc_device_channels_by_property(struct device *dev, int *channels,
				int max_channels,
				const struct iio_adc_props *expected_props);
#endif /* _INDUSTRIAL_IO_ADC_HELPERS_H_ */
