/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * drivers/watchdog/at91sam9_wdt.h
 *
 * Copyright (C) 2007 Andrew Victor
 * Copyright (C) 2007 Atmel Corporation.
 * Copyright (C) 2019 Microchip Technology Inc. and its subsidiaries
 *
 * Watchdog Timer (WDT) - System peripherals regsters.
 * Based on AT91SAM9261 datasheet revision D.
 * Based on SAM9X60 datasheet.
 * Based on SAMA7G5 datasheet.
 * Based on SAM9X75 datasheet.
 *
 */

#ifndef AT91_WDT_H
#define AT91_WDT_H

#include <linux/bits.h>

#define AT91_WDT_CR		0x00		/* Watchdog Control Register */
#define  AT91_WDT_WDRSTT	BIT(0)		/* Restart */
#define  AT91_WDT_KEY		(0xa5UL << 24)	/* KEY Password */
#define AT91_WDT_MR		0x04		/* Watchdog Mode Register */
#define  AT91_WDT_WDV		(0xfffUL << 0)	/* Counter Value */
#define  AT91_WDT_WDFIEN	BIT(12)		/* Fault Interrupt Enable */
#define  AT91_WDT_WDRSTEN	BIT(13)
#define  AT91_WDT_WDRPROC	BIT(14)
#define  AT91_WDT_WDD		(0xfffUL << 16)
#define  AT91_WDT_WDDBGHLT	BIT(28)
#define  AT91_WDT_WDIDLEHLT	BIT(29)
#define AT91_WDT_SR		0x08		/* Watchdog Status Register */
#define  AT91_WDT_WDUNF		BIT(0)		/* Watchdog Underflow */
#define  AT91_WDT_WDERR		BIT(1)		/* Watchdog Error */

#define  AT91_WDT_SET_WDV(x)	((x) & AT91_WDT_WDV)
#define  AT91_WDT_SET_WDD(x)	(((x) << 16) & AT91_WDT_WDD)

#define AT91_WDT_VR		0x08	/* Watchdog Timer Value Register */
#define AT91_WDT_ISR		0x1c	/* Interrupt Status Register */
#define AT91_WDT_IER		0x14	/* Interrupt Enable Register */
#define AT91_WDT_IDR		0x18	/* Interrupt Disable Register */
#define AT91_WDT_WLR		0x0c	/* Watchdog Window Level Register */
#define AT91_WDT_PERIODRST	BIT(4)	/* Period Reset */
#define AT91_WDT_RPTHRST	BIT(5)		/* Minimum Restart Period */
#define  AT91_WDT_PERINT	BIT(0)	/* Period Interrupt Enable */
#define  AT91_WDT_COUNTER	(0xfffUL << 0)	/* Watchdog Period Value */
#define  AT91_WDT_SET_COUNTER(x)	((x) & AT91_WDT_COUNTER)

/*
 * WDDIS bit differs by SoC:
 *   - SAMA5, AT91SAM9261: bit 15
 *   - SAM9X60, SAMA7G5, SAM9X75: bit 12
 * Select at runtime via compatible string.
 */
#define AT91_WDT_WDDIS_LEGACY   BIT(15)
#define AT91_WDT_WDDIS_MODERN   BIT(12)

#endif
