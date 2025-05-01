// SPDX-License-Identifier: GPL-2.0
/*
 * IIO driver for Texas Instruments ADS1662 32-bit ADC
 *
 * Datasheet: https://www.ti.com/product/ADS1262
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/delay.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/triggered_buffer.h>

/* Commands */
#define ADS1262_CMD_RESET		0x06
#define ADS1262_CMD_START1		0x08
#define ADS1262_CMD_STOP1		0x0A
#define ADS1262_CMD_RDATA1		0x12
#define ADS1262_CMD_RREG		0x20
#define ADS1262_CMD_WREG		0x40

/* Registers */
#define ADS1262_REG_ID			0x00
#define ADS1262_REG_POWER		0x01
#define ADS1262_REG_INTERFACE		0x02
#define ADS1262_REG_MODE0		0x03
#define ADS1262_REG_MODE1		0x04
#define ADS1262_REG_MODE2		0x05
#define ADS1262_REG_INPMUX		0x06

/* Configurations */
#define ADS1262_INTREF_ENABLE		0x01
#define ADS1262_MODE0_ONE_SHOT		0x40
#define ADS1262_MODE2_PGA_EN		0x00
#define ADS1262_MODE2_PGA_BYPASS	BIT(7)

/* Masks */
#define ADS1262_MASK_MODE2_DR		GENMASK(4, 0)

/* ADS1262_SPECS */
#define ADS1262_MAX_CHANNELS		11
#define ADS1262_BITS_PER_SAMPLE		32
#define ADS1262_CLK_RATE_HZ		7372800
#define ADS1262_CLOCKS_TO_USECS(x)	\
	(DIV_ROUND_UP((x) * MICROHZ_PER_HZ, ADS1262_CLK_RATE_HZ))
#define ADS1262_VOLTAGE_INT_REF_uV	2500000
#define ADS1262_TEMP_SENSITIVITY_uV_per_C 420

#define ADS1262_SETTLE_TIME_USECS	10000

/* The Read/Write commands require 4 tCLK to encode and decode, for speeds
 * 2x the clock rate, these commands would require extra time between the
 * command byte and the data. A simple way to tacke this issue is by
 * limiting the SPI bus transfer speed while accessing registers.
 */
#define ADS1262_SPI_BUS_SPEED_SLOW	ADS1262_CLK_RATE_HZ

/* For reading and writing we need a buffer of size 3bytes*/
#define ADS1262_SPI_CMD_BUFFER_SIZE	3

/* Read data buffer size for
 * 1 status byte - 4 byte data (32 bit) - 1 byte checksum / CRC
 */
#define ADS1262_SPI_RDATA_BUFFER_SIZE	6

#define MILLI				1000

/**
 * struct ads1262_private - ADS1262 ADC private data structure
 * @spi: SPI device structure
 * @reset_gpio: GPIO descriptor for reset pin
 * @prev_channel: Previously selected channel for MUX configuration
 * @cmd_buffer: Buffer for SPI command transfers
 * @rx_buffer: Buffer for SPI data reception
 */
struct ads1262_private {
	struct spi_device *spi;
	struct gpio_desc *reset_gpio;
	u8 prev_channel;
	u8 cmd_buffer[ADS1262_SPI_CMD_BUFFER_SIZE];
	u8 rx_buffer[ADS1262_SPI_RDATA_BUFFER_SIZE] __aligned(IIO_DMA_MINALIGN);
};

#define ADS1262_CHAN(index)						\
{									\
	.type = IIO_VOLTAGE,						\
	.indexed = 1,							\
	.channel = index,						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.scan_index = index,						\
	.scan_type = {							\
		.sign = 's',						\
		.realbits = ADS1262_BITS_PER_SAMPLE,			\
		.storagebits = 32,					\
		.endianness = IIO_CPU					\
	},								\
}

#define ADS1262_DIFF_CHAN(index, pos_channel, neg_channel)		\
{									\
	.type = IIO_VOLTAGE,						\
	.indexed = 1,							\
	.channel = pos_channel,						\
	.channel2 = neg_channel,					\
	.differential = 1,						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.scan_index = index,						\
	.scan_type = {							\
		.sign = 's',						\
		.realbits = ADS1262_BITS_PER_SAMPLE,			\
		.storagebits = 32,					\
		.endianness = IIO_CPU					\
	},								\
}

#define ADS1262_TEMP_CHAN(index)					\
{									\
	.type = IIO_TEMP,						\
	.indexed = 1,							\
	.channel = index,						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |			\
			     BIT(IIO_CHAN_INFO_SCALE) |			\
			     BIT(IIO_CHAN_INFO_SAMP_FREQ),		\
	.scan_index = index,						\
	.scan_type = {							\
		.sign = 's',						\
		.realbits = ADS1262_BITS_PER_SAMPLE,			\
		.storagebits = 32,					\
		.endianness = IIO_BE,					\
	},								\
}

