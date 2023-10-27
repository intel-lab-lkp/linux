// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Anshul Dalal <anshulusr@gmail.com>
 *
 * Driver for Adafruit Mini I2C Gamepad
 *
 * Based on the work of:
 *	Oleh Kravchenko (Sparkfun Qwiic Joystick driver)
 *
 * Datasheet: https://cdn-learn.adafruit.com/downloads/pdf/gamepad-qt.pdf
 * Product page: https://www.adafruit.com/product/5743
 * Firmware and hardware sources: https://github.com/adafruit/Adafruit_Seesaw
 *
 * TODO:
 *	- Add interrupt support
 */

#include <asm-generic/unaligned.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>

#define SEESAW_DEVICE_NAME	"seesaw-gamepad"

#define SEESAW_STATUS_BASE	0
#define SEESAW_GPIO_BASE	1
#define SEESAW_ADC_BASE		9

#define SEESAW_GPIO_DIRCLR_BULK	3
#define SEESAW_GPIO_BULK	4
#define SEESAW_GPIO_BULK_SET	5
#define SEESAW_GPIO_PULLENSET	11

#define SEESAW_STATUS_HW_ID	1
#define SEESAW_STATUS_SWRST	127

#define SEESAW_ADC_OFFSET	7

#define SEESAW_BUTTON_A		5
#define SEESAW_BUTTON_B		1
#define SEESAW_BUTTON_X		6
#define SEESAW_BUTTON_Y		2
#define SEESAW_BUTTON_START	16
#define SEESAW_BUTTON_SELECT	0

#define SEESAW_ANALOG_X		14
#define SEESAW_ANALOG_Y		15

#define SEESAW_JOYSTICK_MAX_AXIS	1023
#define SEESAW_JOYSTICK_FUZZ		2
#define SEESAW_JOYSTICK_FLAT		4

#define SEESAW_GAMEPAD_POLL_INTERVAL	16
#define SEESAW_GAMEPAD_POLL_MIN		8
#define SEESAW_GAMEPAD_POLL_MAX		32

u32 SEESAW_BUTTON_MASK = BIT(SEESAW_BUTTON_A) | BIT(SEESAW_BUTTON_B) |
			 BIT(SEESAW_BUTTON_X) | BIT(SEESAW_BUTTON_Y) |
			 BIT(SEESAW_BUTTON_START) | BIT(SEESAW_BUTTON_SELECT);

struct seesaw_gamepad {
	struct input_dev *input_dev;
	struct i2c_client *i2c_client;
};

struct seesaw_data {
	__be16 x;
	__be16 y;
	u32 button_state;
} __packed;

struct seesaw_button_description {
	unsigned int code;
	unsigned int bit;
};

static const struct seesaw_button_description seesaw_buttons[] = {
	{
		.code = BTN_EAST,
		.bit = SEESAW_BUTTON_A,
	},
	{
		.code = BTN_SOUTH,
		.bit = SEESAW_BUTTON_B,
	},
	{
		.code = BTN_NORTH,
		.bit = SEESAW_BUTTON_X,
	},
	{
		.code = BTN_WEST,
		.bit = SEESAW_BUTTON_Y,
	},
	{
		.code = BTN_START,
		.bit = SEESAW_BUTTON_START,
	},
	{
		.code = BTN_SELECT,
		.bit = SEESAW_BUTTON_SELECT,
	},
};

static int seesaw_register_read(struct i2c_client *client, u8 register_high,
				u8 register_low, char *buf, int count)
{
	int ret;
	u8 register_buf[2] = { register_high, register_low };

	ret = i2c_master_send(client, register_buf, sizeof(register_buf));
	if (ret < 0)
		return ret;
	ret = i2c_master_recv(client, buf, count);
	if (ret < 0)
		return ret;

	return 0;
}

static int seesaw_register_write_u8(struct i2c_client *client, u8 register_high,
				    u8 register_low, u8 value)
{
	int ret;
	u8 write_buf[3] = { register_high, register_low, value };

	ret = i2c_master_send(client, write_buf, sizeof(write_buf));
	if (ret < 0)
		return ret;

	return 0;
}

static int seesaw_register_write_u32(struct i2c_client *client,
				     u8 register_high, u8 register_low,
				     u32 value)
{
	int ret;
	u8 write_buf[6] = { register_high, register_low };

	put_unaligned_be32(value, write_buf + 2);
	ret = i2c_master_send(client, write_buf, sizeof(write_buf));
	if (ret < 0)
		return ret;

	return 0;
}

static int seesaw_read_data(struct i2c_client *client, struct seesaw_data *data)
{
	int ret;
	u8 read_buf[4];

	ret = seesaw_register_read(client, SEESAW_GPIO_BASE, SEESAW_GPIO_BULK,
				   read_buf, sizeof(read_buf));
	if (ret)
		return ret;

	data->button_state = ~get_unaligned_be32(&read_buf);

	ret = seesaw_register_read(client, SEESAW_ADC_BASE,
				   SEESAW_ADC_OFFSET + SEESAW_ANALOG_X,
				   (char *)&data->x, sizeof(data->x));
	if (ret)
		return ret;
	/*
	 * ADC reads left as max and right as 0, must be reversed since kernel
	 * expects reports in opposite order.
	 */
	data->x = SEESAW_JOYSTICK_MAX_AXIS - be16_to_cpu(data->x);

	ret = seesaw_register_read(client, SEESAW_ADC_BASE,
				   SEESAW_ADC_OFFSET + SEESAW_ANALOG_Y,
				   (char *)&data->y, sizeof(data->y));
	if (ret)
		return ret;
	data->y = be16_to_cpu(data->y);

	return 0;
}

