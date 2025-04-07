/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ZL3073X_CORE_H
#define __ZL3073X_CORE_H

#include <linux/mfd/zl3073x.h>

struct zl3073x_dev *zl3073x_dev_alloc(struct device *dev);
int zl3073x_dev_init(struct zl3073x_dev *zldev, u8 dev_id);
void zl3073x_dev_exit(struct zl3073x_dev *zldev);
const struct regmap_config *zl3073x_get_regmap_config(void);

#endif /* __ZL3073X_CORE_H */
