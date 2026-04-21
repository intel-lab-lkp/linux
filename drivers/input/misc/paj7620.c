// SPDX-License-Identifier: GPL-2.0-only
/*
 * PixArt PAJ7620 Gesture Sensor - Input driver
 *
 * Copyright (C) 2026 Harpreet Saini <sainiharpreet29@yahoo.com>
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

/* Registers */
#define PAJ7620_REG_BANK_SEL        0xEF
#define PAJ7620_REG_GES_RESULT1     0x43
#define PAJ7620_REG_GES_RESULT2     0x44
#define PAJ7620_REG_SLEEP_BANK0     0x65
#define PAJ7620_REG_SLEEP_BANK1     0x05
#define PAJ7620_REG_AUTO_STANDBY    0x073

/* Gesture bits */
#define PAJ_UP           BIT(0)
#define PAJ_DOWN         BIT(1)
#define PAJ_LEFT         BIT(2)
#define PAJ_RIGHT        BIT(3)
#define PAJ_FORWARD      BIT(4)
#define PAJ_BACKWARD     BIT(5)
#define PAJ_CLOCKWISE    BIT(6)
#define PAJ_ANTICLOCK    BIT(7)
#define PAJ_WAVE         BIT(8)
#define PAJ_MAX_GESTURES 9

struct paj7620_data {
	struct i2c_client *client;
	struct regmap *regmap;
	struct input_dev *idev;
	struct regulator_bulk_data supplies[3];
	u32 keymap[PAJ_MAX_GESTURES];
};

/*
 * The following arrays contain undocumented register sequences required to
 * initialize the sensor's internal DSP and gesture engine.
 * These were derived from vendor reference code and verified via testing.
 */
static const struct reg_sequence init_register[] = {
	{ 0xEF, 0x00 }, { 0x37, 0x07 }, { 0x38, 0x17 }, { 0x39, 0x06 },
	{ 0x41, 0x00 }, { 0x42, 0x00 }, { 0x46, 0x2D }, { 0x47, 0x0F },
	{ 0x48, 0x3C }, { 0x49, 0x00 }, { 0x4A, 0x1E }, { 0x4C, 0x20 },
	{ 0x51, 0x10 }, { 0x5E, 0x10 }, { 0x60, 0x27 }, { 0x80, 0x42 },
	{ 0x81, 0x44 }, { 0x82, 0x04 }, { 0x8B, 0x01 }, { 0x90, 0x06 },
	{ 0x95, 0x0A }, { 0x96, 0x0C }, { 0x97, 0x05 }, { 0x9A, 0x14 },
	{ 0x9C, 0x3F }, { 0xA5, 0x19 }, { 0xCC, 0x19 }, { 0xCD, 0x0B },
	{ 0xCE, 0x13 }, { 0xCF, 0x64 }, { 0xD0, 0x21 }, { 0xEF, 0x01 },
	{ 0x02, 0x0F }, { 0x03, 0x10 }, { 0x04, 0x02 }, { 0x25, 0x01 },
	{ 0x27, 0x39 }, { 0x28, 0x7F }, { 0x29, 0x08 }, { 0x3E, 0xFF },
	{ 0x5E, 0x3D }, { 0x65, 0x96 }, { 0x67, 0x97 }, { 0x69, 0xCD },
	{ 0x6A, 0x01 }, { 0x6D, 0x2C }, { 0x6E, 0x01 }, { 0x72, 0x01 },
	{ 0x73, 0x35 }, { 0x74, 0x00 }, { 0x77, 0x01 },
};

/*
 * Specific configuration overrides required to enable the internal
 * 8-gesture state machine.
 */
static const struct reg_sequence init_gesture_array[] = {
	{ 0xEF, 0x00 }, { 0x41, 0x00 }, { 0x42, 0x00 }, { 0xEF, 0x00 },
	{ 0x48, 0x3C }, { 0x49, 0x00 }, { 0x51, 0x10 }, { 0x83, 0x20 },
	{ 0x9F, 0xF9 }, { 0xEF, 0x01 }, { 0x01, 0x1E }, { 0x02, 0x0F },
	{ 0x03, 0x10 }, { 0x04, 0x02 }, { 0x41, 0x40 }, { 0x43, 0x30 },
	{ 0x65, 0x96 }, { 0x66, 0x00 }, { 0x67, 0x97 }, { 0x68, 0x01 },
	{ 0x69, 0xCD }, { 0x6A, 0x01 }, { 0x6B, 0xB0 }, { 0x6C, 0x04 },
	{ 0x6D, 0x2C }, { 0x6E, 0x01 }, { 0x74, 0x00 }, { 0xEF, 0x00 },
	{ 0x41, 0xFF }, { 0x42, 0x01 },
};

