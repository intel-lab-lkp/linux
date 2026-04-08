// SPDX-License-Identifier: GPL-2.0
/* STMicroelectronics FTS Touchscreen device driver
 *
 * Copyright 2017 Samsung Electronics Co., Ltd.
 * Copyright 2017 Andi Shyti <andi@etezian.org>
 * Copyright David Heidelberg <david@ixit.cz>
 * Copyright Petr Hodina <petr.hodina@protonmail.com>
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

/* I2C commands */
#define STMFTS_READ_INFO			0x80
#define STMFTS_READ_STATUS			0x84
#define STMFTS_READ_ONE_EVENT			0x85
#define STMFTS_READ_ALL_EVENT			0x86
#define STMFTS_LATEST_EVENT			0x87
#define STMFTS_SLEEP_IN				0x90
#define STMFTS_SLEEP_OUT			0x91
#define STMFTS_MS_MT_SENSE_OFF			0x92
#define STMFTS_MS_MT_SENSE_ON			0x93
#define STMFTS_SS_HOVER_SENSE_OFF		0x94
#define STMFTS_SS_HOVER_SENSE_ON		0x95
#define STMFTS_MS_KEY_SENSE_OFF			0x9a
#define STMFTS_MS_KEY_SENSE_ON			0x9b
#define STMFTS_SYSTEM_RESET			0xa0
#define STMFTS_CLEAR_EVENT_STACK		0xa1
#define STMFTS_FULL_FORCE_CALIBRATION		0xa2
#define STMFTS_MS_CX_TUNING			0xa3
#define STMFTS_SS_CX_TUNING			0xa4
#define STMFTS5_SET_SCAN_MODE			0xa0

/* events */
#define STMFTS_EV_NO_EVENT			0x00
#define STMFTS_EV_MULTI_TOUCH_DETECTED		0x02
#define STMFTS_EV_MULTI_TOUCH_ENTER		0x03
#define STMFTS_EV_MULTI_TOUCH_LEAVE		0x04
#define STMFTS_EV_MULTI_TOUCH_MOTION		0x05
#define STMFTS_EV_HOVER_ENTER			0x07
#define STMFTS_EV_HOVER_LEAVE			0x08
#define STMFTS_EV_HOVER_MOTION			0x09
#define STMFTS_EV_KEY_STATUS			0x0e
#define STMFTS_EV_ERROR				0x0f
#define STMFTS_EV_CONTROLLER_READY		0x10
#define STMFTS_EV_SLEEP_OUT_CONTROLLER_READY	0x11
#define STMFTS_EV_STATUS			0x16
#define STMFTS_EV_DEBUG				0xdb

/* events FTS5 */
#define STMFTS5_EV_CONTROLLER_READY		0x03
/* FTM5 event IDs (full byte, not masked) */
#define STMFTS5_EV_MULTI_TOUCH_ENTER		0x13
#define STMFTS5_EV_MULTI_TOUCH_MOTION		0x23
#define STMFTS5_EV_MULTI_TOUCH_LEAVE		0x33
#define STMFTS5_EV_STATUS_UPDATE		0x43
#define STMFTS5_EV_USER_REPORT			0x53
#define STMFTS5_EV_DEBUG			0xe3
#define STMFTS5_EV_ERROR			0xf3

/* multi touch related event masks */
#define STMFTS_MASK_EVENT_ID			0x0f
#define STMFTS_MASK_TOUCH_ID			0xf0
#define STMFTS_MASK_LEFT_EVENT			0x0f
#define STMFTS_MASK_X_MSB			0x0f
#define STMFTS_MASK_Y_LSB			0xf0
#define STMFTS5_MASK_TOUCH_TYPE			0x0f

/* touch type classifications */
#define STMFTS_TOUCH_TYPE_INVALID		0x00
#define STMFTS_TOUCH_TYPE_FINGER		0x01
#define STMFTS_TOUCH_TYPE_GLOVE			0x02
#define STMFTS_TOUCH_TYPE_STYLUS		0x03
#define STMFTS_TOUCH_TYPE_PALM			0x04
#define STMFTS_TOUCH_TYPE_HOVER			0x05

/* key related event masks */
#define STMFTS_MASK_KEY_NO_TOUCH		0x00
#define STMFTS_MASK_KEY_MENU			0x01
#define STMFTS_MASK_KEY_BACK			0x02

#define STMFTS_EVENT_SIZE	8
#define STMFTS_STACK_DEPTH	32
#define STMFTS_DATA_MAX_SIZE	(STMFTS_EVENT_SIZE * STMFTS_STACK_DEPTH)
#define STMFTS_MAX_FINGERS	10
#define STMFTS_DEV_NAME		"stmfts"

static const struct regulator_bulk_data stmfts_supplies[] = {
	{ .supply = "vdd" },
	{ .supply = "avdd" },
};

struct stmfts_data {
	const struct stmfts_chip_ops *ops;

	struct i2c_client *client;
	struct input_dev *input;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *mode_switch_gpio;
	struct led_classdev led_cdev;
	struct mutex mutex;

	struct touchscreen_properties prop;

	struct regulator_bulk_data *supplies;

	/*
	 * Presence of ledvdd will be used also to check
	 * whether the LED is supported.
	 */
	struct regulator *ledvdd;

