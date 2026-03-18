// SPDX-License-Identifier: GPL-2.0-only
/*
 * AD5706R 16-bit Current Output Digital to Analog Converter
 *
 * Copyright 2026 Analog Devices Inc.
 */

#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/spi/spi.h>
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
#define AD5706R_SINGLE_BYTE_LEN		1
#define AD5706R_DOUBLE_BYTE_LEN		2

struct ad5706r_state {
	struct spi_device *spi;
	struct mutex lock; /* Protects SPI transfers */

	u8 tx_buf[4] __aligned(ARCH_DMA_MINALIGN);
	u8 rx_buf[2];
};

static int ad5706r_reg_len(unsigned int reg)
{
	if (reg >= AD5706R_MULTIBYTE_REG_START && reg <= AD5706R_MULTIBYTE_REG_END)
		return AD5706R_DOUBLE_BYTE_LEN;

	return AD5706R_SINGLE_BYTE_LEN;
}

static int ad5706r_spi_write(struct ad5706r_state *st, u16 reg, u16 val)
{
	unsigned int num_bytes = ad5706r_reg_len(reg);
	struct spi_transfer xfer = {
		.tx_buf = st->tx_buf,
		.len = num_bytes + 2,
	};

	put_unaligned_be16(reg, &st->tx_buf[0]);

	if (num_bytes == 1)
		st->tx_buf[2] = val;
	else if (num_bytes == 2)
		put_unaligned_be16(val, &st->tx_buf[2]);
	else
		return -EINVAL;

	return spi_sync_transfer(st->spi, &xfer, 1);
}

static int ad5706r_spi_read(struct ad5706r_state *st, u16 reg, u16 *val)
{
	unsigned int num_bytes = ad5706r_reg_len(reg);
	u16 cmd;
	int ret;

	struct spi_transfer xfer[] = {
		{
			.tx_buf = st->tx_buf,
			.len = 2,
		},
		{
			.rx_buf = st->rx_buf,
			.len = num_bytes,
		},
	};

	cmd = AD5706R_RD_MASK | (reg & AD5706R_ADDR_MASK);
	put_unaligned_be16(cmd, &st->tx_buf[0]);

	ret = spi_sync_transfer(st->spi, xfer, ARRAY_SIZE(xfer));
	if (ret)
		return ret;

	if (num_bytes == 1)
		*val = st->rx_buf[0];
	else if (num_bytes == 2)
		*val = get_unaligned_be16(st->rx_buf);
	else
		return -EINVAL;

	return 0;
}

static int ad5706r_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int *val,
			    int *val2, long mask)
{
	struct ad5706r_state *st = iio_priv(indio_dev);
	u16 reg_val;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		scoped_guard(mutex, &st->lock) {
			ret = ad5706r_spi_read(st, AD5706R_REG_DAC_DATA_READBACK_CH(chan->channel),
					       &reg_val);

			if (ret)
				return ret;

			*val = reg_val;
		}
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 50;
		*val2 = AD5706R_DAC_RESOLUTION;
		return IIO_VAL_FRACTIONAL_LOG2;
	}

	return -EINVAL;
}

static int ad5706r_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan, int val,
			     int val2, long mask)
{
	struct ad5706r_state *st = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (val < 0 || val >= AD5706R_DAC_MAX_CODE)
			return -EINVAL;

		guard(mutex)(&st->lock);
		return ad5706r_spi_write(st,
					 AD5706R_REG_DAC_INPUT_A_CH(chan->channel),
					 val);
	default:
		return -EINVAL;
	}
}

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
	struct iio_dev *indio_dev;
	struct ad5706r_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->spi = spi;

	ret = devm_mutex_init(&spi->dev, &st->lock);
	if (ret)
		return ret;

	indio_dev->name = "ad5706r";
	indio_dev->info = &ad5706r_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = ad5706r_channels;
	indio_dev->num_channels = ARRAY_SIZE(ad5706r_channels);

	return devm_iio_device_register(&spi->dev, indio_dev);
}

static const struct of_device_id ad5706r_of_match[] = {
	{ .compatible = "adi,ad5706r" },
	{}
};
MODULE_DEVICE_TABLE(of, ad5706r_of_match);

static const struct spi_device_id ad5706r_id[] = {
	{ "ad5706r" },
	{}
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
