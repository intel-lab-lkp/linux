// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *      uvc_gpio.c  --  USB Video Class driver
 *
 *      Copyright 2025 Google LLC
 */

#include <linux/kernel.h>
#include <linux/gpio/consumer.h>
#include "uvcvideo.h"

static void uvc_gpio_event(struct uvc_device *dev)
{
	struct uvc_entity *unit = dev->gpio_unit;
	struct uvc_video_chain *chain;
	u8 new_val;

	if (!unit)
		return;

	new_val = gpiod_get_value_cansleep(unit->gpio.gpio_privacy);

	/* GPIO entities are always on the first chain. */
	chain = list_first_entry(&dev->chains, struct uvc_video_chain, list);
	uvc_ctrl_status_event(chain, unit->controls, &new_val);
}

static int uvc_gpio_get_cur(struct uvc_device *dev, struct uvc_entity *entity,
			    u8 cs, void *data, u16 size)
{
	if (cs != UVC_CT_PRIVACY_CONTROL || size < 1)
		return -EINVAL;

	*(u8 *)data = gpiod_get_value_cansleep(entity->gpio.gpio_privacy);

	return 0;
}

static int uvc_gpio_get_info(struct uvc_device *dev, struct uvc_entity *entity,
			     u8 cs, u8 *caps)
{
	if (cs != UVC_CT_PRIVACY_CONTROL)
		return -EINVAL;

	*caps = UVC_CONTROL_CAP_GET | UVC_CONTROL_CAP_AUTOUPDATE;
	return 0;
}

static irqreturn_t uvc_gpio_irq(int irq, void *data)
{
	struct uvc_device *dev = data;

	uvc_gpio_event(dev);
	return IRQ_HANDLED;
}

int uvc_gpio_parse(struct uvc_device *dev)
{
	struct uvc_entity *unit;
	struct gpio_desc *gpio_privacy;
	int irq;

	gpio_privacy = devm_gpiod_get_optional(&dev->intf->dev, "privacy",
					       GPIOD_IN);
	if (!gpio_privacy)
		return 0;

	if (IS_ERR(gpio_privacy))
		return dev_err_probe(&dev->intf->dev,
				     PTR_ERR(gpio_privacy),
				     "Can't get privacy GPIO\n");

	irq = gpiod_to_irq(gpio_privacy);
	if (irq < 0)
		return dev_err_probe(&dev->intf->dev, irq,
				     "No IRQ for privacy GPIO\n");

	unit = uvc_alloc_entity(UVC_EXT_GPIO_UNIT, UVC_EXT_GPIO_UNIT_ID, 0, 1);
	if (!unit)
		return -ENOMEM;

	unit->gpio.gpio_privacy = gpio_privacy;
	unit->gpio.irq = irq;
	unit->gpio.bControlSize = 1;
	unit->gpio.bmControls = (u8 *)unit + sizeof(*unit);
	unit->gpio.bmControls[0] = 1;
	unit->get_cur = uvc_gpio_get_cur;
	unit->get_info = uvc_gpio_get_info;
	strscpy(unit->name, "GPIO", sizeof(unit->name));

	list_add_tail(&unit->list, &dev->entities);

	dev->gpio_unit = unit;

	return 0;
}

int uvc_gpio_init_irq(struct uvc_device *dev)
{
	struct uvc_entity *unit = dev->gpio_unit;
	int ret;

	if (!unit || unit->gpio.irq < 0)
		return 0;

	ret = request_threaded_irq(unit->gpio.irq, NULL, uvc_gpio_irq,
				   IRQF_ONESHOT | IRQF_TRIGGER_FALLING |
				   IRQF_TRIGGER_RISING,
				   "uvc_privacy_gpio", dev);

	unit->gpio.initialized = !ret;

	return ret;
}

void uvc_gpio_deinit(struct uvc_device *dev)
{
	if (!dev->gpio_unit || !dev->gpio_unit->gpio.initialized)
		return;

	free_irq(dev->gpio_unit->gpio.irq, dev);
}