static const struct reg_sequence paj7620_suspend_regs[] = {
	{ PAJ7620_REG_BANK_SEL, 0x00 },
	{ PAJ7620_REG_SLEEP_BANK0, 0x01 },
	{ PAJ7620_REG_BANK_SEL, 0x01 },
	{ PAJ7620_REG_SLEEP_BANK1, 0x01 },
};

static void paj7620_report_keys(struct paj7620_data *data, int gesture)
{
	int i;

	for (i = 0; i < PAJ_MAX_GESTURES; i++) {
		if (gesture & BIT(i)) {
			int key = data->keymap[i];

			input_report_key(data->idev, key, 1);
			input_sync(data->idev);
			input_report_key(data->idev, key, 0);
			input_sync(data->idev);
		}
	}
}

static irqreturn_t paj7620_irq_thread(int irq, void *ptr)
{
	struct paj7620_data *data = ptr;
	unsigned int g1, g2;
	int error;

	/* 2. RUNTIME PM: Force awake to read registers */
	pm_runtime_get_sync(&data->client->dev);

	regmap_write(data->regmap, PAJ7620_REG_BANK_SEL, 0);
	error = regmap_read(data->regmap, PAJ7620_REG_GES_RESULT1, &g1);
	error |= regmap_read(data->regmap, PAJ7620_REG_GES_RESULT2, &g2);

	if (!error && (g1 || g2))
		paj7620_report_keys(data, (g2 << 8) | g1);

	pm_runtime_mark_last_busy(&data->client->dev);
	pm_runtime_put_autosuspend(&data->client->dev);

	return IRQ_HANDLED;
}

static int paj7620_init(struct paj7620_data *data)
{
	int state = 0, error, i;

	/* 1. Wake-up sequence: Read register 0x00 until it returns 0x20 */
	for (i = 0; i < 10; i++) {
		error = regmap_read(data->regmap, 0x00, &state);
		if (error >= 0 && state == 0x20)
			break;
		usleep_range(1000, 2000);
	}

	if (state != 0x20) {
		dev_err(&data->client->dev, "Sensor wake-up failed (0x%02x)\n", state);
		return -ENODEV;
	}

	/* 2. Blast full register array into PAJ7620 instantly */
	error = regmap_multi_reg_write(data->regmap, init_register,
				       ARRAY_SIZE(init_register));
	if (error < 0) {
		dev_err(&data->client->dev, "Multi-reg write failed (%d)\n", error);
		return error;
	}

	error = regmap_write(data->regmap, PAJ7620_REG_BANK_SEL, 0x00);
	if (error < 0)
		return error;

	error = regmap_multi_reg_write(data->regmap, init_gesture_array,
				       ARRAY_SIZE(init_gesture_array));
	if (error < 0) {
		dev_err(&data->client->dev, "Multi-reg write failed (%d)\n", error);
		return error;
	}

	return 0;
}

static int paj7620_input_open(struct input_dev *idev)
{
	int error;
	struct paj7620_data *data = input_get_drvdata(idev);

	error = regulator_bulk_enable(ARRAY_SIZE(data->supplies), data->supplies);
	if (error)
		return error;

	error = paj7620_init(data);
	if (error) {
		regulator_bulk_disable(ARRAY_SIZE(data->supplies), data->supplies);
		return error;
	}

	return 0;
}

static void paj7620_input_close(struct input_dev *idev)
{
	struct paj7620_data *data = input_get_drvdata(idev);

	regmap_multi_reg_write(data->regmap, paj7620_suspend_regs,
			       ARRAY_SIZE(paj7620_suspend_regs));

	regulator_bulk_disable(ARRAY_SIZE(data->supplies), data->supplies);
}

static int paj7620_runtime_suspend(struct device *dev)
{
	int error;
	struct paj7620_data *data = dev_get_drvdata(dev);

	error = regmap_write(data->regmap, PAJ7620_REG_BANK_SEL, 0x01);
	if (error)
		return error;

	error = regmap_write(data->regmap, PAJ7620_REG_AUTO_STANDBY, 0x30);
	if (error)
		return error;

	return 0;
}

