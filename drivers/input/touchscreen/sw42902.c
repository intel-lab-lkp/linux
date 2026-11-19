// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Abdur Rofik <rofikcoga@gmail.com>
 * Silicon Works SW42902 touchscreen, as used by LG V60 (timelm).
 * Register protocol and report layout derived from the LGE vendor driver
 * (Copyright (C) 2015 LGE, author hoyeon.jang@lge.com).
 * This driver uses the controller's existing flash firmware.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#define SW42902_MAX_CONTACTS	10
#define SW42902_REPORT_SIZE	(12 + 24 + 12 * SW42902_MAX_CONTACTS)
#define SW42902_IC_STATUS	0x600
#define SW42902_VERSION		0x642
#define SW42902_DEVICE_CTL	0xc00
#define SW42902_INTERRUPT_CTL	0xc01
#define SW42902_DRIVING_CTL	0xc03
#define SW42902_SERIAL_SPI_EN	0xfe4
#define SW42902_DRIVING_STOP	0x04
#define SW42902_DRIVING_U3	0x8303

static const char * const sw42902_supplies[] = {
	"vcl",
	"vdd",
};

struct sw42902 {
	struct i2c_client *client;
	struct input_dev *input;
	struct touchscreen_properties prop;
	struct gpio_desc *reset;
	struct regulator_bulk_data supplies[ARRAY_SIZE(sw42902_supplies)];
	bool suspended;
	u8 report[SW42902_REPORT_SIZE];
};

static int sw42902_read(struct sw42902 *ts, u16 reg, void *data, u16 len)
{
	u8 command[] = { (reg >> 8) | (len > 4 ? 0x20 : 0), reg & 0xff };
	struct i2c_msg msgs[] = {
		{ .addr = ts->client->addr, .len = sizeof(command),
		  .buf = command },
		{ .addr = ts->client->addr, .flags = I2C_M_RD,
		  .len = len, .buf = data },
	};
	int error;

	error = i2c_transfer(ts->client->adapter, msgs, ARRAY_SIZE(msgs));
	if (error < 0)
		return error;
	return error == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

static int sw42902_write(struct sw42902 *ts, u16 reg, u32 value)
{
	u8 command[6] = { 0x40 | (reg >> 8), reg & 0xff };
	int error;

	put_unaligned_le32(value, command + 2);
	error = i2c_master_send(ts->client, command, sizeof(command));
	if (error < 0)
		return error;
	return error == sizeof(command) ? 0 : -EIO;
}

static void sw42902_release_contacts(struct sw42902 *ts)
{
	int i;

	for (i = 0; i < SW42902_MAX_CONTACTS; i++) {
		input_mt_slot(ts->input, i);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
	}
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

static irqreturn_t sw42902_irq(int irq, void *data)
{
	struct sw42902 *ts = data;
	struct input_dev *input = ts->input;
	u32 status, header;
	unsigned int count, i;
	int error;

	error = sw42902_read(ts, SW42902_IC_STATUS, ts->report,
			     sizeof(ts->report));
	if (error) {
		dev_err_ratelimited(&ts->client->dev, "report read failed: %d\n", error);
		sw42902_release_contacts(ts);
		return IRQ_HANDLED;
	}

	status = get_unaligned_le32(ts->report + 4);
	header = get_unaligned_le32(ts->report + 8);
	/* Only normal absolute-touch reports; ignore pen/gesture/debug IRQs. */
	if (((status >> 16) & 0xf) != 5 || (header & 0xff))
		return IRQ_HANDLED;

	count = (header >> 8) & 0x1f;
	if (count > SW42902_MAX_CONTACTS) {
		dev_warn_ratelimited(&ts->client->dev, "invalid contact count: %u\n", count);
		sw42902_release_contacts(ts);
		return IRQ_HANDLED;
	}

	/* A palm report cancels all active fingers. */
	if (header & BIT(31)) {
		sw42902_release_contacts(ts);
		return IRQ_HANDLED;
	}

	/* Pre-scan for palm report to avoid queueing partial touch events */
	for (i = 0; i < count; i++) {
		const u8 *contact = ts->report + 36 + 12 * i;
		u32 a = get_unaligned_le32(contact);

		if ((a & 0x1f) == 15) {
			sw42902_release_contacts(ts);
			return IRQ_HANDLED;
		}
	}

	for (i = 0; i < count; i++) {
		const u8 *contact = ts->report + 36 + 12 * i;
		u32 a = get_unaligned_le32(contact);
		u32 b = get_unaligned_le32(contact + 4);
		u32 c = get_unaligned_le32(contact + 8);
		unsigned int id = a & 0x1f;
		unsigned int event = (a >> 16) & 3;

		if (id >= SW42902_MAX_CONTACTS || (event != 1 && event != 2))
			continue;

		input_mt_slot(input, id);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
		touchscreen_report_pos(input, &ts->prop, a >> 18, b & 0x3fff, true);
		input_report_abs(input, ABS_MT_PRESSURE, (b >> 14) & 0xff);
		input_report_abs(input, ABS_MT_TOUCH_MAJOR, (c >> 4) & 0x3fff);
		input_report_abs(input, ABS_MT_TOUCH_MINOR, c >> 18);
	}

	input_mt_sync_frame(input);
	input_sync(input);
	return IRQ_HANDLED;
}

static void sw42902_power_off(void *data)
{
	struct sw42902 *ts = data;

	if (ts->suspended)
		return;

	gpiod_set_value_cansleep(ts->reset, 1);
	usleep_range(1000, 2000);
	regulator_bulk_disable(ARRAY_SIZE(ts->supplies), ts->supplies);
	ts->suspended = true;
}

static int sw42902_power_on(struct sw42902 *ts)
{
	int error;

	error = regulator_bulk_enable(ARRAY_SIZE(ts->supplies), ts->supplies);
	if (error)
		return error;

	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ts->reset, 0);
	/* Docomo downstream DT specifies a 90 ms hardware reset delay. */
	msleep(90);

	return 0;
}

static int sw42902_start(struct sw42902 *ts)
{
	u8 version[4];
	int error;

	error = sw42902_read(ts, SW42902_VERSION, version, sizeof(version));
	if (error)
		return error;
	if (version[2] != 10 || (version[3] & 0xf) != 4) {
		dev_err(&ts->client->dev, "unexpected chip/protocol: %u/%u\n",
			version[2], version[3] & 0xf);
		return -ENODEV;
	}

	error = sw42902_write(ts, SW42902_SERIAL_SPI_EN, 0);
	if (error)
		return error;
	error = sw42902_write(ts, SW42902_DEVICE_CTL, 1);
	if (error)
		return error;
	error = sw42902_write(ts, SW42902_INTERRUPT_CTL, 1);
	if (error)
		return error;
	error = sw42902_write(ts, SW42902_DRIVING_CTL, SW42902_DRIVING_STOP);
	if (error)
		return error;
	msleep(30);
	error = sw42902_write(ts, SW42902_DRIVING_CTL, SW42902_DRIVING_U3);
	if (error)
		return error;

	return 0;
}

static int sw42902_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sw42902 *ts;
	struct input_dev *input;
	int error, i;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	if (client->irq <= 0)
		return dev_err_probe(dev, -EINVAL, "missing interrupt\n");

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->client = client;
	i2c_set_clientdata(client, ts);

	ts->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset))
		return dev_err_probe(dev, PTR_ERR(ts->reset), "reset GPIO\n");

	for (i = 0; i < ARRAY_SIZE(sw42902_supplies); i++)
		ts->supplies[i].supply = sw42902_supplies[i];

	error = devm_regulator_bulk_get(dev, ARRAY_SIZE(ts->supplies), ts->supplies);
	if (error)
		return dev_err_probe(dev, error, "failed to get regulators\n");

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	ts->input = input;
	input->name = "Silicon Works SW42902";
	input->id.bustype = BUS_I2C;
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, 1079, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, 2459, 0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE, 0, 255, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 16383, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MINOR, 0, 16383, 0, 0);
	touchscreen_parse_properties(input, true, &ts->prop);
	error = input_mt_init_slots(input, SW42902_MAX_CONTACTS,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error)
		return error;

	error = sw42902_power_on(ts);
	if (error)
		return error;

	error = devm_add_action_or_reset(dev, sw42902_power_off, ts);
	if (error)
		return error;

	error = devm_request_threaded_irq(dev, client->irq, NULL, sw42902_irq,
					  IRQF_ONESHOT | IRQF_NO_AUTOEN,
					  dev_name(dev), ts);
	if (error)
		return error;

	error = sw42902_start(ts);
	if (error)
		return dev_err_probe(dev, error, "controller initialization failed\n");
	error = input_register_device(input);
	if (error)
		return error;
	enable_irq(client->irq);
	dev_info(dev, "SW42902 initialized using existing flash firmware\n");
	return 0;
}

