// SPDX-License-Identifier: GPL-2.0-only
/*
 * rt5575-spi.c  --  ALC5575 SPI driver
 *
 * Copyright(c) 2025 Realtek Semiconductor Corp.
 *
 */

#include <linux/firmware.h>
#include <linux/spi/spi.h>

#include "rt5575-spi.h"

#define RT5575_SPI_CMD_BURST_WRITE	5
#define RT5575_SPI_BUF_LEN		240

struct rt5575_spi_burst_write {
	u8 cmd;
	u32 addr;
	u8 data[RT5575_SPI_BUF_LEN];
	u8 dummy;
} __packed;

static int rt5575_spi_device_match(struct device *dev, const void *data)
{
	struct spi_device *spi = container_of(dev, struct spi_device, dev);

	if (!strcmp("rt5575", spi->modalias))
		return 1;

	return 0;
}

struct spi_device *rt5575_spi_find_device(void)
{
	struct device *dev;

	dev = bus_find_device(&spi_bus_type, NULL, NULL, rt5575_spi_device_match);
	if (dev)
		return container_of(dev, struct spi_device, dev);
	else
		return NULL;
}

/**
 * rt5575_spi_burst_write - Write data to SPI by rt5575 address.
 * @spi: SPI device.
 * @addr: Start address.
 * @txbuf: Data buffer for writing.
 * @len: Data length.
 *
 */
static int rt5575_spi_burst_write(struct spi_device *spi, u32 addr, const u8 *txbuf,
						size_t len)
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
		spi_write(spi, &buf, sizeof(buf));

		offset += RT5575_SPI_BUF_LEN;
	}

	return 0;
}

int rt5575_spi_fw_load(struct spi_device *spi)
{
	const struct firmware *firmware;
	struct device *dev = &spi->dev;
	static const char * const fw_path[] = {
		"realtek/rt5575/rt5575_fw1.bin",
		"realtek/rt5575/rt5575_fw2.bin",
		"realtek/rt5575/rt5575_fw3.bin",
		"realtek/rt5575/rt5575_fw4.bin"
	};
	static const u32 fw_addr[] = { 0x5f400000, 0x5f600000, 0x5f7fe000, 0x5f7ff000 };
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(fw_addr); i++) {
		ret = request_firmware(&firmware, fw_path[i], dev);
		if (!ret) {
			rt5575_spi_burst_write(spi, fw_addr[i], firmware->data, firmware->size);
			release_firmware(firmware);
		} else {
			dev_err(dev, "Request firmware failure: %d\n", ret);
			return ret;
		}
	}

	return 0;
}
