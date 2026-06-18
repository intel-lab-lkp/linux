// SPDX-License-Identifier: GPL-2.0
/*
 * LED driver for Analog Devices LTC3208 Multi-Display Driver
 *
 * Copyright 2026 Analog Devices Inc.
 *
 * Author: Jan Carlo Roleda <jancarlo.roleda@analog.com>
 */
#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/types.h>

/* Registers */
#define LTC3208_REG_A_GRNRED 0x1 /* Green and Red current DAC*/
#define LTC3208_REG_B_AUXBLU 0x2 /* AUX and Blue current DAC*/
#define LTC3208_REG_C_MAIN 0x3 /* Main current DAC */
#define LTC3208_REG_D_SUB 0x4 /* Sub current DAC */
#define LTC3208_REG_E_AUX_SELECT 0x5 /* AUX DAC Select */
#define  LTC3208_AUX1_MASK GENMASK(1, 0)
#define  LTC3208_AUX2_MASK GENMASK(3, 2)
#define  LTC3208_AUX3_MASK GENMASK(5, 4)
#define  LTC3208_AUX4_MASK GENMASK(7, 6)
#define LTC3208_REG_F_CAM 0x6 /* CAM (High and Low) current DAC*/
#define LTC3208_REG_G_OPT 0x7 /* Device Options */
#define  LTC3208_OPT_CPO_MASK GENMASK(7, 6)
#define  LTC3208_OPT_DIS_RGBDROP BIT(3)
#define  LTC3208_OPT_DIS_CAMHILO BIT(2)
#define  LTC3208_OPT_EN_RGBS BIT(1)

#define LTC3208_MAX_BRIGHTNESS_4BIT 0xF
#define LTC3208_MAX_BRIGHTNESS_8BIT 0xFF

#define LTC3208_NUM_LED_GRPS 8
#define LTC3208_NUM_AUX_LEDS 4

#define LTC3208_NUM_AUX_OPT 4
#define LTC3208_MAX_CPO_OPT 3

enum ltc3208_aux_channel {
	LTC3208_AUX_CHAN_AUX = 0,
	LTC3208_AUX_CHAN_MAIN,
	LTC3208_AUX_CHAN_SUB,
	LTC3208_AUX_CHAN_CAM
};

enum ltc3208_channel {
	LTC3208_CHAN_MAIN = 0,
	LTC3208_CHAN_SUB,
	LTC3208_CHAN_AUX,
	LTC3208_CHAN_CAML,
	LTC3208_CHAN_CAMH,
	LTC3208_CHAN_RED,
	LTC3208_CHAN_BLUE,
	LTC3208_CHAN_GREEN,
	LTC3208_CHAN_N_COUNT,
};

static const char *const ltc3208_dt_aux_channels[] = { "adi,aux1-channel",
						       "adi,aux2-channel",
						       "adi,aux3-channel",
						       "adi,aux4-channel" };

static const char *const ltc3208_aux_opt[] = { "aux", "main", "sub", "cam" };

struct ltc3208_led {
	struct led_classdev cdev;
	struct i2c_client *client;
	struct regmap_field *rfield;
	enum ltc3208_channel channel;
};

struct ltc3208 {
	struct ltc3208_led leds[LTC3208_NUM_LED_GRPS];
	struct regmap *regmap;
};

static const struct regmap_config ltc3208_regmap_cfg = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = LTC3208_REG_G_OPT,
	.cache_type = REGCACHE_FLAT_S,
};

static const struct reg_field ltc3208_led_reg_field[LTC3208_CHAN_N_COUNT] = {
	[LTC3208_CHAN_MAIN] =  REG_FIELD(LTC3208_REG_C_MAIN, 0, 7),
	[LTC3208_CHAN_SUB] =   REG_FIELD(LTC3208_REG_D_SUB, 0, 7),
	[LTC3208_CHAN_BLUE] =  REG_FIELD(LTC3208_REG_B_AUXBLU, 0, 3),
	[LTC3208_CHAN_AUX] =   REG_FIELD(LTC3208_REG_B_AUXBLU, 4, 7),
	[LTC3208_CHAN_CAML] =  REG_FIELD(LTC3208_REG_F_CAM, 0, 3),
	[LTC3208_CHAN_CAMH] =  REG_FIELD(LTC3208_REG_F_CAM, 4, 7),
	[LTC3208_CHAN_RED] =   REG_FIELD(LTC3208_REG_A_GRNRED, 0, 3),
	[LTC3208_CHAN_GREEN] = REG_FIELD(LTC3208_REG_A_GRNRED, 4, 7),
};

static int ltc3208_led_set_brightness(struct led_classdev *led_cdev,
				      enum led_brightness brightness)
{
	struct ltc3208_led *led =
		container_of(led_cdev, struct ltc3208_led, cdev);
	u8 current_level = brightness;

	return regmap_field_write(led->rfield, current_level);
}

