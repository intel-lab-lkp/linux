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

#include <linux/mutex.h>
#include <linux/types.h>

/**
 * struct aaeon_mcu_dev - Internal representation of the Aaeon MCU
 * @dev: Pointer to kernel device structure
 * @i2c_lock: Mutex to serialize I2C bus access
 */

struct aaeon_mcu_dev {
	struct device *dev;
	struct mutex i2c_lock;
};

int aaeon_mcu_i2c_xfer(struct device *dev,
		       const u8 *cmd, int cmd_len,
		       u8 *rsp, int rsp_len);

#endif /*  __LINUX_MFD_AAEON_MCU_H */
