// SPDX-License-Identifier: GPL-2.0-only
/*
 * MDIO controller for RTL9300 switches with integrated SoC.
 *
 * The MDIO communication is abstracted by the switch. At the software level
 * communication uses the switch port to address the PHY with the actual MDIO
 * bus and address having been setup via the realtek,smi-address property.
 */

#include <linux/mdio.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define SMI_GLB_CTRL			0x000
#define   GLB_CTRL_INTF_SEL(intf)	BIT(16 + (intf))
#define SMI_PORT0_15_POLLING_SEL	0x008
#define SMI_ACCESS_PHY_CTRL_0		0x170
#define SMI_ACCESS_PHY_CTRL_1		0x174
#define   PHY_CTRL_RWOP			BIT(2)
#define   PHY_CTRL_TYPE			BIT(1)
#define   PHY_CTRL_CMD			BIT(0)
#define   PHY_CTRL_FAIL			BIT(25)
#define SMI_ACCESS_PHY_CTRL_2		0x178
#define SMI_ACCESS_PHY_CTRL_3		0x17c
#define SMI_PORT0_5_ADDR_CTRL		0x180

#define MAX_PORTS       32
#define MAX_SMI_BUSSES  4

struct realtek_mdio_priv {
	struct regmap *regmap;
	u8 smi_bus[MAX_PORTS];
	u8 smi_addr[MAX_PORTS];
	bool smi_bus_isc45[MAX_SMI_BUSSES];
	u32 reg_base;
};

static int realtek_mdio_wait_ready(struct realtek_mdio_priv *priv)
{
	u32 val;

	return regmap_read_poll_timeout(priv->regmap, priv->reg_base + SMI_ACCESS_PHY_CTRL_1,
					val, !(val & PHY_CTRL_CMD), 10, 500);
}

static int realtek_mdio_read_c45(struct mii_bus *bus, int phy_id, int dev_addr, int regnum)
{
	struct realtek_mdio_priv *priv = bus->priv;
	u32 val;
	int err;

	err = realtek_mdio_wait_ready(priv);
	if (err)
		return err;

	err = regmap_write(priv->regmap, priv->reg_base + SMI_ACCESS_PHY_CTRL_2, phy_id << 16);
	if (err)
		return err;

	err = regmap_write(priv->regmap, priv->reg_base + SMI_ACCESS_PHY_CTRL_3,
			   dev_addr << 16 | (regnum & 0xffff));
	if (err)
		return err;

	err = regmap_write(priv->regmap, priv->reg_base + SMI_ACCESS_PHY_CTRL_1,
			   PHY_CTRL_TYPE | PHY_CTRL_CMD);
	if (err)
		return err;

	err = realtek_mdio_wait_ready(priv);
	if (err)
		return err;

	/* get_phy_c45_ids() will stop the mdio bus scan if we return an error
	 * here. So even though the SMI controller indicates an error for an
	 * absent device don't proagate it here.
	 */
	//if (val & BIT(25)) {
	//	err = -ENODEV;
	//	return err;
	//}

	err = regmap_read(priv->regmap, priv->reg_base + SMI_ACCESS_PHY_CTRL_2, &val);
	if (err)
		return err;

	return val & 0xffff;
}

static int realtek_mdio_write_c45(struct mii_bus *bus, int phy_id, int dev_addr,
				  int regnum, u16 value)
{
	struct realtek_mdio_priv *priv = bus->priv;
	u32 val;
	int err;

	err = realtek_mdio_wait_ready(priv);
	if (err)
		return err;

	err = regmap_write(priv->regmap, priv->reg_base + SMI_ACCESS_PHY_CTRL_0, BIT(phy_id));
	if (err)
		return err;

	err = regmap_write(priv->regmap, priv->reg_base + SMI_ACCESS_PHY_CTRL_2, value << 16);
	if (err)
		return err;

	err = regmap_write(priv->regmap, priv->reg_base + SMI_ACCESS_PHY_CTRL_3,
			   dev_addr << 16 | (regnum & 0xffff));
	if (err)
		return err;

	err = regmap_write(priv->regmap, priv->reg_base + SMI_ACCESS_PHY_CTRL_1,
			   PHY_CTRL_RWOP | PHY_CTRL_TYPE | PHY_CTRL_CMD);
	if (err)
		return err;

	err = regmap_read_poll_timeout(priv->regmap, priv->reg_base + SMI_ACCESS_PHY_CTRL_1,
				       val, !(val & PHY_CTRL_CMD), 10, 100);
	if (err)
		return err;

	if (val & PHY_CTRL_FAIL) {
		err = -ENXIO;
		return err;
	}

	return err;
}

