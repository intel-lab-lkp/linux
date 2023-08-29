// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for MPS MP3309C White LED driver with I2C interface
 *
 * Copyright (C) 2023 ASEM Srl
 * Author: Flavio Suligoi <f.suligoi@asem.it>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/pwm.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define REG_I2C_0	0x00
#define REG_I2C_1	0x01

#define REG_I2C_0_EN	0x80
#define REG_I2C_0_D0	0x40
#define REG_I2C_0_D1	0x20
#define REG_I2C_0_D2	0x10
#define REG_I2C_0_D3	0x08
#define REG_I2C_0_D4	0x04
#define REG_I2C_0_RSRV1	0x02
#define REG_I2C_0_RSRV2	0x01

#define REG_I2C_1_RSRV1	0x80
#define REG_I2C_1_DIMS	0x40
#define REG_I2C_1_SYNC	0x20
#define REG_I2C_1_OVP0	0x10
#define REG_I2C_1_OVP1	0x08
#define REG_I2C_1_VOS	0x04
#define REG_I2C_1_LEDO	0x02
#define REG_I2C_1_OTP	0x01

#define ANALOG_MAX_VAL	31
#define ANALOG_REG_MASK 0x7c

enum backlight_status {
	FIRST_POWER_ON,
	BACKLIGHT_OFF,
	BACKLIGHT_ON,
};

enum dimming_mode_value {
	DIMMING_PWM,
	DIMMING_ANALOG_I2C,
};

struct mp3309c_platform_data {
	u32 max_brightness;
	u32 brightness;
	u32 switch_on_delay_ms;
	u32 switch_off_delay_ms;
	u32 reset_on_delay_ms;
	u32 reset_on_length_ms;
	u8  dimming_mode;
	u8  reset_pulse_enable;
	u8  over_voltage_protection;

	unsigned int status;
};

struct mp3309c_chip {
	struct device *dev;
	struct mp3309c_platform_data *pdata;
	struct backlight_device *bl;
	struct gpio_desc *enable_gpio;
	struct regmap *regmap;
	struct pwm_device *pwmd;

	struct delayed_work enable_work;
	struct delayed_work reset_gpio_work;
	int irq;

	struct gpio_desc *reset_gpio;
};

static const struct regmap_config mp3309c_regmap = {
	.name = "mp3309c_regmap",
	.reg_bits = 8,
	.reg_stride = 1,
	.val_bits = 8,
	.max_register = REG_I2C_1,
};

static int pm3309c_parse_dt_node(struct mp3309c_chip *chip,
				 struct mp3309c_platform_data *pdata)
{
	struct device_node *node = chip->dev->of_node;
	const char *tmp_string;
	int ret;

	if (!node) {
		dev_err(chip->dev, "failed to get DT node\n");
		return -ENODEV;
	}

	/*
	 * Dimming mode: the MP3309C provides two dimming methods:
	 *
	 * - PWM mode
	 * - Analog by I2C control mode
	 */
	ret = of_property_read_string(node, "mps,dimming-mode", &tmp_string);
	if (ret < 0) {
		dev_err(chip->dev, "missed dimming-mode in DT\n");
		return ret;
	}
	if (!strcmp(tmp_string, "pwm")) {
		dev_info(chip->dev, "dimming method: PWM\n");
		pdata->dimming_mode = DIMMING_PWM;
	}
	if (!strcmp(tmp_string, "analog-i2c")) {
		dev_info(chip->dev, "dimming method: analog by I2C commands\n");
		pdata->dimming_mode = DIMMING_ANALOG_I2C;
	}

	/* PWM steps (levels): 0 .. max_brightness */
	ret = of_property_read_u32(node, "max-brightness",
				   &pdata->max_brightness);
	if (ret < 0) {
		dev_err(chip->dev, "failed to get max-brightness from DT\n");
		return ret;
	}

	/* Default brightness at startup */
	ret = of_property_read_u32(node, "default-brightness",
				   &pdata->brightness);
	if (ret < 0) {
		dev_err(chip->dev,
			"failed to get default-brightness from DT\n");
		return ret;
	}