	u16 chip_id;
	u8 chip_ver;
	u16 fw_ver;
	u8 config_id;
	u8 config_ver;

	u8 data[STMFTS_DATA_MAX_SIZE];

	struct completion cmd_done;

	unsigned long touch_id;
	unsigned long stylus_id;

	bool use_key;
	bool led_status;
	bool hover_enabled;
	bool stylus_enabled;
	bool running;
};

struct stmfts_chip_ops {
	int  (*configure)(struct stmfts_data *sdata);
	void (*power_off)(struct stmfts_data *sdata);
	int  (*setup_input)(struct stmfts_data *sdata);
	int  (*input_open)(struct input_dev *dev);
	void (*input_close)(struct input_dev *dev);
	void (*parse_events)(struct stmfts_data *sdata);
	int  (*set_hover)(struct stmfts_data *sdata, bool enable);
	int  (*runtime_resume)(struct stmfts_data *sdata);
};

static int stmfts_brightness_set(struct led_classdev *led_cdev,
					enum led_brightness value)
{
	struct stmfts_data *sdata = container_of(led_cdev,
					struct stmfts_data, led_cdev);
	int err;

	if (value != sdata->led_status && sdata->ledvdd) {
		if (!value) {
			regulator_disable(sdata->ledvdd);
		} else {
			err = regulator_enable(sdata->ledvdd);
			if (err) {
				dev_warn(&sdata->client->dev,
					 "failed to enable ledvdd regulator: %d\n",
					 err);
				return err;
			}
		}
		sdata->led_status = value;
	}

	return 0;
}

static enum led_brightness stmfts_brightness_get(struct led_classdev *led_cdev)
{
	struct stmfts_data *sdata = container_of(led_cdev,
						struct stmfts_data, led_cdev);

	return !!regulator_is_enabled(sdata->ledvdd);
}

/*
 * We can't simply use i2c_smbus_read_i2c_block_data because we
 * need to read 256 bytes, which exceeds the 255-byte SMBus block limit.
 */
