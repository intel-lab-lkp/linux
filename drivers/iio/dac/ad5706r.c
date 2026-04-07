// SPDX-License-Identifier: GPL-2.0-only
/*
 * AD5706R 16-bit Current Output Digital to Analog Converter
 *
 * Copyright 2026 Analog Devices Inc.
 */

#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/iio/iio.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <linux/types.h>
#include <linux/unaligned.h>

/* SPI frame layout */
#define AD5706R_RD_MASK			BIT(15)
#define AD5706R_ADDR_MASK		GENMASK(11, 0)

/* Registers */
#define AD5706R_REG_DAC_INPUT_A_CH(x)		(0x60 + ((x) * 2))
#define AD5706R_REG_DAC_DATA_READBACK_CH(x)	(0x68 + ((x) * 2))

#define AD5706R_DAC_RESOLUTION		16
#define AD5706R_DAC_MAX_CODE		BIT(16)
#define AD5706R_MULTIBYTE_REG_START	0x14
#define AD5706R_MULTIBYTE_REG_END	0x71
#define AD5706R_MAX_REG			0x77
#define AD5706R_SINGLE_BYTE_LEN		1
#define AD5706R_DOUBLE_BYTE_LEN		2

struct ad5706r_state {
	struct spi_device *spi;
	struct regmap *regmap;

	u8 tx_buf[4] __aligned(IIO_DMA_MINALIGN);
	u8 rx_buf[4];
};

static int ad5706r_reg_len(unsigned int reg)
{
	if (reg >= AD5706R_MULTIBYTE_REG_START && reg <= AD5706R_MULTIBYTE_REG_END)
		return AD5706R_DOUBLE_BYTE_LEN;

	return AD5706R_SINGLE_BYTE_LEN;
}

static int ad5706r_regmap_write(void *context, const void *data, size_t count)
{
	struct ad5706r_state *st = context;
	unsigned int num_bytes, val;
	u16 reg;

	reg = get_unaligned_be16(data);
	num_bytes = ad5706r_reg_len(reg);

	struct spi_transfer xfer = {
		.tx_buf = st->tx_buf,
		.len = num_bytes + 2,
	};

	val = get_unaligned_be32(data);
	put_unaligned_be32(val, st->tx_buf);

	/* For single byte, copy the data to the correct position */
	if (num_bytes == AD5706R_SINGLE_BYTE_LEN)
		st->tx_buf[2] = st->tx_buf[3];

	return spi_sync_transfer(st->spi, &xfer, 1);
}

static int ad5706r_regmap_read(void *context, const void *reg_buf,
			       size_t reg_size, void *val_buf, size_t val_size)
{
	struct ad5706r_state *st = context;
	unsigned int num_bytes;
	u16 reg, cmd, val;
	int ret;

	reg = get_unaligned_be16(reg_buf);
	num_bytes = ad5706r_reg_len(reg);

	/* Full duplex, device responds immediately after command */
	struct spi_transfer xfer = {
		.tx_buf = st->tx_buf,
		.rx_buf = st->rx_buf,
		.len = 2 + num_bytes,
	};

	cmd = AD5706R_RD_MASK | (reg & AD5706R_ADDR_MASK);
	put_unaligned_be16(cmd, st->tx_buf);
	put_unaligned_be16(0, &st->tx_buf[2]);

	ret = spi_sync_transfer(st->spi, &xfer, 1);
	if (ret)
		return ret;

	/* Extract value from response (skip 2-byte command echo) */
	if (num_bytes == AD5706R_SINGLE_BYTE_LEN)
		val = st->rx_buf[2];
	else if (num_bytes == AD5706R_DOUBLE_BYTE_LEN)
		val = get_unaligned_be16(&st->rx_buf[2]);
	else
		return -EINVAL;

	put_unaligned_be16(val, val_buf);

	return 0;
}

static int ad5706r_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct ad5706r_state *st = iio_priv(indio_dev);
	unsigned int reg, reg_val;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		reg = AD5706R_REG_DAC_DATA_READBACK_CH(chan->channel);
		ret = regmap_read(st->regmap, reg, &reg_val);
		if (ret)
			return ret;

		*val = reg_val;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 50;
		*val2 = AD5706R_DAC_RESOLUTION;
		return IIO_VAL_FRACTIONAL_LOG2;
	default:
		return -EINVAL;
	}
}

static int ad5706r_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct ad5706r_state *st = iio_priv(indio_dev);
	unsigned int reg;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!in_range(val, 0, AD5706R_DAC_MAX_CODE))
			return -EINVAL;

		reg = AD5706R_REG_DAC_INPUT_A_CH(chan->channel);
		return regmap_write(st->regmap, reg, val);
	default:
		return -EINVAL;
	}
}

static const struct regmap_bus ad5706r_regmap_bus = {
	.write = ad5706r_regmap_write,
	.read = ad5706r_regmap_read,
	.reg_format_endian_default = REGMAP_ENDIAN_BIG,
	.val_format_endian_default = REGMAP_ENDIAN_BIG,
};

static const struct regmap_config ad5706r_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.max_register = AD5706R_MAX_REG,
};

static const struct iio_info ad5706r_info = {
	.read_raw = ad5706r_read_raw,
	.write_raw = ad5706r_write_raw,
};

#define AD5706R_CHAN(_channel) {				\
	.type = IIO_CURRENT,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
			      BIT(IIO_CHAN_INFO_SCALE),	\
	.output = 1,						\
	.indexed = 1,						\
	.channel = _channel,					\
}

static const struct iio_chan_spec ad5706r_channels[] = {
	AD5706R_CHAN(0),
	AD5706R_CHAN(1),
	AD5706R_CHAN(2),
	AD5706R_CHAN(3),
};

static int ad5706r_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct iio_dev *indio_dev;
	struct ad5706r_state *st;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->spi = spi;

	st->regmap = devm_regmap_init(dev, &ad5706r_regmap_bus,
				      st, &ad5706r_regmap_config);
	if (IS_ERR(st->regmap))
		return dev_err_probe(dev, PTR_ERR(st->regmap),
				     "Failed to init regmap\n");

	indio_dev->name = "ad5706r";
	indio_dev->info = &ad5706r_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = ad5706r_channels;
	indio_dev->num_channels = ARRAY_SIZE(ad5706r_channels);

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id ad5706r_of_match[] = {
	{ .compatible = "adi,ad5706r" },
	{ }
};
MODULE_DEVICE_TABLE(of, ad5706r_of_match);

static const struct spi_device_id ad5706r_id[] = {
	{ "ad5706r" },
	{ }
};
MODULE_DEVICE_TABLE(spi, ad5706r_id);

static struct spi_driver ad5706r_driver = {
	.driver = {
		.name = "ad5706r",
		.of_match_table = ad5706r_of_match,
	},
	.probe = ad5706r_probe,
	.id_table = ad5706r_id,
};
module_spi_driver(ad5706r_driver);

MODULE_AUTHOR("Alexis Czezar Torreno <alexisczezar.torreno@analog.com>");
MODULE_DESCRIPTION("AD5706R 16-bit Current Output DAC driver");
MODULE_LICENSE("GPL");
