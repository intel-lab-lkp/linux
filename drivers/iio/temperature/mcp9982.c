// SPDX-License-Identifier: GPL-2.0+
/*
 * IIO driver for MCP998X/33 and MCP998XD/33D Multichannel Automotive Temperature Monitor Family
 *
 * Copyright (C) 2025 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Victor Duicu <victor.duicu@microchip.com>
 *
 * Datasheet can be found here:
 * https://ww1.microchip.com/downloads/aemDocuments/documents/MSLD/ProductDocuments/DataSheets/MCP998X-Family-Data-Sheet-DS20006827.pdf
 *
 */

#include <linux/bitfield.h>
#include <linux/i2c.h>
#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/irq.h>
#include <linux/limits.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/units.h>

/* MCP9982 Registers */
#define MCP9982_INT_HIGH_BYTE_ADDR(index)	(2 * (index))
#define MCP9982_INT_LOW_BYTE_ADDR(index)	(2 * (index) + 1)
#define MCP9982_ONE_SHOT_ADDR			0x0A
#define MCP9982_INT_HIGH_LIMIT_ADDR		0x0B
#define MCP9982_INT_LOW_LIMIT_ADDR		0x0C
#define MCP9982_EXT1_HIGH_LIMIT_HIGH_BYTE_ADDR	0x0D
#define MCP9982_EXT1_HIGH_LIMIT_LOW_BYTE_ADDR	0x0E
#define MCP9982_EXT1_LOW_LIMIT_HIGH_BYTE_ADDR	0x0F
#define MCP9982_EXT1_LOW_LIMIT_LOW_BYTE_ADDR	0x10
#define MCP9982_INT_THERM_LIMIT_ADDR		0x1D
#define MCP9982_EXT1_THERM_LIMIT_ADDR		0x1E
#define MCP9982_CFG_ADDR			0x22
#define MCP9982_CONV_ADDR			0x24
#define MCP9982_HYS_ADDR			0x25
#define MCP9982_CONSEC_ALRT_ADDR		0x26
#define MCP9982_ALRT_CFG_ADDR			0x27
#define MCP9982_RUNNING_AVG_ADDR		0x28
#define MCP9982_HOTTEST_CFG_ADDR		0x29
#define MCP9982_STATUS_ADDR			0x2A
#define MCP9982_EXT_FAULT_STATUS_ADDR		0x2B
#define MCP9982_HIGH_LIMIT_STATUS_ADDR		0x2C
#define MCP9982_LOW_LIMIT_STATUS_ADDR		0x2D
#define MCP9982_THERM_LIMIT_STATUS_ADDR		0x2E
#define MCP9982_HOTTEST_HIGH_BYTE_ADDR		0x2F
#define MCP9982_HOTTEST_LOW_BYTE_ADDR		0x30
#define MCP9982_HOTTEST_STATUS_ADDR		0x31
#define MCP9982_THERM_SHTDWN_CFG_ADDR		0x32
#define MCP9982_HRDW_THERM_SHTDWN_LIMIT_ADDR	0x33
#define MCP9982_EXT_BETA_CFG_ADDR(index)	((index) + 52)
#define MCP9982_EXT_IDEAL_ADDR(index)		((index) + 54)

/* MCP9982 Bits */
#define MCP9982_CFG_MSKAL			BIT(7)
#define MCP9982_CFG_RS				BIT(6)
#define MCP9982_CFG_ATTHM			BIT(5)
#define MCP9982_CFG_RECD12			BIT(4)
#define MCP9982_CFG_RECD34			BIT(3)
#define MCP9982_CFG_RANGE			BIT(2)
#define MCP9982_CFG_DA_ENA			BIT(1)
#define MCP9982_CFG_APDD			BIT(0)