static int stmfts_read_events(struct stmfts_data *sdata)
{
	u8 cmd = STMFTS_READ_ALL_EVENT;
	struct i2c_msg msgs[2] = {
		{
			.addr	= sdata->client->addr,
			.len	= 1,
			.buf	= &cmd,
		},
		{
			.addr	= sdata->client->addr,
			.flags	= I2C_M_RD,
			.len	= STMFTS_DATA_MAX_SIZE,
			.buf	= sdata->data,
		},
	};
	int ret;

	ret = i2c_transfer(sdata->client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;

	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

/* FTS4 event handling functions */
static void stmfts_report_contact_event(struct stmfts_data *sdata,
					const u8 event[])
{
	u8 slot_id = (event[0] & STMFTS_MASK_TOUCH_ID) >> 4;
	u16 x = event[1] | ((event[2] & STMFTS_MASK_X_MSB) << 8);
	u16 y = (event[2] >> 4) | (event[3] << 4);
	u8 maj = event[4];
	u8 min = event[5];
	u8 orientation = event[6];
	u8 area = event[7];

	input_mt_slot(sdata->input, slot_id);

	input_mt_report_slot_state(sdata->input, MT_TOOL_FINGER, true);
	input_report_abs(sdata->input, ABS_MT_POSITION_X, x);
	input_report_abs(sdata->input, ABS_MT_POSITION_Y, y);
	input_report_abs(sdata->input, ABS_MT_TOUCH_MAJOR, maj);
	input_report_abs(sdata->input, ABS_MT_TOUCH_MINOR, min);
	input_report_abs(sdata->input, ABS_MT_PRESSURE, area);
	input_report_abs(sdata->input, ABS_MT_ORIENTATION, orientation);

	input_sync(sdata->input);
}

static void stmfts_report_contact_release(struct stmfts_data *sdata,
					  const u8 event[])
{
	u8 slot_id = (event[0] & STMFTS_MASK_TOUCH_ID) >> 4;

	input_mt_slot(sdata->input, slot_id);
	input_mt_report_slot_inactive(sdata->input);

	input_sync(sdata->input);
}

/* FTS5 event handling functions */
static void stmfts5_report_contact_event(struct stmfts_data *sdata,
					 const u8 event[])
{
	u8 area;
	u8 maj;
	u8 min;
	/* FTM5 event format:
	 * event[0] = event ID (0x13/0x23)
	 * event[1] = touch type (low 4 bits) | touch ID (high 4 bits)
	 * event[2] = X LSB
	 * event[3] = X MSB (low 4 bits) | Y MSB (high 4 bits)
	 * event[4] = Y LSB
	 * event[5] = pressure
	 * event[6] = major (low 4 bits) | minor (high 4 bits)
	 * event[7] = minor (high 2 bits)
	 */
	u8 touch_id = (event[1] & STMFTS_MASK_TOUCH_ID) >> 4;
	u8 touch_type = event[1] & STMFTS5_MASK_TOUCH_TYPE;
	int x, y, distance;
	unsigned int tool = MT_TOOL_FINGER;
	bool touch_condition = true;

	/* Parse coordinates with better precision */
	x = (((int)event[3] & STMFTS_MASK_X_MSB) << 8) | event[2];
	y = ((int)event[4] << 4) | ((event[3] & STMFTS_MASK_Y_LSB) >> 4);

	/* Parse pressure - ensure non-zero for active touch */
	area = event[5];
	if (area <= 0 && touch_type != STMFTS_TOUCH_TYPE_HOVER) {
		/* Should not happen for contact events. Set minimum pressure
		 * to prevent touch from being dropped
		 */
		dev_warn_once(&sdata->client->dev,
			      "zero pressure on contact event, slot %d\n", touch_id);
		area = 1;
	}

	/* Parse touch area with improved bit extraction */
	maj = (((event[0] & 0x0C) << 2) | ((event[6] & 0xF0) >> 4));
	min = (((event[7] & 0xC0) >> 2) | (event[6] & 0x0F));

	/* Distance is 0 for touching, max for hovering */
	distance = 0;

	/* Classify touch type and set appropriate tool and parameters */
	switch (touch_type) {
	case STMFTS_TOUCH_TYPE_STYLUS:
		if (sdata->stylus_enabled) {
			tool = MT_TOOL_PEN;
			__set_bit(touch_id, &sdata->stylus_id);
			__clear_bit(touch_id, &sdata->touch_id);
			break;
		}
		fallthrough; /* Report as finger if stylus not enabled */

	case STMFTS_TOUCH_TYPE_FINGER:
	case STMFTS_TOUCH_TYPE_GLOVE:
		tool = MT_TOOL_FINGER;
		__set_bit(touch_id, &sdata->touch_id);
		__clear_bit(touch_id, &sdata->stylus_id);
		break;

	case STMFTS_TOUCH_TYPE_PALM:
		/* Palm touch - report but can be filtered by userspace */
		tool = MT_TOOL_PALM;
		__set_bit(touch_id, &sdata->touch_id);
		__clear_bit(touch_id, &sdata->stylus_id);
		break;

	case STMFTS_TOUCH_TYPE_HOVER:
		tool = MT_TOOL_FINGER;
		touch_condition = false;
		area = 0;
		distance = 255;
		__set_bit(touch_id, &sdata->touch_id);
		__clear_bit(touch_id, &sdata->stylus_id);
		break;

	case STMFTS_TOUCH_TYPE_INVALID:
	default:
		dev_warn(&sdata->client->dev,
			 "invalid touch type %d for slot %d\n",
			 touch_type, touch_id);
		return;
	}

	/* Boundary check - some devices report max value, adjust */
	if (x >= sdata->prop.max_x)
		x = sdata->prop.max_x - 1;
	if (y >= sdata->prop.max_y)
		y = sdata->prop.max_y - 1;

	input_mt_slot(sdata->input, touch_id);
	input_report_key(sdata->input, BTN_TOUCH, touch_condition);
	input_mt_report_slot_state(sdata->input, tool, true);

	input_report_abs(sdata->input, ABS_MT_POSITION_X, x);
	input_report_abs(sdata->input, ABS_MT_POSITION_Y, y);
	input_report_abs(sdata->input, ABS_MT_TOUCH_MAJOR, maj);
	input_report_abs(sdata->input, ABS_MT_TOUCH_MINOR, min);
	input_report_abs(sdata->input, ABS_MT_PRESSURE, area);
	input_report_abs(sdata->input, ABS_MT_DISTANCE, distance);

	input_sync(sdata->input);
}

static void stmfts5_report_contact_release(struct stmfts_data *sdata,
					   const u8 event[])
{
	/* FTM5 format: touch ID is in high 4 bits of event[1] */
	u8 touch_id = (event[1] & STMFTS_MASK_TOUCH_ID) >> 4;
	u8 touch_type = event[1] & STMFTS5_MASK_TOUCH_TYPE;
	unsigned int tool = MT_TOOL_FINGER;

	/* Determine tool type based on touch classification */
	switch (touch_type) {
	case STMFTS_TOUCH_TYPE_STYLUS:
		if (sdata->stylus_enabled) {
			tool = MT_TOOL_PEN;
			__clear_bit(touch_id, &sdata->stylus_id);
		} else {
			__clear_bit(touch_id, &sdata->touch_id);
		}
		break;

	case STMFTS_TOUCH_TYPE_PALM:
		tool = MT_TOOL_PALM;
		__clear_bit(touch_id, &sdata->touch_id);
		break;

	case STMFTS_TOUCH_TYPE_FINGER:
	case STMFTS_TOUCH_TYPE_GLOVE:
	case STMFTS_TOUCH_TYPE_HOVER:
	default:
		tool = MT_TOOL_FINGER;
		__clear_bit(touch_id, &sdata->touch_id);
		break;
	}

	input_mt_slot(sdata->input, touch_id);
	input_report_abs(sdata->input, ABS_MT_PRESSURE, 0);
	input_mt_report_slot_state(sdata->input, tool, false);

	/* Report BTN_TOUCH only if no touches remain */
	if (!sdata->touch_id && !sdata->stylus_id)
		input_report_key(sdata->input, BTN_TOUCH, 0);

	input_sync(sdata->input);
}

static void stmfts_report_hover_event(struct stmfts_data *sdata,
				      const u8 event[])
{
	u16 x = (event[2] << 4) | (event[4] >> 4);
	u16 y = (event[3] << 4) | (event[4] & STMFTS_MASK_Y_LSB);
	u8 z = event[5];

	input_report_abs(sdata->input, ABS_X, x);
	input_report_abs(sdata->input, ABS_Y, y);
	input_report_abs(sdata->input, ABS_DISTANCE, z);

	input_sync(sdata->input);
}

static void stmfts_report_key_event(struct stmfts_data *sdata, const u8 event[])
{
	switch (event[2]) {
	case 0:
		input_report_key(sdata->input, KEY_BACK, 0);
		input_report_key(sdata->input, KEY_MENU, 0);
		break;

	case STMFTS_MASK_KEY_BACK:
		input_report_key(sdata->input, KEY_BACK, 1);
		break;

	case STMFTS_MASK_KEY_MENU:
		input_report_key(sdata->input, KEY_MENU, 1);
		break;

	default:
		dev_warn(&sdata->client->dev,
			 "unknown key event: %#02x\n", event[2]);
		break;
	}

	input_sync(sdata->input);
}

static void stmfts_parse_events(struct stmfts_data *sdata)
{
	int i;

	for (i = 0; i < STMFTS_STACK_DEPTH; i++) {
		u8 *event = &sdata->data[i * STMFTS_EVENT_SIZE];

		switch (event[0]) {
		case STMFTS_EV_CONTROLLER_READY:
		case STMFTS_EV_SLEEP_OUT_CONTROLLER_READY:
		case STMFTS_EV_STATUS:
			complete(&sdata->cmd_done);
			fallthrough;

		case STMFTS_EV_NO_EVENT:
		case STMFTS_EV_DEBUG:
			return;
		}

		switch (event[0] & STMFTS_MASK_EVENT_ID) {
		case STMFTS_EV_MULTI_TOUCH_ENTER:
		case STMFTS_EV_MULTI_TOUCH_MOTION:
			stmfts_report_contact_event(sdata, event);
			break;

		case STMFTS_EV_MULTI_TOUCH_LEAVE:
			stmfts_report_contact_release(sdata, event);
			break;

		case STMFTS_EV_HOVER_ENTER:
		case STMFTS_EV_HOVER_LEAVE:
		case STMFTS_EV_HOVER_MOTION:
			stmfts_report_hover_event(sdata, event);
			break;

		case STMFTS_EV_KEY_STATUS:
			stmfts_report_key_event(sdata, event);
			break;

		case STMFTS_EV_ERROR:
			dev_warn(&sdata->client->dev,
					"error code: 0x%x%x%x%x%x%x",
					event[6], event[5], event[4],
					event[3], event[2], event[1]);
			break;

		default:
			dev_err(&sdata->client->dev,
				"unknown event %#02x\n", event[0]);
		}
	}
}

static void stmfts5_parse_events(struct stmfts_data *sdata)
{
	for (int i = 0; i < STMFTS_STACK_DEPTH; i++) {
		u8 *event = &sdata->data[i * STMFTS_EVENT_SIZE];

		switch (event[0]) {
		case STMFTS5_EV_CONTROLLER_READY:
			complete(&sdata->cmd_done);
			fallthrough;

		case STMFTS_EV_NO_EVENT:
		case STMFTS5_EV_STATUS_UPDATE:
		case STMFTS5_EV_USER_REPORT:
		case STMFTS5_EV_DEBUG:
			return;

		case STMFTS5_EV_MULTI_TOUCH_ENTER:
		case STMFTS5_EV_MULTI_TOUCH_MOTION:
			stmfts5_report_contact_event(sdata, event);
			break;

		case STMFTS5_EV_MULTI_TOUCH_LEAVE:
			stmfts5_report_contact_release(sdata, event);
			break;

		case STMFTS5_EV_ERROR:
			dev_warn(&sdata->client->dev,
				 "error code: 0x%x%x%x%x%x%x",
				 event[6], event[5], event[4],
				 event[3], event[2], event[1]);
			break;

		default:
			dev_err(&sdata->client->dev,
				"unknown FTS5 event %#02x\n", event[0]);
		}
	}
}

static irqreturn_t stmfts_irq_handler(int irq, void *dev)
{
	struct stmfts_data *sdata = dev;
	int err;

	guard(mutex)(&sdata->mutex);

	err = stmfts_read_events(sdata);
	if (unlikely(err))
		dev_err(&sdata->client->dev,
			"failed to read events: %d\n", err);
	else
		sdata->ops->parse_events(sdata);

	return IRQ_HANDLED;
}

static int stmfts_command(struct stmfts_data *sdata, const u8 cmd)
{
	int err;

	reinit_completion(&sdata->cmd_done);

	err = i2c_smbus_write_byte(sdata->client, cmd);
	if (err)
		return err;

	if (!wait_for_completion_timeout(&sdata->cmd_done,
					 msecs_to_jiffies(1000)))
		return -ETIMEDOUT;

	return 0;
}

static int stmfts5_set_scan_mode(struct stmfts_data *sdata, const u8 val)
{
	int err;

	u8 scan_mode_cmd[3] = { STMFTS5_SET_SCAN_MODE, 0x00, val };
	struct i2c_msg msg = {
		.addr = sdata->client->addr,
		.len = sizeof(scan_mode_cmd),
		.buf = scan_mode_cmd,
	};

	err = i2c_transfer(sdata->client->adapter, &msg, 1);
	if (err != 1)
		return err < 0 ? err : -EIO;

	return 0;

}

static int stmfts_input_open(struct input_dev *dev)
{
	struct stmfts_data *sdata = input_get_drvdata(dev);
	int err;

	err = pm_runtime_resume_and_get(&sdata->client->dev);
	if (err)
		return err;

	err = i2c_smbus_write_byte(sdata->client, STMFTS_MS_MT_SENSE_ON);
	if (err) {
		pm_runtime_put_sync(&sdata->client->dev);
		return err;
	}

	scoped_guard(mutex, &sdata->mutex) {
		sdata->running = true;

		if (sdata->hover_enabled) {
			err = i2c_smbus_write_byte(sdata->client,
						   STMFTS_SS_HOVER_SENSE_ON);
			if (err)
				dev_warn(&sdata->client->dev,
					 "failed to enable hover\n");
		}
	}

	if (sdata->use_key) {
		err = i2c_smbus_write_byte(sdata->client,
					   STMFTS_MS_KEY_SENSE_ON);
		if (err)
			/* I can still use only the touch screen */
			dev_warn(&sdata->client->dev,
				 "failed to enable touchkey\n");
	}

	return 0;
}

static int stmfts5_input_open(struct input_dev *dev)
{
	struct stmfts_data *sdata = input_get_drvdata(dev);
	int err;

	err = pm_runtime_resume_and_get(&sdata->client->dev);
	if (err)
		return err;

	mutex_lock(&sdata->mutex);
	sdata->running = true;
	mutex_unlock(&sdata->mutex);

	err = stmfts5_set_scan_mode(sdata, 0xff);
	if (err) {
		pm_runtime_put_sync(&sdata->client->dev);
		return err;
	}

	return 0;
}

static void stmfts_input_close(struct input_dev *dev)
{
	struct stmfts_data *sdata = input_get_drvdata(dev);
	int err;

	err = i2c_smbus_write_byte(sdata->client, STMFTS_MS_MT_SENSE_OFF);
	if (err)
		dev_warn(&sdata->client->dev,
			 "failed to disable touchscreen: %d\n", err);

	scoped_guard(mutex, &sdata->mutex) {
		sdata->running = false;

		if (sdata->hover_enabled) {
			err = i2c_smbus_write_byte(sdata->client,
						   STMFTS_SS_HOVER_SENSE_OFF);
			if (err)
				dev_warn(&sdata->client->dev,
					 "failed to disable hover: %d\n", err);
		}
	}

	if (sdata->use_key) {
		err = i2c_smbus_write_byte(sdata->client,
					   STMFTS_MS_KEY_SENSE_OFF);
		if (err)
			dev_warn(&sdata->client->dev,
				 "failed to disable touchkey: %d\n", err);
	}

	pm_runtime_put_sync(&sdata->client->dev);
}

static void stmfts5_input_close(struct input_dev *dev)
{
	struct stmfts_data *sdata = input_get_drvdata(dev);
	int err;

	err = stmfts5_set_scan_mode(sdata, 0x00);
	if (err)
		dev_warn(&sdata->client->dev,
			 "failed to disable touchscreen: %d\n", err);

	mutex_lock(&sdata->mutex);
	sdata->running = false;
	mutex_unlock(&sdata->mutex);

	pm_runtime_put_sync(&sdata->client->dev);
}

static ssize_t stmfts_sysfs_chip_id(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%#x\n", sdata->chip_id);
}

static ssize_t stmfts_sysfs_chip_version(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", sdata->chip_ver);
}

static ssize_t stmfts_sysfs_fw_ver(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", sdata->fw_ver);
}

static ssize_t stmfts_sysfs_config_id(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%#x\n", sdata->config_id);
}

static ssize_t stmfts_sysfs_config_version(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", sdata->config_ver);
}

static ssize_t stmfts_sysfs_read_status(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);
	u8 status[4];
	int err;

	err = i2c_smbus_read_i2c_block_data(sdata->client, STMFTS_READ_STATUS,
					    sizeof(status), status);
	if (err)
		return err;

	return sysfs_emit(buf, "%#02x\n", status[0]);
}

static ssize_t stmfts_sysfs_hover_enable_read(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", sdata->hover_enabled);
}

static ssize_t stmfts_sysfs_hover_enable_write(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);
	unsigned long value;
	bool hover;
	int err;

	if (kstrtoul(buf, 0, &value))
		return -EINVAL;

	hover = !!value;

	guard(mutex)(&sdata->mutex);

	if (hover != sdata->hover_enabled) {
		if (sdata->running && sdata->ops->set_hover) {
			err = sdata->ops->set_hover(sdata, hover);
			if (err)
				return err;
		}

		sdata->hover_enabled = hover;
	}

	return len;
}

