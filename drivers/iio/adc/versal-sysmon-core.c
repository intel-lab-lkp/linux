// SPDX-License-Identifier: GPL-2.0
/*
 * AMD Versal SysMon core driver
 *
 * Copyright (C) 2019 - 2022, Xilinx, Inc.
 * Copyright (C) 2022 - 2026, Advanced Micro Devices, Inc.
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/units.h>

#include <linux/iio/iio.h>

#include "versal-sysmon.h"

#define SYSMON_CHAN_TEMP(_chan, _address, _name)		\
{								\
	.type = IIO_TEMP,					\
	.indexed = 1,						\
	.address = _address,					\
	.channel = _chan,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
	.datasheet_name = _name,				\
}

/*
 * Static temperature channels (always present).
 *
 * These are hardware-computed aggregate registers across all active
 * temperature satellites:
 *   temp:     current max temperature across all active satellites
 *   min:      current min temperature across all active satellites
 *   max_max:  highest peak recorded since last hardware reset
 *   min_min:  lowest trough recorded since last hardware reset
 */
static const struct iio_chan_spec temp_channels[] = {
	SYSMON_CHAN_TEMP(0, SYSMON_TEMP_MAX, "temp"),
	SYSMON_CHAN_TEMP(1, SYSMON_TEMP_MIN, "min"),
	SYSMON_CHAN_TEMP(2, SYSMON_TEMP_MAX_MAX, "max_max"),
	SYSMON_CHAN_TEMP(3, SYSMON_TEMP_MIN_MIN, "min_min"),
};

static void sysmon_supply_rawtoprocessed(int raw_data, int *val)
{
	int mantissa, format, exponent;

	mantissa = FIELD_GET(SYSMON_MANTISSA_MASK, raw_data);
	exponent = SYSMON_SUPPLY_MANTISSA_BITS - FIELD_GET(SYSMON_MODE_MASK, raw_data);
	format = FIELD_GET(SYSMON_FMT_MASK, raw_data);
	/*
	 * When format bit is set the mantissa is two's complement
	 * (per hardware spec); sign-extend to int for correct arithmetic.
	 */
	if (format)
		mantissa = sign_extend32(mantissa, 15);

	*val = (mantissa * (int)MILLI) >> exponent;
}

