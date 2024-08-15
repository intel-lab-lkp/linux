// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*
 * Copyright (c) 2024 Robert Bosch GmbH.
 *
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <linux/bitfield.h>
#include <linux/iio/iio.h>

#include "smi240.h"

#define SMI240_CRC_INIT 0x05
#define SMI240_CRC_POLY 0x0B
#define SMI240_BUS_ID 0x00

#define SMI240_SD_BIT_MASK 0x80000000
#define SMI240_CS_BIT_MASK 0x00000008

#define SMI240_WRITE_ADDR_MASK GENMASK(29, 22)
#define SMI240_WRITE_BIT_MASK 0x00200000
#define SMI240_WRITE_DATA_MASK GENMASK(18, 3)
#define SMI240_CAP_BIT_MASK 0x00100000
#define SMI240_READ_DATA_MASK GENMASK(19, 4)

static u8 smi240_crc3(u32 data, u8 init, u8 poly)
{
	u8 crc = init;
	u8 do_xor;
	s8 i = 31;

	do {
		do_xor = crc & 0x04;
		crc <<= 1;
		crc |= 0x01 & (data >> i);
		if (do_xor)
			crc ^= poly;

		crc &= 0x07;
	} while (--i >= 0);

	return crc;
}

static bool smi240_sensor_data_is_valid(u32 data)
{
	if (smi240_crc3(data, SMI240_CRC_INIT, SMI240_CRC_POLY) != 0)
		return false;

	if (FIELD_GET(SMI240_SD_BIT_MASK, data) &
	    FIELD_GET(SMI240_CS_BIT_MASK, data))
		return false;

	return true;
}

static int smi240_regmap_spi_read(void *context, const void *reg_buf,
				  size_t reg_size, void *val_buf,
				  size_t val_size)
{
	int ret;
	__be32 request, response;
	struct spi_device *spi = context;
	struct iio_dev *indio_dev = dev_get_drvdata(&spi->dev);
	struct smi240_data *data = iio_priv(indio_dev);

	request = SMI240_BUS_ID << 30;
	request |= FIELD_PREP(SMI240_CAP_BIT_MASK, data->capture);
	request |= FIELD_PREP(SMI240_WRITE_ADDR_MASK, *(u8 *)reg_buf);
	request |= smi240_crc3(request, SMI240_CRC_INIT, SMI240_CRC_POLY);
	request = cpu_to_be32(request);

	/*
	 * SMI240 module consists of a 32Bit Out Of Frame (OOF)
	 * SPI protocol, where the slave interface responds to
	 * the Master request in the next frame.
	 * CS signal must toggle (> 700 ns) between the frames.
	 */
	ret = spi_write(spi, &request, sizeof(request));
	if (ret)
		return ret;

	ret = spi_read(spi, &response, sizeof(response));
	if (ret)
		return ret;

	response = be32_to_cpu(response);

	if (!smi240_sensor_data_is_valid(response))
		return -EIO;

	response = FIELD_GET(SMI240_READ_DATA_MASK, response);
	memcpy(val_buf, &response, val_size);

	return 0;
}

static int smi240_regmap_spi_write(void *context, const void *data,
				   size_t count)
{
	__be32 request;
	struct spi_device *spi = context;
	u8 reg_addr = ((u8 *)data)[0];
	u16 reg_data = ((u8 *)data)[2] << 8 | ((u8 *)data)[1];

	request = SMI240_BUS_ID << 30;
	request |= FIELD_PREP(SMI240_WRITE_BIT_MASK, 1);
	request |= FIELD_PREP(SMI240_WRITE_ADDR_MASK, reg_addr);
	request |= FIELD_PREP(SMI240_WRITE_DATA_MASK, reg_data);
	request |= smi240_crc3(request, SMI240_CRC_INIT, SMI240_CRC_POLY);
	request = cpu_to_be32(request);

	return spi_write(spi, &request, sizeof(request));
}

static const struct regmap_bus smi240_regmap_bus = {
	.read = smi240_regmap_spi_read,
	.write = smi240_regmap_spi_write,
};

static const struct regmap_config smi240_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
};

static int smi240_spi_probe(struct spi_device *spi)
{
	struct regmap *regmap;

	regmap = devm_regmap_init(&spi->dev, &smi240_regmap_bus, &spi->dev,
				  &smi240_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(&spi->dev, PTR_ERR(regmap),
				     "Failed to initialize SPI Regmap\n");

	return smi240_core_probe(&spi->dev, regmap);
}

static const struct spi_device_id smi240_spi_id[] = { { "smi240", 0 }, {} };
MODULE_DEVICE_TABLE(spi, smi240_spi_id);

static const struct of_device_id smi240_of_match[] = {
	{ .compatible = "bosch,smi240" },
	{},
};
MODULE_DEVICE_TABLE(of, smi240_of_match);

static struct spi_driver smi240_spi_driver = {
	.probe = smi240_spi_probe,
	.id_table = smi240_spi_id,
	.driver = {
		.of_match_table = smi240_of_match,
		.name = "smi240",
	},
};
module_spi_driver(smi240_spi_driver);

MODULE_AUTHOR("Markus Lochmann <markus.lochmann@de.bosch.com>");
MODULE_AUTHOR("Stefan Gutmann <stefan.gutmann@de.bosch.com>");
MODULE_DESCRIPTION("Bosch SMI240 SPI driver");
MODULE_LICENSE("Dual BSD/GPL");
