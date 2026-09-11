// SPDX-License-Identifier: GPL-2.0-only
/*
 * ESWIN EPH8621 series touchscreen driver
 *
 * Copyright (C) 2026 Luca Weiss <luca.weiss@fairphone.com>
 *
 * Based on original EPH861X driver by chris.ollerenshaw@eswin.com
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>
#include <linux/spi/spi.h>

#define ESWIN_MAX_TOUCHES		10
#define ESWIN_COMMS_BUF_SIZE		3072

/* TLV Protocol Definitions */
#define TLV_HEADER_SIZE			3 /* 1 byte Type + 2 bytes Length */
#define TLV_TYPE_FIELD			0
#define TLV_LENGTH_FIELD		1

#define TLV_REPORT_DATA			0x23
#define EVENT_REPORT_TYPE_OFFSET	4
#define EVENT_REPORT_TYPE_MASK		0xF0
#define EVENT_REPORT_LENGTH_MASK	0x0F

/* Event Types */
#define CONTACT_TYPE			1
#define RELEASE_TYPE			2

struct eswin_touch {
	struct device *dev;
	struct input_dev *input;

	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[2];

	struct touchscreen_properties prop;

	u8 *rx_buf;
	u8 *tx_buf;
};

static int eswin_spi_read(struct eswin_touch *ts, u16 len, u8 *buf)
{
	struct spi_device *spi = to_spi_device(ts->dev);
	struct spi_transfer xfer = {
		.tx_buf = ts->tx_buf,
		.rx_buf = ts->rx_buf,
		.len = len,
	};
	int error;

	if (len > ESWIN_COMMS_BUF_SIZE)
		return -EINVAL;

	/* ESWIN requires 0xFF dummy bytes clocked out to read data */
	memset(ts->tx_buf, 0xFF, len);
	error = spi_sync_transfer(spi, &xfer, 1);
	if (error)
		return error;

	memcpy(buf, ts->rx_buf, len);
	return 0;
}

/*
 * Core Touch Handling
 */
static int eswin_comms_two_stage_read(struct eswin_touch *ts, u8 *buf)
{
	u16 payload_len;
	int error;

	/* Stage 1: Read 3-byte header */
	error = eswin_spi_read(ts, TLV_HEADER_SIZE, buf);
	if (error)
		return error;

	payload_len = buf[TLV_LENGTH_FIELD] | (buf[TLV_LENGTH_FIELD + 1] << 8);
	if (payload_len == 0 || payload_len > ESWIN_COMMS_BUF_SIZE - TLV_HEADER_SIZE)
		return -EINVAL;

	udelay(50);

	/* Stage 2: Read full packet (Header + Payload) */
	return eswin_spi_read(ts, payload_len + TLV_HEADER_SIZE, buf);
}

static void eswin_report_contact(struct eswin_touch *ts, u8 *payload)
{
	u8 touch_type = (payload[0] & EVENT_REPORT_TYPE_MASK) >> EVENT_REPORT_TYPE_OFFSET;
	u8 slot = payload[1];
	u16 x = payload[2] | (payload[3] << 8);
	u16 y = payload[4] | (payload[5] << 8);
	u8 width = payload[6];
	u8 height = payload[7];
	bool active = false;

	switch (touch_type) {
	case CONTACT_TYPE:
		active = true;
		break;
	case RELEASE_TYPE:
		active = false;
		break;
	default:
		dev_warn_ratelimited(ts->dev, "Unhandled touch type: %d\n", touch_type);
		return;
	}

	input_mt_slot(ts->input, slot);
	input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, active);

	if (active) {
		touchscreen_report_pos(ts->input, &ts->prop, x, y, true);
		input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, height);
		input_report_abs(ts->input, ABS_MT_TOUCH_MINOR, width);
	}
}

