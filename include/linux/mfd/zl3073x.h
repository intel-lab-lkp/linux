/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __LINUX_MFD_ZL3073X_H
#define __LINUX_MFD_ZL3073X_H

#include <linux/mutex.h>

struct device;
struct regmap;

/**
 * struct zl3073x_dev - zl3073x device
 * @dev: pointer to device
 * @regmap: regmap to access device registers
 */
struct zl3073x_dev {
	struct device		*dev;
	struct regmap		*regmap;
};

#endif /* __LINUX_MFD_ZL3073X_H */
