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

#endif /* __LINUX_MFD_ZL3073X_H */
