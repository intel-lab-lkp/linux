// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 Analog Devices, Inc.
 * Author: Marcelo Schmitt <marcelo.schmitt@analog.com>
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/crc8.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/iio/types.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/time.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/units.h>

#define AD4134_RESET_TIME_US			(10 * USEC_PER_SEC)

#define AD4134_REG_READ_MASK			BIT(7)
#define AD4134_SPI_MAX_XFER_LEN			3

#define AD4134_EXT_CLOCK_MHZ			(48 * HZ_PER_MHZ)

#define AD4134_NUM_CHANNELS			4
#define AD4134_CHAN_PRECISION_BITS		24

#define AD4134_IFACE_CONFIG_A_REG		0x00
#define AD4134_IFACE_CONFIG_B_REG		0x01
#define AD4134_IFACE_CONFIG_B_SINGLE_INSTR	BIT(7)

#define AD4134_DEVICE_CONFIG_REG		0x02
#define AD4134_DEVICE_CONFIG_POWER_MODE_MASK	BIT(0)
#define AD4134_POWER_MODE_HIGH_PERF		0x1

#define AD4134_SILICON_REV_REG			0x07
#define AD4134_SCRATCH_PAD_REG			0x0A
#define AD4134_STREAM_MODE_REG			0x0E
#define AD4134_SDO_PIN_SRC_SEL_REG		0x10
#define AD4134_SDO_PIN_SRC_SEL_SDO_SEL_MASK	BIT(2)

#define AD4134_DATA_PACKET_CONFIG_REG		0x11
#define AD4134_DATA_PACKET_CONFIG_FRAME_MASK	GENMASK(5, 4)
#define AD4134_DATA_PACKET_24BIT_FRAME		0x2

#define AD4134_DIG_IF_CFG_REG			0x12
#define AD4134_DIF_IF_CFG_FORMAT_MASK		GENMASK(1, 0)
#define AD4134_DATA_FORMAT_SINGLE_CH_MODE	0x0

#define AD4134_PW_DOWN_CTRL_REG			0x13
#define AD4134_DEVICE_STATUS_REG		0x15
#define AD4134_ODR_VAL_INT_LSB_REG		0x16
#define AD4134_CH3_OFFSET_MSB_REG		0x3E
#define AD4134_AIN_OR_ERROR_REG			0x48

/*
 * AD4134 register map ends at address 0x48 and there is no register for
 * retrieving ADC sample data. Though, to make use of Linux regmap API both
 * for register access and sample read, we define one virtual register for each
 * ADC channel. AD4134_CH_VREG(x) maps a channel number to it's virtual register
 * address while AD4134_VREG_CH(x) tells which channel given the address.
 */
#define AD4134_CH_VREG(x)			((x) + 0x50)
#define AD4134_VREG_CH(x)			((x) - 0x50)

#define AD4134_SPI_CRC_POLYNOM			0x07
#define AD4134_SPI_CRC_INIT_VALUE		0xA5
static unsigned char ad4134_spi_crc_table[CRC8_TABLE_SIZE];

#define AD4134_CHANNEL(_index) {						\
	.type = IIO_VOLTAGE,							\
	.indexed = 1,								\
	.channel = (_index),							\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),				\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),			\
}

static const struct iio_chan_spec ad4134_chan_set[] = {
	AD4134_CHANNEL(0),
	AD4134_CHANNEL(1),
	AD4134_CHANNEL(2),
	AD4134_CHANNEL(3),
};

struct ad4134_state {
	struct spi_device *spi;
	struct device *dev;
	struct regmap *regmap;
	unsigned long sys_clk_hz;
	struct gpio_desc *odr_gpio;
	int refin_mv;
	/*
	 * DMA (thus cache coherency maintenance) requires the transfer buffers
	 * to live in their own cache lines.
	 */
	u8 rx_buf[AD4134_SPI_MAX_XFER_LEN] __aligned(IIO_DMA_MINALIGN);
	u8 tx_buf[AD4134_SPI_MAX_XFER_LEN];
};

static const struct regmap_range ad4134_regmap_rd_range[] = {
	regmap_reg_range(AD4134_IFACE_CONFIG_A_REG, AD4134_SILICON_REV_REG),
	regmap_reg_range(AD4134_SCRATCH_PAD_REG, AD4134_PW_DOWN_CTRL_REG),
	regmap_reg_range(AD4134_DEVICE_STATUS_REG, AD4134_AIN_OR_ERROR_REG),
	regmap_reg_range(AD4134_CH_VREG(0), AD4134_CH_VREG(AD4134_NUM_CHANNELS)),
};

static const struct regmap_range ad4134_regmap_wr_range[] = {
	regmap_reg_range(AD4134_IFACE_CONFIG_A_REG, AD4134_DEVICE_CONFIG_REG),
	regmap_reg_range(AD4134_SCRATCH_PAD_REG, AD4134_SCRATCH_PAD_REG),
	regmap_reg_range(AD4134_STREAM_MODE_REG, AD4134_PW_DOWN_CTRL_REG),
	regmap_reg_range(AD4134_ODR_VAL_INT_LSB_REG, AD4134_CH3_OFFSET_MSB_REG),
};