static int sysmon_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct sysmon *sysmon = iio_priv(indio_dev);
	unsigned int regval;
	int ret;

	guard(mutex)(&sysmon->lock);

	switch (chan->type) {
	case IIO_TEMP:
		if (mask == IIO_CHAN_INFO_SCALE) {
			/* Q8.7 to millicelsius: raw * 1000 / 128 */
			*val = MILLIDEGREE_PER_DEGREE;
			*val2 = BIT(SYSMON_FRACTIONAL_SHIFT);
			return IIO_VAL_FRACTIONAL;
		}
		if (mask != IIO_CHAN_INFO_RAW)
			return -EINVAL;

		ret = regmap_read(sysmon->regmap, chan->address, &regval);
		if (ret)
			return ret;

		*val = sign_extend32(regval, 15);
		return IIO_VAL_INT;

	case IIO_VOLTAGE:
		if (mask != IIO_CHAN_INFO_PROCESSED)
			return -EINVAL;

		ret = regmap_read(sysmon->regmap,
				  chan->address * SYSMON_REG_STRIDE +
				  SYSMON_SUPPLY_BASE, &regval);
		if (ret)
			return ret;

		sysmon_supply_rawtoprocessed(regval, val);
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static int sysmon_read_label(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     char *label)
{
	if (chan->datasheet_name)
		return sysfs_emit(label, "%s\n", chan->datasheet_name);

	return -EINVAL;
}

static const struct iio_info sysmon_iio_info = {
	.read_raw = sysmon_read_raw,
	.read_label = sysmon_read_label,
};

/**
 * sysmon_parse_fw() - Parse firmware nodes and configure IIO channels.
 * @indio_dev: IIO device instance
 * @dev: Parent device
 *
 * Reads voltage-channels and temperature-channels container nodes from
 * firmware and builds the IIO channel array. Static temperature channels
 * are prepended, followed by supply and satellite channels from DT.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int sysmon_parse_fw(struct iio_dev *indio_dev, struct device *dev)
{
	unsigned int num_chan, num_static, num_supply, num_temp;
	unsigned int idx, temp_chan_idx, volt_chan_idx;
	struct iio_chan_spec *sysmon_channels;
	const char *label;
	u32 reg;
	int ret;

	struct fwnode_handle *supply_node __free(fwnode_handle) =
		device_get_named_child_node(dev, "voltage-channels");
	num_supply = fwnode_get_child_node_count(supply_node);

	struct fwnode_handle *temp_node __free(fwnode_handle) =
		device_get_named_child_node(dev, "temperature-channels");
	num_temp = fwnode_get_child_node_count(temp_node);

	num_static = ARRAY_SIZE(temp_channels);
	num_chan = size_add(num_temp, size_add(num_static, num_supply));
	sysmon_channels = devm_kcalloc(dev, num_chan, sizeof(*sysmon_channels), GFP_KERNEL);
	if (!sysmon_channels)
		return -ENOMEM;

	/* Static temperature channels first */
	memcpy(sysmon_channels, temp_channels, sizeof(temp_channels));
	idx = num_static;

	/* Supply channels from DT */
	fwnode_for_each_child_node_scoped(supply_node, child) {
		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret)
			return dev_err_probe(dev, ret,
					     "missing reg for supply channel\n");

		if (reg > SYSMON_SUPPLY_IDX_MAX)
			return dev_err_probe(dev, -EINVAL,
					     "supply reg %u exceeds max %u\n",
					     reg, SYSMON_SUPPLY_IDX_MAX);

		ret = fwnode_property_read_string(child, "label", &label);
		if (ret)
			return dev_err_probe(dev, ret,
					     "missing label for supply channel\n");

		sysmon_channels[idx++] = (struct iio_chan_spec) {
			.type = IIO_VOLTAGE,
			.indexed = 1,
			.address = reg,
			.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
			.datasheet_name = label,
		};
	}

	/* Temperature satellite channels from DT */
	fwnode_for_each_child_node_scoped(temp_node, child) {
		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret)
			return dev_err_probe(dev, ret,
					     "missing reg for temp channel\n");

		if (reg < 1 || reg > SYSMON_TEMP_SAT_MAX)
			return dev_err_probe(dev, -EINVAL,
					     "temp reg %u out of range [1..%u]\n",
					     reg, SYSMON_TEMP_SAT_MAX);

		ret = fwnode_property_read_string(child, "label", &label);
		if (ret)
			return dev_err_probe(dev, ret,
					     "missing label for temp channel\n");

		sysmon_channels[idx++] = (struct iio_chan_spec) {
			.type = IIO_TEMP,
			.indexed = 1,
			.address = SYSMON_TEMP_SAT_BASE +
				   (reg - 1) * SYSMON_REG_STRIDE,
			.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
			.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
			.datasheet_name = label,
		};
	}

	indio_dev->num_channels = idx;
	indio_dev->info = &sysmon_iio_info;

	/*
	 * Assign per-type sequential channel numbers.
	 * IIO sysfs uses type prefix (in_tempN, in_voltageN)
	 * so numbers only need to be unique within each type.
	 */
	temp_chan_idx = 0;
	volt_chan_idx = 0;
	for (unsigned int idx = 0; idx < indio_dev->num_channels; idx++) {
		if (sysmon_channels[idx].type == IIO_TEMP)
			sysmon_channels[idx].channel = temp_chan_idx++;
		else
			sysmon_channels[idx].channel = volt_chan_idx++;
	}

	indio_dev->channels = sysmon_channels;

	return 0;
}

/**
 * devm_versal_sysmon_core_probe() - Initialize Versal SysMon core
 * @dev: Parent device
 * @regmap: Register map for hardware access
 *
 * Return: 0 on success, negative errno on failure.
 */
int devm_versal_sysmon_core_probe(struct device *dev, struct regmap *regmap)
{
	struct iio_dev *indio_dev;
	struct sysmon *sysmon;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*sysmon));
	if (!indio_dev)
		return -ENOMEM;

	sysmon = iio_priv(indio_dev);
	sysmon->regmap = regmap;

	ret = devm_mutex_init(dev, &sysmon->lock);
	if (ret)
		return ret;

	/* Disable all interrupts and clear pending status */
	ret = regmap_write(sysmon->regmap, SYSMON_IDR, SYSMON_INTR_ALL_MASK);
	if (ret)
		return ret;
	ret = regmap_write(sysmon->regmap, SYSMON_ISR, SYSMON_INTR_ALL_MASK);
	if (ret)
		return ret;

	indio_dev->name = "versal-sysmon";
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = sysmon_parse_fw(indio_dev, dev);
	if (ret)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}
EXPORT_SYMBOL_NS_GPL(devm_versal_sysmon_core_probe, "VERSAL_SYSMON");

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AMD Versal SysMon Core Driver");
MODULE_AUTHOR("Salih Erim <salih.erim@amd.com>");
