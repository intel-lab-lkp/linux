// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2019 Texas Instruments Incorporated -  http://www.ti.com/
 * Author: Tomi Valkeinen <tomi.valkeinen@ti.com>
 *
 * Based on pwm_bl.c
 */

#include <linux/backlight.h>
#include <linux/led_bl.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

struct led_bl_data {
	struct device		*dev;
	struct backlight_device	*bl_dev;
	struct led_classdev	**leds;
	bool			enabled;
	int			nb_leds;
	unsigned int		*levels;
	unsigned int		default_brightness;
	unsigned int		max_brightness;
};

static void led_bl_set_brightness(struct led_bl_data *priv, int level)
{
	int i;
	int bkl_brightness;

	if (priv->levels)
		bkl_brightness = priv->levels[level];
	else
		bkl_brightness = level;

	for (i = 0; i < priv->nb_leds; i++)
		led_set_brightness(priv->leds[i], bkl_brightness);

	priv->enabled = true;
}

static void led_bl_power_off(struct led_bl_data *priv)
{
	int i;

	if (!priv->enabled)
		return;

	for (i = 0; i < priv->nb_leds; i++)
		led_set_brightness(priv->leds[i], LED_OFF);

	priv->enabled = false;
}

static int led_bl_update_status(struct backlight_device *bl)
{
	struct led_bl_data *priv = bl_get_data(bl);
	int brightness = backlight_get_brightness(bl);

	if (brightness > 0)
		led_bl_set_brightness(priv, brightness);
	else
		led_bl_power_off(priv);

	return 0;
}

static const struct backlight_ops led_bl_ops = {
	.update_status	= led_bl_update_status,
};

static int led_bl_get_leds(struct device *dev,
			   struct led_bl_data *priv)
{
	int i, nb_leds, ret;
	struct device_node *node = dev->of_node;
	struct led_classdev **leds;
	unsigned int max_brightness;
	unsigned int default_brightness;

	ret = of_count_phandle_with_args(node, "leds", NULL);
	if (ret < 0) {
		dev_err(dev, "Unable to get led count\n");
		return -EINVAL;
	}

	nb_leds = ret;
	if (nb_leds < 1) {
		dev_err(dev, "At least one LED must be specified!\n");
		return -EINVAL;
	}

	leds = devm_kcalloc(dev, nb_leds, sizeof(struct led_classdev *),
			    GFP_KERNEL);
	if (!leds)
		return -ENOMEM;

	for (i = 0; i < nb_leds; i++) {
		leds[i] = devm_of_led_get(dev, i);
		if (IS_ERR(leds[i]))
			return PTR_ERR(leds[i]);
	}

	/* check that the LEDs all have the same brightness range */
	max_brightness = leds[0]->max_brightness;
	for (i = 1; i < nb_leds; i++) {
		if (max_brightness != leds[i]->max_brightness) {
			dev_err(dev, "LEDs must have identical ranges\n");
			return -EINVAL;
		}
	}

	/* get the default brightness from the first LED from the list */
	default_brightness = leds[0]->brightness;

	priv->nb_leds = nb_leds;
	priv->leds = leds;
	priv->max_brightness = max_brightness;
	priv->default_brightness = default_brightness;

	return 0;
}

static int led_bl_parse_levels(struct device *dev,
			   struct led_bl_data *priv)
{
	struct device_node *node = dev->of_node;
	int num_levels;
	u32 value;
	int ret;

	if (!node)
		return -ENODEV;

	num_levels = of_property_count_u32_elems(node, "brightness-levels");
	if (num_levels > 1) {
		int i;
		unsigned int db;
		u32 *levels = NULL;

		levels = devm_kcalloc(dev, num_levels, sizeof(u32),
				      GFP_KERNEL);
		if (!levels)
			return -ENOMEM;

		ret = of_property_read_u32_array(node, "brightness-levels",
						levels,
						num_levels);
		if (ret < 0)
			return ret;

		/*
		 * Try to map actual LED brightness to backlight brightness
		 * level
		 */
		db = priv->default_brightness;
		for (i = 0 ; i < num_levels; i++) {
			if ((i && db > levels[i-1]) && db <= levels[i])
				break;
		}
		priv->default_brightness = i;
		priv->max_brightness = num_levels - 1;
		priv->levels = levels;
	} else if (num_levels >= 0)
		dev_warn(dev, "Not enough levels defined\n");

