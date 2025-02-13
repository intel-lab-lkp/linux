// SPDX-License-Identifier: GPL-2.0
/*
 * ADG1414 Serially-Controlled Octal SPST Switches
 *
 * Copyright 2025 Analog Devices Inc.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>

#define ADG1414_MAX_DEVICES		4

struct adg1414_state {
	struct spi_device *spi;
	struct gpio_chip chip;
	struct regmap *regmap;
	struct mutex lock; /* protect sensor state */
	u32 buf;

	__be32 tx __aligned(ARCH_DMA_MINALIGN);
};

static int adg1414_spi_write(void *context, const void *data, size_t count)
{
	struct adg1414_state *st = context;

	struct spi_transfer xfer = {
		.tx_buf = &st->tx,
		.len = count,
	};

	return spi_sync_transfer(st->spi, &xfer, 1);
}

static int adg1414_spi_read(void *context, const void *reg, size_t reg_size,
			    void *val, size_t val_size)
{
	return 0;
}

static int adg1414_get(struct gpio_chip *chip, unsigned int offset)
{
	struct adg1414_state *st = gpiochip_get_data(chip);

	guard(mutex)(&st->lock);

	return st->buf & BIT(offset);
}

static void adg1414_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	struct adg1414_state *st = gpiochip_get_data(chip);

	guard(mutex)(&st->lock);

	if (value)
		st->buf |= BIT(offset);
	else
		st->buf &= ~BIT(offset);

	st->tx = cpu_to_be32(st->buf << (32 - st->chip.ngpio));

	adg1414_spi_write(st, 0, st->chip.ngpio / 8);
}

static const struct regmap_bus adg1414_regmap_bus = {
	.write = adg1414_spi_write,
	.read =	adg1414_spi_read,
	.reg_format_endian_default = REGMAP_ENDIAN_BIG,
	.val_format_endian_default = REGMAP_ENDIAN_BIG,
};

static const struct regmap_config adg1414_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int adg1414_get_direction(struct gpio_chip *chip,
				 unsigned int offset)
{
	return GPIO_LINE_DIRECTION_OUT;
}

static int adg1414_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct adg1414_state *st;
	struct gpio_desc *reset;
	u32 num_devices;
	int ret;

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	st->spi = spi;

	st->regmap = devm_regmap_init(dev, &adg1414_regmap_bus, st,
				      &adg1414_regmap_config);
	if (IS_ERR(st->regmap))
		return dev_err_probe(dev, PTR_ERR(st->regmap),
				     "Failed to initialize regmap");

	/* Use reset pin to reset the device */
	reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset),
				     "Failed to get reset gpio");

	if (reset) {
		fsleep(1);
		gpiod_set_value_cansleep(reset, 0);
	}

	num_devices = 1;
	ret = device_property_read_u32(dev, "#daisy-chained-devices",
				       &num_devices);
	if (!ret) {
		if (!num_devices || num_devices > ADG1414_MAX_DEVICES)
			return dev_err_probe(dev, ret,
			       "Failed to get daisy-chained-devices property\n");
	}

	st->chip.label = "adg1414";
	st->chip.parent = dev;
	st->chip.get_direction = adg1414_get_direction;
	st->chip.set = adg1414_set;
	st->chip.get = adg1414_get;
	st->chip.base = -1;
	st->chip.ngpio =  num_devices * 8;
	st->chip.can_sleep = true;

	ret = devm_mutex_init(dev, &st->lock);
	if (ret)
		return ret;

	return devm_gpiochip_add_data(dev, &st->chip, st);
}

static const struct of_device_id adg1414_of_match[] = {
	{ .compatible = "adi,adg1414-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, adg1414_of_match);

static struct spi_driver adg1414_driver = {
	.driver = {
		.name = "adg1414-gpio",
		.of_match_table = adg1414_of_match,
	},
	.probe = adg1414_probe,
};
module_spi_driver(adg1414_driver);

MODULE_AUTHOR("Kim Seer Paller <kimseer.paller@analog.com>");
MODULE_DESCRIPTION("ADG1414 Serially-Controlled Octal SPST Switches");
MODULE_LICENSE("GPL");