static int ltc3208_probe(struct i2c_client *client)
{
	enum ltc3208_aux_channel aux_channels[LTC3208_NUM_AUX_LEDS];
	struct ltc3208 *ddata;
	struct regmap *regmap;
	bool disable_rgb_aux4_dropout_signal;
	bool disable_camhl_pin;
	bool set_sub_control_pin;
	int ret;
	u8 reg_val;

	regmap = devm_regmap_init_i2c(client, &ltc3208_regmap_cfg);
	if (IS_ERR(regmap))
		return dev_err_probe(&client->dev, PTR_ERR(regmap),
				     "Failed to initialize regmap\n");

	ddata = devm_kzalloc(&client->dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	ddata->regmap = regmap;

	disable_camhl_pin = device_property_read_bool(&client->dev,
						      "adi,disable-camhl-pin");
	set_sub_control_pin =
		device_property_read_bool(&client->dev, "adi,cfg-enrgbs-pin");
	disable_rgb_aux4_dropout_signal = device_property_read_bool(
		&client->dev, "adi,disable-rgb-aux4-dropout");

	reg_val = FIELD_PREP(LTC3208_OPT_EN_RGBS, set_sub_control_pin) |
		  FIELD_PREP(LTC3208_OPT_DIS_CAMHILO, disable_camhl_pin) |
		  FIELD_PREP(LTC3208_OPT_DIS_RGBDROP,
			     disable_rgb_aux4_dropout_signal);

	ret = regmap_write(regmap, LTC3208_REG_G_OPT, reg_val);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "error writing to options register\n");

	/* Initialize aux channel configurations */
	for (int i = 0; i < LTC3208_NUM_AUX_LEDS; i++) {
		ret = device_property_match_property_string(
			&client->dev, ltc3208_dt_aux_channels[i],
			ltc3208_aux_opt, LTC3208_NUM_AUX_OPT);
		/* Fallback to default value (AUX) if not found */
		if (ret == -EINVAL)
			aux_channels[i] = LTC3208_AUX_CHAN_AUX;
		else if (ret >= 0)
			aux_channels[i] = ret;
	}

	reg_val = FIELD_PREP(LTC3208_AUX1_MASK, aux_channels[0]) |
		  FIELD_PREP(LTC3208_AUX2_MASK, aux_channels[1]) |
		  FIELD_PREP(LTC3208_AUX3_MASK, aux_channels[2]) |
		  FIELD_PREP(LTC3208_AUX4_MASK, aux_channels[3]);

	ret = regmap_write(regmap, LTC3208_REG_E_AUX_SELECT, reg_val);
	if (ret)
		return dev_err_probe(&client->dev, ret,
			"error writing to aux channel register.\n");

	i2c_set_clientdata(client, ddata);

	device_for_each_child_node_scoped(&client->dev, child) {
		struct ltc3208_led *led;
		struct led_init_data init_data = {};
		u32 chan;

		ret = fwnode_property_read_u32(child, "reg", &chan);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					    "Failed to get reg value of LED\n");
		else if (chan >= LTC3208_NUM_LED_GRPS)
			return dev_err_probe(&client->dev, ret,
					     "%d is an invalid LED ID\n", chan);

		led = &ddata->leds[chan];

		led->rfield =
			devm_regmap_field_alloc(&client->dev, ddata->regmap,
						ltc3208_led_reg_field[chan]);
		if (IS_ERR(led->rfield))
			return dev_err_probe(&client->dev, PTR_ERR(led->rfield),
					     "cannot allocate regmap field\n");
		led->client = client;
		led->channel = chan;
		led->cdev.brightness_set_blocking = ltc3208_led_set_brightness;
		led->cdev.max_brightness = LTC3208_MAX_BRIGHTNESS_4BIT;

		if (chan == LTC3208_CHAN_MAIN || chan == LTC3208_CHAN_SUB)
			led->cdev.max_brightness = LTC3208_MAX_BRIGHTNESS_8BIT;

		init_data.fwnode = child;

		ret = devm_led_classdev_register_ext(&client->dev, &led->cdev,
						     &init_data);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "LED %u Register failed.\n", chan);
	}

	return 0;
}

static const struct of_device_id ltc3208_match_table[] = {
	{.compatible = "adi,ltc3208"},
	{}
};
MODULE_DEVICE_TABLE(of, ltc3208_match_table);

static const struct i2c_device_id ltc3208_idtable[] = {
	{ "ltc3208" },
	{}
};
MODULE_DEVICE_TABLE(i2c, ltc3208_idtable);

static struct i2c_driver ltc3208_driver = {
	.driver = {
		.name = "ltc3208",
		.of_match_table = ltc3208_match_table,
	},
	.id_table = ltc3208_idtable,
	.probe = ltc3208_probe,
};
module_i2c_driver(ltc3208_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jan Carlo Roleda <jancarlo.roleda@analog.com>");
MODULE_DESCRIPTION("LTC3208 LED Driver");
