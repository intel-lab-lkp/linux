// SPDX-License-Identifier: GPL-2.0-only
/*
 * AD5529R Digital-to-Analog Converter Driver
 * 16-Channel, 12/16-Bit, 40V High Voltage Precision DAC
 *
 * Copyright 2026 Analog Devices Inc.
 * Author: Janani Sunil <janani.sunil@analog.com>
 */

#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/iio/iio.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/spi/spi.h>

#define AD5529R_REG_INTERFACE_CONFIG_A		0x00
#define AD5529R_REG_DEVICE_CONFIG		0x02
#define AD5529R_REG_CHIP_GRADE			0x06
#define AD5529R_REG_SCRATCH_PAD			0x0A
#define AD5529R_REG_SPI_REVISION		0x0B
#define AD5529R_REG_VENDOR_H			0x0D
#define AD5529R_REG_STREAM_MODE			0x0E
#define AD5529R_REG_INTERFACE_STATUS_A		0x11
#define AD5529R_REG_MULTI_DAC_CH_SEL		0x14
#define AD5529R_REG_OUT_RANGE_BASE		0x3C
#define AD5529R_REG_OUT_RANGE(ch)		(AD5529R_REG_OUT_RANGE_BASE + (ch) * 2)
#define AD5529R_REG_DAC_INPUT_A_BASE		0x148
#define AD5529R_REG_DAC_INPUT_A(ch)		(AD5529R_REG_DAC_INPUT_A_BASE + (ch) * 2)
#define AD5529R_REG_DAC_DATA_READBACK_BASE	0x16A
#define AD5529R_REG_TSENS_ALERT_FLAG		0x18C
#define AD5529R_REG_TSENS_SHTD_FLAG		0x18E
#define AD5529R_REG_FUNC_BUSY			0x1A0
#define AD5529R_REG_REF_SEL			0x1A2
#define AD5529R_REG_INIT_CRC_ERR_STAT		0x1A4
#define AD5529R_REG_MULTI_DAC_HOTPATH_SW_LDAC	0x1A8

#define   AD5529R_INTERFACE_CONFIG_A_SW_RESET	(BIT(7) | BIT(0))
#define   AD5529R_INTERFACE_CONFIG_A_ADDR_ASCENSION	BIT(5)
#define   AD5529R_INTERFACE_CONFIG_A_SDO_ENABLE	BIT(4)
#define   AD5529R_REF_SEL_MASK			BIT(0)
#define   AD5529R_MAX_REGISTER			0x232
#define   AD5529R_8BIT_REG_MAX			0x13
#define   AD5529R_SPI_READ_FLAG			0x80

struct ad5529r_model_data {
	const char *model_name;
	unsigned int resolution;
	const struct iio_chan_spec *channels;
	unsigned int num_channels;
};

#define AD5529R_DAC_CHANNEL(chan, bits) {			\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.output = 1,						\
	.channel = (chan),					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
			      BIT(IIO_CHAN_INFO_SCALE) |	\
			      BIT(IIO_CHAN_INFO_OFFSET),	\
	.scan_type = {						\
		.format = 'u',					\
		.realbits = (bits),				\
		.storagebits = 16,				\
	},							\
}

static const char * const ad5529r_supply_names[] = {
	"vdd",
	"avdd",
	"hvdd",
};

static const struct iio_chan_spec ad5529r_channels_16bit[] = {
	AD5529R_DAC_CHANNEL(0, 16),
	AD5529R_DAC_CHANNEL(1, 16),
	AD5529R_DAC_CHANNEL(2, 16),
	AD5529R_DAC_CHANNEL(3, 16),
	AD5529R_DAC_CHANNEL(4, 16),
	AD5529R_DAC_CHANNEL(5, 16),
	AD5529R_DAC_CHANNEL(6, 16),
	AD5529R_DAC_CHANNEL(7, 16),
	AD5529R_DAC_CHANNEL(8, 16),
	AD5529R_DAC_CHANNEL(9, 16),
	AD5529R_DAC_CHANNEL(10, 16),
	AD5529R_DAC_CHANNEL(11, 16),
	AD5529R_DAC_CHANNEL(12, 16),
	AD5529R_DAC_CHANNEL(13, 16),
	AD5529R_DAC_CHANNEL(14, 16),
	AD5529R_DAC_CHANNEL(15, 16),
};