static void eswin_process_report(struct eswin_touch *ts, u8 *buf)
{
	u16 total_len = buf[TLV_LENGTH_FIELD] | (buf[TLV_LENGTH_FIELD + 1] << 8);
	u16 offset = TLV_HEADER_SIZE;

	if (buf[TLV_TYPE_FIELD] != TLV_REPORT_DATA)
		return;

	while (offset < total_len + TLV_HEADER_SIZE) {
		u8 ev_len = buf[offset] & EVENT_REPORT_LENGTH_MASK;

		eswin_report_contact(ts, &buf[offset]);
		offset += (ev_len + 1);
	}

	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

static irqreturn_t eswin_interrupt(int irq, void *dev_id)
{
	struct eswin_touch *ts = dev_id;
	int error;

	error = eswin_comms_two_stage_read(ts, ts->rx_buf);
	if (!error)
		eswin_process_report(ts, ts->rx_buf);

	return IRQ_HANDLED;
}

static int eswin_power_on(struct eswin_touch *ts)
{
	int error;

	error = regulator_bulk_enable(ARRAY_SIZE(ts->supplies), ts->supplies);
	if (error)
		return error;

	msleep(150);
	gpiod_set_value_cansleep(ts->reset_gpio, 0);
	msleep(100);

	return 0;
}

static void eswin_power_off(struct eswin_touch *ts)
{
	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ts->supplies), ts->supplies);
}

static void eswin_power_off_act(void *data)
{
	struct eswin_touch *ts = data;

	eswin_power_off(ts);
}

static int eswin_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct eswin_touch *ts;
	int error;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->dev = dev;
	spi_set_drvdata(spi, ts);

	spi->mode = SPI_MODE_3;
	spi->bits_per_word = 8;
	error = spi_setup(spi);
	if (error)
		return dev_err_probe(dev, error, "Failed to setup SPI\n");

	ts->rx_buf = devm_kzalloc(dev, ESWIN_COMMS_BUF_SIZE, GFP_KERNEL);
	ts->tx_buf = devm_kzalloc(dev, ESWIN_COMMS_BUF_SIZE, GFP_KERNEL);
	if (!ts->rx_buf || !ts->tx_buf)
		return -ENOMEM;

	ts->supplies[0].supply = "vddio";
	ts->supplies[1].supply = "avdd";
	error = devm_regulator_bulk_get(dev, ARRAY_SIZE(ts->supplies), ts->supplies);
	if (error)
		return dev_err_probe(dev, error, "Failed to get regulators\n");

	ts->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ts->reset_gpio),
				     "Failed to request reset GPIO\n");

	error = eswin_power_on(ts);
	if (error)
		return dev_err_probe(dev, error, "Failed to power on\n");

	error = devm_add_action_or_reset(dev, eswin_power_off_act, ts);
	if (error)
		return error;

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;

	ts->input->name = "ESWIN Touchscreen";
	ts->input->id.bustype = BUS_SPI;

	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, SZ_64K - 1, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, SZ_64K - 1, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);

	touchscreen_parse_properties(ts->input, true, &ts->prop);

	error = input_mt_init_slots(ts->input, ESWIN_MAX_TOUCHES, INPUT_MT_DIRECT);
	if (error)
		return dev_err_probe(dev, error, "Failed to initialize MT slots\n");

	error = devm_request_threaded_irq(dev, spi->irq, NULL, eswin_interrupt,
					  IRQF_ONESHOT, dev_name(dev), ts);
	if (error)
		return dev_err_probe(dev, error, "Failed to request IRQ\n");

	return input_register_device(ts->input);
}

static int eswin_suspend(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct eswin_touch *ts = spi_get_drvdata(spi);

	disable_irq(spi->irq);
	eswin_power_off(ts);

	return 0;
}

static int eswin_resume(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct eswin_touch *ts = spi_get_drvdata(spi);
	int error;

	error = eswin_power_on(ts);
	if (error)
		return error;

	enable_irq(spi->irq);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(eswin_pm_ops, eswin_suspend, eswin_resume);

static const struct of_device_id eswin_of_match[] = {
	{ .compatible = "eswin,eph8621" },
	{ }
};
MODULE_DEVICE_TABLE(of, eswin_of_match);

static const struct spi_device_id eswin_spi_id[] = {
	{ .name = "eph8621" },
	{ }
};
MODULE_DEVICE_TABLE(spi, eswin_spi_id);

static struct spi_driver eswin_spi_driver = {
	.driver = {
		.name = "eswin_eph8621",
		.of_match_table = eswin_of_match,
		.pm = pm_sleep_ptr(&eswin_pm_ops),
	},
	.probe = eswin_probe,
	.id_table = eswin_spi_id,
};
module_spi_driver(eswin_spi_driver);

MODULE_AUTHOR("Luca Weiss <luca.weiss@fairphone.com>");
MODULE_DESCRIPTION("ESWIN EPH8621 SPI touchscreen driver");
MODULE_LICENSE("GPL");