static DEVICE_ATTR(chip_id, 0444, stmfts_sysfs_chip_id, NULL);
static DEVICE_ATTR(chip_version, 0444, stmfts_sysfs_chip_version, NULL);
static DEVICE_ATTR(fw_ver, 0444, stmfts_sysfs_fw_ver, NULL);
static DEVICE_ATTR(config_id, 0444, stmfts_sysfs_config_id, NULL);
static DEVICE_ATTR(config_version, 0444, stmfts_sysfs_config_version, NULL);
static DEVICE_ATTR(status, 0444, stmfts_sysfs_read_status, NULL);
static DEVICE_ATTR(hover_enable, 0644, stmfts_sysfs_hover_enable_read,
					stmfts_sysfs_hover_enable_write);

static struct attribute *stmfts_sysfs_attrs[] = {
	&dev_attr_chip_id.attr,
	&dev_attr_chip_version.attr,
	&dev_attr_fw_ver.attr,
	&dev_attr_config_id.attr,
	&dev_attr_config_version.attr,
	&dev_attr_status.attr,
	&dev_attr_hover_enable.attr,
	NULL
};
ATTRIBUTE_GROUPS(stmfts_sysfs);

static int stmfts_read_system_info(struct stmfts_data *sdata)
{
	int err;
	u8 reg[8];

	err = i2c_smbus_read_i2c_block_data(sdata->client, STMFTS_READ_INFO,
					    sizeof(reg), reg);
	if (err < 0)
		return err;
	if (err != sizeof(reg))
		return -EIO;

	sdata->chip_id = be16_to_cpup((__be16 *)&reg[6]);
	sdata->chip_ver = reg[0];
	sdata->fw_ver = be16_to_cpup((__be16 *)&reg[2]);
	sdata->config_id = reg[4];
	sdata->config_ver = reg[5];

	return 0;
}

