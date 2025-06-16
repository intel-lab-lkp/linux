/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _ZL3073X_H
#define _ZL3073X_H

#include <linux/types.h>

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
extern const struct regmap_config zl3073x_regmap_config;

struct zl3073x_dev *zl3073x_devm_alloc(struct device *dev);
int zl3073x_dev_probe(struct zl3073x_dev *zldev,
		      const struct zl3073x_chip_info *chip_info);

/**********************
 * Registers operations
 **********************/

int zl3073x_poll_zero_u8(struct zl3073x_dev *zldev, unsigned int reg, u8 mask);
int zl3073x_read_u8(struct zl3073x_dev *zldev, unsigned int reg, u8 *val);
int zl3073x_read_u16(struct zl3073x_dev *zldev, unsigned int reg, u16 *val);
int zl3073x_read_u32(struct zl3073x_dev *zldev, unsigned int reg, u32 *val);
int zl3073x_read_u48(struct zl3073x_dev *zldev, unsigned int reg, u64 *val);
int zl3073x_write_u8(struct zl3073x_dev *zldev, unsigned int reg, u8 val);
int zl3073x_write_u16(struct zl3073x_dev *zldev, unsigned int reg, u16 val);
int zl3073x_write_u32(struct zl3073x_dev *zldev, unsigned int reg, u32 val);
int zl3073x_write_u48(struct zl3073x_dev *zldev, unsigned int reg, u64 val);

#endif /* _ZL3073X_H */