static int paj7620_runtime_resume(struct device *dev)
{
	int error;
	struct paj7620_data *data = dev_get_drvdata(dev);

	error = regmap_write(data->regmap, PAJ7620_REG_BANK_SEL, 0x01);
	if (error)
		return error;

	error = regmap_write(data->regmap, PAJ7620_REG_AUTO_STANDBY, 0x00);
	if (error)
		return error;

	error = regmap_write(data->regmap, PAJ7620_REG_BANK_SEL, 0x00);
	if (error)
		return error;

	usleep_range(1000, 2000);	// Stabilization delay (1ms minimum)
	return 0;
}

static const struct dev_pm_ops paj7620_pm_ops = {
	SET_RUNTIME_PM_OPS(paj7620_runtime_suspend, paj7620_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
};

static const struct regmap_config paj7620_reg_config = {
	.reg_bits = 8, .val_bits = 8, .max_register = 0xEF,
};

static void paj7620_disable_pm(void *dev)
{
	pm_runtime_disable(dev);
	pm_runtime_dont_use_autosuspend(dev);
}

static int paj7620_probe(struct i2c_client *client)
{
	struct paj7620_data *data;
	int error, i;

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	i2c_set_clientdata(client, data);

	data->supplies[0].supply = "vdd";
	data->supplies[1].supply = "vbus";
	data->supplies[2].supply = "vled";

	error = devm_regulator_bulk_get(&client->dev, ARRAY_SIZE(data->supplies), data->supplies);
	if (error)
		return dev_err_probe(&client->dev, error, "Failed to get regulators\n");

	error = device_property_read_u32_array(&client->dev, "linux,keycodes",
					       data->keymap, ARRAY_SIZE(data->keymap));

	if (error) {
		data->keymap[0] = KEY_UP;
		data->keymap[1] = KEY_DOWN;
		data->keymap[2] = KEY_LEFT;
		data->keymap[3] = KEY_RIGHT;
		data->keymap[4] = KEY_ENTER;
		data->keymap[5] = KEY_BACK;
		data->keymap[6] = KEY_NEXT;
		data->keymap[7] = KEY_PREVIOUS;
		data->keymap[8] = KEY_MENU;
	}

	data->regmap = devm_regmap_init_i2c(client, &paj7620_reg_config);
	if (IS_ERR(data->regmap))
		return PTR_ERR(data->regmap);

	data->idev = devm_input_allocate_device(&client->dev);
	if (!data->idev)
		return -ENOMEM;

	data->idev->name = "PAJ7620 Gesture Sensor";
	data->idev->id.bustype = BUS_I2C;
	data->idev->open = paj7620_input_open;
	data->idev->close = paj7620_input_close;
	data->idev->keycode = data->keymap;
	data->idev->keycodemax = ARRAY_SIZE(data->keymap);
	data->idev->keycodesize = sizeof(u32);

	for (i = 0; i < ARRAY_SIZE(data->keymap); i++)
		input_set_capability(data->idev, EV_KEY, data->keymap[i]);

	input_set_drvdata(data->idev, data);

	error = input_register_device(data->idev);
	if (error)
		return error;

	pm_runtime_set_active(&client->dev);
	pm_runtime_enable(&client->dev);
	pm_runtime_set_autosuspend_delay(&client->dev, 2000);
	pm_runtime_use_autosuspend(&client->dev);

	error = devm_add_action_or_reset(&client->dev, paj7620_disable_pm, &client->dev);
	if (error)
		return error;

	return devm_request_threaded_irq(&client->dev, client->irq,
									 NULL, paj7620_irq_thread,
									 IRQF_ONESHOT, "paj7620",
									 data);
}

static const struct of_device_id paj7620_of_match[] = {
	{ .compatible = "pixart,paj7620" },
	{ }
};
MODULE_DEVICE_TABLE(of, paj7620_of_match);

static struct i2c_driver paj7620_driver = {
	.driver = {
		.name = "paj7620",
		.of_match_table = paj7620_of_match,
		.pm = &paj7620_pm_ops,
	},
	.probe = paj7620_probe,
};
module_i2c_driver(paj7620_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Harpreet Saini");
MODULE_DESCRIPTION("PAJ7620 Gesture Input Driver");
