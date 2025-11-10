// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 Analog Devices, Inc.
 * Author: Marcelo Schmitt <marcelo.schmitt@analog.com>
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/crc8.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/iio/types.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#include "ad4134-common.h"

static const struct iio_chan_spec ad4134_chan_set[] = {
	AD4134_CHANNEL(0),
	AD4134_CHANNEL(1),
	AD4134_CHANNEL(2),
	AD4134_CHANNEL(3),
};

static int ad4134_calc_spi_crc(u8 inst, u8 data)
{
	u8 buf[] = {inst, data};

	return crc8(ad4134_spi_crc_table, buf, ARRAY_SIZE(buf),
		    AD4134_SPI_CRC_INIT_VALUE);
}

static void ad4134_prepare_spi_tx_buf(u8 inst, u8 data, u8 *buf)
{
	buf[0] = inst;
	buf[1] = data;
	buf[2] = ad4134_calc_spi_crc(inst, data);
}

static int ad4134_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct ad4134_state *st = context;
	struct spi_device *spi = to_spi_device(st->dev);
	struct spi_transfer xfer = {
		.tx_buf = st->tx_buf,
		.rx_buf = st->rx_buf,
		.len = AD4134_SPI_MAX_XFER_LEN,
	};
	int ret;

	ad4134_prepare_spi_tx_buf(reg, val, st->tx_buf);

	ret = spi_sync_transfer(spi, &xfer, 1);
	if (ret)
		return ret;

	if (st->rx_buf[2] != st->tx_buf[2])
		dev_dbg(st->dev, "reg write CRC check failed\n");

	return 0;
}

static int ad4134_data_read(struct ad4134_state *st, unsigned int reg,
			    unsigned int *val)
{
	struct spi_device *spi = to_spi_device(st->dev);
	struct iio_scan_type *scan_type = &ad4134_scan_types[st->current_scan_type];
	unsigned int i;
	int ret;

	/*
	 * Data from all four channels is serialized and output on SDO. Read
	 * them all but keep only the requested data.
	 */
	for (i = 0; i < ARRAY_SIZE(ad4134_chan_set); i++) {
		ret = spi_write_then_read(spi, NULL, 0, st->rx_buf,
					  BITS_TO_BYTES(scan_type->storagebits));
		if (ret)
			return ret;

		if (i != AD4134_VREG_CH(reg))
			continue;

		if (scan_type->realbits == 16)
			*val = get_unaligned_be16(st->rx_buf);
		else
			*val = get_unaligned_be24(st->rx_buf);

		*val >>= scan_type->shift;
	}

	return 0;
}

static int ad4134_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct ad4134_state *st = context;
	struct spi_device *spi = to_spi_device(st->dev);
	struct spi_transfer xfer = {
		.tx_buf = st->tx_buf,
		.rx_buf = st->rx_buf,
		.len = AD4134_SPI_MAX_XFER_LEN,
	};
	unsigned int inst;
	int ret;

	if (reg >= AD4134_CH_VREG(0))
		return ad4134_data_read(st, reg, val);

	inst = AD4134_REG_READ_MASK | reg;
	ad4134_prepare_spi_tx_buf(inst, 0, st->tx_buf);

	ret = spi_sync_transfer(spi, &xfer, 1);
	if (ret)
		return ret;

	*val = st->rx_buf[1];

	/* Check CRC */
	if (st->rx_buf[2] != st->tx_buf[2])
		dev_dbg(st->dev, "reg read CRC check failed\n");

	return 0;
}

static const struct regmap_config ad4134_regmap_config = {
	.reg_read = ad4134_reg_read,
	.reg_write = ad4134_reg_write,
	.rd_table = &ad4134_regmap_rd_table,
	.wr_table = &ad4134_regmap_wr_table,
	.max_register = AD4134_CH_VREG(ARRAY_SIZE(ad4134_chan_set)),
};

static int ad4134_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long info)
{
	struct ad4134_state *st = iio_priv(indio_dev);
	const struct iio_scan_type *scan_type;
	int ret;

	scan_type = iio_get_current_scan_type(indio_dev, chan);
	if (IS_ERR(scan_type))
		return PTR_ERR(scan_type);

	switch (info) {
	case IIO_CHAN_INFO_RAW:
		gpiod_set_value_cansleep(st->odr_gpio, 1);
		fsleep(1);
		gpiod_set_value_cansleep(st->odr_gpio, 0);
		ret = regmap_read(st->regmap, AD4134_CH_VREG(chan->channel), val);
		if (ret)
			return ret;

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = st->refin_mv;
		*val2 = scan_type->realbits - (scan_type->sign == 's' ? 1 : 0);

		return IIO_VAL_FRACTIONAL_LOG2;
	default:
		return -EINVAL;
	}
}

