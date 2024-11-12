// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *      uvc_gpio.c  --  USB Video Class driver
 *
 *      Copyright 2024 Google LLC
 */

#include <linux/kernel.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include "uvcvideo.h"

static irqreturn_t uvc_gpio_irq(int irq, void *data)
{
	struct uvc_device *dev = data;
	struct uvc_gpio *uvc_gpio = &dev->gpio_unit->gpio;
	int new_val;

	new_val = gpiod_get_value_cansleep(uvc_gpio->gpio_privacy);
	if (new_val < 0)
		return IRQ_HANDLED;

	input_report_switch(dev->input, SW_CAMERA_LENS_COVER, new_val);
	input_sync(dev->input);

	return IRQ_HANDLED;
}

int uvc_gpio_parse(struct uvc_device *dev)
{
	struct gpio_desc *gpio_privacy;
	struct uvc_entity *unit;
	int irq;

	gpio_privacy = devm_gpiod_get_optional(&dev->intf->dev, "privacy",
					       GPIOD_IN);
	if (IS_ERR_OR_NULL(gpio_privacy))
		return PTR_ERR_OR_ZERO(gpio_privacy);

	irq = gpiod_to_irq(gpio_privacy);
	if (irq < 0)
		return dev_err_probe(&dev->intf->dev, irq,
				     "No IRQ for privacy GPIO\n");

	unit = uvc_alloc_new_entity(dev, UVC_EXT_GPIO_UNIT,
				    UVC_EXT_GPIO_UNIT_ID, 0, 0);
	if (IS_ERR(unit))
		return PTR_ERR(unit);

	unit->gpio.gpio_privacy = gpio_privacy;
	unit->gpio.irq = irq;
	strscpy(unit->name, "GPIO", sizeof(unit->name));
	list_add_tail(&unit->list, &dev->entities);

	dev->gpio_unit = unit;

	return 0;
}

int uvc_gpio_init(struct uvc_device *dev)
{
	struct uvc_entity *unit = dev->gpio_unit;
	int init_val;
	int ret;

	if (!unit || unit->gpio.irq < 0)
		return 0;

	init_val = gpiod_get_value_cansleep(unit->gpio.gpio_privacy);
	if (init_val < 0)
		return init_val;

	ret = request_threaded_irq(unit->gpio.irq, NULL, uvc_gpio_irq,
				   IRQF_ONESHOT | IRQF_TRIGGER_FALLING |
				   IRQF_TRIGGER_RISING,
				   "uvc_privacy_gpio", dev);
	if (ret)
		return ret;

	input_report_switch(dev->input, SW_CAMERA_LENS_COVER, init_val);
	input_sync(dev->input);

	unit->gpio.initialized = true;

	return 0;
}

void uvc_gpio_deinit(struct uvc_device *dev)
{
	if (!dev->gpio_unit || !dev->gpio_unit->gpio.initialized)
		return;

	free_irq(dev->gpio_unit->gpio.irq, dev);
}
