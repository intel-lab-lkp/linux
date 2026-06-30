/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Aaeon MCU driver definitions
 *
 * Copyright (C) 2026 Bootlin
 * Author: Jérémie Dautheribes <jeremie.dautheribes@bootlin.com>
 * Author: Thomas Perrot <thomas.perrot@bootlin.com>
 */

#ifndef __LINUX_MFD_AAEON_MCU_H
#define __LINUX_MFD_AAEON_MCU_H

/*
 * MCU register address: the high byte is the command opcode, the low
 * byte is the argument.  This matches the 3-byte wire format
 * [opcode, arg, value] used by the MCU I2C protocol.
 */
#define AAEON_MCU_REG(op, arg)		(((op) << 8) | (arg))

/*
 * Opcode for GPIO input reads. These registers are volatile, their values
 * are driven by external signals and can change without CPU involvement.
 * Used by the MFD driver's volatile_reg callback to bypass the regmap cache.
 */
#define AAEON_MCU_READ_GPIO_OPCODE	0x72

/*
 * Opcode for watchdog control and status commands.
 * The status register (arg=0x02) reflects hardware state and is volatile.
 */
#define AAEON_MCU_CONTROL_WDT_OPCODE	0x63

/*
 * Highest register address in the MCU register map.
 * The WRITE_GPIO opcode (0x77) with the highest GPIO argument (0x0B = 11,
 * i.e. MAX_GPIOS - 1) produces the largest encoded address.
 */
#define AAEON_MCU_MAX_REGISTER		AAEON_MCU_REG(0x77, 0x0B)

#endif /* __LINUX_MFD_AAEON_MCU_H */