/* MCP9982 Default Values */
#define MCP9982_TEMP_OFFSET			64
#define MCP9982_CFG_MSKAL_DEFAULT		1
#define MCP9982_CFG_RS_DEFAULT			1
#define MCP9982_CFG_ATTHM_DEFAULT		1
#define MCP9982_CFG_DA_ENA_DEFAULT		0
#define MCP9982_CONSEC_ALRT_DEFAULT		112
#define MCP9982_SAMPLING_FREQ_CODE_DEFAULT	6
#define MCP9982_BETA_AUTODETECT_ENABLE		16
#define MCP9982_BETA_MAX_VALUE			15
#define MCP9982_IDEALITY_MAX_VALUE		63
#define MCP9982_HYSTERESIS_MAX_VALUE		255
#define MCP9982_IDEALITY_FACTOR_DEFAULT		18
#define MCP9982_HYSTERESIS_DEFAULT		10
#define MCP9982_TEMP_RANGE_DEFAULT		0
#define MCP9982_APDD_DEFAULT			1
#define MCP9982_RECD_DEFAULT			1

#define MCP9982_NR_CUSTOM_ATTR			2
#define MCP9982_DEV_ATTR(name) (&iio_dev_attr_##name.dev_attr.attr)

/* Running average window can be set as:
 * - 1 sample
 * - 4 samples
 * - 8 samples
 */
#define MCP9982_RUNNING_AVERAGE_WINDOW_AVAILABLE	"1 4 8"

/* The maximum number of channels a member of the family can have */
#define MCP9982_MAX_NUM_CHANNELS		5

#define MCP9982_CHAN(index, si, __address) ({ \
	struct iio_chan_spec __chan = { \
		.type = IIO_TEMP, \
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
		.info_mask_shared_by_all_available = BIT(IIO_CHAN_INFO_SAMP_FREQ), \
		.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ), \
		.channel = index, \
		.address = __address, \
		.scan_index = si, \
		.scan_type = { \
			.sign = 'u', \
			.realbits = 8, \
			.storagebits = 8, \
			.endianness = IIO_CPU \
		}, \
		.indexed = 1, \
	}; \
	__chan; \
})

enum mcp9982_ids {
	MCP9933,
	MCP9933D,
	MCP9982,
	MCP9982D,
	MCP9983,
	MCP9983D,
	MCP9984,
	MCP9984D,
	MCP9985,
	MCP9985D,
};

/*
 * struct mcp9982_features - features of a mcp9982 instance
 * @phys_channels:	number of physical channels supported by the chip
 * @name:		chip's name
 */
struct mcp9982_features {
	u8		phys_channels;
	const char	*name;
};

static const struct mcp9982_features mcp9982_chip_config[] = {
	[MCP9933] = {
	    .name = "mcp9933",
	    .phys_channels = 3,
	},
	[MCP9933D] = {
	    .name = "mcp9933D",
	    .phys_channels = 3,
	},
	[MCP9982] = {
	    .name = "mcp9982",
	    .phys_channels = 2,
	},
	[MCP9982D] = {
	    .name = "mcp9982D",
	    .phys_channels = 2,
	},
	[MCP9983] = {
	    .name = "mcp9983",
	    .phys_channels = 3,
	},
	[MCP9983D] = {
	    .name = "mcp9983D",
	    .phys_channels = 3,
	},
	[MCP9984] = {
	    .name = "mcp9984",
	    .phys_channels = 4,
	},
	[MCP9984D] = {
	    .name = "mcp9984D",
	    .phys_channels = 4,
	},
	[MCP9985] = {
	    .name = "mcp9985",
	    .phys_channels = 5,
	},
	[MCP9985D] = {
	    .name = "mcp9985D",
	    .phys_channels = 5,
	},
};

static const int mcp9982_fractional_values[] = {
	0,
	125000,
	250000,
	375000,
	500000,
	625000,
	750000,
	875000,
};

static const int mcp9982_conv_rate[][2] = {
	{0, 62500},
	{0, 125000},
	{0, 250000},
	{0, 500000},
	{1, 0},
	{2, 0},
	{4, 0},
	{8, 0},
	{16, 0},
	{32, 0},
	{64, 0},
};

