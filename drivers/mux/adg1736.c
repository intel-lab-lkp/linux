// SPDX-License-Identifier: GPL-2.0
/*
 * Analog Devices ADG1736 Dual SPDT Switch Multiplexer driver
 *
 * Copyright 2025 Analog Devices Inc.
 *
 * Author: Antoniu Miclaus <antoniu.miclaus@analog.com>
 */

#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mux/driver.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#define ADG1736_MUX_CONTROLLERS	2
#define ADG1736_MUX_STATES	2

struct adg1736_mux {
	struct gpio_desc *ctrl_gpios[ADG1736_MUX_CONTROLLERS];
	struct gpio_desc *en_gpio;
};

static int adg1736_set(struct mux_control *mux, int state)
{
	struct adg1736_mux *adg1736 = mux_chip_priv(mux->chip);
	unsigned int controller = mux_control_get_index(mux);

	if (controller >= ADG1736_MUX_CONTROLLERS)
		return -EINVAL;

	if (state == MUX_IDLE_DISCONNECT) {
		/* When idle disconnect is requested, disable the EN pin */
		if (controller == 0)
			gpiod_set_value_cansleep(adg1736->en_gpio, 0);
		return 0;
	}

	/* Set the control GPIO for this mux controller */
	gpiod_set_value_cansleep(adg1736->ctrl_gpios[controller], state);

	/* Enable the mux if disabled */
	gpiod_set_value_cansleep(adg1736->en_gpio, 1);

	return 0;
}

static const struct mux_control_ops adg1736_ops = {
	.set = adg1736_set,
};

static int adg1736_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mux_chip *mux_chip;
	struct adg1736_mux *adg1736;
	s32 idle_state[ADG1736_MUX_CONTROLLERS];
	char gpio_name[16];
	int ret, i;

	mux_chip = devm_mux_chip_alloc(dev, ADG1736_MUX_CONTROLLERS,
				       sizeof(*adg1736));
	if (IS_ERR(mux_chip))
		return PTR_ERR(mux_chip);

	adg1736 = mux_chip_priv(mux_chip);
	mux_chip->ops = &adg1736_ops;

	/* Get control GPIOs (IN1, IN2) */
	for (i = 0; i < ADG1736_MUX_CONTROLLERS; i++) {
		snprintf(gpio_name, sizeof(gpio_name), "ctrl%d", i);
		adg1736->ctrl_gpios[i] = devm_gpiod_get_index(dev, "ctrl", i,
							      GPIOD_OUT_LOW);
		if (IS_ERR(adg1736->ctrl_gpios[i]))
			return dev_err_probe(dev, PTR_ERR(adg1736->ctrl_gpios[i]),
					     "failed to get ctrl%d gpio\n", i);
	}

	/* Get enable GPIO (EN) */
	adg1736->en_gpio = devm_gpiod_get(dev, "en", GPIOD_OUT_LOW);
	if (IS_ERR(adg1736->en_gpio))
		return dev_err_probe(dev, PTR_ERR(adg1736->en_gpio),
				     "failed to get en gpio\n");

	/* Read idle-state property */
	ret = device_property_read_u32_array(dev, "idle-state",
					     (u32 *)idle_state,
					     ADG1736_MUX_CONTROLLERS);
	if (ret < 0) {
		/* Default to AS_IS if not specified */
		idle_state[0] = MUX_IDLE_AS_IS;
		idle_state[1] = MUX_IDLE_AS_IS;
	}

	/* Configure each mux controller */
	for (i = 0; i < ADG1736_MUX_CONTROLLERS; i++) {
		struct mux_control *mux = &mux_chip->mux[i];

		mux->states = ADG1736_MUX_STATES;

		switch (idle_state[i]) {
		case MUX_IDLE_DISCONNECT:
		case MUX_IDLE_AS_IS:
		case 0 ... ADG1736_MUX_STATES - 1:
			mux->idle_state = idle_state[i];
			break;
		default:
			dev_err(dev, "invalid idle-state[%d] = %d\n",
				i, idle_state[i]);
			return -EINVAL;
		}
	}

	ret = devm_mux_chip_register(dev, mux_chip);
	if (ret < 0)
		return ret;

	dev_info(dev, "ADG1736 %u dual SPDT mux controllers registered\n",
		 mux_chip->controllers);

	return 0;
}

static const struct of_device_id adg1736_dt_ids[] = {
	{ .compatible = "adi,adg1736", },
	{ }
};
MODULE_DEVICE_TABLE(of, adg1736_dt_ids);

static struct platform_driver adg1736_driver = {
	.driver = {
		.name = "adg1736",
		.of_match_table = adg1736_dt_ids,
	},
	.probe = adg1736_probe,
};
module_platform_driver(adg1736_driver);

MODULE_DESCRIPTION("Analog Devices ADG1736 Dual SPDT Switch Multiplexer driver");
MODULE_AUTHOR("Antoniu Miclaus <antoniu.miclaus@analog.com>");
MODULE_LICENSE("GPL");
