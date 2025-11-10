// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 Analog Devices, Inc.
 * Author: Marcelo Schmitt <marcelo.schmitt@analog.com>
 */

#include <linux/clk.h>
#include <linux/crc8.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include "ad4134-common.h"

const struct ad4134_chip_info ad4134_chip_info = {
	.name = "ad4134",
};
EXPORT_SYMBOL_NS_GPL(ad4134_chip_info, "IIO_AD4134");

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

const struct regmap_access_table ad4134_regmap_rd_table = {
	.yes_ranges = ad4134_regmap_rd_range,
	.n_yes_ranges = ARRAY_SIZE(ad4134_regmap_rd_range),
};
EXPORT_SYMBOL_NS_GPL(ad4134_regmap_rd_table, "IIO_AD4134");

const struct regmap_access_table ad4134_regmap_wr_table = {
	.yes_ranges = ad4134_regmap_wr_range,
	.n_yes_ranges = ARRAY_SIZE(ad4134_regmap_wr_range),
};
EXPORT_SYMBOL_NS_GPL(ad4134_regmap_wr_table, "IIO_AD4134");

static const char * const ad4143_regulator_names[] = {
	"avdd5", "dvdd5", "iovdd", "refin",	/* Required supplies */
	"avdd1v8", "dvdd1v8", "clkvdd",		/* Required if ldoin not provided */
	"ldoin",
};

static const char *const ad4134_clk_sel[] = {
	"xtal1-xtal2", "clkin"
};

static int ad4134_clock_select(struct ad4134_state *st)
{
	struct device *dev = st->dev;
	struct clk *sys_clk;
	int ret;

	ret = device_property_match_property_string(dev, "clock-names",
						    ad4134_clk_sel,
						    ARRAY_SIZE(ad4134_clk_sel));
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to find external clock\n");

	sys_clk = devm_clk_get_enabled(dev, ad4134_clk_sel[ret]);
	if (IS_ERR(sys_clk))
		return dev_err_probe(dev, PTR_ERR(sys_clk),
				     "failed to get %s external clock\n",
				     ad4134_clk_sel[ret]);

	st->sys_clk_rate = clk_get_rate(sys_clk);
	if (st->sys_clk_rate != AD4134_EXT_CLOCK_MHZ)
		dev_warn(dev, "invalid external clock frequency %lu\n",
			 st->sys_clk_rate);

	return 0;
}

int ad4134_probe(struct device *dev, const struct ad4134_bus_info *bus_info)
{
	bool use_internal_ldo_retulator;
	struct gpio_desc *reset_gpio;
	struct iio_dev *indio_dev;
	struct ad4134_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->dev = dev;

	indio_dev->name = bus_info->chip_info->name;

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

	use_internal_ldo_retulator = ret == 0;

	if (!use_internal_ldo_retulator) {
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

	ret = ad4134_clock_select(st);
	if (ret)
		return ret;

	crc8_populate_msb(ad4134_spi_crc_table, AD4134_SPI_CRC_POLYNOM);

	reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset_gpio))
		return dev_err_probe(dev, PTR_ERR(reset_gpio),
				     "failed to find reset GPIO\n");

	if (reset_gpio) {
		fsleep(AD4134_RESET_TIME_US);
		gpiod_set_value_cansleep(reset_gpio, 0);
	}

	ret = bus_info->bops->config_iio_dev(indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to config IIO device\n");

	st->regmap = bus_info->bops->init_regmap(st);
	if (IS_ERR(st->regmap))
		return dev_err_probe(st->dev, PTR_ERR(st->regmap),
				     "failed to initialize regmap");

	/* wiring/configuration specific setup */
	ret = bus_info->bops->setup(st);
	if (ret)
		return dev_err_probe(dev, ret, "failed to setup bus\n");

	/* Bump precision to 24-bit */
	st->current_scan_type = AD4134_DATA_PACKET_24BIT_FRAME;
	ret = regmap_update_bits(st->regmap, AD4134_DATA_PACKET_CONFIG_REG,
				 AD4134_DATA_PACKET_CONFIG_FRAME_MASK,
				 FIELD_PREP(AD4134_DATA_PACKET_CONFIG_FRAME_MASK,
					    st->current_scan_type));
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
EXPORT_SYMBOL_NS_GPL(ad4134_probe, "IIO_AD4134");

unsigned char ad4134_spi_crc_table[CRC8_TABLE_SIZE];
EXPORT_SYMBOL_NS_GPL(ad4134_spi_crc_table, "IIO_AD4134");

MODULE_AUTHOR("Marcelo Schmitt <marcelo.schmitt@analog.com>");
MODULE_DESCRIPTION("Analog Devices AD4134 driver");
MODULE_LICENSE("GPL");
