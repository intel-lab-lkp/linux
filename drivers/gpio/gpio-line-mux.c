// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO line mux which acts as virtual gpiochip and provides a 1-to-many
 * mapping between virtual GPIOs and a real GPIO + multiplexer. 
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

struct gpio_lmux {
	struct gpio_chip gc;
	struct mux_control *mux;
	struct device *dev;

	struct mutex lock;

	struct gpio_desc *shared_gpio;
	/* dynamically sized, must be last */
	unsigned int gpio_mux_states[];
};

DEFINE_GUARD(gpio_lmux, struct gpio_lmux *, mutex_lock(&_T->lock), mutex_unlock(&_T->lock))

static int gpio_lmux_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct gpio_lmux *glm = (struct gpio_lmux *)gpiochip_get_data(gc);
	int ret;

	if (offset > gc->ngpio)
		return -EINVAL;

	guard(gpio_lmux)(glm);

	ret = mux_control_select(glm->mux, glm->gpio_mux_states[offset]);
	if (ret < 0)
		return ret;

	ret = gpiod_get_raw_value_cansleep(glm->shared_gpio);
	mux_control_deselect(glm->mux);
	return ret;
}

static int gpio_lmux_gpio_set(struct gpio_chip *gc, unsigned int offset,
			      int value)
{
	struct gpio_lmux *glm = (struct gpio_lmux *)gpiochip_get_data(gc);
	int ret;

	if (offset > gc->ngpio)
		return -EINVAL;

	guard(gpio_lmux)(glm);

	ret = mux_control_select(glm->mux, glm->gpio_mux_states[offset]);
	if (ret < 0)
		return ret;

	gpiod_set_raw_value_cansleep(glm->shared_gpio, value);
	mux_control_deselect(glm->mux);
	return 0;
}

static int gpio_lmux_gpio_get_direction(struct gpio_chip *gc,
					unsigned int offset)
{
	struct gpio_lmux *glm = (struct gpio_lmux *)gpiochip_get_data(gc);

	if (offset > gc->ngpio)
		return -EINVAL;

	guard(gpio_lmux)(glm);

	return gpiod_get_direction(glm->shared_gpio);
}

static int gpio_lmux_gpio_direction_input(struct gpio_chip *gc,
					  unsigned int offset)
{
	struct gpio_lmux *glm = (struct gpio_lmux *)gpiochip_get_data(gc);

	if (offset > gc->ngpio)
		return -EINVAL;

	guard(gpio_lmux)(glm);

	return gpiod_direction_input(glm->shared_gpio);
}

static int gpio_lmux_gpio_direction_output(struct gpio_chip *gc,
					   unsigned int offset, int value)
{
	struct gpio_lmux *glm = (struct gpio_lmux *)gpiochip_get_data(gc);

	if (offset > gc->ngpio)
		return -EINVAL;

	guard(gpio_lmux)(glm);

	return gpiod_direction_output_raw(glm->shared_gpio, value);
}

static int gpio_lmux_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpio_lmux *glm;
	unsigned int ngpio, size;
	int ret;

	ngpio = device_property_count_u32(dev, "gpio-line-mux-states");
	if (!ngpio)
		return -EINVAL;

	size = sizeof(*glm) + (sizeof(unsigned int) * ngpio);
	glm = devm_kzalloc(dev, size, GFP_KERNEL);
	if (!glm)
		return -ENOMEM;

	mutex_init(&glm->lock);

	glm->dev = dev;
	glm->gc.base = -1;
	glm->gc.can_sleep = true;
	glm->gc.fwnode = dev_fwnode(dev);
	glm->gc.label = "gpio-line-mux";
	glm->gc.ngpio = ngpio;
	glm->gc.owner = THIS_MODULE;
	glm->gc.parent = dev;

	glm->gc.get = gpio_lmux_gpio_get;
	glm->gc.set = gpio_lmux_gpio_set;
	glm->gc.get_direction = gpio_lmux_gpio_get_direction;
	glm->gc.direction_input = gpio_lmux_gpio_direction_input;
	glm->gc.direction_output = gpio_lmux_gpio_direction_output;

	glm->mux = devm_mux_control_get(dev, NULL);
	if (IS_ERR(glm->mux)) {
		if (PTR_ERR(glm->mux) == -EPROBE_DEFER) {
			dev_err(dev, "mux-controller not ready, deferring probe\n");
			return -EPROBE_DEFER;
		}

		dev_err(dev, "could not get mux-controller\n");
		return PTR_ERR(glm->mux);
	}

	glm->shared_gpio = devm_gpiod_get(dev, "shared", GPIOD_ASIS);
	if (IS_ERR(glm->shared_gpio)) {
		dev_err(dev, "could not get shared-gpio\n");
		return PTR_ERR(glm->shared_gpio);
	}

	ret = device_property_read_u32_array(dev, "gpio-line-mux-states",
					     &glm->gpio_mux_states[0], ngpio);
	if (ret) {
		dev_err(dev, "could not get mux states\n");
		return ret;
	}
		
	ret = devm_gpiochip_add_data(dev, &glm->gc, glm);
	if (ret) {
		dev_err(dev, "failed to add gpiochip: %d\n", ret);
		return ret;
	}

	dev_info(dev, "providing %u virtual GPIOs for real GPIO %u\n", ngpio,
		 desc_to_gpio(glm->shared_gpio));
	return 0;
}

static const struct of_device_id gpio_lmux_of_match[] = {
	{ .compatible = "gpio-line-mux" },
	{ }
};
MODULE_DEVICE_TABLE(of, gpio_lmux_of_match);

static struct platform_driver gpio_lmux_driver = {
	.driver = {
		.name = "gpio-line-mux",
		.of_match_table = gpio_lmux_of_match,
	},
	.probe = gpio_lmux_probe,
};
module_platform_driver(gpio_lmux_driver);

MODULE_AUTHOR("Jonas Jelonek <jelonek.jonas@gmail.com>");
MODULE_DESCRIPTION("GPIO line mux driver");
MODULE_LICENSE("GPL");