static void stmfts_reset(struct stmfts_data *sdata)
{
	gpiod_set_value_cansleep(sdata->reset_gpio, 1);
	msleep(20);

	gpiod_set_value_cansleep(sdata->reset_gpio, 0);
	msleep(50);
}

static int stmfts_configure(struct stmfts_data *sdata)
{
	int err;

	err = stmfts_read_system_info(sdata);
	if (err)
		return err;

	enable_irq(sdata->client->irq);

	msleep(50);

	err = stmfts_command(sdata, STMFTS_SYSTEM_RESET);
	if (err)
		return err;

	err = stmfts_command(sdata, STMFTS_SLEEP_OUT);
	if (err)
		return err;

	/* optional tuning */
	err = stmfts_command(sdata, STMFTS_MS_CX_TUNING);
	if (err)
		dev_warn(&sdata->client->dev,
			 "failed to perform mutual auto tune: %d\n", err);

	/* optional tuning */
	err = stmfts_command(sdata, STMFTS_SS_CX_TUNING);
	if (err)
		dev_warn(&sdata->client->dev,
			 "failed to perform self auto tune: %d\n", err);

	err = stmfts_command(sdata, STMFTS_FULL_FORCE_CALIBRATION);
	if (err)
		return err;

	/*
	 * At this point no one is using the touchscreen
	 * and I don't really care about the return value
	 */
	(void) i2c_smbus_write_byte(sdata->client, STMFTS_SLEEP_IN);

	return 0;
}