/* mcp9982 regmap configuration */
static const struct regmap_range mcp9982_regmap_wr_ranges[] = {
	regmap_reg_range(MCP9982_ONE_SHOT_ADDR, MCP9982_EXT1_LOW_LIMIT_LOW_BYTE_ADDR),
	regmap_reg_range(MCP9982_INT_THERM_LIMIT_ADDR, MCP9982_EXT1_THERM_LIMIT_ADDR),
	regmap_reg_range(MCP9982_CFG_ADDR, MCP9982_CFG_ADDR),
	regmap_reg_range(MCP9982_CONV_ADDR, MCP9982_HOTTEST_CFG_ADDR),
	regmap_reg_range(MCP9982_THERM_SHTDWN_CFG_ADDR, MCP9982_THERM_SHTDWN_CFG_ADDR),
	regmap_reg_range(MCP9982_EXT_BETA_CFG_ADDR(0), MCP9982_EXT_IDEAL_ADDR(3)),
};

static const struct regmap_access_table mcp9982_regmap_wr_table = {
	.yes_ranges = mcp9982_regmap_wr_ranges,
	.n_yes_ranges = ARRAY_SIZE(mcp9982_regmap_wr_ranges),
};

static const struct regmap_range mcp9982_regmap_rd_ranges[] = {
	regmap_reg_range(MCP9982_INT_HIGH_BYTE_ADDR(0), MCP9982_EXT1_LOW_LIMIT_LOW_BYTE_ADDR),
	regmap_reg_range(MCP9982_INT_THERM_LIMIT_ADDR, MCP9982_EXT1_THERM_LIMIT_ADDR),
	regmap_reg_range(MCP9982_CFG_ADDR, MCP9982_CFG_ADDR),
	regmap_reg_range(MCP9982_CONV_ADDR, MCP9982_EXT_IDEAL_ADDR(3)),
};

static const struct regmap_access_table mcp9982_regmap_rd_table = {
	.yes_ranges = mcp9982_regmap_rd_ranges,
	.n_yes_ranges = ARRAY_SIZE(mcp9982_regmap_rd_ranges),
};

static const struct regmap_config mcp9982_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.rd_table = &mcp9982_regmap_rd_table,
	.wr_table = &mcp9982_regmap_wr_table,
};

enum mcp9982_channels {
	MCP9982_CHAN_INT = 0,
	MCP9982_CHAN_EXT1 = 1,
	MCP9982_CHAN_EXT2 = 2,
	MCP9982_CHAN_EXT3 = 3,
	MCP9982_CHAN_EXT4 = 4,
};

/**
 * struct mcp9992_priv - information about chip parameters
 * @client:			the i2c-client attached to the device
 * @regmap:			device register map
 * @iio_info			iio_info
 * @iio_chan			specifications of channels
 * @num_channels		number of physical channels
 * @lock			synchronize access to driver's state members
 * @running_avg			number of samples in the running average window
 * @hysteresis			value of temperature hysteresis
 * @temp_range_code		coded value representing the set temperature range
 * @labels			labels of the channels
 * @chip_name			name of the chip present
 * @beta_user_value		value given by the user for beta on channel 1 and 2
 * @apdd			state of anti-parallel diode mode
 * @recd12			state of REC on channels 1 and 2
 * @recd34			state of REC on channels 3 and 4
 * @ideality_user_value		values given by user to ideality factor for all channels
 */

struct mcp9982_priv {
	struct i2c_client *client;
	struct regmap *regmap;
	struct iio_info iio_info;

	struct iio_chan_spec *iio_chan;
	u8 num_channels;

	/*
	 * Synchronize access to private members, and ensure
	 * atomicity of consecutive regmap operations.
	 */
	struct mutex lock;

	int running_avg;
	int hysteresis;
	int temp_range_code;
	char *labels[MCP9982_MAX_NUM_CHANNELS];
	char *chip_name;
	int beta_user_value[2];
	int apdd;
	int recd12;
	int recd34;
	int ideality_user_value[4];
};

static int mcp9982_read_avail(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      const int **vals, int *type, int *length, long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*type = IIO_VAL_INT_PLUS_MICRO;
		*vals = mcp9982_conv_rate[0];
		*length = ARRAY_SIZE(mcp9982_conv_rate) * 2;
		return IIO_AVAIL_LIST;
	default:
		return -EINVAL;
	}
}

