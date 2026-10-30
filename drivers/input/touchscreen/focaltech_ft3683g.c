// SPDX-License-Identifier: GPL-2.0-only
/*
 * FocalTech FT3683G Touchscreen Driver
 *
 * Copyright (c) 2012-2020 FocalTech Systems, Ltd.
 * Copyright (c) Alexander Koskovich <akoskovich@pm.me>
 */

#include <linux/cache.h>
#include <linux/crc-ccitt.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/string.h>

#define FT3683G_MAX_POINTS	10

#define FT3683G_SPI_CMD_READ	0xa0
#define FT3683G_SPI_DUMMY	3
#define FT3683G_SPI_RETRIES	3
#define FT3683G_SPI_CS_DELAY_US	150

#define FT3683G_REG_CHIP_ID	0xa3
#define FT3683G_REG_CHIP_ID2	0x9f

#define FT3683G_CHIP_IDH	0x56
#define FT3683G_CHIP_IDL	0x72

#define FT3683G_REG_TOUCH_DATA	0x01

#define FT3683G_TOUCH_LEN	8
#define FT3683G_TOUCH_HEADER	4
#define FT3683G_TOUCH_BUF_SIZE	(FT3683G_MAX_POINTS * FT3683G_TOUCH_LEN + \
				 FT3683G_TOUCH_HEADER)

#define FT3683G_TOUCH_OFF_XH	0
#define FT3683G_TOUCH_OFF_XL	1
#define FT3683G_TOUCH_OFF_YH	2
#define FT3683G_TOUCH_OFF_YL	3

#define FT3683G_EVENT_DOWN	0
#define FT3683G_EVENT_UP	1
#define FT3683G_EVENT_CONTACT	2

#define FT3683G_PROTOCOL_V2	0x02

#define FT3683G_FW_INIT		0xff

struct ft3683g_data {
	struct spi_device *spi;
	struct input_dev *input;
	struct touchscreen_properties props;
	struct regulator_bulk_data supplies[2];
	struct gpio_desc *reset_gpio;
	struct device *dev;
	struct mutex spi_lock; /* protects tx_buf/rx_buf */
	u8 tx_buf[256] ____cacheline_aligned;
	u8 rx_buf[256] ____cacheline_aligned;
};

static int ft3683g_spi_read(struct ft3683g_data *ts, u8 addr,
			    u8 *data, u32 len)
{
	struct spi_transfer xfer = { };
	u32 dp = 4 + FT3683G_SPI_DUMMY;
	u32 txlen = dp + len + 2;
	u16 crc_calc, crc_read;
	int error, i;

	guard(mutex)(&ts->spi_lock);

	memset(ts->tx_buf, 0, txlen);
	ts->tx_buf[0] = addr;
	ts->tx_buf[1] = FT3683G_SPI_CMD_READ;
	ts->tx_buf[2] = (len >> 8) & 0xff;
	ts->tx_buf[3] = len & 0xff;

	xfer.tx_buf = ts->tx_buf;
	xfer.rx_buf = ts->rx_buf;
	xfer.len = txlen;

	for (i = 0; i < FT3683G_SPI_RETRIES; i++) {
		error = spi_sync_transfer(ts->spi, &xfer, 1);
		if (error)
			continue;

		if (ts->rx_buf[3] & 0xa0) {
			fsleep(FT3683G_SPI_CS_DELAY_US);
			continue;
		}

		crc_calc = crc_ccitt(0xffff, &ts->rx_buf[dp], len);
		crc_read = ts->rx_buf[dp + len] |
			   (ts->rx_buf[dp + len + 1] << 8);
		if (crc_calc != crc_read) {
			fsleep(FT3683G_SPI_CS_DELAY_US);
			continue;
		}

		memcpy(data, &ts->rx_buf[dp], len);
		return 0;
	}

	return error ? error : -EIO;
}

static int ft3683g_read_reg(struct ft3683g_data *ts, u8 addr, u8 *val)
{
	return ft3683g_spi_read(ts, addr, val, 1);
}

static int ft3683g_check_chip_id(struct ft3683g_data *ts)
{
	u8 idh, idl;
	int error;

	error = ft3683g_read_reg(ts, FT3683G_REG_CHIP_ID, &idh);
	if (error)
		return error;

	error = ft3683g_read_reg(ts, FT3683G_REG_CHIP_ID2, &idl);
	if (error)
		return error;

	if (idh != FT3683G_CHIP_IDH || idl != FT3683G_CHIP_IDL) {
		dev_err(ts->dev,
			"chip ID mismatch: expected %02x%02x, got %02x%02x\n",
			FT3683G_CHIP_IDH, FT3683G_CHIP_IDL, idh, idl);
		return -ENODEV;
	}

	dev_dbg(ts->dev, "chip ID: %02x%02x\n", idh, idl);
	return 0;
}