static const struct regmap_access_table ad4134_regmap_rd_table = {
	.yes_ranges = ad4134_regmap_rd_range,
	.n_yes_ranges = ARRAY_SIZE(ad4134_regmap_rd_range),
};

static const struct regmap_access_table ad4134_regmap_wr_table = {
	.yes_ranges = ad4134_regmap_wr_range,
	.n_yes_ranges = ARRAY_SIZE(ad4134_regmap_wr_range),
};

static int ad4134_calc_spi_crc(u8 inst, u8 data)
{
	u8 buf[] = { inst, data };

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
	struct spi_transfer xfer = {
		.tx_buf = st->tx_buf,
		.rx_buf = st->rx_buf,
		.len = AD4134_SPI_MAX_XFER_LEN,
	};
	int ret;

	ad4134_prepare_spi_tx_buf(reg, val, st->tx_buf);

	ret = spi_sync_transfer(st->spi, &xfer, 1);
	if (ret)
		return ret;

	if (st->rx_buf[2] != st->tx_buf[2])
		dev_dbg(&st->spi->dev, "reg write CRC check failed\n");

	return 0;
}

static int ad4134_data_read(struct ad4134_state *st, unsigned int reg,
			    unsigned int *val)
{
	unsigned int i;
	int ret;

	/*
	 * To be able to read data from all 4 channels through a single line, we
	 * set DOUTx output format to 0 in the digital interface config register
	 * (0x12). With that, data from all four channels is serialized and
	 * output on DOUT0. During probe, we also set SDO_PIN_SRC_SEL in
	 * DEVICE_CONFIG_1 register to duplicate DOUT0 on the SDO pin. Combined,
	 * those configurations enable ADC data read through a conventional SPI
	 * interface. Now we read data from all channels but keep only the bits
	 * from the requested one.
	 */
	for (i = 0; i < ARRAY_SIZE(ad4134_chan_set); i++) {
		ret = spi_write_then_read(st->spi, NULL, 0, st->rx_buf,
					  BITS_TO_BYTES(AD4134_CHAN_PRECISION_BITS));
		if (ret)
			return ret;

		if (i != AD4134_VREG_CH(reg))
			continue;

		*val = get_unaligned_be24(st->rx_buf);
	}

	return 0;
}

static int ad4134_register_read(struct ad4134_state *st, unsigned int reg,
				unsigned int *val)
{
	struct spi_transfer xfer = {
		.tx_buf = st->tx_buf,
		.rx_buf = st->rx_buf,
		.len = AD4134_SPI_MAX_XFER_LEN,
	};
	unsigned int inst;
	int ret;

	inst = AD4134_REG_READ_MASK | reg;
	ad4134_prepare_spi_tx_buf(inst, 0, st->tx_buf);

	ret = spi_sync_transfer(st->spi, &xfer, 1);
	if (ret)
		return ret;

	*val = st->rx_buf[1];

	/* Check CRC */
	if (st->rx_buf[2] != st->tx_buf[2])
		dev_dbg(&st->spi->dev, "reg read CRC check failed\n");

	return 0;
}

static int ad4134_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct ad4134_state *st = context;

	if (reg >= AD4134_CH_VREG(0))
		return ad4134_data_read(st, reg, val);

	return ad4134_register_read(st, reg, val);
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
	int ret;

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
		*val2 = AD4134_CHAN_PRECISION_BITS - 1;

		return IIO_VAL_FRACTIONAL_LOG2;
	default:
		return -EINVAL;
	}
}

static int ad4134_debugfs_reg_access(struct iio_dev *indio_dev,
				     unsigned int reg, unsigned int writeval,
				     unsigned int *readval)
{
	struct ad4134_state *st = iio_priv(indio_dev);

	if (readval)
		return regmap_read(st->regmap, reg, readval);

	return regmap_write(st->regmap, reg, writeval);
}

static int ad4134_min_io_mode_setup(struct ad4134_state *st)
{
	struct device *dev = &st->spi->dev;
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
		return dev_err_probe(dev, ret,
				     "failed to set single channel mode\n");

	ret = regmap_set_bits(st->regmap, AD4134_SDO_PIN_SRC_SEL_REG,
			      AD4134_SDO_PIN_SRC_SEL_SDO_SEL_MASK);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to set SDO source selection\n");

	return regmap_set_bits(st->regmap, AD4134_IFACE_CONFIG_B_REG,
			       AD4134_IFACE_CONFIG_B_SINGLE_INSTR);
}

static const struct iio_info ad4134_info = {
	.read_raw = ad4134_read_raw,
	.debugfs_reg_access = ad4134_debugfs_reg_access,
};

static const char * const ad4143_regulator_names[] = {
	"avdd5", "dvdd5", "iovdd", "refin",
	"avdd1v8", "dvdd1v8", "clkvdd",	"ldoin",
};