	ret = of_property_read_u32(node, "default-brightness-level", &value);
	if (!ret && value <= priv->max_brightness)
		priv->default_brightness = value;
	else if (!ret  && value > priv->max_brightness)
		dev_warn(dev, "Invalid default brightness. Ignoring it\n");

	return 0;
}

static void led_bl_disable(void *data)
{
	struct led_bl_data *priv = data;
	int i;

	led_bl_power_off(priv);
	for (i = 0; i < priv->nb_leds; i++) {
		mutex_lock(&priv->leds[i]->led_access);
		led_sysfs_enable(priv->leds[i]);
		mutex_unlock(&priv->leds[i]->led_access);
	}
}

static int led_bl_register(struct device *dev, struct led_bl_data *priv)
{
	struct backlight_properties props;
	int ret, i;

	priv->dev = dev;

	memset(&props, 0, sizeof(struct backlight_properties));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = priv->max_brightness;
	props.brightness = priv->default_brightness;
	props.power = (priv->default_brightness > 0) ? BACKLIGHT_POWER_OFF :
		      BACKLIGHT_POWER_ON;
	priv->bl_dev = devm_backlight_device_register(dev, dev_name(dev), dev,
						      priv, &led_bl_ops, &props);
	if (IS_ERR(priv->bl_dev))
		return dev_err_probe(dev, PTR_ERR(priv->bl_dev),
				     "Failed to register backlight\n");

	for (i = 0; i < priv->nb_leds; i++) {
		struct device *supplier = priv->leds[i]->dev->parent;
		struct device_link *link;

		/*
		 * BL and the LED are the same device if instantiated via
		 * devm_led_backlight_register()
		 */
		if (supplier == dev)
			continue;

		link = device_link_add(dev, supplier, DL_FLAG_AUTOREMOVE_CONSUMER);
		if (!link)
			return dev_err_probe(dev, -EINVAL,
					     "Failed to add devlink (consumer %s, supplier %s)\n",
					     dev_name(dev), dev_name(supplier));
	}

	for (i = 0; i < priv->nb_leds; i++) {
		mutex_lock(&priv->leds[i]->led_access);
		led_sysfs_disable(priv->leds[i]);
		mutex_unlock(&priv->leds[i]->led_access);
	}

	ret = devm_add_action_or_reset(dev, led_bl_disable, priv);
	if (ret)
		return ret;

	backlight_update_status(priv->bl_dev);

	return 0;
}

static int led_bl_probe(struct platform_device *pdev)
{
	struct led_bl_data *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ret = led_bl_get_leds(&pdev->dev, priv);
	if (ret)
		return ret;

	ret = led_bl_parse_levels(&pdev->dev, priv);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to parse DT data\n");
		return ret;
	}

	return led_bl_register(&pdev->dev, priv);
}

/**
 * devm_led_backlight_register - expose a LED as a backlight device
 * @dev: LED provider device, also the parent and lifecycle owner
 * @led: LED class device to drive the backlight
 *
 * Registers a backlight class device driven by @led, without device tree and
 * tied to the lifetime of @dev. This lets self-contained (e.g. hot-pluggable
 * I2C) LED drivers offer a backlight interface without static platform
 * plumbing. It is a no-op when the led-backlight support is not built in.
 *
 * Return: 0 on success, negative errno otherwise.
 */
int devm_led_backlight_register(struct device *dev, struct led_classdev *led)
{
	struct led_bl_data *priv;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->leds = devm_kmalloc(dev, sizeof(*priv->leds), GFP_KERNEL);
	if (!priv->leds)
		return -ENOMEM;
	priv->leds[0] = led;
	priv->nb_leds = 1;
	priv->max_brightness = led->max_brightness;
	priv->default_brightness = led->brightness;

	return led_bl_register(dev, priv);
}
EXPORT_SYMBOL_GPL(devm_led_backlight_register);

static const struct of_device_id led_bl_of_match[] = {
	{ .compatible = "led-backlight" },
	{ }
};

MODULE_DEVICE_TABLE(of, led_bl_of_match);

static struct platform_driver led_bl_driver = {
	.driver		= {
		.name		= "led-backlight",
		.of_match_table	= led_bl_of_match,
	},
	.probe		= led_bl_probe,
};

module_platform_driver(led_bl_driver);

MODULE_DESCRIPTION("LED based Backlight Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:led-backlight");