static int stmfts_power_on(struct stmfts_data *sdata)
{
	int err;

	err = regulator_bulk_enable(ARRAY_SIZE(stmfts_supplies),
				    sdata->supplies);
	if (err)
		return err;

	/*
	 * The datasheet does not specify the power on time, but considering
	 * that the reset time is < 10ms, I sleep 20ms to be sure
	 */
	msleep(20);

	if (sdata->reset_gpio)
		stmfts_reset(sdata);

	err = sdata->ops->configure(sdata);
	if (err)
		regulator_bulk_disable(ARRAY_SIZE(stmfts_supplies),
				       sdata->supplies);

	return err;
}

static int stmfts5_configure(struct stmfts_data *sdata)
{
	u8 event[STMFTS_EVENT_SIZE];
	int ret;

	/* Verify I2C communication */
	ret = i2c_smbus_read_i2c_block_data(sdata->client,
					    STMFTS_READ_ALL_EVENT,
					    sizeof(event), event);
	if (ret < 0)
		return ret;

	enable_irq(sdata->client->irq);

	return 0;
}

static void stmfts5_chip_power_off(struct stmfts_data *sdata)
{
	i2c_smbus_write_byte(sdata->client, STMFTS_SLEEP_IN);
	msleep(20);
}

static void stmfts_power_off(void *data)
{
	struct stmfts_data *sdata = data;

	disable_irq(sdata->client->irq);

	if (sdata->reset_gpio)
		gpiod_set_value_cansleep(sdata->reset_gpio, 1);

	if (sdata->ops->power_off)
		sdata->ops->power_off(sdata);

	regulator_bulk_disable(ARRAY_SIZE(stmfts_supplies),
			       sdata->supplies);
}

static int stmfts_setup_input(struct stmfts_data *sdata)
{
	struct device *dev = &sdata->client->dev;

	input_set_abs_params(sdata->input, ABS_MT_ORIENTATION, 0, 255, 0, 0);
	input_set_abs_params(sdata->input, ABS_DISTANCE, 0, 255, 0, 0);

	sdata->use_key = device_property_read_bool(dev, "touch-key-connected");
	if (sdata->use_key) {
		input_set_capability(sdata->input, EV_KEY, KEY_MENU);
		input_set_capability(sdata->input, EV_KEY, KEY_BACK);
	}

	return input_mt_init_slots(sdata->input, STMFTS_MAX_FINGERS,
				   INPUT_MT_DIRECT);
}