static int ad4134_reg_access(struct iio_dev *indio_dev, unsigned int reg,
			     unsigned int writeval, unsigned int *readval)
{
	struct ad4134_state *st = iio_priv(indio_dev);

	if (readval)
		return regmap_read(st->regmap, reg, readval);

	return regmap_write(st->regmap, reg, writeval);
}

static struct regmap *ad4134_minimum_io_regmap_init(struct ad4134_state *st)
{
	return devm_regmap_init(st->dev, NULL, st, &ad4134_regmap_config);
}

static int ad4134_get_current_scan_type(const struct iio_dev *indio_dev,
					const struct iio_chan_spec *chan)
{
	struct ad4134_state *st = iio_priv(indio_dev);

	return st->current_scan_type;
}

static int ad4134_min_io_mode_setup(struct ad4134_state *st)
{
	struct device *dev = st->dev;
	int ret;

	st->odr_gpio = devm_gpiod_get(dev, "odr", GPIOD_OUT_LOW);
	if (IS_ERR(st->odr_gpio))
		return dev_err_probe(dev, PTR_ERR(st->odr_gpio),
				     "failed to get ODR GPIO\n");

	ret = regmap_update_bits(st->regmap, AD4134_DIG_IF_CFG_REG,
				 AD4134_DIF_IF_CFG_FORMAT_MASK,
				 FIELD_PREP(AD4134_DIF_IF_CFG_FORMAT_MASK,
					    AD4134_DATA_FORMAT_SINGLE_CH_MODE));
	if (ret)
		return ret;

	ret = regmap_set_bits(st->regmap, AD4134_SDO_PIN_SRC_SEL_REG,
			      AD4134_SDO_PIN_SRC_SEL_SDO_SEL_MASK);
	if (ret)
		return ret;

	return regmap_set_bits(st->regmap, AD4134_IFACE_CONFIG_B_REG,
			       AD4134_IFACE_CONFIG_B_SINGLE_INSTR);
}

static const struct iio_info ad4134_info = {
	.read_raw = ad4134_read_raw,
	.get_current_scan_type = ad4134_get_current_scan_type,
	.debugfs_reg_access = ad4134_reg_access,
};

static int ad4134_config_iio_dev(struct iio_dev *indio_dev)
{
	indio_dev->channels = ad4134_chan_set;
	indio_dev->num_channels = ARRAY_SIZE(ad4134_chan_set);

	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ad4134_info;

	return 0;
};

static const struct ad4134_bus_ops ad4134_min_io_bops = {
	.config_iio_dev = &ad4134_config_iio_dev,
	.init_regmap = &ad4134_minimum_io_regmap_init,
	.setup = &ad4134_min_io_mode_setup,
};

static const struct ad4134_bus_info ad4134_min_io_bus_info = {
	.chip_info = &ad4134_chip_info,
	.bops = &ad4134_min_io_bops,
};

static int ad4134_spi_probe(struct spi_device *spi)
{
	const struct ad4134_bus_info *bus_info;

	bus_info = spi_get_device_match_data(spi);
	if (!bus_info)
		return -EINVAL;

	return ad4134_probe(&spi->dev, bus_info);
}

static const struct spi_device_id ad4134_id[] = {
	{ "ad4134", (kernel_ulong_t)&ad4134_min_io_bus_info },
	{ },
};
MODULE_DEVICE_TABLE(spi, ad4134_id);

static const struct of_device_id ad4134_of_match[] = {
	{ .compatible = "adi,ad4134", .data = &ad4134_min_io_bus_info },
	{ }
};
MODULE_DEVICE_TABLE(of, ad4134_of_match);

static struct spi_driver ad4134_driver = {
	.driver = {
		.name = "ad4134",
		.of_match_table = ad4134_of_match,
	},
	.probe = ad4134_spi_probe,
	.id_table = ad4134_id,
};
module_spi_driver(ad4134_driver);

MODULE_AUTHOR("Marcelo Schmitt <marcelo.schmitt@analog.com>");
MODULE_DESCRIPTION("Analog Devices AD4134 SPI driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_AD4134");
