// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for MaxLinear MxL862xx switch family
 *
 * Copyright (C) 2024 MaxLinear Inc.
 * Copyright (C) 2025 John Crispin <john@phrozen.org>
 * Copyright (C) 2025 Daniel Golle <daniel@makrotopia.org>
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/of_device.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/phylink.h>
#include <net/dsa.h>

#include "mxl862xx.h"
#include "mxl862xx-api.h"
#include "mxl862xx-cmd.h"
#include "mxl862xx-host.h"

#define MXL862XX_API_WRITE(dev, cmd, data) \
	mxl862xx_api_wrap(dev, cmd, &(data), sizeof((data)), false)
#define MXL862XX_API_READ(dev, cmd, data) \
	mxl862xx_api_wrap(dev, cmd, &(data), sizeof((data)), true)

#define DSA_MXL_PORT(port) ((port) + 1)

#define MXL862XX_SDMA_PCTRLP(p) (0xBC0 + ((p) * 0x6))
#define MXL862XX_SDMA_PCTRL_EN BIT(0)

#define MXL862XX_FDMA_PCTRLP(p) (0xA80 + ((p) * 0x6))
#define MXL862XX_FDMA_PCTRL_EN BIT(0)

/* PHY access via firmware relay */
static int mxl862xx_phy_read_mmd(struct mxl862xx_priv *priv, int port,
				 int devadd, int reg)
{
	struct mdio_relay_data param = {
		.phy = port,
		.mmd = devadd,
		.reg = reg & 0xffff,
	};
	int ret;

	ret = MXL862XX_API_READ(priv, INT_GPHY_READ, param);
	if (ret)
		return ret;

	return param.data;
}

static int mxl862xx_phy_write_mmd(struct mxl862xx_priv *priv, int port,
				  int devadd, int reg, u16 data)
{
	struct mdio_relay_data param = {
		.phy = port,
		.mmd = devadd,
		.reg = reg,
		.data = data,
	};

	return MXL862XX_API_WRITE(priv, INT_GPHY_WRITE, param);
}

static int mxl862xx_phy_read(struct dsa_switch *ds, int port, int reg)
{
	return mxl862xx_phy_read_mmd(ds->priv, port, 0, reg);
}

static int mxl862xx_phy_write(struct dsa_switch *ds, int port, int reg, u16 data)
{
	return mxl862xx_phy_write_mmd(ds->priv, port, 0, reg, data);
}

/* Configure CPU tagging */
static int mxl862xx_configure_tag_proto(struct dsa_switch *ds, int port,
					bool enable)
{
	struct mxl862xx_ss_sp_tag tag = {
		.pid = DSA_MXL_PORT(port),
		.mask = BIT(0) | BIT(1),
		.rx = enable ? 2 : 1,
		.tx = enable ? 2 : 3,
	};
	struct mxl862xx_ctp_port_assignment assign = {
		.number_of_ctp_port = enable ? (32 - DSA_MXL_PORT(port)) : 1,
		.logical_port_id = DSA_MXL_PORT(port),
		.first_ctp_port_id = DSA_MXL_PORT(port),
		.mode = MXL862XX_LOGICAL_PORT_GPON,
	};
	int ret;

	ret = MXL862XX_API_WRITE(ds->priv, MXL862XX_SS_SPTAG_SET, tag);
	if (ret)
		return ret;

	return MXL862XX_API_WRITE(ds->priv, MXL862XX_CTP_PORTASSIGNMENTSET, assign);
}

/* Port enable/disable */
static int mxl862xx_port_state(struct dsa_switch *ds, int port, bool enable)
{
	struct mxl862xx_register_mod sdma = {
		.addr = MXL862XX_SDMA_PCTRLP(DSA_MXL_PORT(port)),
		.data = enable ? MXL862XX_SDMA_PCTRL_EN : 0,
		.mask = MXL862XX_SDMA_PCTRL_EN,
	};
	struct mxl862xx_register_mod fdma = {
		.addr = MXL862XX_FDMA_PCTRLP(DSA_MXL_PORT(port)),
		.data = enable ? MXL862XX_FDMA_PCTRL_EN : 0,
		.mask = MXL862XX_FDMA_PCTRL_EN,
	};
	int ret;

	if (!dsa_is_user_port(ds, port))
		return 0;

	ret = MXL862XX_API_WRITE(ds->priv, MXL862XX_COMMON_REGISTERMOD, sdma);
	if (ret)
		return ret;

	return MXL862XX_API_WRITE(ds->priv, MXL862XX_COMMON_REGISTERMOD, fdma);
}

