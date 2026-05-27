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
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/units.h>

#include <linux/iio/iio.h>

#include "versal-sysmon.h"

/*
 * Both RAW and PROCESSED are exposed: RAW is needed for event thresholds
 * (which operate in hardware register format), PROCESSED gives userspace
 * the converted millivolt or millicelsius value.
 */
#define SYSMON_CHAN_TEMP(_chan, _address, _name) {		\
	.type = IIO_TEMP,					\
	.indexed = 1,						\
	.address = _address,					\
	.channel = _chan,					\
	.info_mask_separate =					\
		BIT(IIO_CHAN_INFO_RAW) |				\
		BIT(IIO_CHAN_INFO_PROCESSED),			\
	.scan_type = {						\
		.sign = 's',					\
		.realbits = 15,					\
		.storagebits = 16,				\
		.endianness = IIO_CPU,				\
	},							\
	.datasheet_name = _name,				\
}

/* Static temperature channels (always present) */
static const struct iio_chan_spec temp_channels[] = {
	SYSMON_CHAN_TEMP(0, SYSMON_TEMP_MAX, "temp"),
	SYSMON_CHAN_TEMP(1, SYSMON_TEMP_MIN, "min"),
	SYSMON_CHAN_TEMP(2, SYSMON_TEMP_MAX_MAX, "max_max"),
	SYSMON_CHAN_TEMP(3, SYSMON_TEMP_MIN_MIN, "min_min"),
};

static void sysmon_q8p7_to_millicelsius(s16 raw_data, int *val)
{
	*val = (raw_data * (int)MILLI) >> SYSMON_FRACTIONAL_SHIFT;
}

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

	if (mask != IIO_CHAN_INFO_RAW && mask != IIO_CHAN_INFO_PROCESSED)
		return -EINVAL;

	guard(mutex)(&sysmon->lock);

	switch (chan->type) {
	case IIO_TEMP:
		ret = regmap_read(sysmon->regmap, chan->address, &regval);
		if (ret)
			return ret;
		if (mask == IIO_CHAN_INFO_PROCESSED)
			sysmon_q8p7_to_millicelsius(regval, val);
		else
			*val = regval;
		return IIO_VAL_INT;

	case IIO_VOLTAGE:
		ret = regmap_read(sysmon->regmap,
				  (chan->address * SYSMON_REG_STRIDE) +
				  SYSMON_SUPPLY_BASE, &regval);
		if (ret)
			return ret;
		if (mask == IIO_CHAN_INFO_PROCESSED)
			sysmon_supply_rawtoprocessed(regval, val);
		else
			*val = regval;
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
	struct fwnode_handle *supply_node __free(fwnode_handle) =
		device_get_named_child_node(dev, "voltage-channels");
	struct fwnode_handle *temp_node __free(fwnode_handle) =
		device_get_named_child_node(dev, "temperature-channels");
	unsigned int num_supply = 0, num_temp = 0;
	unsigned int idx, temp_chan_idx, volt_chan_idx;
	struct iio_chan_spec *sysmon_channels;
	const char *label;
	u32 reg;
	int ret;

	if (supply_node)
		num_supply = fwnode_get_child_node_count(supply_node);
	if (temp_node)
		num_temp = fwnode_get_child_node_count(temp_node);

	sysmon_channels = devm_kcalloc(dev,
				       size_add(ARRAY_SIZE(temp_channels),
						num_supply + num_temp),
				       sizeof(*sysmon_channels), GFP_KERNEL);
	if (!sysmon_channels)
		return -ENOMEM;

	/* Static temperature channels first (fixed indices) */
	idx = 0;
	memcpy(sysmon_channels, temp_channels, sizeof(temp_channels));
	idx += ARRAY_SIZE(temp_channels);

	/* Supply channels from DT */
	fwnode_for_each_child_node_scoped(supply_node, child) {
		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "missing reg for supply channel\n");

		if (reg > SYSMON_SUPPLY_IDX_MAX)
			return dev_err_probe(dev, -EINVAL,
					     "supply reg %u exceeds max %u\n",
					     reg, SYSMON_SUPPLY_IDX_MAX);

		ret = fwnode_property_read_string(child, "label", &label);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "missing label for supply channel\n");

		sysmon_channels[idx++] = (struct iio_chan_spec) {
			.type = IIO_VOLTAGE,
			.indexed = 1,
			.address = reg,
			.info_mask_separate =
				BIT(IIO_CHAN_INFO_RAW) |
				BIT(IIO_CHAN_INFO_PROCESSED),
			.scan_type = {
				.realbits = 19,
				.storagebits = 32,
				.endianness = IIO_CPU,
				.sign = fwnode_property_read_bool(child,
					"bipolar") ? 's' : 'u',
			},
			.datasheet_name = label,
		};
	}

	/* Temperature satellite channels from DT */
	fwnode_for_each_child_node_scoped(temp_node, child) {
		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "missing reg for temp channel\n");

		if (reg < 1 || reg > SYSMON_TEMP_SAT_MAX)
			return dev_err_probe(dev, -EINVAL,
					     "temp reg %u out of range [1..%u]\n",
					     reg, SYSMON_TEMP_SAT_MAX);

		ret = fwnode_property_read_string(child, "label", &label);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "missing label for temp channel\n");

		sysmon_channels[idx++] = (struct iio_chan_spec) {
			.type = IIO_TEMP,
			.indexed = 1,
			.address = SYSMON_TEMP_SAT_BASE +
				   ((reg - 1) * SYSMON_REG_STRIDE),
			.info_mask_separate =
				BIT(IIO_CHAN_INFO_RAW) |
				BIT(IIO_CHAN_INFO_PROCESSED),
			.scan_type = {
				.sign = 's',
				.realbits = 15,
				.storagebits = 16,
				.endianness = IIO_CPU,
			},
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
	for (idx = 0; idx < indio_dev->num_channels; idx++) {
		if (sysmon_channels[idx].type == IIO_TEMP)
			sysmon_channels[idx].channel = temp_chan_idx++;
		else
			sysmon_channels[idx].channel = volt_chan_idx++;
	}

	indio_dev->channels = sysmon_channels;

	return 0;
}

/**
 * sysmon_core_probe() - Initialize Versal SysMon core
 * @dev: Parent device
 * @regmap: Register map for hardware access
 *
 * Return: 0 on success, negative errno on failure.
 */
int sysmon_core_probe(struct device *dev, struct regmap *regmap)
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
EXPORT_SYMBOL_GPL(sysmon_core_probe);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AMD Versal SysMon Core Driver");
MODULE_AUTHOR("Salih Erim <salih.erim@amd.com>");
