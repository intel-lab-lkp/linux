/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __LINUX_MFD_ZL3073X_H
#define __LINUX_MFD_ZL3073X_H

#include <linux/device.h>
#include <linux/regmap.h>

struct zl3073x_dev {
	struct device		*dev;
	struct regmap		*regmap;
	struct mutex		lock;
};

/**
 * zl3073x_lock - Lock the device
 * @zldev: device structure pointer
 *
 * Caller has to take this lock when it needs to access device registers.
 */
static inline void zl3073x_lock(struct zl3073x_dev *zldev)
{
	mutex_lock(&zldev->lock);
}

/**
 * zl3073x_unlock - Unlock the device
 * @zldev: device structure pointer
 *
 * Caller unlocks the device when it does not need to access device
 * registers anymore.
 */
static inline void zl3073x_unlock(struct zl3073x_dev *zldev)
{
	mutex_unlock(&zldev->lock);
}

DEFINE_GUARD(zl3073x, struct zl3073x_dev *, zl3073x_lock(_T),
	     zl3073x_unlock(_T));

int zl3073x_read_reg(struct zl3073x_dev *zldev, unsigned int reg,
		     unsigned int len, void *value);

int zl3073x_write_reg(struct zl3073x_dev *zldev, unsigned int reg,
		      unsigned int len, const void *value);

#endif /* __LINUX_MFD_ZL3073X_H */