static const struct iio_chan_spec ad5529r_channels_12bit[] = {
	AD5529R_DAC_CHANNEL(0, 12),
	AD5529R_DAC_CHANNEL(1, 12),
	AD5529R_DAC_CHANNEL(2, 12),
	AD5529R_DAC_CHANNEL(3, 12),
	AD5529R_DAC_CHANNEL(4, 12),
	AD5529R_DAC_CHANNEL(5, 12),
	AD5529R_DAC_CHANNEL(6, 12),
	AD5529R_DAC_CHANNEL(7, 12),
	AD5529R_DAC_CHANNEL(8, 12),
	AD5529R_DAC_CHANNEL(9, 12),
	AD5529R_DAC_CHANNEL(10, 12),
	AD5529R_DAC_CHANNEL(11, 12),
	AD5529R_DAC_CHANNEL(12, 12),
	AD5529R_DAC_CHANNEL(13, 12),
	AD5529R_DAC_CHANNEL(14, 12),
	AD5529R_DAC_CHANNEL(15, 12),
};

static const struct ad5529r_model_data ad5529r_16bit_model_data = {
	.model_name = "ad5529r-16",
	.resolution = 16,
	.channels = ad5529r_channels_16bit,
	.num_channels = ARRAY_SIZE(ad5529r_channels_16bit),
};

static const struct ad5529r_model_data ad5529r_12bit_model_data = {
	.model_name = "ad5529r-12",
	.resolution = 12,
	.channels = ad5529r_channels_12bit,
	.num_channels = ARRAY_SIZE(ad5529r_channels_12bit),
};

enum ad5529r_output_range {
	AD5529R_RANGE_0V_5V,
	AD5529R_RANGE_0V_10V,
	AD5529R_RANGE_0V_20V,
	AD5529R_RANGE_0V_40V,
	AD5529R_RANGE_NEG5V_5V,
	AD5529R_RANGE_NEG10V_10V,
	AD5529R_RANGE_NEG15V_15V,
	AD5529R_RANGE_NEG20V_20V,
};

static const s32 ad5529r_output_ranges_mv[8][2] = {
	[AD5529R_RANGE_0V_5V] = { 0, 5000 },
	[AD5529R_RANGE_0V_10V] = { 0, 10000 },
	[AD5529R_RANGE_0V_20V] = { 0, 20000 },
	[AD5529R_RANGE_0V_40V] = { 0, 40000 },
	[AD5529R_RANGE_NEG5V_5V] = { -5000, 5000 },
	[AD5529R_RANGE_NEG10V_10V] = { -10000, 10000 },
	[AD5529R_RANGE_NEG15V_15V] = { -15000, 15000 },
	[AD5529R_RANGE_NEG20V_20V] = { -20000, 20000 },
};

struct ad5529r_state {
	struct spi_device *spi;
	const struct ad5529r_model_data *model_data;
	struct regmap *regmap_8bit;
	struct regmap *regmap_16bit;
	struct regulator *vref_regulator;
	enum ad5529r_output_range output_range_idx[16];
};

static const struct regmap_range ad5529r_8bit_readable_ranges[] = {
	regmap_reg_range(AD5529R_REG_INTERFACE_CONFIG_A, AD5529R_REG_CHIP_GRADE),
	regmap_reg_range(AD5529R_REG_SCRATCH_PAD, AD5529R_REG_VENDOR_H),
	regmap_reg_range(AD5529R_REG_STREAM_MODE, AD5529R_REG_INTERFACE_STATUS_A),
};