static int stmfts5_setup_input(struct stmfts_data *sdata)
{
	struct device *dev = &sdata->client->dev;

	sdata->mode_switch_gpio = devm_gpiod_get_optional(dev, "mode-switch",
							  GPIOD_OUT_HIGH);
	if (IS_ERR(sdata->mode_switch_gpio))
		return dev_err_probe(dev, PTR_ERR(sdata->mode_switch_gpio),
				     "Failed to get GPIO 'switch'\n");

	/* Mark as direct input device for calibration support */
	__set_bit(INPUT_PROP_DIRECT, sdata->input->propbit);

	/* Set up basic touch capabilities */
	input_set_capability(sdata->input, EV_KEY, BTN_TOUCH);

	/* Set resolution for accurate calibration */
	if (!input_abs_get_res(sdata->input, ABS_MT_POSITION_X)) {
		input_abs_set_res(sdata->input, ABS_MT_POSITION_X, 10);
		input_abs_set_res(sdata->input, ABS_MT_POSITION_Y, 10);
	}

	input_set_abs_params(sdata->input, ABS_MT_DISTANCE, 0, 255, 0, 0);

	/* Enable stylus support if requested */
	sdata->stylus_enabled = device_property_read_bool(dev, "stylus-enabled");

	/* Initialize touch tracking bitmaps */
	sdata->touch_id = 0;
	sdata->stylus_id = 0;

	/* Initialize MT slots with support for pen tool type */
	return input_mt_init_slots(sdata->input, STMFTS_MAX_FINGERS,
				   INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
}

static int stmfts_set_hover(struct stmfts_data *sdata, bool enable)
{
	return i2c_smbus_write_byte(sdata->client,
				    enable ? STMFTS_SS_HOVER_SENSE_ON :
					     STMFTS_SS_HOVER_SENSE_OFF);
}

static int stmfts_enable_led(struct stmfts_data *sdata)
{
	int err;

	/* get the regulator for powering the leds on */
	sdata->ledvdd = devm_regulator_get(&sdata->client->dev, "ledvdd");
	if (IS_ERR(sdata->ledvdd))
		return PTR_ERR(sdata->ledvdd);

	sdata->led_cdev.name = STMFTS_DEV_NAME;
	sdata->led_cdev.max_brightness = LED_ON;
	sdata->led_cdev.brightness = LED_OFF;
	sdata->led_cdev.brightness_set_blocking = stmfts_brightness_set;
	sdata->led_cdev.brightness_get = stmfts_brightness_get;

	err = devm_led_classdev_register(&sdata->client->dev, &sdata->led_cdev);
	if (err) {
		devm_regulator_put(sdata->ledvdd);
		return err;
	}

	return 0;
}

static int stmfts_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	int err;
	struct stmfts_data *sdata;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C |
						I2C_FUNC_SMBUS_BYTE_DATA |
						I2C_FUNC_SMBUS_I2C_BLOCK))
		return -ENODEV;

	sdata = devm_kzalloc(dev, sizeof(*sdata), GFP_KERNEL);
	if (!sdata)
		return -ENOMEM;

	i2c_set_clientdata(client, sdata);

	sdata->client = client;
	mutex_init(&sdata->mutex);
	init_completion(&sdata->cmd_done);

	sdata->ops = of_device_get_match_data(dev);

	err = devm_regulator_bulk_get_const(dev,
					    ARRAY_SIZE(stmfts_supplies),
					    stmfts_supplies,
					    &sdata->supplies);
	if (err)
		return err;

	sdata->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						    GPIOD_OUT_HIGH);
	if (IS_ERR(sdata->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sdata->reset_gpio),
				     "Failed to get GPIO 'reset'\n");

	sdata->input = devm_input_allocate_device(dev);
	if (!sdata->input)
		return -ENOMEM;

	sdata->input->name = STMFTS_DEV_NAME;
	sdata->input->id.bustype = BUS_I2C;
	sdata->input->open = sdata->ops->input_open;
	sdata->input->close = sdata->ops->input_close;

	input_set_capability(sdata->input, EV_ABS, ABS_MT_POSITION_X);
	input_set_capability(sdata->input, EV_ABS, ABS_MT_POSITION_Y);
	touchscreen_parse_properties(sdata->input, true, &sdata->prop);

	input_set_abs_params(sdata->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(sdata->input, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);
	input_set_abs_params(sdata->input, ABS_MT_PRESSURE, 0, 255, 0, 0);

	err = sdata->ops->setup_input(sdata);
	if (err)
		return err;

	input_set_drvdata(sdata->input, sdata);

	/*
	 * stmfts_power_on expects interrupt to be disabled, but
	 * at this point the device is still off and I do not trust
	 * the status of the irq line that can generate some spurious
	 * interrupts. To be on the safe side it's better to not enable
	 * the interrupts during their request.
	 */
	err = devm_request_threaded_irq(dev, client->irq,
					NULL, stmfts_irq_handler,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					"stmfts_irq", sdata);
	if (err)
		return err;

	dev_dbg(dev, "initializing ST-Microelectronics FTS...\n");

	err = stmfts_power_on(sdata);
	if (err)
		return err;

	err = devm_add_action_or_reset(dev, stmfts_power_off, sdata);
	if (err)
		return err;

	err = input_register_device(sdata->input);
	if (err)
		return err;

	if (sdata->use_key) {
		err = stmfts_enable_led(sdata);
		if (err) {
			/*
			 * Even if the LEDs have failed to be initialized and
			 * used in the driver, I can still use the device even
			 * without LEDs. The ledvdd regulator pointer will be
			 * used as a flag.
			 */
			dev_warn(dev, "unable to use touchkey leds\n");
			sdata->ledvdd = NULL;
		}
	}

	pm_runtime_enable(dev);
	device_enable_async_suspend(dev);

	return 0;
}

