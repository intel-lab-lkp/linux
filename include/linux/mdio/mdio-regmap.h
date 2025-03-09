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

/* If a non empty valid_addr_mask is passed, PHY address and
 * read/write register are encoded in the regmap register
 * by placing the register in the first 16 bits and the PHY address
 * right after.
 */
#define MDIO_REGMAP_PHY_ADDR		GENMASK(20, 16)
#define MDIO_REGMAP_PHY_REG		GENMASK(15, 0)

struct mdio_regmap_config {
	struct device *parent;
	struct device_node *np;
	struct regmap *regmap;
	char name[MII_BUS_ID_SIZE];
	u8 valid_addr;
	u32 valid_addr_mask;
	bool autoscan;
};

struct mii_bus *devm_mdio_regmap_register(struct device *dev,
					  const struct mdio_regmap_config *config);

#endif