static const struct regmap_range ad5529r_16bit_readable_ranges[] = {
	regmap_reg_range(AD5529R_REG_MULTI_DAC_CH_SEL, AD5529R_REG_INIT_CRC_ERR_STAT),
	regmap_reg_range(AD5529R_REG_MULTI_DAC_HOTPATH_SW_LDAC, AD5529R_MAX_REGISTER),
};

static const struct regmap_access_table ad5529r_8bit_readable_table = {
	.yes_ranges = ad5529r_8bit_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(ad5529r_8bit_readable_ranges),
};

static const struct regmap_access_table ad5529r_16bit_readable_table = {
	.yes_ranges = ad5529r_16bit_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(ad5529r_16bit_readable_ranges),
};

static const struct regmap_range ad5529r_8bit_read_only_ranges[] = {
	regmap_reg_range(AD5529R_REG_DEVICE_CONFIG, AD5529R_REG_CHIP_GRADE),
	regmap_reg_range(AD5529R_REG_SPI_REVISION, AD5529R_REG_VENDOR_H),
};

static const struct regmap_range ad5529r_16bit_read_only_ranges[] = {
	regmap_reg_range(AD5529R_REG_DAC_DATA_READBACK_BASE,
			 (AD5529R_REG_DAC_DATA_READBACK_BASE + 15 * 2)),
	regmap_reg_range(AD5529R_REG_TSENS_ALERT_FLAG, AD5529R_REG_TSENS_SHTD_FLAG),
	regmap_reg_range(AD5529R_REG_FUNC_BUSY, AD5529R_REG_FUNC_BUSY),
	regmap_reg_range(AD5529R_REG_INIT_CRC_ERR_STAT, AD5529R_REG_INIT_CRC_ERR_STAT),
};

static const struct regmap_access_table ad5529r_8bit_writeable_table = {
	.no_ranges = ad5529r_8bit_read_only_ranges,
	.n_no_ranges = ARRAY_SIZE(ad5529r_8bit_read_only_ranges),
};

static const struct regmap_access_table ad5529r_16bit_writeable_table = {
	.no_ranges = ad5529r_16bit_read_only_ranges,
	.n_no_ranges = ARRAY_SIZE(ad5529r_16bit_read_only_ranges),
};

static const struct regmap_config ad5529r_regmap_8bit_config = {
	.name = "ad5529r-8bit",
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = AD5529R_8BIT_REG_MAX,
	.read_flag_mask = AD5529R_SPI_READ_FLAG,
	.rd_table = &ad5529r_8bit_readable_table,
	.wr_table = &ad5529r_8bit_writeable_table,
};

static const struct regmap_config ad5529r_regmap_16bit_config = {
	.name = "ad5529r-16bit",
	.reg_bits = 16,
	.val_bits = 16,
	.max_register = AD5529R_MAX_REGISTER,
	.read_flag_mask = AD5529R_SPI_READ_FLAG,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
	.rd_table = &ad5529r_16bit_readable_table,
	.wr_table = &ad5529r_16bit_writeable_table,
	.reg_stride = 2,
};

static struct regmap *ad5529r_get_regmap(struct ad5529r_state *st,
					 unsigned int reg)
{
	if (reg <= AD5529R_8BIT_REG_MAX)
		return st->regmap_8bit;

	return st->regmap_16bit;
}

static int ad5529r_reset(struct ad5529r_state *st)
{
	struct reset_control *rst;
	int ret;

	rst = devm_reset_control_get_optional_exclusive(&st->spi->dev, NULL);
	if (IS_ERR(rst))
		return PTR_ERR(rst);

	if (rst) {
		ret = reset_control_deassert(rst);
		if (ret)
			return ret;
	} else {
		ret = regmap_write(st->regmap_8bit, AD5529R_REG_INTERFACE_CONFIG_A,
				   AD5529R_INTERFACE_CONFIG_A_SW_RESET);
		if (ret)
			return ret;
	}

	fsleep(10000);

	return regmap_write(st->regmap_8bit, AD5529R_REG_INTERFACE_CONFIG_A,
			    AD5529R_INTERFACE_CONFIG_A_SDO_ENABLE |
			    AD5529R_INTERFACE_CONFIG_A_ADDR_ASCENSION);
}

