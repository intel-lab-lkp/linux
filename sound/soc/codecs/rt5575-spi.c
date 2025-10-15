// SPDX-License-Identifier: GPL-2.0-only
/*
 * rt5575-spi.c  --  ALC5575 SPI driver
 *
 * Copyright 2022 Realtek Semiconductor Corp.
 * Author: Oder Chiou <oder_chiou@realtek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/spi/spi.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_qos.h>
#include <linux/sysfs.h>
#include <linux/clk.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>
#include <sound/tlv.h>

#include "rt5575-spi.h"

static struct spi_device *rt5575_spi;

/**
 * rt5575_spi_burst_write - Write data to SPI by rt5575 address.
 * @addr: Start address.
 * @txbuf: Data Buffer for writng.
 * @len: Data length.
 *
 *
 * Returns true for success.
 */
int rt5575_spi_burst_write(u32 addr, const u8 *txbuf, size_t len)
{
	u8 spi_cmd = RT5575_SPI_CMD_BURST_WRITE;
	u8 *write_buf;
	unsigned int end, offset = 0;

	write_buf = kmalloc(RT5575_SPI_BUF_LEN + 6, GFP_KERNEL);

	while (offset < len) {
		if (offset + RT5575_SPI_BUF_LEN <= len)
			end = RT5575_SPI_BUF_LEN;
		else
			end = len % RT5575_SPI_BUF_LEN;

		write_buf[0] = spi_cmd;
		write_buf[1] = ((addr + offset) & 0x000000ff) >> 0;
		write_buf[2] = ((addr + offset) & 0x0000ff00) >> 8;
		write_buf[3] = ((addr + offset) & 0x00ff0000) >> 16;
		write_buf[4] = ((addr + offset) & 0xff000000) >> 24;

		memcpy(&write_buf[5], &txbuf[offset], end);

		if (end % 8)
			end = (end / 8 + 1) * 8;

		spi_write(rt5575_spi, write_buf, end + 6);

		offset += RT5575_SPI_BUF_LEN;
	}

	kfree(write_buf);

	return 0;
}
EXPORT_SYMBOL_GPL(rt5575_spi_burst_write);

static int rt5575_spi_probe(struct spi_device *spi)
{
	rt5575_spi = spi;

	return 0;
}

static const struct of_device_id rt5575_of_match[] = {
	{ .compatible = "realtek,rt5575", },
	{},
};
MODULE_DEVICE_TABLE(of, rt5575_of_match);

static struct spi_driver rt5575_spi_driver = {
	.driver = {
		.name = "rt5575",
		.of_match_table = of_match_ptr(rt5575_of_match),
	},
	.probe = rt5575_spi_probe,
};
module_spi_driver(rt5575_spi_driver);

MODULE_DESCRIPTION("ALC5575 SPI driver");
MODULE_AUTHOR("Oder Chiou <oder_chiou@realtek.com>");
MODULE_LICENSE("GPL");
