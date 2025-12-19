// SPDX-License-Identifier: GPL-2.0
/*
 * Analog Devices ADG2404 4:1 multiplexer driver
 *
 * Copyright 2025 Analog Devices Inc.
 *
 * Author: Antoniu Miclaus <antoniu.miclaus@analog.com>
 */

#include <linux/bitmap.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mux/driver.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#define ADG2404_CHANNELS	4

struct adg2404_mux {
	struct gpio_descs *addr_gpios;
	struct gpio_desc *en_gpio;
};

static int adg2404_set(struct mux_control *mux, int state)
{
	struct adg2404_mux *adg2404 = mux_chip_priv(mux->chip);
	DECLARE_BITMAP(values, BITS_PER_TYPE(state));
	u32 value = state;

	if (state == MUX_IDLE_DISCONNECT) {
		gpiod_set_value_cansleep(adg2404->en_gpio, 0);
		return 0;
	}

	/*
	 * Disable the mux before changing address lines to prevent
	 * glitches. Changing address while enabled could briefly activate
	 * an unintended channel during the transition.
	 */
	gpiod_set_value_cansleep(adg2404->en_gpio, 0);

	bitmap_from_arr32(values, &value, BITS_PER_TYPE(value));
	gpiod_set_array_value_cansleep(adg2404->addr_gpios->ndescs,
				       adg2404->addr_gpios->desc,
				       adg2404->addr_gpios->info,
				       values);

	/* Enable the mux with the new address */
	gpiod_set_value_cansleep(adg2404->en_gpio, 1);

	return 0;
}

static const struct mux_control_ops adg2404_ops = {
	.set = adg2404_set,
};

static int adg2404_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mux_chip *mux_chip;
	struct adg2404_mux *adg2404;
	s32 idle_state;
	int ret;

	mux_chip = devm_mux_chip_alloc(dev, 1, sizeof(*adg2404));
	if (IS_ERR(mux_chip))
		return PTR_ERR(mux_chip);

	adg2404 = mux_chip_priv(mux_chip);
	mux_chip->ops = &adg2404_ops;

	adg2404->addr_gpios = devm_gpiod_get_array(dev, "addr", GPIOD_OUT_LOW);
	if (IS_ERR(adg2404->addr_gpios))
		return dev_err_probe(dev, PTR_ERR(adg2404->addr_gpios),
				     "failed to get addr gpios\n");

	if (adg2404->addr_gpios->ndescs != 2)
		return dev_err_probe(dev, -EINVAL,
				     "addr-gpios must have exactly 2 GPIOs\n");

	adg2404->en_gpio = devm_gpiod_get(dev, "en", GPIOD_OUT_LOW);
	if (IS_ERR(adg2404->en_gpio))
		return dev_err_probe(dev, PTR_ERR(adg2404->en_gpio),
				     "failed to get en gpio\n");

	mux_chip->mux->states = ADG2404_CHANNELS;

	ret = device_property_read_u32(dev, "idle-state", (u32 *)&idle_state);
	if (ret < 0)
		idle_state = MUX_IDLE_AS_IS;

	switch (idle_state) {
	case MUX_IDLE_DISCONNECT:
	case MUX_IDLE_AS_IS:
	case 0 ... ADG2404_CHANNELS - 1:
		mux_chip->mux->idle_state = idle_state;
		break;
	default:
		dev_err(dev, "invalid idle-state %d\n", idle_state);
		return -EINVAL;
	}

	ret = devm_mux_chip_register(dev, mux_chip);
	if (ret < 0)
		return ret;

	dev_info(dev, "ADG2404 %u-way mux-controller registered\n",
		 mux_chip->mux->states);

	return 0;
}

static const struct of_device_id adg2404_dt_ids[] = {
	{ .compatible = "adi,adg2404", },
	{ }
};
MODULE_DEVICE_TABLE(of, adg2404_dt_ids);

static struct platform_driver adg2404_driver = {
	.driver = {
		.name = "adg2404",
		.of_match_table	= adg2404_dt_ids,
	},
	.probe = adg2404_probe,
};
module_platform_driver(adg2404_driver);

MODULE_DESCRIPTION("Analog Devices ADG2404 multiplexer driver");
MODULE_AUTHOR("Antoniu Miclaus <antoniu.miclaus@analog.com>");
MODULE_LICENSE("GPL");