static int mxl862xx_port_enable(struct dsa_switch *ds, int port,
				struct phy_device *phydev)
{
	return mxl862xx_port_state(ds, port, true);
}

static void mxl862xx_port_disable(struct dsa_switch *ds, int port)
{
	mxl862xx_port_state(ds, port, false);
}

/* MDIO bus for PHYs */
static int mxl862xx_phy_read_mii_bus(struct mii_bus *bus, int port, int regnum)
{
	return mxl862xx_phy_read_mmd(bus->priv, port, 0, regnum);
}

static int mxl862xx_phy_write_mii_bus(struct mii_bus *bus, int port,
				      int regnum, u16 val)
{
	return mxl862xx_phy_write_mmd(bus->priv, port, 0, regnum, val);
}

static int mxl862xx_phy_read_c45_mii_bus(struct mii_bus *bus, int port,
					 int devadd, int regnum)
{
	return mxl862xx_phy_read_mmd(bus->priv, port, devadd, regnum);
}

static int mxl862xx_phy_write_c45_mii_bus(struct mii_bus *bus, int port,
					  int devadd, int regnum, u16 val)
{
	return mxl862xx_phy_write_mmd(bus->priv, port, devadd, regnum, val);
}

static int mxl862xx_setup_mdio(struct dsa_switch *ds)
{
	struct mxl862xx_priv *priv = ds->priv;
	struct device *dev = ds->dev;
	struct device_node *mdio_np;
	struct mii_bus *bus;
	static int idx;
	int ret;

	bus = devm_mdiobus_alloc(dev);
	if (!bus)
		return -ENOMEM;

	bus->priv = priv;
	ds->user_mii_bus = bus;
	bus->name = KBUILD_MODNAME "-mii";
	snprintf(bus->id, MII_BUS_ID_SIZE, KBUILD_MODNAME "-%d", idx++);
	bus->read_c45 = mxl862xx_phy_read_c45_mii_bus;
	bus->write_c45 = mxl862xx_phy_write_c45_mii_bus;
	bus->read = mxl862xx_phy_read_mii_bus;
	bus->write = mxl862xx_phy_write_mii_bus;
	bus->parent = dev;
	bus->phy_mask = ~ds->phys_mii_mask;

	mdio_np = of_get_child_by_name(dev->of_node, "mdio");
	if (!mdio_np)
		return -ENODEV;

	ret = devm_of_mdiobus_register(dev, bus, mdio_np);
	of_node_put(mdio_np);

	return ret;
}

/* Reset switch via MMD write */
static int mxl862xx_mmd_write(struct dsa_switch *ds, int reg, u16 data)
{
	struct mxl862xx_priv *priv = ds->priv;
	int ret;

	mutex_lock(&priv->bus->mdio_lock);
	ret = __mdiobus_c45_write(priv->bus, priv->sw_addr, MXL862XX_MMD_DEV,
				  reg, data);
	mutex_unlock(&priv->bus->mdio_lock);

	return ret;
}

/* Phylink integration */
static void mxl862xx_phylink_get_caps(struct dsa_switch *ds, int port,
				      struct phylink_config *config)
{
	struct mxl862xx_priv *priv = ds->priv;

	config->mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE | MAC_10 |
				   MAC_100 | MAC_1000 | MAC_2500FD;

	if (port < priv->hw_info->phy_ports)
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  config->supported_interfaces);
	else
		__set_bit(PHY_INTERFACE_MODE_NA,
			  config->supported_interfaces);
}

/* Tag protocol */
static enum dsa_tag_protocol mxl862xx_get_tag_protocol(struct dsa_switch *ds,
						       int port,
						       enum dsa_tag_protocol m)
{
	return DSA_TAG_PROTO_MXL862;
}

