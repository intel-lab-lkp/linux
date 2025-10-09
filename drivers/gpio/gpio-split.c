// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO splitter acting as virtual gpiochip which "splits" a physical GPIO into
 * multiple using a multiplexer (e.g. SFP signals RX_LOS, TX_FAULT, MOD_DEF0
 * muxed on a single GPIO).
 *
 * Copyright (c) 2025 Jonas Jelonek <jelonek.jonas@gmail.com>
 */

#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/mux/consumer.h>
#include <linux/mux/driver.h>
#include <linux/platform_device.h>


struct gpio_split_gpio {
	unsigned int mux_state;
};

struct gpio_split {
	struct gpio_chip gc;
	struct mux_control *mux;
	struct device *dev;

	struct mutex lock;

	struct gpio_desc *shared_gpio;
	/* dynamically sized, must be last */
	struct gpio_split_gpio gpios[];
};

DEFINE_GUARD(gpio_split, struct gpio_split *, mutex_lock(&_T->lock), mutex_unlock(&_T->lock))

static int gpio_split_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct gpio_split *gs = (struct gpio_split *)gpiochip_get_data(gc);
	struct gpio_split_gpio *gs_gpio;
	int ret;

	if (offset > gc->ngpio)
		return -EINVAL;

	guard(gpio_split)(gs);

	gs_gpio = &gs->gpios[offset];
	ret = mux_control_select(gs->mux, gs_gpio->mux_state);
	if (ret < 0)
		return ret;

	ret = gpiod_get_raw_value_cansleep(gs->shared_gpio);
	mux_control_deselect(gs->mux);
	return ret;
}

static void gpio_split_gpio_set(struct gpio_chip *gc, unsigned int offset,
				int value)
{
	struct gpio_split *gs = (struct gpio_split *)gpiochip_get_data(gc);
	struct gpio_split_gpio *gs_gpio;
	int ret;

	if (offset > gc->ngpio)
		return;

	guard(gpio_split)(gs);

	gs_gpio = &gs->gpios[offset];
	ret = mux_control_select(gs->mux, gs_gpio->mux_state);
	if (ret < 0)
		return;

	gpiod_set_raw_value_cansleep(gs->shared_gpio, value);
	mux_control_deselect(gs->mux);
}

static int gpio_split_gpio_get_direction(struct gpio_chip *gc,
					 unsigned int offset)
{
	struct gpio_split *gs = (struct gpio_split *)gpiochip_get_data(gc);

	if (offset > gc->ngpio)
		return -EINVAL;

	guard(gpio_split)(gs);

	return gpiod_get_direction(gs->shared_gpio);
}

static int gpio_split_gpio_direction_input(struct gpio_chip *gc,
					   unsigned int offset)
{
	struct gpio_split *gs = (struct gpio_split *)gpiochip_get_data(gc);

	if (offset > gc->ngpio)
		return -EINVAL;

	guard(gpio_split)(gs);

	return gpiod_direction_input(gs->shared_gpio);
}

static int gpio_split_gpio_direction_output(struct gpio_chip *gc,
					    unsigned int offset, int value)
{
	struct gpio_split *gs = (struct gpio_split *)gpiochip_get_data(gc);

	if (offset > gc->ngpio)
		return -EINVAL;

	guard(gpio_split)(gs);

	return gpiod_direction_output_raw(gs->shared_gpio, value);
}

static const struct of_device_id gpio_split_of_match[] = {
	{ .compatible = "gpio-split" },
	{ }
};
MODULE_DEVICE_TABLE(of, gpio_split_of_match);

static int gpio_split_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpio_split *gs;
	struct fwnode_handle *child;
	unsigned int ngpio, size, i;
	int ret;

	ngpio = device_get_child_node_count(dev);
	size = sizeof(*gs) + (sizeof(struct gpio_split_gpio) * ngpio);

	gs = devm_kzalloc(dev, size, GFP_KERNEL);
	if (!gs)
		return -ENOMEM;

	mutex_init(&gs->lock);

	gs->dev = dev;
	gs->gc.base = -1;
	gs->gc.can_sleep = true;
	gs->gc.fwnode = dev_fwnode(dev);
	gs->gc.label = "gpio-split";
	gs->gc.ngpio = ngpio;
	gs->gc.owner = THIS_MODULE;
	gs->gc.parent = dev;

	gs->gc.get = gpio_split_gpio_get;
	gs->gc.set = gpio_split_gpio_set;
	gs->gc.get_direction = gpio_split_gpio_get_direction;
	gs->gc.direction_input = gpio_split_gpio_direction_input;
	gs->gc.direction_output = gpio_split_gpio_direction_output;

	gs->mux = devm_mux_control_get(dev, NULL);
	if (IS_ERR(gs->mux)) {
		if (PTR_ERR(gs->mux) == -EPROBE_DEFER) {
			dev_err(dev, "mux-controller not ready, deferring probe\n");
			return -EPROBE_DEFER;
		}

		dev_err(dev, "could not get mux-controller\n");
		return PTR_ERR(gs->mux);
	}

	gs->shared_gpio = devm_gpiod_get(dev, "shared", GPIOD_ASIS);
	if (IS_ERR(gs->shared_gpio)) {
		dev_err(dev, "could not get shared-gpio\n");
		return PTR_ERR(gs->shared_gpio);
	}

	i = 0;
	device_for_each_child_node(dev, child) {
		struct gpio_split_gpio *gpio = &gs->gpios[i];
		u32 mux_state;

		ret = fwnode_property_read_u32(child, "mux-state", &mux_state);
		if (ret) {
			dev_err(dev, "gpio %u: failed to read mux-state\n", i);
			return ret;
		}

		gpio->mux_state = (unsigned int)mux_state;
		i++;
	}

	ret = devm_gpiochip_add_data(dev, &gs->gc, gs);
	if (ret) {
		dev_err(dev, "failed to add gpiochip: %d\n", ret);
		return ret;
	}

	dev_info(dev, "providing %u virtual GPIOs for real GPIO %u\n", i,
		 desc_to_gpio(gs->shared_gpio));
	return 0;
}

static struct platform_driver gpio_split_driver = {
	.driver = {
		.name = "gpio-split",
		.of_match_table = gpio_split_of_match,
	},
	.probe = gpio_split_probe,
};
module_platform_driver(gpio_split_driver);

MODULE_AUTHOR("Jonas Jelonek <jelonek.jonas@gmail.com>");
MODULE_DESCRIPTION("GPIO split driver");
MODULE_LICENSE("GPL");
