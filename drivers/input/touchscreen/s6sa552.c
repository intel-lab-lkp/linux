// SPDX-License-Identifier: GPL-2.0
// Samsung S6SA552 Touchscreen device driver
//
// Copyright (c) 2025 Ivaylo Ivanov <ivo.ivanov.ivanov1@gmail.com>
//
// Based on the s6sy761 driver:
//   Copyright (c) 2017 Samsung Electronics Co., Ltd.
//   Copyright (c) 2017 Andi Shyti <andi@etezian.org>

#include <linux/unaligned.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

/* commands */
#define S6SA552_SENSE_ON		0x40
#define S6SA552_SENSE_OFF		0x41
#define S6SA552_TOUCH_FUNCTION		0x63
#define S6SA552_DEVICE_ID		0x52
#define S6SA552_BOOT_STATUS		0x55
#define S6SA552_READ_ONE_EVENT		0x71
#define S6SA552_CLEAR_EVENT_STACK	0x60
#define S6SA552_PANEL_INFO		0x23

/* acknowledge events */
#define S6SA552_EVENT_ACK_BOOT		0x0c

/* status event types */
#define S6SA552_EVENT_TYPE_ACK		0x01
#define S6SA552_EVENT_TYPE_ERR		0x02
#define S6SA552_EVENT_TYPE_INFO		0x03
#define S6SA552_EVENT_TYPE_GEST		0x06
#define S6SA552_EVENT_TYPE_SPONGE	0x5a

/* boot status (BS) */
#define S6SA552_BS_APPLICATION		0x20

/* event id */
#define S6SA552_EVENT_ID_COORDINATE	0x01
#define S6SA552_EVENT_ID_STATUS		0x00

/* event register masks */
#define S6SA552_MASK_TID		0x0f  /* byte 1, lower 4 bits */
#define S6SA552_MASK_NT			0xf0  /* byte 1, upper 4 bits */
#define S6SA552_MASK_EID		0xc0  /* byte 0, bits 6-7 */
#define S6SA552_MASK_TOUCH_STATE	0x07  /* byte 0, bits 0-2 */
#define S6SA552_MASK_TOUCH_TYPE		0x38  /* byte 0, bits 3-5 */

/* touch states */
#define S6SA552_TS_NONE			0x00
#define S6SA552_TS_PRESS		0x01
#define S6SA552_TS_MOVE			0x02
#define S6SA552_TS_RELEASE		0x03

#define S6SA552_EVENT_SIZE		8
#define S6SA552_DEVID_SIZE		3
#define S6SA552_PANEL_ID_SIZE		11
#define S6SA552_MAX_FINGERS		10

/*
 * Hardcoded values, since at least on herolte, the subid register doesn't
 * read/exist.
 */
#define S6SA552_MAX_X			4095
#define S6SA552_MAX_Y			4095
#define S6SA552_TX_CHANNELS		16

#define S6SA552_DEV_NAME		"s6sa552"

enum s6sa552_regulators {
	S6SA552_REGULATOR_VDD,
	S6SA552_REGULATOR_AVDD,
};

struct s6sa552_data {
	struct i2c_client *client;
	struct regulator_bulk_data regulators[2];
	struct input_dev *input;
	struct touchscreen_properties prop;

	u8 data[S6SA552_EVENT_SIZE];

	u16 devid;
	u8 tx_channel;
};

static void s6sa552_report_coordinates(struct s6sa552_data *sdata,
				       u8 *event, u8 tid)
{
	u8 major = event[6];
	u8 minor = event[7];
	u8 z = event[5];
	u16 x = (event[2] << 4) | ((event[4] >> 4) & 0x0F);
	u16 y = (event[3] << 4) | (event[4] & 0x0F);

	input_mt_slot(sdata->input, tid);

	input_mt_report_slot_state(sdata->input, MT_TOOL_FINGER, true);
	input_report_abs(sdata->input, ABS_MT_POSITION_X, x);
	input_report_abs(sdata->input, ABS_MT_POSITION_Y, y);
	input_report_abs(sdata->input, ABS_MT_TOUCH_MAJOR, major);
	input_report_abs(sdata->input, ABS_MT_TOUCH_MINOR, minor);
	input_report_abs(sdata->input, ABS_MT_PRESSURE, z);

	input_sync(sdata->input);
}

static void s6sa552_report_release(struct s6sa552_data *sdata, u8 tid)
{
	input_mt_slot(sdata->input, tid);
	input_mt_report_slot_state(sdata->input, MT_TOOL_FINGER, false);

	input_sync(sdata->input);
}