	/*
	 * Optional backlight switch-on/off delay
	 *
	 * Note: set 10ms as minimal value for switch-on delay, to stabilize
	 *       video data
	 */
	pdata->switch_on_delay_ms = 50;
	of_property_read_u32(node, "mps,switch-on-delay-ms",
			     &pdata->switch_on_delay_ms);
	if (pdata->switch_on_delay_ms < 10) {
		pdata->switch_on_delay_ms = 10;
		dev_warn(chip->dev,
			 "switch-on-delay-ms set to 10ms as minimal value\n");
	}
	pdata->switch_off_delay_ms = 0;
	of_property_read_u32(node, "mps,switch-off-delay-ms",
			     &pdata->switch_off_delay_ms);

	/*
	 * Reset: GPIO, initial delay and pulse length
	 *
	 * Use this optional GPIO to reset an external device (LCD panel, video
	 * FPGA, etc) when the backlight is switched on
	 */
	pdata->reset_pulse_enable = 0;
	chip->reset_gpio = devm_gpiod_get_optional(chip->dev, "mps,reset",
						   GPIOD_OUT_LOW);
	if (IS_ERR(chip->reset_gpio)) {
		ret = PTR_ERR(chip->reset_gpio);
		dev_err(chip->dev, "error acquiring reset gpio: %d\n", ret);
		return ret;
	}
	if (chip->reset_gpio) {
		pdata->reset_pulse_enable = 1;

		pdata->reset_on_delay_ms = 10;
		of_property_read_u32(node, "mps,reset-on-delay-ms",
				     &pdata->reset_on_delay_ms);
		pdata->reset_on_length_ms = 10;
		of_property_read_u32(node, "mps,reset-on-length-ms",
				     &pdata->reset_on_length_ms);
	}

	/*
	 * Over-voltage protection (OVP)
	 *
	 * These (optional) properties are:
	 *
	 *  - overvoltage-protection-13v --> OVP point set to 13.5V
	 *  - overvoltage-protection-24v --> OVP point set to 24V
	 *  - overvoltage-protection-35v --> OVP point set to 35.5V
	 *
	 * If not chosen, the hw default value for OVP is 35.5V
	 */
	pdata->over_voltage_protection = REG_I2C_1_OVP1;
	if (of_property_read_bool(node, "mps,overvoltage-protection-13v"))
		pdata->over_voltage_protection = 0x00;
	if (of_property_read_bool(node, "mps,overvoltage-protection-24v"))
		pdata->over_voltage_protection = REG_I2C_1_OVP0;
	if (of_property_read_bool(node, "mps,overvoltage-protection-35v"))
		pdata->over_voltage_protection = REG_I2C_1_OVP1;

	return 0;
}

static int mp3309c_enable_device(struct mp3309c_chip *chip)
{
	u8 reg_val = 0; /* Configuration for analog by I2C commands */
	int ret;

	/* I2C register #0 - Device enable */
	ret = regmap_update_bits(chip->regmap, REG_I2C_0, REG_I2C_0_EN,
				 REG_I2C_0_EN);
	if (ret)
		return ret;

	/*
	 * I2C register #1 - Set working mode:
	 *  - set one of the two dimming mode:
	 *    - PWM dimming using an external PWM dimming signal
	 *    - analog dimming using I2C commands
	 *  - enable synchronous mode (fixed for now)
	 *  - set overvoltage protection (OVP)
	 */
	if (chip->pdata->dimming_mode == DIMMING_PWM)
		reg_val = REG_I2C_1_DIMS;
	ret = regmap_write(chip->regmap, REG_I2C_1, reg_val | REG_I2C_1_SYNC |
			   chip->pdata->over_voltage_protection);
	if (ret)
		return ret;

	return 0;
}

/* For delayed backlight enabled */
static void mp3309c_enable(struct work_struct *work)
{
	struct mp3309c_chip *chip = container_of(work, struct mp3309c_chip,
						 enable_work.work);
	if (mp3309c_enable_device(chip))
		dev_err(chip->dev, "failed writing I2C register\n");
}