static int mcp9982_read_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct mcp9982_priv *priv = iio_priv(indio_dev);
	int ret, val3, HIGH_BYTE, LOW_BYTE;

	/* Write in ONESHOT register to take a new reading */
	ret = regmap_write(priv->regmap, MCP9982_ONE_SHOT_ADDR, 1);
	if (ret)
		return ret;

	guard(mutex)(&priv->lock);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		HIGH_BYTE = MCP9982_INT_HIGH_BYTE_ADDR(chan->channel);
		LOW_BYTE = MCP9982_INT_LOW_BYTE_ADDR(chan->channel);

		ret = regmap_read(priv->regmap, HIGH_BYTE, val);
		if (ret)
			return ret;

		if (priv->temp_range_code)
			*val -= MCP9982_TEMP_OFFSET;

		ret = regmap_read(priv->regmap, LOW_BYTE, val2);
		if (ret)
			return ret;

		*val2 = mcp9982_fractional_values[*val2 >> 5];

		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = regmap_read(priv->regmap, MCP9982_CONV_ADDR, &val3);
		if (ret)
			return ret;

		*val = mcp9982_conv_rate[val3][0];
		*val2 = mcp9982_conv_rate[val3][1];

		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int mcp9982_read_label(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
			      char *label)
{
	struct mcp9982_priv *priv = iio_priv(indio_dev);

	if (chan->channel < 0 || chan->channel > 4)
		return -EINVAL;

	return sprintf(label, "%s\n", priv->labels[chan->channel]);
}

static int mcp9982_write_raw_get_fmt(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
				     long info)
{
	switch (info) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int mcp9982_write_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct mcp9982_priv *priv = iio_priv(indio_dev);
	struct device *dev = &priv->client->dev;
	int i;
	int status = 0;

	guard(mutex)(&priv->lock);
	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		for (i = 0; i < ARRAY_SIZE(mcp9982_conv_rate); i++) {
			if (val == mcp9982_conv_rate[i][0] &&
			    val2 == mcp9982_conv_rate[i][1]){
				status = 1;
				break;
			}
		}
		if (!status)
			return dev_err_probe(dev, -EINVAL, "Sampling Frequency is invalid\n");

		return regmap_write(priv->regmap, MCP9982_CONV_ADDR, i);
	default:
		return -EINVAL;
	}
}

static ssize_t mcp9982_running_average_window_show(struct device *dev,
						   struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct mcp9982_priv *priv = iio_priv(indio_dev);

	return sprintf(buf, "%u sample(s)\n", priv->running_avg);
}

static ssize_t mcp9982_running_average_window_avail_show(struct device *dev,
							 struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", MCP9982_RUNNING_AVERAGE_WINDOW_AVAILABLE);
}

static ssize_t mcp9982_running_average_window_store(struct device *dev,
						    struct device_attribute *attr,
						    const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct mcp9982_priv *priv = iio_priv(indio_dev);
	int val, ret, reg_val;

	if (kstrtouint(buf, 10, &val)) {
		dev_err(dev, "Value is not a number\n");
		return -EINVAL;
	}

	switch (val) {
	case 1:
		reg_val = 0;
		break;
	case 4:
		reg_val = 1;
		break;
	case 8:
		reg_val = 3;
		break;
	default:
		dev_err(dev, "Value is invalid\n");
		return -EINVAL;
	}

	guard(mutex)(&priv->lock);

	ret = regmap_write(priv->regmap, MCP9982_RUNNING_AVG_ADDR, reg_val);
	priv->running_avg = val;

	return count;
}

static IIO_DEVICE_ATTR(running_average_window, 0644, mcp9982_running_average_window_show,
		       mcp9982_running_average_window_store, 0);

static IIO_DEVICE_ATTR(running_average_window_available, 0444,
		       mcp9982_running_average_window_avail_show, NULL, 1);

