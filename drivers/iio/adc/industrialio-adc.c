// SPDX-License-Identifier: GPL-2.0-only
/*
 * Helpers for parsing common ADC information from a firmware node.
 *
 * Copyright (c) 2025 Matti Vaittinen <mazziesaccount@gmail.com>
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/property.h>

#include <linux/iio/adc-helpers.h>

int iio_adc_fwnode_num_channels(struct fwnode_handle *fwnode)
{
	struct fwnode_handle *child;
	int num_chan = 0;

	fwnode_for_each_child_node(fwnode, child)
		if (fwnode_name_eq(child, "channel"))
			num_chan++;

	return num_chan;
}
EXPORT_SYMBOL_GPL(iio_adc_fwnode_num_channels);

/**
 * iio_adc_device_get_channels - get ADC channel IDs
 *
 * Scan the device node for ADC channel information. Return an array of found
 * IDs. Caller need to allocate the memory for the array and provide maximum
 * number of IDs the array can store.
 *
 * @dev:		Pointer to the ADC device
 * @channels:		Array where the found IDs will be stored.
 * @max_channels:	Number of IDs that fit in the array.
 *
 * Return:		Number of found channels on succes. Negative value to
 *			indicate failure.
 */
int iio_adc_device_get_channels(struct device *dev, int *channels,
				int max_channels)
{
	struct fwnode_handle *fwnode, *child;
	int num_chan = 0, ret;

	fwnode = dev_fwnode(dev);
	if (!fwnode) {
		fwnode = dev_fwnode(dev->parent);
		if (!fwnode)
			return -ENODEV;
	}
	fwnode_for_each_child_node(fwnode, child) {
		if (fwnode_name_eq(child, "channel")) {
			u32 ch;

			if (num_chan == max_channels)
				return -EINVAL;

			ret = fwnode_property_read_u32(child, "reg", &ch);
			if (ret)
				return ret;

			/*
			 * We assume the channel IDs start from 0. If it seems
			 * this is not a sane assumption, then we can relax
			 * this check or add 'allowed ID range' parameter.
			 *
			 * Let's just start with this simple assumption.
			 */
			if (ch > max_channels)
				return -ERANGE;

			channels[num_chan] = ch;
			num_chan++;
		}
	}

	return num_chan;

}
EXPORT_SYMBOL_GPL(iio_adc_device_get_channels);

/**
 * devm_iio_adc_device_alloc_chaninfo - allocate and fill iio_chan_spec for adc
 *
 * Scan the device node for ADC channel information. Allocate and populate the
 * iio_chan_spec structure corresponding to channels that are found. The memory
 * for iio_chan_spec structure will be freed upon device detach. Try parent
 * device node if given device has no fwnode associated to cover also MFD
 * devices.
 *
 * @dev:	Pointer to the ADC device
 * @template:	Template iio_chan_spec from which the fields of all found and
 *		allocated channels are initialized.
 * @cs:		Location where pointer to allocated iio_chan_spec should be
 *		stored
 *
 * Return:	Number of found channels on succes. Negative value to indicate
 *		failure.
 */
int devm_iio_adc_device_alloc_chaninfo(struct device *dev,
				       const struct iio_chan_spec *template,
				       struct iio_chan_spec **cs)
{
	struct fwnode_handle *fwnode, *child;
	struct iio_chan_spec *chan;
	int num_chan = 0, ret;

	fwnode = dev_fwnode(dev);
	if (!fwnode) {
		fwnode = dev_fwnode(dev->parent);
		if (!fwnode)
			return -ENODEV;
	}

	num_chan = iio_adc_fwnode_num_channels(fwnode);
	if (num_chan < 0)
		return num_chan;

	*cs = devm_kcalloc(dev, num_chan, sizeof(**cs), GFP_KERNEL);
	if (!*cs)
		return -ENOMEM;

	chan = &(*cs)[0];

	fwnode_for_each_child_node(fwnode, child) {
		if (fwnode_name_eq(child, "channel")) {
			u32 ch;

			ret = fwnode_property_read_u32(child, "reg", &ch);
			if (ret)
				return ret;

			*chan = *template;
			chan->channel = ch;

			if (num_chan > 1)
				chan->indexed = 1;

			chan++;
		}
	}

	return num_chan;
}
EXPORT_SYMBOL_GPL(devm_iio_adc_device_alloc_chaninfo);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matti Vaittinen <mazziesaccount@gmail.com>");
MODULE_DESCRIPTION("IIO ADC fwnode parsing helpers");
