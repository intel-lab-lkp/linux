// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Waveshare International Limited
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/fb.h>
#include <linux/gpio/driver.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

/* I2C registers of the microcontroller. */
#define REG_TP		0x94
#define REG_LCD		0x95
#define REG_PWM		0x96
#define REG_SIZE	0x97
#define REG_ID		0x98
#define REG_VERSION	0x99

enum {
	GPIO_AVDD = 0,
	GPIO_PANEL_RESET = 1,
	GPIO_BL_ENABLE = 2,
	GPIO_IOVCC = 4,
	GPIO_VCC = 8,
	GPIO_TS_RESET = 9,
	NUM_GPIO = 16,
};

struct waveshare_gpio {
	struct mutex dir_lock;
	struct mutex pwr_lock;
	struct regmap *regmap;
	u16 poweron_state;

	struct gpio_chip gc;
};

static const struct regmap_config waveshare_gpio_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = REG_PWM,
};

static int waveshare_gpio_get(struct waveshare_gpio *state, unsigned int offset)
{
	u16 pwr_state;

	mutex_lock(&state->pwr_lock);
	pwr_state = state->poweron_state & BIT(offset);
	mutex_unlock(&state->pwr_lock);

	return !!pwr_state;
}

static int waveshare_gpio_set(struct waveshare_gpio *state, unsigned int offset, int value)
{
	u16 last_val;

	mutex_lock(&state->pwr_lock);

	last_val = state->poweron_state;
	if (value)
		last_val |= BIT(offset);
	else
		last_val &= ~BIT(offset);

	state->poweron_state = last_val;

	regmap_write(state->regmap, REG_TP, last_val >> 8);
	regmap_write(state->regmap, REG_LCD, last_val & 0xff);

	mutex_unlock(&state->pwr_lock);

	return 0;
}

static int waveshare_gpio_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	return GPIO_LINE_DIRECTION_OUT;
}

static int waveshare_gpio_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct waveshare_gpio *state = gpiochip_get_data(gc);

	return waveshare_gpio_get(state, offset);
}

static int waveshare_gpio_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct waveshare_gpio *state = gpiochip_get_data(gc);

	return waveshare_gpio_set(state, offset, value);
}

static int waveshare_gpio_update_status(struct backlight_device *bl)
{
	struct waveshare_gpio *state = bl_get_data(bl);
	int brightness = backlight_get_brightness(bl);

	waveshare_gpio_set(state, GPIO_BL_ENABLE, brightness);

	return regmap_write(state->regmap, REG_PWM, brightness);
}

static const struct backlight_ops waveshare_gpio_bl = {
	.update_status = waveshare_gpio_update_status,
};

static int waveshare_gpio_i2c_read(struct i2c_client *client, u8 reg, unsigned int *buf)
{
	int val;

	val = i2c_smbus_read_byte_data(client, reg);
	if (val < 0)
		return val;

	*buf = val;

	return 0;
}

static int waveshare_gpio_probe(struct i2c_client *i2c)
{
	struct backlight_properties props = {};
	struct waveshare_gpio *state;
	struct device *dev = &i2c->dev;
	struct backlight_device *bl;
	struct regmap *regmap;
	unsigned int data;
	int ret;

	state = devm_kzalloc(dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	ret = devm_mutex_init(dev, &state->dir_lock);
	if (ret)
		return ret;

	ret = devm_mutex_init(dev, &state->pwr_lock);
	if (ret)
		return ret;

	i2c_set_clientdata(i2c, state);

	regmap = devm_regmap_init_i2c(i2c, &waveshare_gpio_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "Failed to allocate register map\n");

	ret = waveshare_gpio_i2c_read(i2c, REG_ID, &data);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to read register\n");

	dev_dbg(dev, "waveshare panel hw id = 0x%x\n", data);

	ret = waveshare_gpio_i2c_read(i2c, REG_SIZE, &data);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to read register\n");

	dev_dbg(dev, "waveshare panel size = %d\n", data);

	ret = waveshare_gpio_i2c_read(i2c, REG_VERSION, &data);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to read register\n");

	dev_dbg(dev, "waveshare panel mcu version = 0x%x\n", data);

	state->poweron_state = BIT(GPIO_TS_RESET);
	regmap_write(regmap, REG_TP, state->poweron_state >> 8);
	regmap_write(regmap, REG_LCD, state->poweron_state & 0xff);
	msleep(20);

	state->regmap = regmap;
	state->gc.parent = dev;
	state->gc.label = i2c->name;
	state->gc.owner = THIS_MODULE;
	state->gc.base = -1;
	state->gc.ngpio = NUM_GPIO;

	/* it is output only */
	state->gc.get = waveshare_gpio_gpio_get;
	state->gc.set = waveshare_gpio_gpio_set;
	state->gc.get_direction = waveshare_gpio_gpio_get_direction;
	state->gc.can_sleep = true;

	ret = devm_gpiochip_add_data(dev, &state->gc, state);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to create gpiochip\n");

	props.type = BACKLIGHT_RAW;
	props.max_brightness = 255;
	props.brightness = 255;
	bl = devm_backlight_device_register(dev, dev_name(dev), dev, state,
					    &waveshare_gpio_bl, &props);
	return PTR_ERR_OR_ZERO(bl);
}

static const struct of_device_id waveshare_gpio_dt_ids[] = {
	{ .compatible = "waveshare,dsi-touch-gpio" },
	{},
};
MODULE_DEVICE_TABLE(of, waveshare_gpio_dt_ids);

static struct i2c_driver waveshare_gpio_regulator_driver = {
	.driver = {
		.name = "waveshare-regulator",
		.of_match_table = of_match_ptr(waveshare_gpio_dt_ids),
	},
	.probe = waveshare_gpio_probe,
};

module_i2c_driver(waveshare_gpio_regulator_driver);

MODULE_DESCRIPTION("GPIO controller driver for Waveshare DSI touch panels");
MODULE_LICENSE("GPL");
