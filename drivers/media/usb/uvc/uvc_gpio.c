// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *      uvc_gpio.c  --  USB Video Class driver
 *
 *      Copyright 2024 Google LLC
 */

#include <linux/dmi.h>
#include <linux/kernel.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include "uvcvideo.h"

static irqreturn_t uvc_gpio_irq(int irq, void *data)
{
	struct uvc_device *dev = data;
	struct uvc_gpio *uvc_gpio = &dev->gpio_unit->gpio;
	int new_val;

	if (!uvc_gpio->gpio_ready)
		return IRQ_HANDLED;

	new_val = gpiod_get_value_cansleep(uvc_gpio->gpio_privacy);
	if (new_val < 0)
		return IRQ_HANDLED;

	input_report_switch(dev->input, SW_CAMERA_LENS_COVER, new_val);
	input_sync(dev->input);

	return IRQ_HANDLED;
}

static const struct dmi_system_id privacy_valid_during_streamon[] = {
	{
		.ident = "HP Elite c1030 Chromebook",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Jinlon"),
		},
	},
	{
		.ident = "HP Pro c640 Chromebook",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Dratini"),
		},
	},
	{ } /* terminate list */
};

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

	/*
	 * Note: This quirk will not match external UVC cameras,
	 * as they will not have the corresponding ACPI GPIO entity.
	 */
	if (dmi_check_system(privacy_valid_during_streamon))
		dev->quirks |= UVC_QUIRK_PRIVACY_DURING_STREAM;
	else
		unit->gpio.gpio_ready = true;

	unit->gpio.gpio_privacy = gpio_privacy;
	unit->gpio.irq = irq;
	strscpy(unit->name, "GPIO", sizeof(unit->name));
	list_add_tail(&unit->list, &dev->entities);

	dev->gpio_unit = unit;

	return 0;
}

void uvc_gpio_quirk(struct uvc_device *dev, bool stream_on)
{
	if (!dev->gpio_unit || !(dev->quirks & UVC_QUIRK_PRIVACY_DURING_STREAM))
		return;

	dev->gpio_unit->gpio.gpio_ready = stream_on;
	if (stream_on)
		uvc_gpio_irq(0, dev);
}

int uvc_gpio_init(struct uvc_device *dev)
{
	struct uvc_entity *unit = dev->gpio_unit;
	int init_val;
	int ret;

	if (!unit || unit->gpio.irq < 0)
		return 0;

	ret = request_threaded_irq(unit->gpio.irq, NULL, uvc_gpio_irq,
				   IRQF_ONESHOT | IRQF_TRIGGER_FALLING |
				   IRQF_TRIGGER_RISING,
				   "uvc_privacy_gpio", dev);
	if (ret)
		return ret;

	if ((dev->quirks & UVC_QUIRK_PRIVACY_DURING_STREAM)) {
		uvc_gpio_quirk(dev, false);
		init_val = false;
	} else {
		unit->gpio.gpio_ready = true;

		init_val = gpiod_get_value_cansleep(unit->gpio.gpio_privacy);
		if (init_val < 0) {
			free_irq(dev->gpio_unit->gpio.irq, dev);
			return init_val;
		}
	}

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
