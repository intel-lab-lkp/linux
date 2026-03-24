/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Aaeon MCU driver definitions
 *
 * Copyright (C) 2025 Bootlin
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
#define AAEON_MCU_REG(op, arg)	(((op) << 8) | (arg))

#endif /* __LINUX_MFD_AAEON_MCU_H */