static int realtek_mdiobus_init(struct realtek_mdio_priv *priv)
{
	u32 port_addr[5] = { };
	u32 poll_sel[2] = { 0, 0 };
	u32 glb_ctrl_mask = 0, glb_ctrl_val = 0;
	int i, err;

	for (i = 0; i < MAX_PORTS; i++) {
		int pos;

		if (priv->smi_bus[i] > 3)
			continue;

		pos = (i % 6) * 5;
		port_addr[i / 6] |=  priv->smi_addr[i] << pos;

		pos = (i % 16) * 2;
		poll_sel[i / 16] |= priv->smi_bus[i] << pos;
	}

	for (i = 0; i < MAX_SMI_BUSSES; i++) {
		if (priv->smi_bus_isc45[i]) {
			glb_ctrl_mask |= GLB_CTRL_INTF_SEL(i);
			glb_ctrl_val |= GLB_CTRL_INTF_SEL(i);
		}
	}

	err = regmap_bulk_write(priv->regmap, priv->reg_base + SMI_PORT0_5_ADDR_CTRL,
				port_addr, 5);
	if (err)
		return err;

	err = regmap_bulk_write(priv->regmap, priv->reg_base + SMI_PORT0_15_POLLING_SEL,
				poll_sel, 2);
	if (err)
		return err;

	err = regmap_update_bits(priv->regmap, priv->reg_base + SMI_GLB_CTRL,
				 glb_ctrl_mask, glb_ctrl_val);
	if (err)
		return err;

	return 0;
}

static int realtek_mdiobus_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct realtek_mdio_priv *priv;
	struct fwnode_handle *child;
	struct mii_bus *bus;
	int err;

	bus = devm_mdiobus_alloc_size(dev, sizeof(*priv));
	if (!bus)
		return -ENOMEM;

	bus->name = "Reaktek Switch MDIO Bus";
	bus->read_c45 = realtek_mdio_read_c45;
	bus->write_c45 =  realtek_mdio_write_c45;
	bus->parent = dev;
	priv = bus->priv;

	priv->regmap = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	err = device_property_read_u32(dev, "reg", &priv->reg_base);
	if (err)
		return err;

	snprintf(bus->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));

	device_for_each_child_node(dev, child) {
		u32 pn, smi_addr[2];

		err = fwnode_property_read_u32(child, "reg", &pn);
		if (err)
			return err;

		if (pn > MAX_PORTS)
			return dev_err_probe(dev, -EINVAL, "illegal port number %d\n", pn);

		err = fwnode_property_read_u32_array(child, "realtek,smi-address", smi_addr, 2);
		if (err) {
			smi_addr[0] = 0;
			smi_addr[1] = pn;
		}

		if (fwnode_device_is_compatible(child, "ethernet-phy-ieee802.3-c45"))
			priv->smi_bus_isc45[smi_addr[0]] = true;

		priv->smi_bus[pn] = smi_addr[0];
		priv->smi_addr[pn] = smi_addr[1];
	}

	err = realtek_mdiobus_init(priv);
	if (err)
		return dev_err_probe(dev, err, "failed to initialise MDIO bus controller\n");

	err = devm_of_mdiobus_register(dev, bus, dev->of_node);
	if (err)
		return dev_err_probe(dev, err, "cannot register MDIO bus\n");

	return 0;
}

static const struct of_device_id realtek_mdio_ids[] = {
	{ .compatible = "realtek,rtl9301-mdio" },
	{ .compatible = "realtek,rtl9302b-mdio" },
	{ .compatible = "realtek,rtl9302c-mdio" },
	{ .compatible = "realtek,rtl9303-mdio" },
	{}
};
MODULE_DEVICE_TABLE(of, realtek_mdio_ids);

static struct platform_driver rtl9300_mdio_driver = {
	.probe = realtek_mdiobus_probe,
	.driver = {
		.name = "mdio-rtl9300",
		.of_match_table = realtek_mdio_ids,
	},
};

module_platform_driver(rtl9300_mdio_driver);

MODULE_DESCRIPTION("RTL9300 MDIO driver");
MODULE_LICENSE("GPL");
