/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rt5575-spi.h  --  ALC5575 SPI driver
 *
 * Copyright(c) 2025 Realtek Semiconductor Corp.
 *
 */

#ifndef __RT5575_SPI_H__
#define __RT5575_SPI_H__

extern bool rt5575_spi_ready;

int rt5575_spi_burst_write(u32 addr, const u8 *txbuf, size_t len);

#endif /* __RT5575_SPI_H__ */