static int ad4134_regulator_setup(struct ad4134_state *st)
{
	struct device *dev = &st->spi->dev;
	bool use_internal_ldo_regulator;
	int ret;

	/* Required regulators */
	ret = devm_regulator_bulk_get_enable(dev, 3, ad4143_regulator_names);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable power supplies\n");

	/* Required regulator that we need to read the voltage */
	ret = devm_regulator_get_enable_read_voltage(dev, "refin");
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get REFIN voltage.\n");

	st->refin_mv = ret / MILLI;

	/*
	 * If ldoin is not provided, then avdd1v8, dvdd1v8, and clkvdd are
	 * required.
	 */
	ret = devm_regulator_get_enable_optional(dev, "ldoin");
	if (ret < 0 && ret != -ENODEV)
		return dev_err_probe(dev, ret, "failed to enable ldoin supply\n");

	use_internal_ldo_regulator = ret == 0;

	if (!use_internal_ldo_regulator) {
		ret = devm_regulator_get_enable(dev, "avdd1v8");
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "failed to enable avdd1v8 supply\n");

		ret = devm_regulator_get_enable(dev, "dvdd1v8");
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "failed to enable dvdd1v8 supply\n");

		ret = devm_regulator_get_enable(dev, "clkvdd");
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "failed to enable clkvdd supply\n");
	}

	return 0;
}

static int ad4134_clock_select(struct ad4134_state *st)
{
	struct device *dev = &st->spi->dev;
	struct clk *sys_clk;
	int ret;

	sys_clk = devm_clk_get_optional_enabled(dev, "xtal1-xtal2");
	if (IS_ERR_OR_NULL(sys_clk)) {
		ret = PTR_ERR_OR_ZERO(sys_clk);
		sys_clk = devm_clk_get_enabled(dev, "clkin");
		if (IS_ERR(sys_clk))
			return dev_err_probe(dev, PTR_ERR(sys_clk),
					     "failed to get xtal1-xtal2: %d, clkin: %ld\n",
					     ret, PTR_ERR(sys_clk));
	}

	st->sys_clk_hz = clk_get_rate(sys_clk);
	if (st->sys_clk_hz != AD4134_EXT_CLOCK_MHZ)
		dev_warn(dev, "invalid external clock frequency %lu\n",
			 st->sys_clk_hz);

	return 0;
}

static int ad4134_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct gpio_desc *reset_gpio;
	struct iio_dev *indio_dev;
	struct ad4134_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->spi = spi;

	indio_dev->name = "ad4134";
	indio_dev->channels = ad4134_chan_set;
	indio_dev->num_channels = ARRAY_SIZE(ad4134_chan_set);
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ad4134_info;

	ret = ad4134_regulator_setup(st);
	if (ret)
		return ret;

	ret = ad4134_clock_select(st);
	if (ret)
		return ret;

	reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset_gpio))
		return dev_err_probe(dev, PTR_ERR(reset_gpio),
				     "failed to find reset GPIO\n");

	if (reset_gpio) {
		fsleep(AD4134_RESET_TIME_US);
		gpiod_set_value_cansleep(reset_gpio, 0);
	}

	crc8_populate_msb(ad4134_spi_crc_table, AD4134_SPI_CRC_POLYNOM);

	st->regmap = devm_regmap_init(dev, NULL, st, &ad4134_regmap_config);
	if (IS_ERR(st->regmap))
		return dev_err_probe(dev, PTR_ERR(st->regmap),
				     "failed to initialize regmap");

	ret = ad4134_min_io_mode_setup(st);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to setup minimum I/O mode\n");

	/* Bump precision to 24-bit */
	ret = regmap_update_bits(st->regmap, AD4134_DATA_PACKET_CONFIG_REG,
				 AD4134_DATA_PACKET_CONFIG_FRAME_MASK,
				 FIELD_PREP(AD4134_DATA_PACKET_CONFIG_FRAME_MASK,
					    AD4134_DATA_PACKET_24BIT_FRAME));
	if (ret)
		return ret;

	/* Set high performance power mode */
	ret = regmap_update_bits(st->regmap, AD4134_DEVICE_CONFIG_REG,
				 AD4134_DEVICE_CONFIG_POWER_MODE_MASK,
				 FIELD_PREP(AD4134_DEVICE_CONFIG_POWER_MODE_MASK,
					    AD4134_POWER_MODE_HIGH_PERF));
	if (ret)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct spi_device_id ad4134_id[] = {
	{ "ad4134" },
	{ }
};
MODULE_DEVICE_TABLE(spi, ad4134_id);

static const struct of_device_id ad4134_of_match[] = {
	{ .compatible = "adi,ad4134" },
	{ }
};
MODULE_DEVICE_TABLE(of, ad4134_of_match);

static struct spi_driver ad4134_driver = {
	.driver = {
		.name = "ad4134",
		.of_match_table = ad4134_of_match,
	},
	.probe = ad4134_probe,
	.id_table = ad4134_id,
};
module_spi_driver(ad4134_driver);

MODULE_AUTHOR("Marcelo Schmitt <marcelo.schmitt@analog.com>");
MODULE_DESCRIPTION("Analog Devices AD4134 SPI driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_AD4134");
