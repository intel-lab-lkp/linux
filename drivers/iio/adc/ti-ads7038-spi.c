// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * This driver supports TI 12Bit ADC devices
 *
 *	 - ADS7038 with SPI interface
 *
 * Copyright (C) 2023 SYS TEC electronic AG
 * Author: Andre Werner <andre.werner@systec-electronic.com>
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <linux/types.h>

#include "ti-ads7038.h"

#define ADS7038_OPCODE_NOOP		0x00
#define ADS7038_OPCODE_REGREAD		0x10
#define ADS7038_OPCODE_REGWRITE		0x08
#define ADS7038_OPCODE_SETBIT		0x18
#define ADS7038_OPCODE_CLEATBIT		0x20

/*
 * The bitwidth for ADC channel results differ
 * by setting average and status
 * in the dedicated configuration registers.
 */
#define ADS7038_NO_AVG_NO_STAT		12	/* bits */
#define ADS7038_NO_AVG_STAT		16	/* bits */
#define ADS7038_AVG_NO_STAT		16	/* bits */
#define ADS7038_AVG_STAT		20	/* bits */

#define ADS7038_SPI_FRAME_SIZE_REG		3	/* bytes */
#define ADS7038_SPI_FRAME_SIZE_CHAN_MAX		2	/* elements */

static int ads7038_regmap_spi_read(void *context, unsigned int reg,
				   unsigned int *val)
{
	struct device *dev = context;
	struct spi_device *spi = to_spi_device(dev);
	int ret;
	const u8 tx_buf[ADS7038_SPI_FRAME_SIZE_REG] = {
		[0] = ADS7038_OPCODE_REGREAD,
		[1] = (u8)(reg & GENMASK(7, 0)),
		[2] = 0,	/* dummy data */
	};
	u8 rx_buf[ADS7038_SPI_FRAME_SIZE_REG] = { 0 };

	/* Data contains 8bit address and 8bit register data */
	struct spi_transfer xfer[] = {
		{
			.tx_buf = tx_buf,
			.rx_buf = NULL,
			.len = ADS7038_SPI_FRAME_SIZE_REG,
			.bits_per_word = ADS7038_REGISTER_NUM_BITS,
			.cs_change = 1,
		},
		{
			.tx_buf = NULL,
			.rx_buf = rx_buf,
			.len = ADS7038_SPI_FRAME_SIZE_REG,
			.bits_per_word = ADS7038_REGISTER_NUM_BITS,
		},
	};

	if (!val)
		return -EINVAL;

	ret = spi_sync_transfer(spi, xfer, ARRAY_SIZE(xfer));

	if (ret < 0)
		return ret;

	*val = rx_buf[0];

	return 0;
}

static int ads7038_regmap_spi_write(void *context, unsigned int reg,
				    unsigned int val)
{
	unsigned int ret;
	struct device *dev = (struct device *)context;
	struct spi_device *spi = to_spi_device(dev);
	const u8 buf[ADS7038_SPI_FRAME_SIZE_REG] = {
		[0] = ADS7038_OPCODE_REGWRITE,
		[1] = (u8)(reg & GENMASK(7, 0)),
		[2] = (u8)(val & GENMASK(7, 0)),
	};

	struct spi_transfer xfer[] = {
		{
			.tx_buf = buf,
			.rx_buf = NULL,
			.len = ARRAY_SIZE(buf),
		},
	};

	ret = spi_sync_transfer(spi, xfer, ARRAY_SIZE(xfer));

	if (ret < 0)
		return ret;

	return 0;
}

static int ads7038_read_channel(struct iio_dev *const indio_dev,
				const enum MANUAL_CHID chan,
				struct ads7038_ch_meas_result *const res)
{
	int ret;
	struct ads7038_data *data = iio_priv(indio_dev);
	struct spi_device *spi_dev = to_spi_device(data->dev);
	__be16 rx_buf[ADS7038_SPI_FRAME_SIZE_CHAN_MAX] = { 0 };

	const u8 tx_buf[] = {
		[0] = ADS7038_OPCODE_REGWRITE,
		[1] = ADS7038_CHANNEL_SEL_REG,
		[2] = chan
	};

