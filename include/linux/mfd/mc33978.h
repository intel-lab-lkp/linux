/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024 David Jander <david@protonic.nl>, Protonic Holland
 * Copyright (C) 2026 Oleksij Rempel <kernel@pengutronix.de>, Pengutronix
 *
 * MC34978/MC33978 Multiple Switch Detection Interface - Shared Definitions
 */

#ifndef _LINUX_MFD_MC33978_H
#define _LINUX_MFD_MC33978_H

#include <linux/bits.h>

/* Register Map - All addresses are base command bytes (R/W bit = 0) */
#define MC33978_REG_CHECK	0x00	/* SPI communication check */
#define MC33978_REG_CONFIG	0x02	/* Device configuration */
#define MC33978_REG_TRI_SP	0x04	/* Tri-state enable SP */
#define MC33978_REG_TRI_SG	0x06	/* Tri-state enable SG */
#define MC33978_REG_WET_SP	0x08	/* Wetting current level SP */
#define MC33978_REG_WET_SG0	0x0a	/* Wetting current level SG0 (SG7-SG0) */
#define MC33978_REG_WET_SG1	0x0c	/* Wetting current level SG1 (SG13-SG8) */
#define MC33978_REG_CWET_SP	0x16	/* Continuous wetting current SP */
#define MC33978_REG_CWET_SG	0x18	/* Continuous wetting current SG */
#define MC33978_REG_IE_SP	0x1a	/* Interrupt enable SP */
#define MC33978_REG_IE_SG	0x1c	/* Interrupt enable SG */
#define MC33978_REG_LPM_CONFIG	0x1e	/* Low-power mode configuration */
#define MC33978_REG_WAKE_SP	0x20	/* Wake-up enable SP */
#define MC33978_REG_WAKE_SG	0x22	/* Wake-up enable SG */
#define MC33978_REG_COMP_SP	0x24	/* Comparator only mode SP */
#define MC33978_REG_COMP_SG	0x26	/* Comparator only mode SG */
#define MC33978_REG_LPM_VT_SP	0x28	/* LPM voltage threshold SP */
#define MC33978_REG_LPM_VT_SG	0x2a	/* LPM voltage threshold SG */
#define MC33978_REG_IP_SP	0x2c	/* Polling current SP */
#define MC33978_REG_IP_SG	0x2e	/* Polling current SG */
#define MC33978_REG_SPOLL_SP	0x30	/* Slow polling SP */
#define MC33978_REG_SPOLL_SG	0x32	/* Slow polling SG */
#define MC33978_REG_WDEB_SP	0x34	/* Wake-up debounce SP */
#define MC33978_REG_WDEB_SG	0x36	/* Wake-up debounce SG */
#define MC33978_REG_ENTER_LPM	0x38	/* Enter low-power mode (write-only) */
#define MC33978_REG_AMUX_CTRL	0x3a	/* AMUX control */
#define MC33978_REG_READ_IN	0x3e	/* Read switch status (READ_SW in datasheet) */
#define MC33978_REG_FAULT	0x42	/* Fault status register */
#define MC33978_REG_IRQ		0x46	/* Interrupt request (write-only) */
#define MC33978_REG_RESET	0x48	/* Reset (write-only) */

/*
 * FAULT Register (0x42) bit definitions
 * Reading this register clears most fault flags except persistent conditions
 */
#define MC33978_FAULT_SPI_ERROR	BIT(10)	/* SPI communication error */
#define MC33978_FAULT_HASH	BIT(9)	/* SPI register hash mismatch */
#define MC33978_FAULT_UV	BIT(7)	/* VBATP undervoltage */
#define MC33978_FAULT_OV	BIT(6)	/* VBATP overvoltage */
#define MC33978_FAULT_TEMP_WARN	BIT(5)	/* Temperature warning threshold */
#define MC33978_FAULT_OT	BIT(4)	/* Over-temperature */
#define MC33978_FAULT_INTB_WAKE	BIT(3)	/* Woken by INT_B pin */
#define MC33978_FAULT_WAKEB_WAKE BIT(2)	/* Woken by WAKE_B pin */
#define MC33978_FAULT_SPI_WAKE	BIT(1)	/* Woken by SPI message */
#define MC33978_FAULT_POR	BIT(0)	/* Power-on reset occurred */

/* Critical faults that need immediate attention */
#define MC33978_FAULT_CRITICAL	(MC33978_FAULT_UV | \
				 MC33978_FAULT_OV | \
				 MC33978_FAULT_OT)

/* Bits relevant as hwmon alarms; excludes wake/reset/SPI status bits */
#define MC33978_FAULT_ALARM_MASK	(MC33978_FAULT_UV | \
					 MC33978_FAULT_OV | \
					 MC33978_FAULT_TEMP_WARN | \
					 MC33978_FAULT_OT)

#define MC33978_NUM_PINS	22

/*
 * Virtual IRQ number for fault handling.
 * Using hwirq 22 (beyond the 22 pin IRQs 0-21).
 */
#define MC33978_HWIRQ_FAULT	22

/*
 * AMUX channel definitions
 * The AMUX can route one of 24 signals to the external AMUX pin
 */
#define MC33978_AMUX_CH_SG0	0	/* Switch-to-Ground inputs 0-13 */
#define MC33978_AMUX_CH_SG13	13
#define MC33978_AMUX_CH_SP0	14	/* Programmable switch inputs 0-7 */
#define MC33978_AMUX_CH_SP7	21
#define MC33978_AMUX_CH_TEMP	22	/* Internal temperature diode */
#define MC33978_AMUX_CH_VBATP	23	/* Battery voltage sense */
#define MC33978_NUM_AMUX_CH	24	/* Total number of AMUX channels */

#endif /* _LINUX_MFD_MC33978_H */
