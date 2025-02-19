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

int iio_adc_device_num_channels(struct device *dev)
{
	int num_chan = 0;

	device_for_each_child_node_scoped(dev, child)
		if (fwnode_name_eq(child, "channel"))
			num_chan++;

	return num_chan;
}
EXPORT_SYMBOL_GPL(iio_adc_device_num_channels);

static const char *iio_adc_type2prop(int type)
{
	switch (type) {
	case IIO_ADC_CHAN_PROP_TYPE_REG:
		return "reg";
	case IIO_ADC_CHAN_PROP_TYPE_SINGLE_ENDED:
		return "single-channel";
	case IIO_ADC_CHAN_PROP_TYPE_DIFF:
		return "diff-channels";
	case IIO_ADC_CHAN_PROP_COMMON:
		return "common-mode-channel";
	default:
		return "unknown";
	}
}

/*
 * Sanity check. Ensure that:
 * - At least some type(s) are allowed
 * - All types found are also expected
 * - If plain "reg" is not allowed, either single-ended or differential
 *   properties are found.
 */
static int iio_adc_prop_type_check_sanity(struct device *dev,
		const struct iio_adc_props *expected_props, int found_types)
{
	unsigned long allowed_types = expected_props->allowed |
				      expected_props->required;

	if (!allowed_types || allowed_types & (~IIO_ADC_CHAN_PROP_TYPE_ALL)) {
		dev_dbg(dev, "Invalid adc allowed prop types 0x%lx\n",
			allowed_types);

		return -EINVAL;
	}
	if (found_types & (~allowed_types)) {
		long unknown_types = found_types & (~allowed_types);
		int type;

		for_each_set_bit(type, &unknown_types,
				 IIO_ADC_CHAN_NUM_PROP_TYPES - 1) {
			dev_err(dev, "Unsupported channel property %s\n",
				iio_adc_type2prop(type));
		}

		return -EINVAL;
	}

	/*
	 * The IIO_ADC_CHAN_PROP_TYPE_REG is special. We always require it to
	 * be found in the dt. (If not, we'll error out before calling this
	 * function.) However, listing it in 'allowed' types means the "reg"
	 * alone can be used to indicate the channel ID.
	 *
	 * Thus, we don't add it in the found properties either - so check for
	 * found and allowed properties passes even if user hasn't explicitly
	 * added the 'IIO_ADC_CHAN_PROP_TYPE_REG' to be allowed. (This is the
	 * case if either differential or single-ended property is required).
	 *
	 * Hence, for this check we need to explicitly add the
	 * IIO_ADC_CHAN_PROP_TYPE_REG to 'found' properties to make the check
	 * pass when "reg" is the property which is required to have the
	 * channel ID.
	 *
	 * We could of course always add the IIO_ADC_CHAN_PROP_TYPE_REG in
	 * allowed types and found types - but then we wouldn't catch the case
	 * where user says the "reg" alone is not sufficient.
	 */
	if ((~(found_types | IIO_ADC_CHAN_PROP_TYPE_REG)) & expected_props->required) {
		long missing_types;
		int type;

		missing_types = (~found_types) & expected_props->required;

		for_each_set_bit(type, &missing_types,
				 IIO_ADC_CHAN_NUM_PROP_TYPES - 1) {
			dev_err(dev, "required channel specifier '%s' not found\n",
				iio_adc_type2prop(type));
		}

		return -EINVAL;
	}

	/* Check if we require something else but the "reg" property */
	if (!(allowed_types & IIO_ADC_CHAN_PROP_TYPE_REG)) {
		if (found_types & IIO_ADC_CHAN_PROP_TYPE_SINGLE_ENDED ||
				found_types & IIO_ADC_CHAN_PROP_TYPE_DIFF)
			return 0;

		dev_err(dev, "channel specifier not found\n");

		return -EINVAL;
	}

	return 0;
}

/**
 * iio_adc_device_channels_by_property - get ADC channel IDs
 *
 * Scan the device node for ADC channel information. Return an array of found
 * IDs. Caller needs to provide the memory for the array and provide maximum
 * number of IDs the array can store.
 *
 * @dev:		Pointer to the ADC device
 * @channels:		Array where the found IDs will be stored.
 * @max_channels:	Number of IDs that fit in the array.
 * @expected_props:	Bitmaps of channel property types (for checking).
 *
 * Return:		Number of found channels on succes. 0 if no channels
 *			was found. Negative value to indicate failure.
 */
