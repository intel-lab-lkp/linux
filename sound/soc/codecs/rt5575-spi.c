// SPDX-License-Identifier: GPL-2.0-only
/*
 * rt5575-spi.c  --  ALC5575 SPI driver
 *
 * Copyright(c) 2025 Realtek Semiconductor Corp.
 *
 */

#include <linux/of.h>
#include <linux/spi/spi.h>

#include "rt5575-spi.h"

#define RT5575_SPI_BUF_LEN	240

/* SPI Command */
enum {
	RT5575_SPI_CMD_16_READ = 0,
	RT5575_SPI_CMD_16_WRITE,
	RT5575_SPI_CMD_32_READ,
	RT5575_SPI_CMD_32_WRITE,
	RT5575_SPI_CMD_BURST_READ,
	RT5575_SPI_CMD_BURST_WRITE,
};

struct rt5575_spi_burst_write {
	u8 cmd;
	u32 addr;
	u8 data[RT5575_SPI_BUF_LEN];
	u8 dummy;
} __packed;

bool rt5575_spi_ready;
static struct spi_device *rt5575_spi;

/**
 * rt5575_spi_burst_write - Write data to SPI by rt5575 address.
 * @addr: Start address.
 * @txbuf: Data buffer for writng.
 * @len: Data length.
 *
 */
int rt5575_spi_burst_write(u32 addr, const u8 *txbuf, size_t len)
{
	struct rt5575_spi_burst_write buf = {
		.cmd = RT5575_SPI_CMD_BURST_WRITE
	};
	unsigned int end, offset = 0;

	while (offset < len) {
		if (offset + RT5575_SPI_BUF_LEN <= len)
			end = RT5575_SPI_BUF_LEN;
		else
			end = len % RT5575_SPI_BUF_LEN;

		buf.addr = cpu_to_le32(addr + offset);

		memcpy(&buf.data, &txbuf[offset], end);

		spi_write(rt5575_spi, &buf, sizeof(struct rt5575_spi_burst_write));

		offset += RT5575_SPI_BUF_LEN;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(rt5575_spi_burst_write);

static int rt5575_spi_probe(struct spi_device *spi)
{
	rt5575_spi = spi;

	rt5575_spi_ready = true;

	return 0;
}

static const struct of_device_id rt5575_of_match[] = {
	{ .compatible = "realtek,rt5575" },
	{ }
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