	/*
	 * If the channel changes it needs two additional cycles before the result
	 * is ready to tranmit.
	 */
	struct spi_transfer xfer[] = {
		{
			.tx_buf = tx_buf,
			.rx_buf = NULL,
			.len = sizeof(tx_buf),
			.tx_nbits = SPI_NBITS_SINGLE,
			.cs_change = 1,
			.cs_change_delay = {
				.value = data->measure_cycle_time_us,
				.unit = SPI_DELAY_UNIT_USECS
			},
		},
		{
			.dummy_data = 1,
			.len = sizeof(tx_buf),
			.tx_nbits = SPI_NBITS_SINGLE,
			.cs_change = 1,
			.cs_change_delay = {
				.value = data->measure_cycle_time_us,
				.unit = SPI_DELAY_UNIT_USECS
			},
		},
		{
			.tx_buf = NULL,
			.rx_buf = rx_buf,
			.len = sizeof(rx_buf),
			.tx_nbits = SPI_NBITS_SINGLE,
			.cs_change = 1,
			.cs_change_delay = {
				.value = data->measure_cycle_time_us,
				.unit = SPI_DELAY_UNIT_USECS
			},
		},
		{
			.dummy_data = 1,
			.len = sizeof(rx_buf),
			.tx_nbits = SPI_NBITS_SINGLE,
		},
	};

	if (chan > AIN_MAX)
		return -EINVAL;

	mutex_lock(&data->lock);

	/* If the channel does not switch we do not need to send that transfers again */
	if (data->latest_chanid != chan)
		ret = spi_sync_transfer(spi_dev, xfer, ARRAY_SIZE(xfer));
	else
		ret = spi_sync_transfer(spi_dev, &xfer[1], (ARRAY_SIZE(xfer) - 1));

	if (ret < 0)
		goto out;

	if (data->avaraging_enabled) {
		res->raw = be16_to_cpu(rx_buf[0]);
		res->raw_is_an_average = true;
		if (data->status_appended) {
			res->status = FIELD_GET(GENMASK(15, 12), (be16_to_cpup(&rx_buf[1])));
			res->status_valid = true;
		}
	} else {
		res->raw = be16_to_cpu(rx_buf[0]);
		res->raw >>= 4;
		if (data->status_appended) {
			res->status = FIELD_GET(GENMASK(3, 0), (be16_to_cpup(&rx_buf[0])));
			res->status_valid = true;
		}
	}

out:
	mutex_unlock(&data->lock);
	return ret;
}

static struct regmap_bus ads7038_regmap_bus = {
	.reg_write = ads7038_regmap_spi_write,
	.reg_read = ads7038_regmap_spi_read,
	.reg_format_endian_default = REGMAP_ENDIAN_LITTLE,
	.val_format_endian_default = REGMAP_ENDIAN_LITTLE,
};

static int ads7038_spi_probe(struct spi_device *spi)
{
	const struct spi_device_id *id = spi_get_device_id(spi);
	struct regmap *regmap;
	int ret;

	ret = spi_setup(spi);
	if (ret < 0) {
		dev_dbg(&spi->dev, "spi_setup failed!\n");
		goto error_spi;
	}

	regmap = devm_regmap_init(&spi->dev, &ads7038_regmap_bus, &spi->dev,
				  &ads7038_regmap_config);
	if (IS_ERR(regmap)) {
		dev_dbg(&spi->dev, "Failed to allocate register map\n");
		ret = PTR_ERR(regmap);
		goto error_spi;
	}

	return ads7038_common_probe(&spi->dev, ads7038_read_channel, regmap,
				    id->name, spi->irq);

error_spi:
	dev_err(&spi->dev, "Probe failed\n");
	return ret;
}

static const struct of_device_id ads7038_of_spi_match[] = {
	{.compatible = "ti,ads7038" },
	{ }
};

MODULE_DEVICE_TABLE(of, ads7038_of_spi_match);

const struct spi_device_id ads7038_spi_id[] = {
	{.name = "ads7038" },
	{ }
};

MODULE_DEVICE_TABLE(spi, ads7038_spi_id);

static struct spi_driver ads7038_spi_driver = {
	.driver = {
		   .name = "ads7038-spi",
		   .of_match_table = ads7038_of_spi_match,
	},
	.id_table = ads7038_spi_id,
	.probe = ads7038_spi_probe,
};

module_spi_driver(ads7038_spi_driver);

MODULE_IMPORT_NS(IIO_ADS7038);
MODULE_AUTHOR("Andre Werner <andre.werner@systec-electronic.com>");
MODULE_DESCRIPTION("ADS7038 SPI bus driver");
MODULE_LICENSE("GPL");