static int ad5529r_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct ad5529r_state *st = iio_priv(indio_dev);
	unsigned int reg_addr, reg_val_h;
	int ret, range_idx, span_mv;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		reg_addr = AD5529R_REG_DAC_INPUT_A(chan->channel);
		ret = regmap_read(st->regmap_16bit, reg_addr, &reg_val_h);
		if (ret)
			return ret;

		*val = reg_val_h;

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		range_idx = st->output_range_idx[chan->channel];

		span_mv = ad5529r_output_ranges_mv[range_idx][1] -
			  ad5529r_output_ranges_mv[range_idx][0];
		*val = span_mv;
		*val2 = st->model_data->resolution;

		return IIO_VAL_FRACTIONAL_LOG2;
	case IIO_CHAN_INFO_OFFSET:
		range_idx = st->output_range_idx[chan->channel];

		if (ad5529r_output_ranges_mv[range_idx][0] < 0)
			*val = -(1 << (st->model_data->resolution - 1));
		else
			*val = 0;

		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int ad5529r_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct ad5529r_state *st = iio_priv(indio_dev);
	unsigned int reg_addr;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (val < 0 || val > GENMASK(st->model_data->resolution - 1, 0))
			return -EINVAL;

		reg_addr = AD5529R_REG_DAC_INPUT_A(chan->channel);

		return regmap_write(st->regmap_16bit, reg_addr, val);
	default:
		return -EINVAL;
	}
}

static int ad5529r_find_output_range(const s32 *vals)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ad5529r_output_ranges_mv); i++) {
		if (vals[0] == ad5529r_output_ranges_mv[i][0] * 1000 &&
		    vals[1] == ad5529r_output_ranges_mv[i][1] * 1000)
			return i;
	}

	return -EINVAL;
}

static int ad5529r_parse_channel_ranges(struct device *dev,
					struct ad5529r_state *st)
{
	int ret, ch, range_idx;
	s32 vals[2];

	device_for_each_child_node_scoped(dev, child) {
		range_idx = AD5529R_RANGE_0V_5V;

		ret = fwnode_property_read_u32(child, "reg", &ch);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Missing reg property in channel node\n");

		if (ch >= 16)
			return dev_err_probe(dev, -EINVAL,
					     "Invalid channel number: %d\n", ch);

		if (!fwnode_property_read_u32_array(child,
						    "adi,output-range-microvolt",
						    vals, 2)) {
			range_idx = ad5529r_find_output_range(vals);
			if (range_idx < 0)
				return dev_err_probe(dev, range_idx,
						     "Invalid range [%d %d] for ch %d\n",
						     vals[0], vals[1], ch);
		}

		st->output_range_idx[ch] = range_idx;
		ret = regmap_write(st->regmap_16bit,
				   AD5529R_REG_OUT_RANGE(ch), range_idx);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to configure range for ch %d\n",
					     ch);
	}

	return 0;
}

static int ad5529r_debugfs_reg_read(struct ad5529r_state *st, unsigned int reg,
				    unsigned int *val)
{
	return regmap_read(ad5529r_get_regmap(st, reg), reg, val);
}

static int ad5529r_debugfs_reg_write(struct ad5529r_state *st, unsigned int reg,
				     unsigned int val)
{
	return regmap_write(ad5529r_get_regmap(st, reg), reg, val);
}

static int ad5529r_reg_access(struct iio_dev *indio_dev,
			      unsigned int reg,
			      unsigned int writeval,
			      unsigned int *readval)
{
	struct ad5529r_state *st = iio_priv(indio_dev);

	if (readval)
		return ad5529r_debugfs_reg_read(st, reg, readval);

	return ad5529r_debugfs_reg_write(st, reg, writeval);
}

