// SPDX-License-Identifier: GPL-2.0-only
/*
 * Elantech eKTP1059 SPI Touchpad
 * Copyright (C) 2025 Andreas Kemnade <andreas@kemnade.info>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/spi/spi.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/kthread.h>

#define TOUCHPAD_WIDTH		4000
#define TOUCHPAD_HEIGHT		2426
#define TOUCH_AREA		20
#define PRESSURE_MAX 256

struct elan_tp_spi {
	struct input_dev *input_touch;
	struct spi_device *spi;
};

static int elan_spi_write(struct elan_tp_spi *elanspi, const void *buf, size_t len)
{
	/*
	 * running this as single transfer with word_delay set
	 * results in an irq storm. Epson vendor kernel uses a single spi_sync
	 * multiple 1 byte transfers.
	 */
	size_t i;
	int err;

	for (i = 0 ; i < len; i++) {
		err = spi_write(elanspi->spi, buf, 1);
		if (err)
			return err;

		udelay(100);
	}
	return 0;
}

static int elan_spi_read(struct elan_tp_spi *elanspi, void *buf, size_t len)
{
	/* reads 0x51 on sync */
	struct spi_transfer t = { 0 };
	int err;
	size_t i;

	for (i = 0; i < len; i++) {
		u8 dummy = 0xff;

		t.len = 1;
		t.tx_buf = &dummy;
		t.rx_buf = buf + i;
		err = spi_sync_transfer(elanspi->spi, &t, 1);
		if (err)
			return err;

		udelay(80);
	}
	return 0;
}

static irqreturn_t elan_tp_irq_handler(int irq, void *dev_id)
{
	struct elan_tp_spi *elanspi = dev_id;
	u8 buf[14];
	int fingercnt = 0;
	int x, y, pres, width;

	if (elan_spi_read(elanspi, buf, 14))
		return IRQ_HANDLED;

	if (buf[13] != 0x1)
		return IRQ_HANDLED;

	fingercnt = (buf[1] & 0xC0) >> 6;
	input_report_key(elanspi->input_touch, BTN_TOUCH, fingercnt != 0);
	input_report_key(elanspi->input_touch, BTN_TOOL_FINGER, fingercnt == 1);
	input_report_key(elanspi->input_touch, BTN_TOOL_DOUBLETAP, fingercnt == 2);
	input_report_key(elanspi->input_touch, BTN_TOOL_TRIPLETAP, fingercnt == 3);

	x = buf[2] & 0xf;
	x = x << 8;
	x |= buf[3];
	y = buf[5] & 0xf;
	y = y << 8;
	y |= buf[6];

	pres = (buf[2] & 0xf0) | ((buf[5] & 0xf0) >> 4);
	width = ((buf[1] & 0x30) >> 2) | ((buf[4] & 0x30) >> 4);

	input_report_abs(elanspi->input_touch, ABS_PRESSURE, pres);
	input_report_abs(elanspi->input_touch, ABS_TOOL_WIDTH, width);

	if (fingercnt != 0) {
		input_report_abs(elanspi->input_touch, ABS_X, x);
		input_report_abs(elanspi->input_touch, ABS_Y, y);
	}

	input_mt_slot(elanspi->input_touch, 0);
	input_mt_report_slot_state(elanspi->input_touch, MT_TOOL_FINGER, fingercnt == 1);

	if (fingercnt != 0) {
		input_report_abs(elanspi->input_touch, ABS_MT_POSITION_X, x);
		input_report_abs(elanspi->input_touch, ABS_MT_POSITION_Y, y);
	}
	dev_dbg(&elanspi->spi->dev, "1: X: %d Y: %d pres: %d width: %d\n",
		x, y, pres, width);

	if (fingercnt >= 2) {
		x = buf[8] & 0xf;
		x = x << 8;
		x |= buf[9];
		y = buf[11] & 0xf;
		y = y << 8;
		y |= buf[12];
		input_mt_slot(elanspi->input_touch, 1);
		input_mt_report_slot_state(elanspi->input_touch, MT_TOOL_FINGER, 1);
		input_report_abs(elanspi->input_touch, ABS_MT_POSITION_X, x);
		input_report_abs(elanspi->input_touch, ABS_MT_POSITION_Y, y);
		dev_dbg(&elanspi->spi->dev, "2: X: %d Y: %d\n", x, y);
	} else {
		input_mt_slot(elanspi->input_touch, 1);
		input_mt_report_slot_state(elanspi->input_touch, MT_TOOL_FINGER, 0);
	}

	input_sync(elanspi->input_touch);

	return IRQ_HANDLED;
}

