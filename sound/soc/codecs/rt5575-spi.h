/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rt5575-spi.h  --  ALC5575 SPI driver
 *
 * Copyright 2022 Realtek Semiconductor Corp.
 * Author: Oder Chiou <oder_chiou@realtek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __RT5575_SPI_H__
#define __RT5575_SPI_H__

#define RT5575_SPI_BUF_LEN		240

/* SPI Command */
enum {
	RT5575_SPI_CMD_16_READ = 0,
	RT5575_SPI_CMD_16_WRITE,
	RT5575_SPI_CMD_32_READ,
	RT5575_SPI_CMD_32_WRITE,
	RT5575_SPI_CMD_BURST_READ,
	RT5575_SPI_CMD_BURST_WRITE,
};

#if IS_ENABLED(CONFIG_SND_SOC_RT5575_SPI)
int rt5575_spi_burst_write(u32 addr, const u8 *txbuf, size_t len);
#else
static inline int rt5575_spi_burst_write(u32 addr, const u8 *txbuf, size_t len)
{
	return -EINVAL;
}
#endif

#endif /* __RT5575_SPI_H__ */
