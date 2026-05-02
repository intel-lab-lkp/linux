/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AMD Versal SysMon driver
 *
 * Copyright (C) 2019 - 2022, Xilinx, Inc.
 * Copyright (C) 2022 - 2026, Advanced Micro Devices, Inc.
 */

#ifndef _VERSAL_SYSMON_H_
#define _VERSAL_SYSMON_H_

#include <linux/iio/iio.h>
#include <linux/mutex.h>
#include <linux/regmap.h>

/* Register offsets (sorted by address) */
#define SYSMON_NPI_LOCK			0x000C
#define SYSMON_ISR			0x0044
#define SYSMON_IDR			0x0050
#define SYSMON_TEMP_MAX			0x1030
#define SYSMON_TEMP_MIN			0x1034
#define SYSMON_SUPPLY_BASE		0x1040
#define SYSMON_TEMP_MIN_MIN		0x1F8C
#define SYSMON_TEMP_MAX_MAX		0x1F90
#define SYSMON_TEMP_SAT_BASE		0x1FAC
#define SYSMON_MAX_REG			0x24C0

/* NPI unlock value written to SYSMON_NPI_LOCK */
#define SYSMON_NPI_UNLOCK_CODE		0xF9E8D7C6

/* Register stride: 4 bytes per 32-bit register */
#define SYSMON_REG_STRIDE		4

#define SYSMON_SUPPLY_IDX_MAX		159
#define SYSMON_TEMP_SAT_MAX		64
#define SYSMON_INTR_ALL_MASK		GENMASK(31, 0)

/* Supply voltage conversion register fields */
#define SYSMON_MANTISSA_MASK		GENMASK(15, 0)
#define SYSMON_FMT_MASK			BIT(16)
#define SYSMON_MODE_MASK		GENMASK(18, 17)

/* Q8.7 fractional shift */
#define SYSMON_FRACTIONAL_SHIFT		7U
#define SYSMON_SUPPLY_MANTISSA_BITS	16

/* Signed milli scale (MILLI from linux/units.h is unsigned long) */
#define SYSMON_MILLI			1000

/**
 * struct sysmon - Driver data for Versal SysMon
 * @dev: pointer to device struct
 * @indio_dev: pointer to the iio device (needed for work callbacks)
 * @regmap: register map for hardware access
 * @lock: mutex for serializing user-space access
 * @irq: interrupt number
 */
struct sysmon {
	struct device *dev;
	struct iio_dev *indio_dev;
	struct regmap *regmap;
	/* Serializes access to device registers and state */
	struct mutex lock;
	int irq;
};

int sysmon_core_probe(struct device *dev, struct regmap *regmap, int irq);

#endif /* _VERSAL_SYSMON_H_ */