static void s6sa552_handle_coordinates(struct s6sa552_data *sdata, u8 *event)
{
	u8 tid;
	u8 touch_state;

	if (unlikely(!(event[1] & S6SA552_MASK_TID)))
		return;

	tid = (event[1] & S6SA552_MASK_TID) - 1;
	touch_state = event[0] & S6SA552_MASK_TOUCH_STATE;

	switch (touch_state) {
	case S6SA552_TS_PRESS:
	case S6SA552_TS_MOVE:
		s6sa552_report_coordinates(sdata, event, tid);
		break;
	case S6SA552_TS_RELEASE:
		s6sa552_report_release(sdata, tid);
		break;
	case S6SA552_TS_NONE:
	default:
		break;
	}
}

static irqreturn_t s6sa552_irq_handler(int irq, void *dev)
{
	struct s6sa552_data *sdata = dev;
	int ret;
	u8 event_id;

	ret = i2c_smbus_read_i2c_block_data(sdata->client,
					    S6SA552_READ_ONE_EVENT,
					    S6SA552_EVENT_SIZE,
					    sdata->data);
	if (ret < 0) {
		dev_err(&sdata->client->dev, "failed to read event\n");
		return IRQ_HANDLED;
	}

	if (!sdata->data[0])
		return IRQ_HANDLED;

	event_id = sdata->data[0] >> 6;

	switch (event_id) {
	case S6SA552_EVENT_ID_COORDINATE:
		s6sa552_handle_coordinates(sdata, sdata->data);
		break;
	case S6SA552_EVENT_ID_STATUS:
		break;
	default:
		break;
	}

	return IRQ_HANDLED;
}

static int s6sa552_input_open(struct input_dev *dev)
{
	struct s6sa552_data *sdata = input_get_drvdata(dev);

	return i2c_smbus_write_byte(sdata->client, S6SA552_SENSE_ON);
}

static void s6sa552_input_close(struct input_dev *dev)
{
	struct s6sa552_data *sdata = input_get_drvdata(dev);
	int ret;

	ret = i2c_smbus_write_byte(sdata->client, S6SA552_SENSE_OFF);
	if (ret)
		dev_err(&sdata->client->dev, "failed to turn off sensing\n");
}

static ssize_t s6sa552_sysfs_devid(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct s6sa552_data *sdata = dev_get_drvdata(dev);

	return sprintf(buf, "%#x\n", sdata->devid);
}

static DEVICE_ATTR(devid, 0444, s6sa552_sysfs_devid, NULL);

static struct attribute *s6sa552_sysfs_attrs[] = {
	&dev_attr_devid.attr,
	NULL
};
ATTRIBUTE_GROUPS(s6sa552_sysfs);

static int s6sa552_power_on(struct s6sa552_data *sdata)
{
	u8 buffer[S6SA552_EVENT_SIZE];
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(sdata->regulators),
				    sdata->regulators);
	if (ret)
		return ret;

	msleep(140);

	/* double check whether the touch is functional */
	ret = i2c_smbus_read_i2c_block_data(sdata->client,
					    S6SA552_READ_ONE_EVENT,
					    S6SA552_EVENT_SIZE,
					    buffer);
	if (ret < 0)
		return ret;

	if (buffer[0] != S6SA552_EVENT_TYPE_ACK ||
	    buffer[1] != S6SA552_EVENT_ACK_BOOT) {
		return -ENODEV;
	}

	ret = i2c_smbus_read_byte_data(sdata->client, S6SA552_BOOT_STATUS);
	if (ret < 0)
		return ret;

	/* for some reasons the device might be stuck in the bootloader */
	if (ret != S6SA552_BS_APPLICATION)
		return -ENODEV;

	/* enable touch functionality */
	ret = i2c_smbus_write_byte_data(sdata->client,
					S6SA552_TOUCH_FUNCTION, 0x01);
	if (ret)
		return ret;

	mdelay(20); /* make sure everything is up */

	return 0;
}

static int s6sa552_hw_init(struct s6sa552_data *sdata)
{
	u8 buffer[S6SA552_DEVID_SIZE];
	int ret;

	ret = s6sa552_power_on(sdata);
	if (ret)
		return ret;

	ret = i2c_smbus_read_i2c_block_data(sdata->client,
					    S6SA552_DEVICE_ID,
					    S6SA552_DEVID_SIZE,
					    buffer);
	if (ret < 0)
		return ret;

	sdata->devid = get_unaligned_be16(buffer + 1);

	return 0;
}

static void s6sa552_power_off(void *data)
{
	struct s6sa552_data *sdata = data;

	disable_irq(sdata->client->irq);
	regulator_bulk_disable(ARRAY_SIZE(sdata->regulators),
			       sdata->regulators);
}

