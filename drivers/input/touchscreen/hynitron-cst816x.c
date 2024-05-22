// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for I2C connected Hynitron CST816X Touchscreen
 *
 * Copyright (C) 2024 Oleh Kuzhylnyi <kuzhylol@gmail.com>
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/timer.h>

#define CST816X_MAX_X 240
#define CST816X_MAX_Y CST816X_MAX_X

#define CST816X_EVENT_TIMEOUT_MS 10

enum cst816x_registers {
	CST816X_FRAME = 0x01,
	CST816X_MOTION = 0xEC,
};

enum cst816_gesture_code {
	CST816X_SWIPE = 0x00,
	CST816X_SWIPE_UP = 0x01,
	CST816X_SWIPE_DOWN = 0x02,
	CST816X_SWIPE_LEFT = 0x03,
	CST816X_SWIPE_RIGHT = 0x04,
	CST816X_SINGLE_TAP = 0x05,
	CST816X_DOUBLE_TAP = 0x0B,
	CST816X_LONG_PRESS = 0x0C,
};

struct cst816x_info {
	u8 gesture;
	u8 x;
	u8 y;
};

struct cst816x_priv {
	struct device *dev;
	struct i2c_client *client;
	struct gpio_desc *reset;
	struct input_dev *input;
	struct timer_list timer;
	struct delayed_work dw;
	struct cst816x_info info;

	u8 rxtx[8];
};

struct cst816x_gesture_mapping {
	enum cst816_gesture_code gesture_code;
	size_t event_code;
};

static const struct cst816x_gesture_mapping cst816x_gesture_map[] = {
	{CST816X_SWIPE, KEY_UNKNOWN},
	{CST816X_SWIPE_UP, KEY_UP},
	{CST816X_SWIPE_DOWN, KEY_DOWN},
	{CST816X_SWIPE_LEFT, KEY_LEFT},
	{CST816X_SWIPE_RIGHT, KEY_RIGHT},
	{CST816X_SINGLE_TAP, BTN_TOUCH},
	{CST816X_DOUBLE_TAP, BTN_TOOL_DOUBLETAP},
	{CST816X_LONG_PRESS, BTN_TOOL_TRIPLETAP}
};

static int cst816x_i2c_write_reg(struct cst816x_priv *priv, u8 reg, u8 cmd)
{
	struct i2c_client *client;
	struct i2c_msg xfer;
	int rc;

	client = priv->client;

	priv->rxtx[0] = reg;
	priv->rxtx[1] = cmd;

	xfer.addr = client->addr;
	xfer.flags = 0;
	xfer.len = 2;
	xfer.buf = priv->rxtx;

	rc = i2c_transfer(client->adapter, &xfer, 1);
	if (rc != 1) {
		if (rc >= 0)
			rc = -EIO;
	} else {
		rc = 0;
	}

	if (rc < 0)
		dev_err(&client->dev, "i2c tx err: %d\n", rc);

	return rc;
}

static int cst816x_i2c_read_reg(struct cst816x_priv *priv, u8 reg)
{
	struct i2c_client *client;
	struct i2c_msg xfer[2];
	int rc;

	client = priv->client;

	xfer[0].addr = client->addr;
	xfer[0].flags = 0;
	xfer[0].len = sizeof(reg);
	xfer[0].buf = &reg;

	xfer[1].addr = client->addr;
	xfer[1].flags = I2C_M_RD;
	xfer[1].len = sizeof(priv->rxtx);
	xfer[1].buf = priv->rxtx;

	rc = i2c_transfer(client->adapter, xfer, ARRAY_SIZE(xfer));
	if (rc != ARRAY_SIZE(xfer)) {
		if (rc >= 0)
			rc = -EIO;
	} else {
		rc = 0;
	}

	if (rc < 0)
		dev_err(&client->dev, "i2c rx err: %d\n", rc);

	return rc;
}

static int cst816x_setup_regs(struct cst816x_priv *priv)
{
	return cst816x_i2c_write_reg(priv, CST816X_MOTION, CST816X_DOUBLE_TAP);
}

static void report_gesture_event(const struct cst816x_priv *priv,
				 enum cst816_gesture_code gesture_code,
				 bool state)
{
	const struct cst816x_gesture_mapping *lookup = NULL;

	for (u8 i = CST816X_SWIPE_UP; i < ARRAY_SIZE(cst816x_gesture_map); i++) {
		if (cst816x_gesture_map[i].gesture_code == gesture_code) {
			lookup = &cst816x_gesture_map[i];
			break;
		}
	}

	if (lookup)
		input_report_key(priv->input, lookup->event_code, state);
}

