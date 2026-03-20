/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _ZL3073X_HWMON_H
#define _ZL3073X_HWMON_H

#include <linux/kconfig.h>

struct zl3073x_dev;

#if IS_REACHABLE(CONFIG_HWMON)
int zl3073x_hwmon_init(struct zl3073x_dev *zldev);
#else
static inline int zl3073x_hwmon_init(struct zl3073x_dev *zldev) { return 0; }
#endif

#endif /* _ZL3073X_HWMON_H */