/* Setup */
static int mxl862xx_setup(struct dsa_switch *ds)
{
	struct mxl862xx_priv *priv = ds->priv;
	int ret, i;

	for (i = 0; i < ds->num_ports; i++) {
		if (dsa_is_cpu_port(ds, i)) {
			priv->cpu_port = i;
			break;
		}
	}

	ret = mxl862xx_setup_mdio(ds);
	if (ret)
		return ret;

	/* Software reset */
	ret = mxl862xx_mmd_write(ds, 1, 0);
	if (ret)
		return ret;

	ret = mxl862xx_mmd_write(ds, 0, 0x9907);
	if (ret)
		return ret;

	usleep_range(4000000, 6000000);

	return mxl862xx_configure_tag_proto(ds, priv->cpu_port, true);
}

static const struct dsa_switch_ops mxl862xx_switch_ops = {
	.get_tag_protocol = mxl862xx_get_tag_protocol,
	.phylink_get_caps = mxl862xx_phylink_get_caps,
	.phy_read = mxl862xx_phy_read,
	.phy_write = mxl862xx_phy_write,
	.port_disable = mxl862xx_port_disable,
	.port_enable = mxl862xx_port_enable,
	.setup = mxl862xx_setup,
};

/* Probe/remove */
static int mxl862xx_probe(struct mdio_device *mdiodev)
{
	struct device *dev = &mdiodev->dev;
	struct mxl862xx_priv *priv;
	struct dsa_switch *ds;
	struct mxl862xx_sys_fw_image_version fw;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->bus = mdiodev->bus;
	priv->sw_addr = mdiodev->addr;
	priv->hw_info = of_device_get_match_data(dev);
	if (!priv->hw_info)
		return -EINVAL;

	ds = devm_kzalloc(dev, sizeof(*ds), GFP_KERNEL);
	if (!ds)
		return -ENOMEM;

	priv->ds = ds;
	ds->dev = dev;
	ds->priv = priv;
	ds->ops = &mxl862xx_switch_ops;
	ds->num_ports = priv->hw_info->max_ports;

	dev_set_drvdata(dev, ds);

	ret = dsa_register_switch(ds);
	if (ret)
		return ret;

	ret = MXL862XX_API_READ(priv, SYS_MISC_FW_VERSION, fw);
	if (!ret)
		dev_info(dev, "Firmware version %d.%d.%d.%d\n",
			 fw.iv_major, fw.iv_minor,
			 fw.iv_revision, fw.iv_build_num);

	return 0;
}

static void mxl862xx_remove(struct mdio_device *mdiodev)
{
	struct dsa_switch *ds = dev_get_drvdata(&mdiodev->dev);

	dsa_unregister_switch(ds);
}

static const struct mxl862xx_hw_info mxl86282_data = {
	.max_ports = MXL862XX_MAX_PORT_NUM,
	.phy_ports = MXL86282_PHY_PORT_NUM,
	.ext_ports = MXL86282_EXT_PORT_NUM,
};

static const struct mxl862xx_hw_info mxl86252_data = {
	.max_ports = MXL862XX_MAX_PORT_NUM,
	.phy_ports = MXL86252_PHY_PORT_NUM,
	.ext_ports = MXL86252_EXT_PORT_NUM,
};

static const struct of_device_id mxl862xx_of_match[] = {
	{ .compatible = "maxlinear,mxl86282", .data = &mxl86282_data },
	{ .compatible = "maxlinear,mxl86252", .data = &mxl86252_data },
	{ }
};
MODULE_DEVICE_TABLE(of, mxl862xx_of_match);

static struct mdio_driver mxl862xx_driver = {
	.probe  = mxl862xx_probe,
	.remove = mxl862xx_remove,
	.mdiodrv.driver = {
		.name = "mxl862xx",
		.of_match_table = mxl862xx_of_match,
	},
};

mdio_module_driver(mxl862xx_driver);

MODULE_DESCRIPTION("Minimal driver for MaxLinear MxL862xx switch family");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:mxl862xx");