static int mcp9982_prep_custom_attributes(struct mcp9982_priv *priv, struct iio_dev *indio_dev)
{
	struct attribute **mcp9982_custom_attr;
	struct attribute_group *mcp9982_group;
	struct device *dev = &priv->client->dev;

	mcp9982_group = devm_kzalloc(dev, sizeof(*mcp9982_group), GFP_KERNEL);
	if (!mcp9982_group)
		return -ENOMEM;

	mcp9982_custom_attr = devm_kzalloc(dev, MCP9982_NR_CUSTOM_ATTR *
					   sizeof(*mcp9982_group) + 1, GFP_KERNEL);
	if (!mcp9982_custom_attr)
		return -ENOMEM;

	mcp9982_custom_attr[0] = MCP9982_DEV_ATTR(running_average_window);
	mcp9982_custom_attr[1] = MCP9982_DEV_ATTR(running_average_window_available);

	mcp9982_group->attrs = mcp9982_custom_attr;
	priv->iio_info.attrs = mcp9982_group;

	return 0;
}

static const struct iio_info mcp9982_info = {
	.read_raw = mcp9982_read_raw,
	.read_label = mcp9982_read_label,
	.read_avail = mcp9982_read_avail,
	.write_raw_get_fmt = mcp9982_write_raw_get_fmt,
	.write_raw = mcp9982_write_raw,
};