static int cst816x_process_touch(struct cst816x_priv *priv)
{
	u8 *raw;
	int rc;

	rc = cst816x_i2c_read_reg(priv, CST816X_FRAME);
	if (!rc) {
		raw = priv->rxtx;

		priv->info.gesture = raw[0];
		priv->info.x = ((raw[2] & 0x0F) << 8) | raw[3];
		priv->info.y = ((raw[4] & 0x0F) << 8) | raw[5];

		dev_dbg(priv->dev, "x: %d, y: %d, gesture: 0x%x\n",
			priv->info.x, priv->info.y, priv->info.gesture);
	}

	return rc;
}

static int cst816x_register_input(struct cst816x_priv *priv)
{
	priv->input = devm_input_allocate_device(priv->dev);
	if (!priv->input)
		return -ENOMEM;

	priv->input->name = "Hynitron CST816X Touchscreen";
	priv->input->phys = "input/ts";
	priv->input->id.bustype = BUS_I2C;
	input_set_drvdata(priv->input, priv);

	for (u8 i = CST816X_SWIPE_UP; i < ARRAY_SIZE(cst816x_gesture_map); i++) {
		input_set_capability(priv->input, EV_KEY,
				     cst816x_gesture_map[i].event_code);
	}

	input_set_abs_params(priv->input, ABS_X, 0, CST816X_MAX_X, 0, 0);
	input_set_abs_params(priv->input, ABS_Y, 0, CST816X_MAX_Y, 0, 0);
	input_set_capability(priv->input, EV_ABS, ABS_X);
	input_set_capability(priv->input, EV_ABS, ABS_Y);

	return input_register_device(priv->input);
}

static void cst816x_reset(struct cst816x_priv *priv)
{
	gpiod_set_value_cansleep(priv->reset, 0);
	msleep(100);
	gpiod_set_value_cansleep(priv->reset, 1);
	msleep(100);
}

static void cst816x_timer_cb(struct timer_list *timer)
{
	struct cst816x_priv *priv = from_timer(priv, timer, timer);

	report_gesture_event(priv, priv->info.gesture, false);
	input_sync(priv->input);
}

static void cst816x_dw_cb(struct work_struct *work)
{
	struct cst816x_priv *priv =
		container_of(work, struct cst816x_priv, dw.work);

	if (!cst816x_process_touch(priv)) {
		input_report_abs(priv->input, ABS_X, priv->info.x);
		input_report_abs(priv->input, ABS_Y, priv->info.y);
		report_gesture_event(priv, priv->info.gesture, true);
		input_sync(priv->input);

		mod_timer(&priv->timer,
			  jiffies + msecs_to_jiffies(CST816X_EVENT_TIMEOUT_MS));
	}
}

static irqreturn_t cst816x_irq_cb(int irq, void *cookie)
{
	struct cst816x_priv *priv = (struct cst816x_priv *)cookie;

	schedule_delayed_work(&priv->dw, 0);

	return IRQ_HANDLED;
}

static int cst816x_probe(struct i2c_client *client)
{
	struct cst816x_priv *priv;
	struct device *dev = &client->dev;
	int rc;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->client = client;

	INIT_DELAYED_WORK(&priv->dw, cst816x_dw_cb);
	timer_setup(&priv->timer, cst816x_timer_cb, 0);

	priv->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset))
		return dev_err_probe(dev, PTR_ERR(priv->reset),
				     "reset gpio not found\n");

	if (priv->reset)
		cst816x_reset(priv);

	rc = cst816x_setup_regs(priv);
	if (rc)
		return dev_err_probe(dev, rc, "regs setup failed\n");

	client->irq = of_irq_get(dev->of_node, 0);
	if (client->irq <= 0)
		return dev_err_probe(dev, client->irq, "irq lookup failed\n");

	rc = devm_request_threaded_irq(dev, client->irq, NULL, cst816x_irq_cb,
				       IRQF_ONESHOT, dev->driver->name, priv);
	if (rc)
		return dev_err_probe(dev, client->irq, "irq request failed\n");

	return cst816x_register_input(priv);
}

static const struct i2c_device_id cst816x_id[] = {
	{ .name = "cst816s", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cst816x_id);

static const struct of_device_id cst816x_of_match[] = {
	{ .compatible = "hynitron,cst816s", },
	{ }
};
MODULE_DEVICE_TABLE(of, cst816x_of_match);

static struct i2c_driver cst816x_driver = {
	.driver = {
		.name = "cst816x",
		.of_match_table = cst816x_of_match,
	},
	.id_table = cst816x_id,
	.probe = cst816x_probe,
};

module_i2c_driver(cst816x_driver);

MODULE_AUTHOR("Oleh Kuzhylnyi <kuzhylol@gmail.com>");
MODULE_DESCRIPTION("Hynitron CST816X Touchscreen Driver");
MODULE_LICENSE("GPL");