static const struct iio_chan_spec ads1262_channels[] = {
	/* Single ended channels*/
	ADS1262_CHAN(0),
	ADS1262_CHAN(1),
	ADS1262_CHAN(2),
	ADS1262_CHAN(3),
	ADS1262_CHAN(4),
	ADS1262_CHAN(5),
	ADS1262_CHAN(6),
	ADS1262_CHAN(7),
	ADS1262_CHAN(8),
	ADS1262_CHAN(9),
	/* The channel at index 10 is AINCOM, which is the common ground
	 * of the ADC. It is not a valid channel for the user.
	 */

	/* Temperature and Monitor channels */
	ADS1262_TEMP_CHAN(11),	/* TEMP SENSOR */
	ADS1262_CHAN(12),	/* AVDD MON */
	ADS1262_CHAN(13),	/* DVDD MON */
	ADS1262_CHAN(14),	/* TDAC TEST */
	/* Differential channels */
	ADS1262_DIFF_CHAN(15, 0, 1),	/* AIN0 - AIN1 */
	ADS1262_DIFF_CHAN(16, 2, 3),	/* AIN2 - AIN3 */
	ADS1262_DIFF_CHAN(17, 4, 5),	/* AIN4 - AIN5 */
	ADS1262_DIFF_CHAN(18, 6, 7),	/* AIN6 - AIN7 */
	ADS1262_DIFF_CHAN(19, 8, 9),	/* AIN8 - AIN9 */
};

static int ads1262_write_cmd(struct ads1262_private *priv, u8 command)
{
	struct spi_transfer xfer = {
		.tx_buf = priv->cmd_buffer,
		.rx_buf = priv->rx_buffer,
		.len = ADS1262_SPI_RDATA_BUFFER_SIZE,
		.speed_hz = ADS1262_CLK_RATE_HZ,
	};

	priv->cmd_buffer[0] = command;

	return spi_sync_transfer(priv->spi, &xfer, 1);
}

static int ads1262_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct ads1262_private *priv = context;

	priv->cmd_buffer[0] = ADS1262_CMD_WREG | reg;
	priv->cmd_buffer[1] = 0;
	priv->cmd_buffer[2] = val;

	return spi_write(priv->spi, &priv->cmd_buffer[0], 3);
}

static int ads1262_reg_read(void *context, unsigned int reg)
{
	struct ads1262_private *priv = context;
	struct spi_transfer reg_read_xfer = {
		.tx_buf = priv->cmd_buffer,
		.rx_buf = priv->cmd_buffer,
		.len = 3,
		.speed_hz = ADS1262_CLK_RATE_HZ,
	};
	int ret;

	priv->cmd_buffer[0] = ADS1262_CMD_RREG | reg;
	priv->cmd_buffer[1] = 0;
	priv->cmd_buffer[2] = 0;

	ret = spi_sync_transfer(priv->spi, &reg_read_xfer, 1);
	if (ret)
		return ret;

	return 0;
}

static int ads1262_reset(struct iio_dev *indio_dev)
{
	struct ads1262_private *priv = iio_priv(indio_dev);

	if (priv->reset_gpio) {
		gpiod_set_value(priv->reset_gpio, 0);
		usleep_range(200, 300);
		gpiod_set_value(priv->reset_gpio, 1);
	} else {
		return ads1262_write_cmd(priv, ADS1262_CMD_RESET);
	}
	return 0;
}

static int ads1262_init(struct iio_dev *indio_dev)
{
	struct ads1262_private *priv = iio_priv(indio_dev);
	int ret = 0;

	ret = ads1262_reset(indio_dev);
	if (ret)
		return ret;

	/* 10 milliseconds settling time for the ADC to stabilize */
	fsleep(ADS1262_SETTLE_TIME_USECS);

	/* Clearing the RESET bit in the power register to detect ADC reset */
	ret = ads1262_reg_write(priv, ADS1262_REG_POWER, ADS1262_INTREF_ENABLE);
	if (ret)
		return ret;

	/* Setting the ADC to one-shot conversion mode */
	ret = ads1262_reg_write(priv, ADS1262_REG_MODE0, ADS1262_MODE0_ONE_SHOT);
	if (ret)
		return ret;

	ret = ads1262_reg_read(priv, ADS1262_REG_INPMUX);
	if (ret)
		return ret;

	priv->prev_channel = priv->cmd_buffer[2];

	return ret;
}

static int ads1262_get_samp_freq(struct ads1262_private *priv, int *val)
{
	unsigned long samp_freq;
	int ret;

	ret = ads1262_reg_read(priv, ADS1262_REG_MODE2);
	if (ret)
		return ret;

	samp_freq = priv->cmd_buffer[2];

	*val = (samp_freq & ADS1262_MASK_MODE2_DR);

	return IIO_VAL_INT;
}