static irqreturn_t ft3683g_irq(int irq, void *dev_id)
{
	struct ft3683g_data *ts = dev_id;
	u8 buf[FT3683G_TOUCH_BUF_SIZE];
	unsigned int x, y, id, event;
	int event_num, i, base;
	u8 protocol;
	int error;

	error = ft3683g_spi_read(ts, FT3683G_REG_TOUCH_DATA,
				 buf, sizeof(buf));
	if (error) {
		dev_err_ratelimited(ts->dev, "failed to read touch data: %d\n",
				    error);
		return IRQ_HANDLED;
	}

	if (!memchr_inv(&buf[1], FT3683G_FW_INIT, 4)) {
		input_mt_sync_frame(ts->input);
		input_sync(ts->input);
		return IRQ_HANDLED;
	}

	protocol = (buf[1] >> 4) & 0x0f;
	if (protocol != FT3683G_PROTOCOL_V2)
		return IRQ_HANDLED;

	event_num = buf[1] & 0x0f;
	if (!event_num || event_num > FT3683G_MAX_POINTS)
		return IRQ_HANDLED;

	for (i = 0; i < event_num; i++) {
		base = FT3683G_TOUCH_LEN * i + FT3683G_TOUCH_HEADER;

		event = buf[base + FT3683G_TOUCH_OFF_XH] >> 6;
		id = buf[base + FT3683G_TOUCH_OFF_YH] >> 4;
		if (id >= FT3683G_MAX_POINTS)
			continue;

		input_mt_slot(ts->input, id);

		if (event == FT3683G_EVENT_DOWN ||
		    event == FT3683G_EVENT_CONTACT) {
			x = ((buf[base + FT3683G_TOUCH_OFF_XH] & 0x0f) << 8) |
			    buf[base + FT3683G_TOUCH_OFF_XL];
			y = ((buf[base + FT3683G_TOUCH_OFF_YH] & 0x0f) << 8) |
			    buf[base + FT3683G_TOUCH_OFF_YL];

			input_mt_report_slot_state(ts->input,
						   MT_TOOL_FINGER, true);
			touchscreen_report_pos(ts->input, &ts->props,
					       x, y, true);
		} else {
			input_mt_report_slot_state(ts->input,
						   MT_TOOL_FINGER, false);
		}
	}

	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
	return IRQ_HANDLED;
}

static int ft3683g_power_on(struct ft3683g_data *ts)
{
	int error;

	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	usleep_range(2000, 3000);

	error = regulator_bulk_enable(ARRAY_SIZE(ts->supplies), ts->supplies);
	if (error) {
		dev_err(ts->dev, "failed to enable regulators: %d\n", error);
		return error;
	}

	usleep_range(2000, 3000);
	gpiod_set_value_cansleep(ts->reset_gpio, 0);
	msleep(50);

	return 0;
}

static void ft3683g_power_off(struct ft3683g_data *ts)
{
	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ts->supplies), ts->supplies);
}

static void ft3683g_power_off_act(void *data)
{
	struct ft3683g_data *ts = data;

	ft3683g_power_off(ts);
}

static int ft3683g_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct ft3683g_data *ts;
	int error;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->dev = dev;
	ts->spi = spi;
	spi_set_drvdata(spi, ts);
	mutex_init(&ts->spi_lock);

	ts->supplies[0].supply = "vddio";
	ts->supplies[1].supply = "vci";
	error = devm_regulator_bulk_get(dev, ARRAY_SIZE(ts->supplies),
					ts->supplies);
	if (error)
		return dev_err_probe(dev, error, "failed to get regulators\n");

	ts->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ts->reset_gpio),
				     "failed to request reset gpio\n");

	error = ft3683g_power_on(ts);
	if (error)
		return error;

	error = devm_add_action_or_reset(dev, ft3683g_power_off_act, ts);
	if (error)
		return error;

	error = ft3683g_check_chip_id(ts);
	if (error)
		return error;

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;

	ts->input->name = "FocalTech FT3683G Touchscreen";
	ts->input->id.bustype = BUS_SPI;
	input_set_drvdata(ts->input, ts);

	input_set_capability(ts->input, EV_ABS, ABS_MT_POSITION_X);
	input_set_capability(ts->input, EV_ABS, ABS_MT_POSITION_Y);
	touchscreen_parse_properties(ts->input, true, &ts->props);

	error = input_mt_init_slots(ts->input, FT3683G_MAX_POINTS,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error)
		return dev_err_probe(dev, error, "failed to init MT slots\n");

	error = devm_request_threaded_irq(dev, spi->irq, NULL, ft3683g_irq,
					  IRQF_ONESHOT, "ft3683g", ts);
	if (error)
		return dev_err_probe(dev, error, "failed to request IRQ\n");

	error = input_register_device(ts->input);
	if (error)
		return dev_err_probe(dev, error,
				     "failed to register input device\n");

	return 0;
}

static int ft3683g_suspend(struct device *dev)
{
	struct ft3683g_data *ts = dev_get_drvdata(dev);

	disable_irq(ts->spi->irq);

	input_mt_sync_frame(ts->input);
	input_sync(ts->input);

	ft3683g_power_off(ts);

	return 0;
}

static int ft3683g_resume(struct device *dev)
{
	struct ft3683g_data *ts = dev_get_drvdata(dev);
	int error;

	error = ft3683g_power_on(ts);
	if (error)
		return error;

	enable_irq(ts->spi->irq);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ft3683g_pm_ops, ft3683g_suspend, ft3683g_resume);

static const struct spi_device_id ft3683g_id[] = {
	{ "ft3683g" },
	{ }
};
MODULE_DEVICE_TABLE(spi, ft3683g_id);

static const struct of_device_id ft3683g_of_match[] = {
	{ .compatible = "focaltech,ft3683g", },
	{ }
};
MODULE_DEVICE_TABLE(of, ft3683g_of_match);

static struct spi_driver ft3683g_driver = {
	.driver = {
		.name = "focaltech-ft3683g",
		.of_match_table = ft3683g_of_match,
		.pm = pm_sleep_ptr(&ft3683g_pm_ops),
	},
	.probe = ft3683g_probe,
	.id_table = ft3683g_id,
};
module_spi_driver(ft3683g_driver);

MODULE_AUTHOR("Alexander Koskovich <akoskovich@pm.me>");
MODULE_DESCRIPTION("FocalTech FT3683G touchscreen driver");
MODULE_LICENSE("GPL");