static int sw42902_suspend(struct device *dev)
{
	struct sw42902 *ts = i2c_get_clientdata(to_i2c_client(dev));

	if (ts->suspended)
		return 0;

	disable_irq(ts->client->irq);
	sw42902_release_contacts(ts);
	sw42902_write(ts, SW42902_DRIVING_CTL, SW42902_DRIVING_STOP);
	sw42902_power_off(ts);

	return 0;
}

static int sw42902_resume(struct device *dev)
{
	struct sw42902 *ts = i2c_get_clientdata(to_i2c_client(dev));
	int error;

	if (!ts->suspended)
		return 0;

	error = sw42902_power_on(ts);
	if (error)
		return error;

	ts->suspended = false;

	error = sw42902_start(ts);
	if (error) {
		sw42902_power_off(ts);
		return error;
	}
	enable_irq(ts->client->irq);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(sw42902_pm_ops, sw42902_suspend, sw42902_resume);

static const struct of_device_id sw42902_of_match[] = {
	{ .compatible = "lg,sw42902" },
	{ }
};
MODULE_DEVICE_TABLE(of, sw42902_of_match);

static const struct i2c_device_id sw42902_ids[] = {
	{ .name = "sw42902" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sw42902_ids);

static struct i2c_driver sw42902_driver = {
	.driver = {
		.name = "sw42902",
		.of_match_table = sw42902_of_match,
		.pm = pm_sleep_ptr(&sw42902_pm_ops),
	},
	.probe = sw42902_probe,
	.id_table = sw42902_ids,
};
module_i2c_driver(sw42902_driver);

MODULE_AUTHOR("Abdur Rofik <rofikcoga@gmail.com>");
MODULE_DESCRIPTION("Silicon Works SW42902 touchscreen driver");
MODULE_LICENSE("GPL");