static int s6sa552_probe(struct i2c_client *client)
{
	struct s6sa552_data *sdata;
	int err;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C |
						I2C_FUNC_SMBUS_BYTE_DATA |
						I2C_FUNC_SMBUS_I2C_BLOCK))
		return -ENODEV;

	sdata = devm_kzalloc(&client->dev, sizeof(*sdata), GFP_KERNEL);
	if (!sdata)
		return -ENOMEM;

	i2c_set_clientdata(client, sdata);
	sdata->client = client;

	sdata->regulators[S6SA552_REGULATOR_VDD].supply = "vdd";
	sdata->regulators[S6SA552_REGULATOR_AVDD].supply = "avdd";
	err = devm_regulator_bulk_get(&client->dev,
				      ARRAY_SIZE(sdata->regulators),
				      sdata->regulators);
	if (err)
		return err;

	err = devm_add_action_or_reset(&client->dev, s6sa552_power_off, sdata);
	if (err)
		return err;

	err = s6sa552_hw_init(sdata);
	if (err)
		return err;

	sdata->input = devm_input_allocate_device(&client->dev);
	if (!sdata->input)
		return -ENOMEM;

	sdata->input->name = S6SA552_DEV_NAME;
	sdata->input->id.bustype = BUS_I2C;
	sdata->input->open = s6sa552_input_open;
	sdata->input->close = s6sa552_input_close;

	input_set_abs_params(sdata->input, ABS_MT_POSITION_X, 0, S6SA552_MAX_X,
			     0, 0);
	input_set_abs_params(sdata->input, ABS_MT_POSITION_Y, 0, S6SA552_MAX_Y,
			     0, 0);
	input_set_abs_params(sdata->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(sdata->input, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);
	input_set_abs_params(sdata->input, ABS_MT_PRESSURE, 0, 255, 0, 0);

	touchscreen_parse_properties(sdata->input, true, &sdata->prop);

	if (!input_abs_get_max(sdata->input, ABS_X) ||
	    !input_abs_get_max(sdata->input, ABS_Y)) {
		dev_warn(&client->dev, "the axis have not been set\n");
	}

	err = input_mt_init_slots(sdata->input, S6SA552_TX_CHANNELS,
				  INPUT_MT_DIRECT);
	if (err)
		return err;

	input_set_drvdata(sdata->input, sdata);

	err = input_register_device(sdata->input);
	if (err)
		return err;

	err = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					s6sa552_irq_handler,
					IRQF_TRIGGER_LOW | IRQF_ONESHOT,
					"s6sa552_irq", sdata);
	if (err)
		return err;

	pm_runtime_enable(&client->dev);

	return 0;
}

static void s6sa552_remove(struct i2c_client *client)
{
	pm_runtime_disable(&client->dev);
}

static int s6sa552_runtime_suspend(struct device *dev)
{
	struct s6sa552_data *sdata = dev_get_drvdata(dev);

	return i2c_smbus_write_byte(sdata->client, S6SA552_SENSE_OFF);
}

static int s6sa552_runtime_resume(struct device *dev)
{
	struct s6sa552_data *sdata = dev_get_drvdata(dev);

	return i2c_smbus_write_byte(sdata->client, S6SA552_SENSE_ON);
}

static int s6sa552_suspend(struct device *dev)
{
	struct s6sa552_data *sdata = dev_get_drvdata(dev);

	s6sa552_power_off(sdata);

	return 0;
}

static int s6sa552_resume(struct device *dev)
{
	struct s6sa552_data *sdata = dev_get_drvdata(dev);

	enable_irq(sdata->client->irq);

	return s6sa552_power_on(sdata);
}

static const struct dev_pm_ops s6sa552_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(s6sa552_suspend, s6sa552_resume)
	RUNTIME_PM_OPS(s6sa552_runtime_suspend, s6sa552_runtime_resume, NULL)
};

#ifdef CONFIG_OF
static const struct of_device_id s6sa552_of_match[] = {
	{ .compatible = "samsung,s6sa552", },
	{ },
};
MODULE_DEVICE_TABLE(of, s6sa552_of_match);
#endif

static const struct i2c_device_id s6sa552_id[] = {
	{ "s6sa552" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, s6sa552_id);

static struct i2c_driver s6sa552_driver = {
	.driver = {
		.name = S6SA552_DEV_NAME,
		.dev_groups = s6sa552_sysfs_groups,
		.of_match_table = of_match_ptr(s6sa552_of_match),
		.pm = pm_ptr(&s6sa552_pm_ops),
	},
	.probe = s6sa552_probe,
	.remove = s6sa552_remove,
	.id_table = s6sa552_id,
};

module_i2c_driver(s6sa552_driver);

MODULE_AUTHOR("Ivaylo Ivanov <ivo.ivanov.ivanov1@gmail.com>");
MODULE_DESCRIPTION("Samsung S6SA552 Touch Screen");
MODULE_LICENSE("GPL");