static int handle_hello_package(struct elan_tp_spi *elanspi)
{
	u8 buf_recv[4];
	int rc;

	rc = elan_spi_read(elanspi, buf_recv, 4);
	if (rc != 0)
		return rc;

	/* 0xa0, 0x7, 0x0, 0x0 after boot */
	dev_dbg(&elanspi->spi->dev,
		"dump hello packet: %x, %x, %x, %x\n",
		buf_recv[0], buf_recv[1], buf_recv[2], buf_recv[3]);

	return 0;
}

static int init_touchpad(struct elan_tp_spi *elanspi)
{
	u8 buf_cmd[4] = {0x5B, 0x10, 0xC, 0x1};
	u8 buf[14];
	int ret;

	ret = elan_spi_write(elanspi, buf_cmd, 4);

	if (ret != 0)
		return ret;

	msleep(20);
	elan_spi_read(elanspi, buf, 14);

	return 0;
}

static int elan_ektp1059_probe(struct spi_device *spi)
{
	int status = 0;
	struct elan_tp_spi *elanspi;
	struct input_dev *input_touch;

	spi->bits_per_word = 8;
	status = spi_setup(spi);

	elanspi = devm_kzalloc(&spi->dev,
			       sizeof(struct elan_tp_spi), GFP_KERNEL);
	if (!elanspi)
		return -ENOMEM;

	input_touch = devm_input_allocate_device(&spi->dev);
	if (!input_touch)
		return dev_err_probe(&spi->dev, PTR_ERR(input_touch),
				     "create input touch device failed\n");

	elanspi->input_touch = input_touch;

	elanspi->spi = spi;
	spi_set_drvdata(spi, elanspi);

	input_touch->name = "elan-touchpad";
	input_set_abs_params(input_touch, ABS_MT_POSITION_X, 0, TOUCHPAD_WIDTH, 0, 0);
	input_set_abs_params(input_touch, ABS_MT_POSITION_Y, 0, TOUCHPAD_HEIGHT, 0, 0);
	input_set_abs_params(input_touch, ABS_MT_PRESSURE, 0, PRESSURE_MAX, 0, 0);
	input_set_abs_params(input_touch, ABS_TOOL_WIDTH, 0, TOUCH_AREA, 0, 0);
	input_mt_init_slots(input_touch, 3, INPUT_MT_POINTER | INPUT_MT_SEMI_MT);
	input_set_drvdata(input_touch, elanspi);

	status = input_register_device(input_touch);
	if (status < 0)
		return dev_err_probe(&elanspi->spi->dev, status, "input_register_device failed\n");

	status = handle_hello_package(elanspi);
	if (status < 0)
		return dev_err_probe(&elanspi->spi->dev, status, "handle hello package failed\n");

	status = init_touchpad(elanspi);
	if (status < 0)
		return dev_err_probe(&spi->dev, status, "init touchpad failed!\n");

	status = devm_request_threaded_irq(&spi->dev, spi->irq, NULL,
					   elan_tp_irq_handler, IRQF_ONESHOT,
					   spi->dev.driver->name, elanspi);
	if (status < 0)
		return dev_err_probe(&spi->dev, status, "request_irq failed\n");

	return 0;
}

static int elan_ektp1059_suspend(struct device *dev)
{
	disable_irq(to_spi_device(dev)->irq);
	return 0;
}

static int elan_ektp1059_resume(struct device *dev)
{
	enable_irq(to_spi_device(dev)->irq);
	return 0;
}

static const struct spi_device_id elan_ektp1059_id[] = {
	{ "ektp1059", 0 },
	{},
};
MODULE_DEVICE_TABLE(spi, elan_ektp1059_id);

static const struct of_device_id elan_ektp1059_of_spi_match[] = {
	{ .compatible = "elan,ektp1059" },
	{ },
};
MODULE_DEVICE_TABLE(of, elan_ektp1059_of_spi_match);

static SIMPLE_DEV_PM_OPS(elan_ektp1059_pm, elan_ektp1059_suspend, elan_ektp1059_resume);

static struct spi_driver elan_ektp1059_driver = {
	.driver	= {
		.name	 = "elan_ektp1059",
		.of_match_table = elan_ektp1059_of_spi_match,
		.pm = pm_ptr(&elan_ektp1059_pm),
	},
	.id_table = elan_ektp1059_id,
	.probe	= elan_ektp1059_probe,
};

module_spi_driver(elan_ektp1059_driver);

MODULE_DESCRIPTION("Elan eKTP1059 SPI touch pad");
MODULE_LICENSE("GPL");