static void mp3309c_reset_gpio(struct work_struct *work)
{
	struct mp3309c_chip *chip = container_of(work, struct mp3309c_chip,
						 reset_gpio_work.work);

	if (chip->reset_gpio) {
		gpiod_set_value_cansleep(chip->reset_gpio, 0);
		usleep_range(100, 150);
		gpiod_set_value_cansleep(chip->reset_gpio, 1);
		usleep_range(chip->pdata->reset_on_length_ms * 1000,
			     (chip->pdata->reset_on_length_ms * 1000) + 100);
		gpiod_set_value_cansleep(chip->reset_gpio, 0);
	}
}

static int mp3309c_bl_update_status(struct backlight_device *bl)
{
	struct mp3309c_chip *chip = bl_get_data(bl);
	int brightness = backlight_get_brightness(bl);
	struct pwm_state pwmstate;
	unsigned int analog_val, bits_val;
	int i, ret;

	if (chip->pdata->dimming_mode == DIMMING_PWM) {
		/*
		 * PWM dimming mode
		 */
		pwm_init_state(chip->pwmd, &pwmstate);
		pwm_set_relative_duty_cycle(&pwmstate, brightness,
					    chip->pdata->max_brightness);
		pwmstate.enabled = true;
		ret = pwm_apply_state(chip->pwmd, &pwmstate);
		if (ret)
			return ret;
	} else {
		/*
		 * Analog dimming mode by I2C commands
		 *
		 * The 5 bits of the dimming analog value D4..D0 is allocated
		 * in the I2C register #0, in the following way:
		 *
		 *     +--+--+--+--+--+--+--+--+
		 *     |EN|D0|D1|D2|D3|D4|XX|XX|
		 *     +--+--+--+--+--+--+--+--+
		 */
		analog_val = DIV_ROUND_UP(ANALOG_MAX_VAL * brightness,
					  chip->pdata->max_brightness);
		bits_val = 0;
		for (i = 0; i <= 5; i++)
			bits_val += ((analog_val >> i) & 0x01) << (6 - i);
		ret = regmap_update_bits(chip->regmap, REG_I2C_0,
					 ANALOG_REG_MASK, bits_val);
		if (ret)
			return ret;
	}

	if (brightness > 0) {
		switch (chip->pdata->status) {
		case FIRST_POWER_ON:
			/*
			 * Only for the first time, wait for the optional
			 * switch-on delay and then enable the device.
			 * Otherwise enable the backlight immediately.
			 */
			schedule_delayed_work(&chip->enable_work,
					      msecs_to_jiffies(chip->pdata->switch_on_delay_ms));
			/*
			 * Optional external device GPIO reset, with
			 * delay pulse length
			 */
			if (chip->pdata->reset_pulse_enable)
				schedule_delayed_work(&chip->reset_gpio_work,
						      msecs_to_jiffies(chip->pdata->reset_on_delay_ms));
			break;
		case BACKLIGHT_OFF:
			/* Enable the backlight immediately */
			if (chip->pdata->reset_pulse_enable)
				cancel_delayed_work(&chip->reset_gpio_work);
			mp3309c_enable_device(chip);
			break;
		}

		chip->pdata->status = BACKLIGHT_ON;
	} else {
		/* Wait for the optional switch-off delay */
		if (chip->pdata->switch_off_delay_ms > 0) {
			usleep_range(chip->pdata->switch_off_delay_ms * 1000,
				     (chip->pdata->switch_off_delay_ms + 1) *
				      1000);
		}

		chip->pdata->status = BACKLIGHT_OFF;
	}

	return 0;
}

static const struct backlight_ops mp3309c_bl_ops = {
	.update_status = mp3309c_bl_update_status,
};

