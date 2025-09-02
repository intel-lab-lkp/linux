// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
/*
 * Analog Devices MAX14001/MAX14002 ADC driver
 *
 * Copyright (C) 2023-2025 Analog Devices Inc.
 * Copyright (C) 2023 Kim Seer Paller <kimseer.paller@analog.com>
 * Copyright (c) 2025 Marilene Andrade Garcia <marilene.agarcia@gmail.com>
 *
 * Datasheet: https://www.analog.com/media/en/technical-documentation/data-sheets/MAX14001-MAX14002.pdf
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bitrev.h>
#include <linux/bits.h>
#include <linux/byteorder/generic.h>
#include <linux/device.h>
#include <linux/iio/iio.h>
#include <linux/iio/types.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/types.h>

/* MAX14001 Registers Address */
#define MAX14001_REG_ADC		0x00
#define MAX14001_REG_FADC		0x01
#define MAX14001_REG_FLAGS		0x02
#define MAX14001_REG_FLTEN		0x03
#define MAX14001_REG_THL		0x04
#define MAX14001_REG_THU		0x05
#define MAX14001_REG_INRR		0x06
#define MAX14001_REG_INRT		0x07
#define MAX14001_REG_INRP		0x08
#define MAX14001_REG_CFG		0x09
#define MAX14001_REG_ENBL		0x0A
#define MAX14001_REG_ACT		0x0B
#define MAX14001_REG_WEN		0x0C

#define MAX14001_REG_VERIFICATION(x)	((x) + 0x10)

#define MAX14001_REG_CFG_EXRF		BIT(5)

#define MAX14001_MASK_ADDR		GENMASK(15, 11)
#define MAX14001_MASK_DATA		GENMASK(9, 0)

#define MAX14001_SET_WRITE_BIT		BIT(10)
#define MAX14001_WRITE_WEN		0x294

enum max14001_chip_model {
	max14001,
	max14002,
};

struct max14001_chip_info {
	const char *name;
};

struct max14001_state {
	const struct max14001_chip_info *chip_info;
	struct spi_device *spi;
	int vref_mv;
	/*
	 * lock protect against multiple concurrent accesses, RMW sequence,
	 * and SPI transfer.
	 */
	struct mutex lock;
	/*
	 * The following buffers will be bit-reversed during device
	 * communication, because the device transmits and receives data
	 * LSB-first.
	 * DMA (thus cache coherency maintenance) requires the transfer
	 * buffers to live in their own cache lines.
	 */
	__be16 spi_tx_buffer __aligned(IIO_DMA_MINALIGN);
	__be16 spi_rx_buffer;
};

static struct max14001_chip_info max14001_chip_info_tbl[] = {
	[max14001] = {
		.name = "max14001",
	},
	[max14002] = {
		.name = "max14002",
	},
};

static int max14001_read(struct max14001_state *st, u16 reg_addr, u16 *reg_data)
{
	struct spi_transfer xfers[] = {
		{
			.tx_buf = &st->spi_tx_buffer,
			.len = sizeof(st->spi_tx_buffer),
			.cs_change = 1,
		}, {
			.rx_buf = &st->spi_rx_buffer,
			.len = sizeof(st->spi_rx_buffer),
		},
	};
	int ret;

	/*
	 * Prepare SPI transmit buffer 16 bit-value big-endian format and
	 * reverses bit order to align with the LSB-first input on SDI port
	 * in order to meet the device communication requirements.
	 */
	st->spi_tx_buffer = FIELD_PREP(MAX14001_MASK_ADDR, reg_addr);
	st->spi_tx_buffer = bitrev16(cpu_to_be16(st->spi_tx_buffer));

	ret = spi_sync_transfer(st->spi, xfers, ARRAY_SIZE(xfers));
	if (ret)
		return ret;

	/*
	 * Convert received 16-bit value from big-endian to cpu-endian format
	 * and reverses bit order.
	 */
	st->spi_rx_buffer = bitrev16(be16_to_cpu(st->spi_rx_buffer));
	*reg_data = FIELD_GET(MAX14001_MASK_DATA, st->spi_rx_buffer);

	return 0;
}

static int max14001_write(struct max14001_state *st, u16 reg_addr, u16 reg_data)
{
	/*
	 * Prepare SPI transmit buffer 16 bit-value big-endian format and
	 * reverses bit order to align with the LSB-first input on SDI port
	 * in order to meet the device communication requirements.
	 */
	st->spi_tx_buffer = FIELD_PREP(MAX14001_MASK_ADDR, reg_addr) |
			    FIELD_PREP(MAX14001_SET_WRITE_BIT, 1) |
			    FIELD_PREP(MAX14001_MASK_DATA, reg_data);
	st->spi_tx_buffer = bitrev16(cpu_to_be16(st->spi_tx_buffer));

	return spi_write(st->spi, &st->spi_tx_buffer, sizeof(st->spi_tx_buffer));
}

static int max14001_write_single_reg(struct max14001_state *st, u16 reg_addr,
				     u16 reg_data)
{
	int ret;

	/* Enable register write */
	ret = max14001_write(st, MAX14001_REG_WEN, MAX14001_WRITE_WEN);
	if (ret)
		return ret;

	/* Write data into register */
	ret = max14001_write(st, reg_addr, reg_data);
	if (ret)
		return ret;

	/* Disable register write */
	ret = max14001_write(st, MAX14001_REG_WEN, 0);
	if (ret)
		return ret;

	return ret;
}