int iio_adc_device_channels_by_property(struct device *dev, int *channels,
		int max_channels, const struct iio_adc_props *expected_props)
{
	int num_chan = 0, ret;

	device_for_each_child_node_scoped(dev, child) {
		u32 ch, diff[2], se;
		struct iio_adc_props tmp;
		int chtypes_found = 0;

		if (!fwnode_name_eq(child, "channel"))
			continue;

		if (num_chan == max_channels)
			return -EINVAL;

		ret = fwnode_property_read_u32(child, "reg", &ch);
		if (ret)
			return ret;

		ret = fwnode_property_read_u32_array(child, "diff-channels",
						     &diff[0], 2);
		if (!ret)
			chtypes_found |= IIO_ADC_CHAN_PROP_TYPE_DIFF;

		ret = fwnode_property_read_u32(child, "single-channel", &se);
		if (!ret)
			chtypes_found |= IIO_ADC_CHAN_PROP_TYPE_SINGLE_ENDED;

		tmp = *expected_props;
		/*
		 * We don't bother reading the "common-mode-channel" here as it
		 * doesn't really affect on the primary channel ID. We remove
		 * it from the required properties to allow the sanity check
		 * pass here  also for drivers which require it.
		 */
		tmp.required &= (~BIT(IIO_ADC_CHAN_PROP_COMMON));

		ret = iio_adc_prop_type_check_sanity(dev, &tmp, chtypes_found);
		if (ret)
			return ret;

		if (chtypes_found & IIO_ADC_CHAN_PROP_TYPE_DIFF)
			ch = diff[0];
		else if (chtypes_found & IIO_ADC_CHAN_PROP_TYPE_SINGLE_ENDED)
			ch = se;

		/*
		 * We assume the channel IDs start from 0. If it seems this is
		 * not a sane assumption, then we can relax this check or add
		 * 'allowed ID range' parameter.
		 *
		 * Let's just start with this simple assumption.
		 */
		if (ch >= max_channels)
			return -ERANGE;

		channels[num_chan] = ch;
		num_chan++;
	}

	return num_chan;

}
EXPORT_SYMBOL_GPL(iio_adc_device_channels_by_property);

/**
 * devm_iio_adc_device_alloc_chaninfo - allocate and fill iio_chan_spec for adc
 *
 * Scan the device node for ADC channel information. Allocate and populate the
 * iio_chan_spec structure corresponding to channels that are found. The memory
 * for iio_chan_spec structure will be freed upon device detach. Try parent
 * device node if given device has no fwnode associated to cover also MFD
 * devices.
 *
 * @dev:		Pointer to the ADC device.
 * @template:		Template iio_chan_spec from which the fields of all
 *			found and allocated channels are initialized.
 * @cs:			Location where pointer to allocated iio_chan_spec
 *			should be stored.
 * @expected_props:	Bitmaps of channel property types (for checking).
 *
 * Return:	Number of found channels on succes. Negative value to indicate
 *		failure.
 */
int devm_iio_adc_device_alloc_chaninfo(struct device *dev,
				const struct iio_chan_spec *template,
				struct iio_chan_spec **cs,
				const struct iio_adc_props *expected_props)
{
	struct iio_chan_spec *chan;
	int num_chan = 0, ret;

	num_chan = iio_adc_device_num_channels(dev);
	if (num_chan < 1)
		return num_chan;

	*cs = devm_kcalloc(dev, num_chan, sizeof(**cs), GFP_KERNEL);
	if (!*cs)
		return -ENOMEM;

	chan = &(*cs)[0];

	device_for_each_child_node_scoped(dev, child) {
		u32 ch, diff[2], se, common;
		int chtypes_found = 0;

		if (!fwnode_name_eq(child, "channel"))
			continue;

		ret = fwnode_property_read_u32(child, "reg", &ch);
		if (ret)
			return ret;

		ret = fwnode_property_read_u32_array(child, "diff-channels",
						     &diff[0], 2);
		if (!ret)
			chtypes_found |= IIO_ADC_CHAN_PROP_TYPE_DIFF;

		ret = fwnode_property_read_u32(child, "single-channel", &se);
		if (!ret)
			chtypes_found |= IIO_ADC_CHAN_PROP_TYPE_SINGLE_ENDED;

		ret = fwnode_property_read_u32(child, "common-mode-channel",
					       &common);
		if (!ret)
			chtypes_found |= BIT(IIO_ADC_CHAN_PROP_COMMON);

		ret = iio_adc_prop_type_check_sanity(dev, expected_props,
						     chtypes_found);
		if (ret)
			return ret;

		*chan = *template;
		chan->channel = ch;

		if (chtypes_found & IIO_ADC_CHAN_PROP_TYPE_DIFF) {
			chan->differential = 1;
			chan->channel = diff[0];
			chan->channel2 = diff[1];

		} else if (chtypes_found & IIO_ADC_CHAN_PROP_TYPE_SINGLE_ENDED) {
			chan->channel = se;
			if (chtypes_found & BIT(IIO_ADC_CHAN_PROP_COMMON))
				chan->channel2 = common;
		}

		/*
		 * We assume the channel IDs start from 0. If it seems this is
		 * not a sane assumption, then we have to add 'allowed ID ranges'
		 * to the struct iio_adc_props because some of the callers may
		 * rely on the IDs being in this range - and have arrays indexed
		 * by the ID.
		 */
		if (chan->channel >= num_chan)
			return -ERANGE;

		chan++;
	}

	return num_chan;
}
EXPORT_SYMBOL_GPL(devm_iio_adc_device_alloc_chaninfo);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matti Vaittinen <mazziesaccount@gmail.com>");
MODULE_DESCRIPTION("IIO ADC fwnode parsing helpers");
