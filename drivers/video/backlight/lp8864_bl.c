// SPDX-License-Identifier: GPL-2.0-only
/*
 * TI LP8864/LP8866 4/6 Channel LED Backlight Driver
 *
 * Copyright (C) 2024-2026 Siemens AG
 *
 * Based on LP8860 driver by Dan Murphy <dmurphy@ti.com>
 */

#include <linux/backlight.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#define LP8864_BRT_CONTROL		0x00
#define LP8864_USER_CONFIG1		0x04
#define   LP8864_BRT_MODE_MASK		GENMASK(9, 8)
#define   LP8864_BRT_MODE_REG		BIT(9)		/* Brightness control by DISPLAY_BRT reg */
#define LP8864_SUPPLY_STATUS		0x0e
#define LP8864_BOOST_STATUS		0x10
#define LP8864_LED_STATUS		0x12
#define   LP8864_LED_STATUS_WR_MASK	GENMASK(14, 9)	/* Writeable bits in the LED_STATUS reg */

#define LP8864_MAX_BRIGHTNESS		0xffff

/* Textual meaning for status bits, starting from bit 1 */
static const char *const lp8864_supply_status_msg[] = {
	"Vin under-voltage fault",
	"Vin over-voltage fault",
	"Vdd under-voltage fault",
	"Vin over-current fault",
	"Missing charge pump fault",
	"Charge pump fault",
	"Missing boost sync fault",
	"CRC error fault ",
};

/* Textual meaning for status bits, starting from bit 1 */
static const char *const lp8864_boost_status_msg[] = {
	"Boost OVP low fault",
	"Boost OVP high fault",
	"Boost over-current fault",
	"Missing boost FSET resistor fault",
	"Missing MODE SEL resistor fault",
	"Missing LED resistor fault",
	"ISET resistor short to ground fault",
	"Thermal shutdown fault",
};

/* Textual meaning for every register bit */
static const char *const lp8864_led_status_msg[] = {
	"LED 1 fault",
	"LED 2 fault",
	"LED 3 fault",
	"LED 4 fault",
	"LED 5 fault",
	"LED 6 fault",
	"LED open fault",
	"LED internal short fault",
	"LED short to GND fault",
	NULL, NULL, NULL,
	"Invalid string configuration fault",
	NULL,
	"I2C time out fault",
};

/**
 * struct lp8864
 * @client: Pointer to the I2C client
 * @led_dev: optional led class device pointer
 * @bl: backlight device pointer
 * @regmap: Devices register map
 * @led_status_mask: Helps to report LED fault only once
 */
struct lp8864 {
	struct i2c_client *client;
	struct led_classdev *led_dev;
	struct backlight_device *bl;
	struct regmap *regmap;
	u16 led_status_mask;
};

static int lp8864_fault_check(struct lp8864 *priv)
{
	int ret, i;
	unsigned int val;

	ret = regmap_read(priv->regmap, LP8864_SUPPLY_STATUS, &val);
	if (ret)
		goto err;

	/* Odd bits are status bits, even bits are clear bits */
	for (i = 0; i < ARRAY_SIZE(lp8864_supply_status_msg); i++)
		if (val & BIT(i * 2 + 1))
			dev_warn(&priv->client->dev, "%s\n", lp8864_supply_status_msg[i]);

	/*
	 * Clear bits have an index preceding the corresponding Status bits;
	 * both have to be written "1" simultaneously to clear the corresponding
	 * Status bit.
	 */
	if (val)
		ret = regmap_write(priv->regmap, LP8864_SUPPLY_STATUS, val >> 1 | val);
	if (ret)
		goto err;

	ret = regmap_read(priv->regmap, LP8864_BOOST_STATUS, &val);
	if (ret)
		goto err;

	/* Odd bits are status bits, even bits are clear bits */
	for (i = 0; i < ARRAY_SIZE(lp8864_boost_status_msg); i++)
		if (val & BIT(i * 2 + 1))
			dev_warn(&priv->client->dev, "%s\n", lp8864_boost_status_msg[i]);

	if (val)
		ret = regmap_write(priv->regmap, LP8864_BOOST_STATUS, val >> 1 | val);
	if (ret)
		goto err;

	ret = regmap_read(priv->regmap, LP8864_LED_STATUS, &val);
	if (ret)
		goto err;

	/*
	 * Clear already reported faults that maintain their value until device
	 * power-down
	 */
	val &= ~priv->led_status_mask;

	for (i = 0; i < ARRAY_SIZE(lp8864_led_status_msg); i++)
		if (lp8864_led_status_msg[i] && val & BIT(i))
			dev_warn(&priv->client->dev, "%s\n", lp8864_led_status_msg[i]);

	/*
	 * Mark those which maintain their value until device power-down as
	 * "already reported"
	 */
	priv->led_status_mask |= val & ~LP8864_LED_STATUS_WR_MASK;

	/*
	 * Only bits 14, 12, 10 have to be cleared here, but others are RO,
	 * we don't care what we write to them.
	 */
	if (val & LP8864_LED_STATUS_WR_MASK)
		ret = regmap_write(priv->regmap, LP8864_LED_STATUS, val >> 1 | val);
	if (ret)
		goto err;

	return 0;

err:
	dev_err(&priv->client->dev, "Failed to read/clear faults (%pe)\n", ERR_PTR(ret));

	return ret;
}

static int lp8864_brightness_set(struct lp8864 *priv, unsigned int brightness)
{
	int ret;

	ret = lp8864_fault_check(priv);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, LP8864_BRT_CONTROL, brightness);
	if (ret)
		dev_err(&priv->client->dev, "Failed to write brightness value\n");

	return ret;
}

static int lp8864_backlight_update_status(struct backlight_device *bl)
{
	return lp8864_brightness_set(bl_get_data(bl), backlight_get_brightness(bl));
}

