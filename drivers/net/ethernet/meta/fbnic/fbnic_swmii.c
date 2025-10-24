// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include <linux/mdio.h>

#include "fbnic.h"
#include "fbnic_netdev.h"

static int
fbnic_swmii_read_pmapmd(struct fbnic_dev *fbd, int regnum)
{
	u16 ctrl1 = 0, ctrl2 = 0;
	struct fbnic_net *fbn;
	int ret = 0;
	u8 aui;

	if (fbd->netdev) {
		fbn = netdev_priv(fbd->netdev);
		aui = fbn->aui;
	}

	switch (aui) {
	case FBNIC_AUI_25GAUI:
		ctrl1 = MDIO_CTRL1_SPEED25G;
		ctrl2 = MDIO_PMA_CTRL2_25GBCR;
		break;
	case FBNIC_AUI_LAUI2:
		ctrl1 = MDIO_CTRL1_SPEED50G;
		ctrl2 = MDIO_PMA_CTRL2_50GBCR2;
		break;
	case FBNIC_AUI_50GAUI1:
		ctrl1 = MDIO_CTRL1_SPEED50G;
		ctrl2 = MDIO_PMA_CTRL2_50GBCR;
		break;
	case FBNIC_AUI_100GAUI2:
		ctrl1 = MDIO_CTRL1_SPEED100G;
		ctrl2 = MDIO_PMA_CTRL2_100GBCR2;
		break;
	default:
		break;
	}

	switch (regnum) {
	case MDIO_CTRL1:
		ret = ctrl1;
		break;
	case MDIO_STAT1:
		ret = fbd->pmd_state == FBNIC_PMD_SEND_DATA ?
		      MDIO_STAT1_LSTATUS : 0;
		break;
	case MDIO_DEVS1:
		ret = MDIO_DEVS_PMAPMD;
		break;
	case MDIO_CTRL2:
		ret = ctrl2;
		break;
	case MDIO_STAT2:
		ret = MDIO_STAT2_DEVPRST_VAL |
		      MDIO_PMA_STAT2_EXTABLE;
		break;
	case MDIO_PMA_EXTABLE:
		ret = MDIO_PMA_EXTABLE_40_100G |
		      MDIO_PMA_EXTABLE_25G;
		break;
	case MDIO_PMA_40G_EXTABLE:
		ret = MDIO_PMA_40G_EXTABLE_50GBCR2;
		break;
	case MDIO_PMA_25G_EXTABLE:
		ret = MDIO_PMA_25G_EXTABLE_25GBCR;
		break;
	case MDIO_PMA_50G_EXTABLE:
		ret = MDIO_PMA_50G_EXTABLE_50GBCR;
		break;
	case MDIO_PMA_EXTABLE2:
		ret = MDIO_PMA_EXTABLE2_50G;
		break;
	case MDIO_PMA_100G_EXTABLE:
		ret = MDIO_PMA_100G_EXTABLE_100GBCR2;
		break;
	default:
		break;
	}

	return ret;
}

static int
fbnic_swmii_read_c45(struct mii_bus *bus, int addr, int devnum, int regnum)
{
	struct fbnic_dev *fbd = bus->priv;

	if (addr != 0)
		return 0xffff;

	if (devnum == MDIO_MMD_PMAPMD)
		return fbnic_swmii_read_pmapmd(fbd, regnum);

	return 0xffff;
}

static int
fbnic_swmii_write_c45(struct mii_bus *bus, int addr, int devnum,
		      int regnum, u16 val)
{
	/* Currently PHY setup is meant to be read-only */
	return 0;
}

/**
 * fbnic_swmii_create - Create a swmii to allow interfacing phydev w/ FW PHY
 * @fbd: Pointer to FBNIC device structure to populate bus on
 *
 * Initialize an MII bus and place a pointer to it on the fbd struct. This bus
 * will be used to interface with the PMA/PMD for now, and may add support for
 * the PCS in the future.
 *
 * Return: 0 on success, negative on failure
 **/
int fbnic_swmii_create(struct fbnic_dev *fbd)
{
	struct mii_bus *bus;
	int err;

	bus = devm_mdiobus_alloc(fbd->dev);
	if (!bus)
		return -ENOMEM;

	bus->name = "fbnic_mii_bus";
	bus->read_c45 = &fbnic_swmii_read_c45;
	bus->write_c45 = &fbnic_swmii_write_c45;
	bus->parent = fbd->dev;
	bus->phy_mask = GENMASK(31, 1);
	bus->priv = fbd;
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s-mii", dev_name(fbd->dev));

	err = devm_mdiobus_register(fbd->dev, bus);
	if (err) {
		dev_err(fbd->dev, "Failed to create MDIO bus: %d\n", err);
		return err;
	}

	fbd->mii_bus = bus;

	return 0;
}