static int mcp9982_init(struct mcp9982_priv *priv)
{
	int ret, i;
	u8 val;

	val = FIELD_PREP(MCP9982_CFG_MSKAL, MCP9982_CFG_MSKAL_DEFAULT) |
	      FIELD_PREP(MCP9982_CFG_RS, MCP9982_CFG_RS_DEFAULT) |
	      FIELD_PREP(MCP9982_CFG_ATTHM, MCP9982_CFG_ATTHM_DEFAULT) |
	      FIELD_PREP(MCP9982_CFG_RECD12, priv->recd12) |
	      FIELD_PREP(MCP9982_CFG_RECD34, priv->recd34) |
	      FIELD_PREP(MCP9982_CFG_RANGE, priv->temp_range_code) |
	      FIELD_PREP(MCP9982_CFG_DA_ENA, MCP9982_CFG_DA_ENA_DEFAULT) |
	      FIELD_PREP(MCP9982_CFG_APDD, priv->apdd);
	ret = regmap_write(priv->regmap, MCP9982_CFG_ADDR, val);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, MCP9982_CONV_ADDR, MCP9982_SAMPLING_FREQ_CODE_DEFAULT);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, MCP9982_HYS_ADDR, priv->hysteresis);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, MCP9982_CONSEC_ALRT_ADDR, MCP9982_CONSEC_ALRT_DEFAULT);
	if (ret)
		return ret;

	/* RUNNING AVG DEFAULT is 0 */
	ret = regmap_write(priv->regmap, MCP9982_RUNNING_AVG_ADDR, 0);
	if (ret)
		return ret;
	priv->running_avg = 1;

	/* HOTTEST CFG DEFAULT is 0 */
	ret = regmap_write(priv->regmap, MCP9982_HOTTEST_CFG_ADDR, 0);
	if (ret)
		return ret;

	/* Set beta compensation for all channels */
	for (i = 0; i < 2; i++) {
		ret = regmap_write(priv->regmap, MCP9982_EXT_BETA_CFG_ADDR(i),
				   priv->beta_user_value[i]);
		if (ret)
			return ret;
	}

	/* Set ideality factor for all channels */
	for (i = 0; i < 4; i++) {
		ret = regmap_write(priv->regmap, MCP9982_EXT_IDEAL_ADDR(i),
				   priv->ideality_user_value[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int mcp9982_parse_of_fw(struct iio_dev *indio_dev)
{
	struct mcp9982_priv *priv = iio_priv(indio_dev);
	struct device *dev = &priv->client->dev;
	int ret, reg_nr, iio_idx, i;

	if (device_property_present(dev, "microchip,temp-hysteresis")) {
		ret = device_property_read_u32(dev, "microchip,temp-hysteresis", &priv->hysteresis);
		if (ret)
			return dev_err_probe(dev, ret, "Cannot read hysteresis property\n");

		if (priv->hysteresis > MCP9982_HYSTERESIS_MAX_VALUE)
			return dev_err_probe(dev, ret,
					     "Hysteresis value is higher than maximum\n");
	} else {
		priv->hysteresis = MCP9982_HYSTERESIS_DEFAULT;
	}

	if (device_property_present(dev, "microchip,extended-temp-range"))
		priv->temp_range_code = 1;
	else
		priv->temp_range_code = MCP9982_TEMP_RANGE_DEFAULT;

	if (device_property_present(dev, "microchip,beta-channel1")) {
		ret = device_property_read_u32(dev, "microchip,beta-channel1",
					       &priv->beta_user_value[0]);
		if (ret)
			return ret;

		if (priv->beta_user_value[0] > MCP9982_BETA_MAX_VALUE)
			return dev_err_probe(dev, -EINVAL, "Beta 1 value is higher than max\n");
	} else {
		priv->beta_user_value[0] = MCP9982_BETA_AUTODETECT_ENABLE;
	}

	if (device_property_present(dev, "microchip,beta-channel2")) {
		ret = device_property_read_u32(dev, "microchip,beta-channel2",
					       &priv->beta_user_value[1]);
		if (ret)
			return ret;

		if (priv->beta_user_value[1] > MCP9982_BETA_MAX_VALUE)
			return dev_err_probe(dev, -EINVAL, "Beta 2 value is higher than max\n");
	} else {
		priv->beta_user_value[1] = MCP9982_BETA_AUTODETECT_ENABLE;
	}

	if (device_property_present(dev, "microchip,apdd-state"))
		priv->apdd = 0;
	else
		priv->apdd = MCP9982_APDD_DEFAULT;

	if (device_property_present(dev, "microchip,recd12"))
		priv->recd12 = 0;
	else
		priv->recd12 = MCP9982_RECD_DEFAULT;

	if (device_property_present(dev, "microchip,recd34"))
		priv->recd34 = 0;
	else
		priv->recd34 = MCP9982_RECD_DEFAULT;

	ret = device_property_read_string(dev, "compatible", (const char **)&priv->chip_name);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot read compatible property\n");

	priv->chip_name = &priv->chip_name[10];
	priv->num_channels = device_get_child_node_count(dev) + 1;

	for (i = 0; i < 10; i++)
		if (strcmp(priv->chip_name, mcp9982_chip_config[i].name) == 0)
			break;
	if (priv->num_channels > mcp9982_chip_config[i].phys_channels)
		return dev_err_probe(dev, -EINVAL,
				     "The number of channels does not match the chip\n");

	priv->iio_chan = devm_kzalloc(dev, priv->num_channels * sizeof(*priv->iio_chan),
				      GFP_KERNEL);
	if (!priv->iio_chan)
		return -ENOMEM;

	/* first channel is internal and always present */
	priv->iio_chan[0] = MCP9982_CHAN(MCP9982_CHAN_INT, MCP9982_CHAN_INT,
					 MCP9982_INT_HIGH_BYTE_ADDR(0));
	priv->labels[0] = "internal diode";
	iio_idx++;
	device_for_each_child_node_scoped(dev, child) {
		ret = fwnode_property_read_u32(child, "reg", &reg_nr);
		if (reg_nr >= mcp9982_chip_config[i].phys_channels)
			return dev_err_probe(dev, -EINVAL,
				     "The index of the channels does not match the chip\n");

		if (fwnode_property_present(child, "microchip,ideality-factor")) {
			ret = fwnode_property_read_u32(child, "microchip,ideality-factor",
						       &priv->ideality_user_value[reg_nr - 1]);
			if (priv->ideality_user_value[reg_nr - 1] > MCP9982_IDEALITY_MAX_VALUE)
				return dev_err_probe(dev, -EINVAL,
				     "The ideality value is higher than maximum\n");
		} else {
			priv->ideality_user_value[reg_nr - 1] = MCP9982_IDEALITY_FACTOR_DEFAULT;
		}

		ret = fwnode_property_read_string(child, "label",
						  (const char **)&priv->labels[reg_nr]);

		priv->iio_chan[iio_idx++] = MCP9982_CHAN(reg_nr, reg_nr,
							 MCP9982_INT_HIGH_BYTE_ADDR(reg_nr));
	}

	return 0;
}

static int mcp9982_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct mcp9982_priv *priv;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*priv));
	if (!indio_dev)
		return -ENOMEM;

	priv = iio_priv(indio_dev);
	priv->client = client;
	i2c_set_clientdata(client, indio_dev);

	priv->regmap = devm_regmap_init_i2c(client, &mcp9982_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap),
				     "Cannot initialize register map\n");

	ret = devm_mutex_init(dev, &priv->lock);
	if (ret)
		return ret;

	ret = mcp9982_parse_of_fw(indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "Parameter parsing error\n");

	ret = mcp9982_init(priv);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot initialize device\n");

	priv->iio_info = mcp9982_info;

	indio_dev->name = priv->chip_name;
	indio_dev->info = &priv->iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = priv->iio_chan;
	indio_dev->num_channels = priv->num_channels;

	ret = mcp9982_prep_custom_attributes(priv, indio_dev);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
				     "Can't configure custom attributes for MCP9982 device\n");

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot register IIO device\n");

	return 0;
}