static void ad5529r_disable_regulator(void *regulator)
{
	regulator_disable(regulator);
}

static const struct iio_info ad5529r_info = {
	.read_raw = ad5529r_read_raw,
	.write_raw = ad5529r_write_raw,
	.debugfs_reg_access = ad5529r_reg_access,
};

static int ad5529r_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct iio_dev *indio_dev;
	struct ad5529r_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);

	st->spi = spi;

	st->model_data = spi_get_device_match_data(spi);
	if (!st->model_data)
		return dev_err_probe(dev, -EINVAL, "Failed to identify device variant\n");

	ret = devm_regulator_bulk_get_enable(dev, ARRAY_SIZE(ad5529r_supply_names),
					     ad5529r_supply_names);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to get and enable regulators\n");

	ret = devm_regulator_get_enable_optional(dev, "hvss");
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to get and enable hvss regulator\n");

	st->vref_regulator = devm_regulator_get_optional(dev, "vref");
	if (IS_ERR(st->vref_regulator)) {
		if (PTR_ERR(st->vref_regulator) != -ENODEV)
			return dev_err_probe(dev, PTR_ERR(st->vref_regulator),
					     "Failed to get vref regulator\n");
		st->vref_regulator = NULL;
	}

	if (st->vref_regulator) {
		ret = regulator_enable(st->vref_regulator);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to enable vref regulator\n");

		ret = devm_add_action_or_reset(dev, ad5529r_disable_regulator,
					       st->vref_regulator);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to add vref regulator cleanup\n");
	}

	st->regmap_8bit = devm_regmap_init_spi(spi, &ad5529r_regmap_8bit_config);
	if (IS_ERR(st->regmap_8bit))
		return dev_err_probe(dev, PTR_ERR(st->regmap_8bit),
				     "Failed to initialize 8-bit regmap\n");

	st->regmap_16bit = devm_regmap_init_spi(spi, &ad5529r_regmap_16bit_config);
	if (IS_ERR(st->regmap_16bit))
		return dev_err_probe(dev, PTR_ERR(st->regmap_16bit),
				     "Failed to initialize 16-bit regmap\n");

	ret = ad5529r_reset(st);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to reset device\n");

	ret = regmap_update_bits(st->regmap_16bit, AD5529R_REG_REF_SEL,
				 AD5529R_REF_SEL_MASK,
				 st->vref_regulator ? 0 : AD5529R_REF_SEL_MASK);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to configure reference\n");

	ret = ad5529r_parse_channel_ranges(dev, st);
	if (ret)
		return ret;

	indio_dev->name = st->model_data->model_name;
	indio_dev->info = &ad5529r_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = st->model_data->channels;
	indio_dev->num_channels = st->model_data->num_channels;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id ad5529r_of_match[] = {
	{ .compatible = "adi,ad5529r-16", .data = &ad5529r_16bit_model_data },
	{ .compatible = "adi,ad5529r-12", .data = &ad5529r_12bit_model_data },
	{ }
};
MODULE_DEVICE_TABLE(of, ad5529r_of_match);

static const struct spi_device_id ad5529r_id[] = {
	{ "ad5529r-16", .driver_data = (kernel_ulong_t)&ad5529r_16bit_model_data },
	{ "ad5529r-12", .driver_data = (kernel_ulong_t)&ad5529r_12bit_model_data },
	{ }
};
MODULE_DEVICE_TABLE(spi, ad5529r_id);

static struct spi_driver ad5529r_driver = {
	.driver = {
		.name = "ad5529r",
		.of_match_table = ad5529r_of_match,
	},
	.probe = ad5529r_probe,
	.id_table = ad5529r_id,
};
module_spi_driver(ad5529r_driver);

MODULE_AUTHOR("Janani Sunil <janani.sunil@analog.com>");
MODULE_DESCRIPTION("Analog Devices AD5529R 12/16-bit DAC driver");
MODULE_LICENSE("GPL");
