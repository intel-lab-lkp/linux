// SPDX-License-Identifier: GPL-2.0+
/* Copyright (c) Tehuti Networks Ltd. */

#include "tn40.h"

static u32 bdx_mdio_get(struct bdx_priv *priv)
{
	void __iomem *regs = priv->regs;

#define BDX_MAX_MDIO_BUSY_LOOPS 1024
	int tries = 0;

	while (++tries < BDX_MAX_MDIO_BUSY_LOOPS) {
		u32 mdio_cmd_stat = readl(regs + REG_MDIO_CMD_STAT);

		if (GET_MDIO_BUSY(mdio_cmd_stat) == 0)
			return mdio_cmd_stat;
	}
	dev_err(&priv->pdev->dev, "MDIO busy!\n");
	return 0xFFFFFFFF;
}

static u16 bdx_mdio_read(struct bdx_priv *priv, int device, int port, u16 addr)
{
	void __iomem *regs = priv->regs;
	u32 tmp_reg, i;
	/* wait until MDIO is not busy */
	if (bdx_mdio_get(priv) == 0xFFFFFFFF)
		return -1;

	i = ((device & 0x1F) | ((port & 0x1F) << 5));
	writel(i, regs + REG_MDIO_CMD);
	writel((u32)addr, regs + REG_MDIO_ADDR);
	tmp_reg = bdx_mdio_get(priv);
	if (tmp_reg == 0xFFFFFFFF)
		return -1;

	writel(((1 << 15) | i), regs + REG_MDIO_CMD);
	/* read CMD_STAT until not busy */
	tmp_reg = bdx_mdio_get(priv);
	if (tmp_reg == 0xFFFFFFFF)
		return -1;

	if (GET_MDIO_RD_ERR(tmp_reg)) {
		dev_dbg(&priv->pdev->dev, "MDIO error after read command\n");
		return -1;
	}
	tmp_reg = readl(regs + REG_MDIO_DATA);

	return (tmp_reg & 0xFFFF);
}

static int bdx_mdio_write(struct bdx_priv *priv, int device, int port, u16 addr,
			  u16 data)
{
	void __iomem *regs = priv->regs;
	u32 tmp_reg;

	/* wait until MDIO is not busy */
	if (bdx_mdio_get(priv) == 0xFFFFFFFF)
		return -1;
	writel(((device & 0x1F) | ((port & 0x1F) << 5)), regs + REG_MDIO_CMD);
	writel((u32)addr, regs + REG_MDIO_ADDR);
	if (bdx_mdio_get(priv) == 0xFFFFFFFF)
		return -1;
	writel((u32)data, regs + REG_MDIO_DATA);
	/* read CMD_STAT until not busy */
	tmp_reg = bdx_mdio_get(priv);
	if (tmp_reg == 0xFFFFFFFF)
		return -1;

	if (GET_MDIO_RD_ERR(tmp_reg)) {
		dev_err(&priv->pdev->dev, "MDIO error after write command\n");
		return -1;
	}
	return 0;
}

static void bdx_mdio_set_speed(struct bdx_priv *priv, u32 speed)
{
	void __iomem *regs = priv->regs;
	int mdio_cfg;

	mdio_cfg = readl(regs + REG_MDIO_CMD_STAT);
	if (speed == 1)
		mdio_cfg = (0x7d << 7) | 0x08;	/* 1MHz */
	else
		mdio_cfg = 0xA08;	/* 6MHz */
	mdio_cfg |= (1 << 6);
	writel(mdio_cfg, regs + REG_MDIO_CMD_STAT);
	msleep(100);
}

static int mdio_read_reg(struct mii_bus *mii_bus, int addr, int devnum, int regnum)
{
	return bdx_mdio_read(mii_bus->priv, devnum, addr, regnum);
}

static int mdio_write_reg(struct mii_bus *mii_bus, int addr, int devnum, int regnum, u16 val)
{
	return bdx_mdio_write(mii_bus->priv, devnum, addr, regnum, val);
}

int bdx_mdiobus_init(struct bdx_priv *priv)
{
	struct pci_dev *pdev = priv->pdev;
	struct mii_bus *bus;
	struct phy_device *phydev;
	int ret;

	bus = devm_mdiobus_alloc(&pdev->dev);
	if (!bus)
		return -ENOMEM;

	bus->name = BDX_DRV_NAME;
	bus->parent = &pdev->dev;
	snprintf(bus->id, MII_BUS_ID_SIZE, "tn40xx-%x-%x",
		 pci_domain_nr(pdev->bus), pci_dev_id(pdev));
	bus->priv = priv;

	bus->read_c45 = mdio_read_reg;
	bus->write_c45 = mdio_write_reg;

	ret = devm_mdiobus_register(&pdev->dev, bus);
	if (ret) {
		dev_err(&pdev->dev, "failed to register mdiobus %d %u %u\n",
			ret, bus->state, MDIOBUS_UNREGISTERED);
		return ret;
	}

	phydev = phy_find_first(bus);
	if (!phydev) {
		dev_err(&pdev->dev, "failed to find phy\n");
		return -1;
	}
	phydev->irq = PHY_MAC_INTERRUPT;
	priv->mdio = bus;
	priv->phydev = phydev;
	bdx_mdio_set_speed(priv, MDIO_SPEED_6MHZ);
	return 0;
}