static int mp3309c_probe(struct i2c_client *client)
{
	struct mp3309c_platform_data *pdata = dev_get_platdata(&client->dev);
	struct mp3309c_chip *chip;
	struct backlight_properties props;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "failed to check i2c functionality\n");
		return -EOPNOTSUPP;
	}

	chip = devm_kzalloc(&client->dev, sizeof(struct mp3309c_chip),
			    GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->dev = &client->dev;

	chip->regmap = devm_regmap_init_i2c(client, &mp3309c_regmap);
	if (IS_ERR(chip->regmap)) {
		ret = PTR_ERR(chip->regmap);
		dev_err(&client->dev, "failed to allocate register map\n");
		return ret;
	}

	i2c_set_clientdata(client, chip);

	if (!pdata) {
		pdata = devm_kzalloc(chip->dev,
				     sizeof(struct mp3309c_platform_data),
				     GFP_KERNEL);
		if (!pdata)
			return -ENOMEM;

		ret = pm3309c_parse_dt_node(chip, pdata);
		if (ret) {
			dev_err(&client->dev, "failed parsing DT node\n");
			return ret;
		}
	}
	chip->pdata = pdata;

	chip->enable_gpio = devm_gpiod_get_optional(&client->dev, "enable",
						    GPIOD_OUT_HIGH);
	if (IS_ERR(chip->enable_gpio)) {
		ret = PTR_ERR(chip->enable_gpio);
		return ret;
	}

	/* Backlight */
	props.type = BACKLIGHT_RAW;
	props.brightness = pdata->brightness;
	props.max_brightness = pdata->max_brightness;
	props.scale = BACKLIGHT_SCALE_LINEAR;
	chip->bl =
	    devm_backlight_device_register(chip->dev, "mp3309c_bl",
					   chip->dev, chip, &mp3309c_bl_ops,
					   &props);
	if (IS_ERR(chip->bl)) {
		dev_err(&client->dev, "failed registering backlight\n");
		return PTR_ERR(chip->bl);
	}
	pdata->status = FIRST_POWER_ON;

	/* Enable PWM, if required */
	if (pdata->dimming_mode == DIMMING_PWM) {
		chip->pwmd = devm_pwm_get(chip->dev, NULL);
		if (IS_ERR(chip->pwmd)) {
			dev_err(&client->dev, "failed getting pwm device\n");
			return PTR_ERR(chip->pwmd);
		}
		pwm_apply_args(chip->pwmd);
	}

	/*
	 * Workqueue for delayed backlight enabling
	 */
	INIT_DELAYED_WORK(&chip->enable_work, mp3309c_enable);

	/*
	 * Workqueue for (optional) external device GPIO reset
	 */
	if (pdata->reset_pulse_enable) {
		dev_info(&client->dev, "reset pulse enabled\n");
		INIT_DELAYED_WORK(&chip->reset_gpio_work, mp3309c_reset_gpio);
	}

	dev_info(&client->dev, "MP3309C backlight initialized");
	return 0;
}

static int mp3309c_backlight_switch_off(struct pwm_device *pwmd)
{
	struct pwm_state pwmstate;

	/* Switch-off the backlight */
	pwm_get_state(pwmd, &pwmstate);
	pwmstate.duty_cycle = 0;
	pwmstate.enabled = false;
	pwm_apply_state(pwmd, &pwmstate);

	return 0;
}

static void mp3309c_remove(struct i2c_client *client)
{
	struct mp3309c_chip *chip = i2c_get_clientdata(client);

	if (chip->pdata->dimming_mode == DIMMING_PWM)
		mp3309c_backlight_switch_off(chip->pwmd);
	if (chip->pdata->reset_pulse_enable)
		cancel_delayed_work(&chip->reset_gpio_work);
}

static void mp3309c_shutdown(struct i2c_client *client)
{
	struct mp3309c_chip *chip = i2c_get_clientdata(client);

	if (chip->pdata->dimming_mode == DIMMING_PWM)
		mp3309c_backlight_switch_off(chip->pwmd);
}

static const struct of_device_id mp3309c_match_table[] = {
	{ .compatible = "mps,mp3309c-backlight", },
	{ },
};
MODULE_DEVICE_TABLE(of, mp3309c_match_table);

static const struct i2c_device_id mp3309c_id[] = {
	{ "mp3309c", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mp3309c_id);

static struct i2c_driver mp3309c_i2c_driver = {
	.driver	= {
			.name		= "mp3309c-backlight",
			.of_match_table	= mp3309c_match_table,
	},
	.probe		= mp3309c_probe,
	.remove		= mp3309c_remove,
	.shutdown	= mp3309c_shutdown,
	.id_table	= mp3309c_id,
};

module_i2c_driver(mp3309c_i2c_driver);

MODULE_DESCRIPTION("Backlight Driver for MPS MP3309C");
MODULE_AUTHOR("Flavio Suligoi <f.suligoi@asem.it>");
MODULE_LICENSE("GPL");