static int lp8864_backlight_get_brightness(struct backlight_device *bl)
{
	struct lp8864 *priv = bl_get_data(bl);
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap, LP8864_BRT_CONTROL, &val);
	if (ret) {
		dev_err(&priv->client->dev, "Failed to read brightness value\n");
		return ret;
	}

	return val;
}

static const struct backlight_ops lp8864_backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = lp8864_backlight_update_status,
	.get_brightness = lp8864_backlight_get_brightness,
};

static int lp8864_led_brightness_set(struct led_classdev *led_cdev,
				     enum led_brightness brt_val)
{
	struct lp8864 *priv = dev_get_drvdata(led_cdev->dev->parent);

	/* Scale 0..LED_FULL into 16-bit HW brightness */
	return lp8864_brightness_set(priv, brt_val * 0xffff / LED_FULL);
}

static enum led_brightness lp8864_led_brightness_get(struct led_classdev *led_cdev)
{
	struct lp8864 *priv = dev_get_drvdata(led_cdev->dev->parent);
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap, LP8864_BRT_CONTROL, &val);
	if (ret) {
		dev_err(&priv->client->dev, "Failed to read brightness value\n");
		return ret;
	}

	/* Scale 16-bit HW brightness into 0..LED_FULL */
	return val * LED_FULL / 0xffff;
}

static const struct regmap_config lp8864_regmap_config = {
	.reg_bits		= 8,
	.val_bits		= 16,
	.val_format_endian	= REGMAP_ENDIAN_LITTLE,
};

static void lp8864_disable_gpio(void *data)
{
	struct gpio_desc *gpio = data;

	gpiod_set_value(gpio, 0);
}

static int lp8864_probe(struct i2c_client *client)
{
	int ret;
	struct lp8864 *priv;
	struct device_node *np = dev_of_node(&client->dev);
	struct device_node *child_node;
	struct led_init_data init_data = {};
	struct backlight_device *bl;
	struct backlight_properties props;
	struct gpio_desc *enable_gpio;
	u32 val;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ret = devm_regulator_get_enable_optional(&client->dev, "vled");
	if (ret && ret != -ENODEV)
		return dev_err_probe(&client->dev, ret, "Failed to enable vled regulator\n");

	enable_gpio = devm_gpiod_get_optional(&client->dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(enable_gpio))
		return dev_err_probe(&client->dev, PTR_ERR(enable_gpio),
				     "Failed to get enable GPIO\n");

	ret = devm_add_action_or_reset(&client->dev, lp8864_disable_gpio, enable_gpio);
	if (ret)
		return ret;

	priv->client = client;
	i2c_set_clientdata(client, priv);

	priv->regmap = devm_regmap_init_i2c(client, &lp8864_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(priv->regmap),
				     "Failed to allocate regmap\n");

	/* Control brightness by DISPLAY_BRT register */
	ret = regmap_update_bits(priv->regmap, LP8864_USER_CONFIG1, LP8864_BRT_MODE_MASK,
								   LP8864_BRT_MODE_REG);
	if (ret) {
		dev_err(&priv->client->dev, "Failed to set brightness control mode\n");
		return ret;
	}

	ret = lp8864_fault_check(priv);
	if (ret)
		return ret;

	/* Register backlight class device */
	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = LP8864_MAX_BRIGHTNESS;
	props.brightness = LP8864_MAX_BRIGHTNESS;
	props.scale = BACKLIGHT_SCALE_LINEAR;

	if (!device_property_read_u32(&client->dev, "max-brightness", &val))
		props.max_brightness = val;

	if (!device_property_read_u32(&client->dev, "default-brightness", &val))
		props.brightness = val;

	bl = devm_backlight_device_register(&client->dev, "lp8864-backlight",
					    &client->dev, priv,
					    &lp8864_backlight_ops, &props);
	if (IS_ERR(bl))
		return dev_err_probe(&client->dev, PTR_ERR(bl),
				     "Failed to register backlight device\n");

	priv->bl = bl;
	backlight_update_status(bl);

	/* Register LED class device if "led" child node is present */
	child_node = of_get_available_child_by_name(np, "led");
	if (!child_node)
		return 0;

	priv->led_dev = devm_kzalloc(&client->dev, sizeof(*priv->led_dev), GFP_KERNEL);
	if (!priv->led_dev)
		return -ENOMEM;

	priv->led_dev->brightness_set_blocking = lp8864_led_brightness_set;
	priv->led_dev->brightness_get = lp8864_led_brightness_get;

	init_data.fwnode = of_fwnode_handle(child_node);
	init_data.devicename = "lp8864";
	init_data.default_label = ":display_cluster";

	ret = devm_led_classdev_register_ext(&client->dev, priv->led_dev, &init_data);
	if (ret)
		dev_err(&client->dev, "Failed to register LED device (%pe)\n", ERR_PTR(ret));

	return ret;
}

static const struct i2c_device_id lp8864_id[] = {
	{ .name = "lp8864" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, lp8864_id);

static const struct of_device_id of_lp8864_leds_match[] = {
	{ .compatible = "ti,lp8864" },
	{}
};
MODULE_DEVICE_TABLE(of, of_lp8864_leds_match);

static struct i2c_driver lp8864_driver = {
	.driver = {
		.name	= "lp8864",
		.of_match_table = of_lp8864_leds_match,
	},
	.probe		= lp8864_probe,
	.id_table	= lp8864_id,
};
module_i2c_driver(lp8864_driver);

MODULE_DESCRIPTION("Texas Instruments LP8864/LP8866 LED Backlight driver");
MODULE_AUTHOR("Alexander Sverdlin <alexander.sverdlin@siemens.com>");
MODULE_LICENSE("GPL");