static void seesaw_poll(struct input_dev *input)
{
	int err, i;
	struct seesaw_gamepad *private = input_get_drvdata(input);
	struct seesaw_data data;

	err = seesaw_read_data(private->i2c_client, &data);
	if (err != 0) {
		dev_err_ratelimited(&input->dev,
				    "failed to read joystick state: %d\n", err);
		return;
	}

	input_report_abs(input, ABS_X, data.x);
	input_report_abs(input, ABS_Y, data.y);

	for (i = 0; i < ARRAY_SIZE(seesaw_buttons); i++) {
		input_report_key(input, seesaw_buttons[i].code,
				 data.button_state &
					 BIT(seesaw_buttons[i].bit));
	}
	input_sync(input);
}

static int seesaw_probe(struct i2c_client *client)
{
	int err, i;
	u8 hardware_id;
	struct seesaw_gamepad *seesaw;

	err = seesaw_register_write_u8(client, SEESAW_STATUS_BASE,
				       SEESAW_STATUS_SWRST, 0xFF);
	if (err)
		return err;

	/* Wait for the registers to reset before proceeding */
	mdelay(10);

	seesaw = devm_kzalloc(&client->dev, sizeof(*seesaw), GFP_KERNEL);
	if (!seesaw)
		return -ENOMEM;

	err = seesaw_register_read(client, SEESAW_STATUS_BASE,
				   SEESAW_STATUS_HW_ID, &hardware_id, 1);
	if (err)
		return err;

	dev_dbg(&client->dev, "Adafruit Seesaw Gamepad, Hardware ID: %02x\n",
		hardware_id);

	/* Set Pin Mode to input and enable pull-up resistors */
	err = seesaw_register_write_u32(client, SEESAW_GPIO_BASE,
					SEESAW_GPIO_DIRCLR_BULK,
					SEESAW_BUTTON_MASK);
	if (err)
		return err;
	err = seesaw_register_write_u32(client, SEESAW_GPIO_BASE,
					SEESAW_GPIO_PULLENSET,
					SEESAW_BUTTON_MASK);
	if (err)
		return err;
	err = seesaw_register_write_u32(client, SEESAW_GPIO_BASE,
					SEESAW_GPIO_BULK_SET,
					SEESAW_BUTTON_MASK);
	if (err)
		return err;

	seesaw->i2c_client = client;
	i2c_set_clientdata(client, seesaw);

	seesaw->input_dev = devm_input_allocate_device(&client->dev);
	if (!seesaw->input_dev)
		return -ENOMEM;

	seesaw->input_dev->id.bustype = BUS_I2C;
	seesaw->input_dev->name = "Adafruit Seesaw Gamepad";
	seesaw->input_dev->phys = "i2c/" SEESAW_DEVICE_NAME;
	input_set_drvdata(seesaw->input_dev, seesaw);
	input_set_abs_params(seesaw->input_dev, ABS_X, 0,
			     SEESAW_JOYSTICK_MAX_AXIS, SEESAW_JOYSTICK_FUZZ,
			     SEESAW_JOYSTICK_FLAT);
	input_set_abs_params(seesaw->input_dev, ABS_Y, 0,
			     SEESAW_JOYSTICK_MAX_AXIS, SEESAW_JOYSTICK_FUZZ,
			     SEESAW_JOYSTICK_FLAT);
	for (i = 0; i < ARRAY_SIZE(seesaw_buttons); i++) {
		input_set_capability(seesaw->input_dev, EV_KEY,
				     seesaw_buttons[i].code);
	}

	err = input_setup_polling(seesaw->input_dev, seesaw_poll);
	if (err) {
		dev_err(&client->dev, "failed to set up polling: %d\n", err);
		return err;
	}

	input_set_poll_interval(seesaw->input_dev,
				SEESAW_GAMEPAD_POLL_INTERVAL);
	input_set_max_poll_interval(seesaw->input_dev, SEESAW_GAMEPAD_POLL_MAX);
	input_set_min_poll_interval(seesaw->input_dev, SEESAW_GAMEPAD_POLL_MIN);

	err = input_register_device(seesaw->input_dev);
	if (err) {
		dev_err(&client->dev, "failed to register joystick: %d\n", err);
		return err;
	}

	return 0;
}

static const struct i2c_device_id seesaw_id_table[] = {
	{ SEESAW_DEVICE_NAME, 0 },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, seesaw_id_table);

static struct i2c_driver seesaw_driver = {
	.driver = {
		.name = SEESAW_DEVICE_NAME,
	},
	.id_table = seesaw_id_table,
	.probe = seesaw_probe,
};
module_i2c_driver(seesaw_driver);

MODULE_AUTHOR("Anshul Dalal <anshulusr@gmail.com>");
MODULE_DESCRIPTION("Adafruit Mini I2C Gamepad driver");
MODULE_LICENSE("GPL");
