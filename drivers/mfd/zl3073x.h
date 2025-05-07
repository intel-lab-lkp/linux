/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ZL3073X_CORE_H
#define __ZL3073X_CORE_H

struct device;
struct regmap_config;
struct zl3073x_dev;

enum zl3073x_chip_type {
	ZL30731,
	ZL30732,
	ZL30733,
	ZL30734,
	ZL30735,
};

struct zl3073x_chip_info {
	const u16	*ids;
	size_t		num_ids;
	int		num_channels;
};

extern const struct zl3073x_chip_info zl3073x_chip_info[];

struct zl3073x_dev *zl3073x_devm_alloc(struct device *dev);
void zl3073x_dev_init_regmap_config(struct regmap_config *regmap_cfg);
int zl3073x_dev_probe(struct zl3073x_dev *zldev,
		      const struct zl3073x_chip_info *chip_info);

#endif /* __ZL3073X_CORE_H */