static const struct i2c_device_id mcp9982_id[] = {
	{ .name = "mcp9933", .driver_data = (kernel_ulong_t)&mcp9982_chip_config[MCP9933] },
	{ .name = "mcp9933D", .driver_data = (kernel_ulong_t)&mcp9982_chip_config[MCP9933D] },
	{ .name = "mcp9982", .driver_data = (kernel_ulong_t)&mcp9982_chip_config[MCP9982] },
	{ .name = "mcp9982D", .driver_data = (kernel_ulong_t)&mcp9982_chip_config[MCP9982D] },
	{ .name = "mcp9983", .driver_data = (kernel_ulong_t)&mcp9982_chip_config[MCP9983] },
	{ .name = "mcp9983D", .driver_data = (kernel_ulong_t)&mcp9982_chip_config[MCP9983D] },
	{ .name = "mcp9984", .driver_data = (kernel_ulong_t)&mcp9982_chip_config[MCP9984] },
	{ .name = "mcp9984D", .driver_data = (kernel_ulong_t)&mcp9982_chip_config[MCP9984D] },
	{ .name = "mcp9985", .driver_data = (kernel_ulong_t)&mcp9982_chip_config[MCP9985] },
	{ .name = "mcp9985D", .driver_data = (kernel_ulong_t)&mcp9982_chip_config[MCP9985D] },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mcp9982_id);

static const struct of_device_id mcp9982_of_match[] = {
	{
		.compatible = "microchip,mcp9933",
		.data = &mcp9982_chip_config[MCP9933]
	},
	{
		.compatible = "microchip,mcp9933D",
		.data = &mcp9982_chip_config[MCP9933D]
	},
	{
		.compatible = "microchip,mcp9982",
		.data = &mcp9982_chip_config[MCP9982]
	},
	{
		.compatible = "microchip,mcp9982D",
		.data = &mcp9982_chip_config[MCP9982D]
	},
	{
		.compatible = "microchip,mcp9983",
		.data = &mcp9982_chip_config[MCP9983]
	},
	{
		.compatible = "microchip,mcp9983D",
		.data = &mcp9982_chip_config[MCP9983D]
	},
	{
		.compatible = "microchip,mcp9984",
		.data = &mcp9982_chip_config[MCP9984]
	},
	{
		.compatible = "microchip,mcp9984D",
		.data = &mcp9982_chip_config[MCP9984D]
	},
	{
		.compatible = "microchip,mcp9985",
		.data = &mcp9982_chip_config[MCP9985]
	},
	{
		.compatible = "microchip,mcp9985D",
		.data = &mcp9982_chip_config[MCP9985D]
	},
	{ }
};
MODULE_DEVICE_TABLE(of, mcp9982_of_match);

static struct i2c_driver mcp9982_driver = {
	.driver	 = {
		.name = "mcp9982",
		.of_match_table = mcp9982_of_match,
	},
	.probe = mcp9982_probe,
	.id_table = mcp9982_id,
};

module_i2c_driver(mcp9982_driver);

MODULE_AUTHOR("Victor Duicu <victor.duicu@microchip.com>");
MODULE_DESCRIPTION("MCP998X/33 and MCP998XD/33D Multichannel Automotive Temperature Monitor Driver");
MODULE_LICENSE("GPL");
