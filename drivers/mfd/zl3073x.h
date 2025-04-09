/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ZL3073X_CORE_H
#define __ZL3073X_CORE_H

struct device;
struct regmap_config;
struct zl3073x_dev;

struct zl3073x_dev *zl3073x_devm_alloc(struct device *dev);
int zl3073x_dev_init(struct zl3073x_dev *zldev);
const struct regmap_config *zl3073x_get_regmap_config(void);

#endif /* __ZL3073X_CORE_H */