static int max14001_write_verification_reg(struct max14001_state *st,
					   u16 reg_addr)
{
	u16 reg_data;
	int ret;

	ret = max14001_read(st, reg_addr, &reg_data);
	if (ret)
		return ret;

	return max14001_write(st, MAX14001_REG_VERIFICATION(reg_addr), reg_data);
}

static int max14001_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	struct max14001_state *st = iio_priv(indio_dev);
	u16 reg_data;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&st->lock);
		ret = max14001_read(st, MAX14001_REG_ADC, &reg_data);
		mutex_unlock(&st->lock);
		if (ret)
			return ret;

		*val = reg_data;

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_AVERAGE_RAW:
		mutex_lock(&st->lock);
		ret = max14001_read(st, MAX14001_REG_FADC, &reg_data);
		mutex_unlock(&st->lock);
		if (ret)
			return ret;

		*val = reg_data;

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = st->vref_mv;
		*val2 = 10;

		return IIO_VAL_FRACTIONAL_LOG2;
	default:
		return -EINVAL;
	}
}

static const struct iio_info max14001_info = {
	.read_raw = max14001_read_raw,
};

static const struct iio_chan_spec max14001_channel[] = {
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = 0,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_AVERAGE_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
	}
};

static int max14001_disable_mv_fault(struct max14001_state *st)
{
	u16 reg_addr;
	int ret;

	/* Enable SPI Registers Write */
	ret = max14001_write(st, MAX14001_REG_WEN, MAX14001_WRITE_WEN);
	if (ret)
		return ret;

	/*
	 * Reads all registers and writes the values back to their appropriate
	 * verification registers to clear the Memory Validation fault.
	 */
	for (reg_addr = MAX14001_REG_FLTEN; reg_addr <= MAX14001_REG_ENBL; reg_addr++) {
		ret = max14001_write_verification_reg(st, reg_addr);
		if (ret)
			return ret;
	}

	/* Disable SPI Registers Write */
	return max14001_write(st, MAX14001_REG_WEN, 0);
}

static int max14001_probe(struct spi_device *spi)
{
	struct iio_dev *indio_dev;
	struct max14001_state *st;
	struct device *dev = &spi->dev;
	int ret, ext_vrefin = 0;
	u16 reg_data;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->spi = spi;
	st->chip_info = spi_get_device_match_data(spi);
	if (!st->chip_info)
		return dev_err_probe(dev, -ENODEV, "Failed to get match data\n");

	indio_dev->name = st->chip_info->name;
	indio_dev->info = &max14001_info;
	indio_dev->channels = max14001_channel;
	indio_dev->num_channels = ARRAY_SIZE(max14001_channel);
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret,
			"Failed to enable specified Vdd supply\n");

	ret = devm_regulator_get_enable(dev, "vddl");
	if (ret)
		return dev_err_probe(dev, ret,
			"Failed to enable specified Vddl supply\n");

	ret = devm_regulator_get_enable_read_voltage(dev, "vrefin");
	if (ret < 0) {
		st->vref_mv = 1250000 / 1000;
	} else {
		st->vref_mv = ret / 1000;
		ext_vrefin = 1;
	}

	ret = devm_mutex_init(dev, &st->lock);
	if (ret)
		return dev_err_probe(dev, ret,
			"Failed to init the mutex\n");

	if (ext_vrefin) {
		/*
		 * Configure the MAX14001/MAX14002 to use an external voltage
		 * reference source for the ADC.
		 */
		ret = max14001_read(st, MAX14001_REG_CFG, &reg_data);
		if (ret)
			return dev_err_probe(dev, ret,
				"Failed to read Configuration Register\n");

		reg_data |= FIELD_PREP(MAX14001_REG_CFG_EXRF, 1);
		ret = max14001_write_single_reg(st, MAX14001_REG_CFG, reg_data);
		if (ret)
			return dev_err_probe(dev, ret,
				"Failed to set Configuration Register\n");
	}

	ret = max14001_disable_mv_fault(st);
	if (ret)
		return dev_err_probe(dev, ret,
			"Failed to disable MV Fault\n");

	return devm_iio_device_register(dev, indio_dev);
}

static const struct spi_device_id max14001_id_table[] = {
	{ "max14001", (kernel_ulong_t)&max14001_chip_info_tbl[max14001] },
	{ "max14002", (kernel_ulong_t)&max14001_chip_info_tbl[max14002] },
	{ }
};

static const struct of_device_id max14001_of_match[] = {
	{ .compatible = "adi,max14001",
	  .data = &max14001_chip_info_tbl[max14001], },
	{ .compatible = "adi,max14002",
	  .data = &max14001_chip_info_tbl[max14002], },
	{ }
};
MODULE_DEVICE_TABLE(of, max14001_of_match);

static struct spi_driver max14001_driver = {
	.driver = {
		.name = "max14001",
		.of_match_table = max14001_of_match,
	},
	.probe = max14001_probe,
	.id_table = max14001_id_table,
};
module_spi_driver(max14001_driver);

MODULE_AUTHOR("Kim Seer Paller <kimseer.paller@analog.com>");
MODULE_AUTHOR("Marilene Andrade Garcia <marilene.agarcia@gmail.com>");
MODULE_DESCRIPTION("Analog Devices MAX14001/MAX14002 ADCs driver");
MODULE_LICENSE("GPL");