/**
 * ads1262_read - Read a single sample from the ADC
 * @priv: Pointer to the ADS1262 private data structure
 * @chan: Pointer to the IIO channel specification
 * @val: Pointer to store the read value
 *
 * Reads a single sample from the specified ADC channel. For differential
 * channels, it sets up the MUX with both channels. For single-ended channels,
 * it uses the channel number and AINCOM (0x0A).
 *
 * Return: 0 on success, negative error code on failure
 */
static int ads1262_read(struct ads1262_private *priv,
			struct iio_chan_spec const *chan, int *val)
{
	u8 mux_value;
	int ret;

	if (chan->differential) {
		mux_value = (chan->channel << 4) | chan->channel2;
	} else {
		/* For single-ended channels, use the channel number on one end
		 * and AINCOM (0x0A) on the other end
		 */
		mux_value = (chan->channel << 4) | 0x0A;
	}

	if (mux_value != priv->prev_channel) {
		ret = ads1262_write_cmd(priv, ADS1262_CMD_STOP1);
		if (ret)
			return ret;

		ret = ads1262_reg_write(priv, ADS1262_REG_INPMUX, mux_value);
		if (ret)
			return ret;

		priv->prev_channel = mux_value;
	}

	ret = ads1262_write_cmd(priv, ADS1262_CMD_START1);
	if (ret)
		return ret;

	usleep_range(1000, 2000);

	*val = sign_extend64(get_unaligned_be32(priv->rx_buffer + 1),
			     ADS1262_BITS_PER_SAMPLE - 1);

	return 0;
}

static int ads1262_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct ads1262_private *spi = iio_priv(indio_dev);
	s64 temp;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ads1262_read(spi, chan, val);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_VOLTAGE:
			*val = ADS1262_VOLTAGE_INT_REF_uV;
			*val2 = chan->scan_type.realbits;
			return IIO_VAL_FRACTIONAL_LOG2;
		case IIO_TEMP:
			temp = (s64)ADS1262_VOLTAGE_INT_REF_uV * MILLI;
			temp /= ADS1262_TEMP_SENSITIVITY_uV_per_C;
			*val = temp;
			*val2 = chan->scan_type.realbits;
			return IIO_VAL_FRACTIONAL_LOG2;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SAMP_FREQ:
		return ads1262_get_samp_freq(spi, val);
	default:
		return -EINVAL;
	}
}

static const struct iio_info ads1262_info = {
	.read_raw = ads1262_read_raw,
};

static void ads1262_stop(void *ptr)
{
	struct ads1262_private *adc = ptr;

	ads1262_write_cmd(adc, ADS1262_CMD_STOP1);
}

static int ads1262_probe(struct spi_device *spi)
{
	struct ads1262_private *adc;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*adc));
	if (!indio_dev)
		return -ENOMEM;

	adc = iio_priv(indio_dev);
	adc->spi = spi;

	spi->mode = SPI_MODE_1;
	spi->max_speed_hz = ADS1262_SPI_BUS_SPEED_SLOW;
	spi_set_drvdata(spi, indio_dev);

	indio_dev->dev.parent = &spi->dev;
	indio_dev->name = spi_get_device_id(spi)->name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = ads1262_channels;
	indio_dev->num_channels = ARRAY_SIZE(ads1262_channels);
	indio_dev->info = &ads1262_info;

	ret = ads1262_reg_read(adc, ADS1262_REG_ID);
	if (ret)
		return ret;

	if (adc->rx_buffer[2] != ADS1262_REG_ID)
		dev_err_probe(&spi->dev, -EINVAL, "Wrong device ID 0x%x\n",
			      adc->rx_buffer[2]);

	ret = devm_add_action_or_reset(&spi->dev, ads1262_stop, adc);
	if (ret)
		return ret;

	ret = ads1262_init(indio_dev);
	if (ret)
		return ret;

	return devm_iio_device_register(&spi->dev, indio_dev);
}

static struct spi_device_id ads1262_id_table[] = {
	{ "ads1262" },
	{}
};
MODULE_DEVICE_TABLE(spi, ads1262_id_table);

static const struct of_device_id ads1262_of_match[] = {
	{ .compatible = "ti,ads1262" },
	{},
};
MODULE_DEVICE_TABLE(of, ads1262_of_match);

static struct spi_driver ads1262_driver = {
	.driver = {
		.name = "ads1262",
		.of_match_table = ads1262_of_match,
	},
	.probe = ads1262_probe,
	.id_table = ads1262_id_table,
};
module_spi_driver(ads1262_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sayyad Abid <sayyad.abid16@gmail.com>");
MODULE_DESCRIPTION("TI ADS1262 ADC");
