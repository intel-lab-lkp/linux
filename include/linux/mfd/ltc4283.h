/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Analog Devices LTC4283 I2C Negative Voltage Hot Swap Controller
 *
 * Copyright 2025 Analog Devices Inc.
 */

#ifndef __MFD_LTC4283_H_
#define __MFD_LTC4283_H_

#include <linux/bitops.h>
#include <linux/bits.h>

/*
 * We can have up to 8 gpios. 4 PGIOs and 4 ADIOs. PGIOs start at index 4 in the
 * gpios mask.
 */
#define LTC4283_PGIOX_START_NR	4

#define LTC4283_PGIO_CONFIG		0x10
#define   LTC4283_PGIO_CFG_MASK(pin) \
	GENMASK(((pin) - LTC4283_PGIOX_START_NR) * 2 + 1, (((pin) - LTC4283_PGIOX_START_NR) * 2))
#define LTC4283_PGIO_CONFIG_2		0x11
#define   LTC4283_ADC_MASK		GENMASK(2, 0)
#define   LTC4283_PGIO_OUT_MASK(pin)	BIT(4 + (pin))

#define LTC4283_GPIO_MAX	8

/* Non-constant mask variant of FIELD_GET() and FIELD_PREP() */
#define field_get(_mask, _reg)	(((_reg) & (_mask)) >> (ffs(_mask) - 1))
#define field_prep(_mask, _val)	(((_val) << (ffs(_mask) - 1)) & (_mask))

#endif
