/* SPDX-License-Identifier: GPL-2.0 */
/* Driver for MMIO-Mapped MDIO devices. Some IPs expose internal PHYs or PCS
 * within the MMIO-mapped area
 *
 * Copyright (C) 2023 Maxime Chevallier <maxime.chevallier@bootlin.com>
 */
#ifndef MDIO_REGMAP_H
#define MDIO_REGMAP_H

#include <linux/phy.h>

struct device;
struct regmap;

struct mdio_regmap_config {
	struct device *parent;
	struct regmap *regmap;
	char name[MII_BUS_ID_SIZE];
	u8 valid_addr;
	/* devm_mdio_regmap_init is required with this enabled */
	bool support_encoded_addr;
	bool autoscan;
};

struct mii_bus *devm_mdio_regmap_register(struct device *dev,
					  const struct mdio_regmap_config *config);

struct mdio_regmap_init_config {
	const char *name;

	int (*mdio_read)(void *ctx, int addr, int regnum);
	int (*mdio_write)(void *ctx, int addr, int regnum, u16 val);
	int (*mdio_read_c45)(void *ctx, int addr, int devnum, int regnum);
	int (*mdio_write_c45)(void *ctx, int addr, int devnum, int regnum, u16 val);
};

struct regmap *devm_mdio_regmap_init(struct device *dev, void *priv,
				     const struct mdio_regmap_init_config *config);

#endif