static void stmfts_remove(struct i2c_client *client)
{
	pm_runtime_disable(&client->dev);
}

static int stmfts_runtime_suspend(struct device *dev)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);
	int ret;

	ret = i2c_smbus_write_byte(sdata->client, STMFTS_SLEEP_IN);
	if (ret)
		dev_warn(dev, "failed to suspend device: %d\n", ret);

	return ret;
}

static int stmfts_chip_runtime_resume(struct stmfts_data *sdata)
{
	return i2c_smbus_write_byte(sdata->client, STMFTS_SLEEP_OUT);
}

static int stmfts5_chip_runtime_resume(struct stmfts_data *sdata)
{
	struct i2c_client *client = sdata->client;
	struct device *dev = &client->dev;
	u8 int_enable_cmd[4] = {0xB6, 0x00, 0x2C, 0x01};
	struct i2c_msg msg = {
		.addr = client->addr,
		.len = sizeof(int_enable_cmd),
		.buf = int_enable_cmd,
	};
	int ret;

	ret = i2c_smbus_write_byte(client, STMFTS_SLEEP_OUT);
	if (ret)
		return ret;

	msleep(20);

	/* Perform capacitance tuning after wakeup */
	ret = i2c_smbus_write_byte(client, STMFTS_MS_CX_TUNING);
	if (ret)
		dev_warn(dev, "MS_CX_TUNING failed: %d\n", ret);
	msleep(20);

	ret = i2c_smbus_write_byte(client, STMFTS_SS_CX_TUNING);
	if (ret)
		dev_warn(dev, "SS_CX_TUNING failed: %d\n", ret);
	msleep(20);

	/* Force calibration */
	ret = i2c_smbus_write_byte(client, STMFTS_FULL_FORCE_CALIBRATION);
	if (ret)
		dev_warn(dev, "FORCE_CALIBRATION failed: %d\n", ret);
	msleep(50);

	/* Enable controller interrupts */
	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;

	msleep(20);

	return 0;
}

static int stmfts_runtime_resume(struct device *dev)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);
	int ret;

	ret = sdata->ops->runtime_resume(sdata);
	if (ret)
		dev_err(dev, "failed to resume device: %d\n", ret);

	return ret;
}

static int stmfts_suspend(struct device *dev)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);

	stmfts_power_off(sdata);

	return 0;
}

static int stmfts_resume(struct device *dev)
{
	struct stmfts_data *sdata = dev_get_drvdata(dev);

	return stmfts_power_on(sdata);
}

static const struct dev_pm_ops stmfts_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(stmfts_suspend, stmfts_resume)
	RUNTIME_PM_OPS(stmfts_runtime_suspend, stmfts_runtime_resume, NULL)
};

#ifdef CONFIG_OF
static const struct stmfts_chip_ops stmfts4_ops = {
	.configure	= stmfts_configure,
	.setup_input	= stmfts_setup_input,
	.input_open	= stmfts_input_open,
	.input_close	= stmfts_input_close,
	.parse_events	= stmfts_parse_events,
	.set_hover	= stmfts_set_hover,
	.runtime_resume	= stmfts_chip_runtime_resume,
};

static const struct stmfts_chip_ops stmfts5_ops = {
	.configure	= stmfts5_configure,
	.power_off	= stmfts5_chip_power_off,
	.setup_input	= stmfts5_setup_input,
	.input_open	= stmfts5_input_open,
	.input_close	= stmfts5_input_close,
	.parse_events	= stmfts5_parse_events,
	.runtime_resume	= stmfts5_chip_runtime_resume,
};

static const struct of_device_id stmfts_of_match[] = {
	{ .compatible = "st,stmfts",	.data = &stmfts4_ops },
	{ .compatible = "st,stmfts5",	.data = &stmfts5_ops },
	{ },
};
MODULE_DEVICE_TABLE(of, stmfts_of_match);
#endif

static const struct i2c_device_id stmfts_id[] = {
	{ "stmfts" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, stmfts_id);

static struct i2c_driver stmfts_driver = {
	.driver = {
		.name = STMFTS_DEV_NAME,
		.dev_groups = stmfts_sysfs_groups,
		.of_match_table = of_match_ptr(stmfts_of_match),
		.pm = pm_ptr(&stmfts_pm_ops),
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = stmfts_probe,
	.remove = stmfts_remove,
	.id_table = stmfts_id,
};

module_i2c_driver(stmfts_driver);

MODULE_AUTHOR("Andi Shyti <andi.shyti@samsung.com>");
MODULE_AUTHOR("David Heidelberg <david@ixit.cz>");
MODULE_AUTHOR("Petr Hodina <petr.hodina@protonmail.com>");
MODULE_DESCRIPTION("STMicroelectronics FTS Touch Screen");
MODULE_LICENSE("GPL");
