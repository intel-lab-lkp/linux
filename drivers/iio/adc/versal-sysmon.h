/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AMD Versal SysMon driver
 *
 * Copyright (C) 2019 - 2022, Xilinx, Inc.
 * Copyright (C) 2022 - 2026, Advanced Micro Devices, Inc.
 */

#ifndef _VERSAL_SYSMON_H_
#define _VERSAL_SYSMON_H_

#include <linux/bits.h>
#include <linux/mutex.h>

struct device;
struct regmap;

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

/**
 * struct sysmon - Driver data for Versal SysMon
 * @regmap: register map for hardware access
 * @lock: protects read-modify-write sequences on threshold registers
 *        and cached state that spans multiple regmap calls
 */
struct sysmon {
	struct regmap *regmap;
	/*
	 * Protects read-modify-write sequences on threshold registers
	 * and cached state (oversampling ratios, hysteresis values)
	 * that spans multiple regmap calls.
	 */
	struct mutex lock;
};

int devm_versal_sysmon_core_probe(struct device *dev, struct regmap *regmap);

#endif /* _VERSAL_SYSMON_H_ */
