/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * i2c-machxo2.h: Platform data for Lattice MachXO2 I2C controller
 *
 * Copyright (c) 2024 TQ-Systems GmbH <linux@ew.tq-group.com>, D-82229 Seefeld, Germany.
 * Author: Matthias Schiffer
 */

#ifndef _LINUX_I2C_MACHXO2_H
#define _LINUX_I2C_MACHXO2_H

struct machxo2_i2c_platform_data {
	u32 clock_khz; /* input clock in kHz */
	u32 bus_khz; /* I2C bus clock in kHz */
};

#endif /* _LINUX_I2C_MACHXO2_H */
