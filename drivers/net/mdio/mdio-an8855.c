// SPDX-License-Identifier: GPL-2.0+
/*
 * MDIO PBUS driver for Airoha AN8855 Switch
 *
 * Author: Christian Marangi <ansuelsmth@gmail.com>
 *
 */

#include <linux/dsa/an8855.h>
#include <linux/module.h>
#include <linux/of_mdio.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

/* AN8855 permit to access the internal GPHY via the PBUS
 * interface.
 *
 * Some piece of this comes from Reverse-Enginnering
 * by applying the value on the Switch and observing
 * it by reading the raw value on the MDIO BUS.
 *
 * The CL22 address are shifted by 4 left
 * The CL44 address need to be multiplied by 4 (and
 * no shift)
 *
 * The GPHY have additional configuration (like auto
 * downshift) at PAGE 1 in the EXT register.
 * It was discovered that it's possible to access
 * PAGE x address by increasing them by 2 on setting
 * the value in the related mask.
 * The PHY have these custom/vendor register
 * always starting at 0x10.
 * From MDIO bus, for address 0x0 to 0xf, PHY
 * always report PAGE 0 values.
 * From PBUS, on setting the PAGE value, 0x0 to 0xf
 * always report 0.
 *
 * (it can also be notice that PBUS does NOT change the
 *  page on accessing these custom/vendor register)
 *
 * Comparison examples:
 *	(PORT 0 PAGE 1)		  |	(PORT 0 PAGE 2)
 *  PBUS ADDR	  VALUE	    MDIO  |  PBUS ADDR	  VALUE	    MDIO
 * 0xa0803000: 0x00000000  0x1840 | 0xa0804000: 0x00000000  0x1840
 * 0xa0803010: 0x00000000  0x7949 | 0xa0804010: 0x00000000  0x7949
 * 0xa0803020: 0x00000000  0xc0ff | 0xa0804020: 0x00000000  0xc0ff
 * 0xa0803030: 0x00000000  0x0410 | 0xa0804030: 0x00000000  0x0410
 * 0xa0803040: 0x00000000  0x0de1 | 0xa0804040: 0x00000000  0x0de1
 * 0xa0803050: 0x00000000  0x0000 | 0xa0804050: 0x00000000  0x0000
 * 0xa0803060: 0x00000000  0x0004 | 0xa0804060: 0x00000000  0x0004
 * 0xa0803070: 0x00000000  0x2001 | 0xa0804070: 0x00000000  0x2001
 * 0xa0803080: 0x00000000  0x0000 | 0xa0804080: 0x00000000  0x0000
 * 0xa0803090: 0x00000000  0x0200 | 0xa0804090: 0x00000000  0x0200
 * 0xa08030a0: 0x00000000  0x4000 | 0xa08040a0: 0x00000000  0x4000
 * 0xa08030b0: 0x00000000  0x0000 | 0xa08040b0: 0x00000000  0x0000
 * 0xa08030c0: 0x00000000  0x0000 | 0xa08040c0: 0x00000000  0x0000
 * 0xa08030d0: 0x00000000  0x0000 | 0xa08040d0: 0x00000000  0x0000
 * 0xa08030e0: 0x00000000  0x0000 | 0xa08040e0: 0x00000000  0x0000
 * 0xa08030f0: 0x00000000  0x2000 | 0xa08040f0: 0x00000000  0x2000
 * 0xa0803100: 0x00000000  0x0000 | 0xa0804100: 0x00000000  0x0000
 * 0xa0803110: 0x00000000  0x0000 | 0xa0804110: 0x0000030f  0x030f
 * 0xa0803120: 0x00000000  0x0000 | 0xa0804120: 0x00000000  0x0000
 * 0xa0803130: 0x00000030  0x0030 | 0xa0804130: 0x00000000  0x0000
 * 0xa0803140: 0x00003a14  0x3a14 | 0xa0804140: 0x00000000  0x0000
 * 0xa0803150: 0x00000101  0x0101 | 0xa0804150: 0x00000000  0x0000
 * 0xa0803160: 0x00000001  0x0001 | 0xa0804160: 0x00000000  0x0000
 * 0xa0803170: 0x00000800  0x0800 | 0xa0804170: 0x000001ff  0x01ff
 * 0xa0803180: 0x00000000  0x0000 | 0xa0804180: 0x0000ff1f  0xff1f
 * 0xa0803190: 0x0000001f  0x001f | 0xa0804190: 0x000083ff  0x83ff
 * 0xa08031a0: 0x00000000  0x0000 | 0xa08041a0: 0x00000000  0x0000
 * 0xa08031b0: 0x00000000  0x0000 | 0xa08041b0: 0x00000000  0x0000
 * 0xa08031c0: 0x00003001  0x3001 | 0xa08041c0: 0x00000000  0x0000
 * 0xa08031d0: 0x00000000  0x0000 | 0xa08041d0: 0x00000000  0x0000
 * 0xa08031e0: 0x00000000  0x0000 | 0xa08041e0: 0x00000000  0x0000
 * 0xa08031f0: 0x00000000  0x0001 | 0xa08041f0: 0x00000000  0x0002
 *
 * Using the PBUS permits to have consistent access
 * to Switch and PHY without having to relay on checking
 * pages.
 *
 * It does also permit to cut on CL45 access and PAGE1
 * access as the PBUS expose direct register for them.
 *
 * The base address is 0xa0800000 and can be seen as
 * bitmap to derive each specific address.
 *
 * Example:
 *   PORT 1 ADDR 0x2 --> 0xa1800020
 *			    ^    ^
 *			    |    ADDR
 *			    PORT
 *   PORT 2 DEVAD 1 ADDR 0x2 --> 0xa2840008
 *				    ^ ^   ^
 *				    | |   ADDR (*4)
 *				    | DEVAD
 *				    PORT
 *   PORT 3 PAGE 1 ADDR 0x14 --> 0xa3803140
 *				    ^  ^^^
 *				    |  |ADDR
 *				    |  PAGE (+2)
 *				    PORT
 *
 * It's worth mention that trying to read more than the
 * expected PHY address cause the PBUS to ""crash"" and
 * the Switch to lock out (requiring a reset).
 * Validation of the port value is put to prevent this
 * problem.
 */

struct an8855_mdio_priv {
	struct regmap *regmap;
	u32 base_addr;
	u8 next_page;
};

static int an8855_mdio_read(struct mii_bus *bus, int addr, int regnum)
{
	struct an8855_mdio_priv *priv = bus->priv;
	u32 pbus_addr = AN8855_GPHY_ACCESS;
	u32 port = addr - priv->base_addr;
	u32 val;
	int ret;

	/* Signal invalid address for mdio tools */
	if (port >= AN8855_NUM_PHY_PORT)
		return 0xffff;

	pbus_addr |= FIELD_PREP(AN8855_GPHY_PORT, port);
	pbus_addr |= FIELD_PREP(AN8855_CL22_ADDR, regnum);
	if (priv->next_page)
		pbus_addr |= FIELD_PREP(AN8855_PAGE_SELECT,
					priv->next_page + 2);

	ret = regmap_read(priv->regmap, pbus_addr, &val);
	if (ret)
		return ret;

	return val & 0xffff;
}

static int an8855_mdio_write(struct mii_bus *bus, int addr, int regnum,
			     u16 value)
{
	struct an8855_mdio_priv *priv = bus->priv;
	u32 pbus_addr = AN8855_GPHY_ACCESS;
	u32 port = addr - priv->base_addr;

	if (port >= AN8855_NUM_PHY_PORT)
		return -EINVAL;

	/* When PHY ask to change page, skip writing it and
	 * save it for the next read/write.
	 */
	if (regnum == AN8855_PHY_SELECT_PAGE) {
		priv->next_page = value;
		return 0;
	}

	pbus_addr |= FIELD_PREP(AN8855_GPHY_PORT, port);
	pbus_addr |= FIELD_PREP(AN8855_CL22_ADDR, regnum);
	if (priv->next_page)
		pbus_addr |= FIELD_PREP(AN8855_PAGE_SELECT,
					priv->next_page + 2);

	return regmap_write(priv->regmap, pbus_addr, value);
}

static int an8855_mdio_cl45_read(struct mii_bus *bus, int addr, int devnum,
				 int regnum)
{
	struct an8855_mdio_priv *priv = bus->priv;
	u32 pbus_addr = AN8855_GPHY_ACCESS;
	u32 port = addr - priv->base_addr;
	u32 val;
	int ret;

	/* Signal invalid address for mdio tools */
	if (port >= AN8855_NUM_PHY_PORT)
		return 0xffff;

	pbus_addr |= FIELD_PREP(AN8855_GPHY_PORT, port);
	pbus_addr |= FIELD_PREP(AN8855_DEVAD_ADDR, devnum);
	pbus_addr |= FIELD_PREP(AN8855_CL45_ADDR, regnum * 4);

	ret = regmap_read(priv->regmap, pbus_addr, &val);
	if (ret)
		return ret;

	return val & 0xffff;
}

static int an8855_mdio_cl45_write(struct mii_bus *bus, int addr, int devnum,
				  int regnum, u16 value)
{
	struct an8855_mdio_priv *priv = bus->priv;
	u32 pbus_addr = AN8855_GPHY_ACCESS;
	u32 port = addr - priv->base_addr;

	if (port >= AN8855_NUM_PHY_PORT)
		return -EINVAL;

	pbus_addr |= FIELD_PREP(AN8855_GPHY_PORT, port);
	pbus_addr |= FIELD_PREP(AN8855_DEVAD_ADDR, devnum);
	pbus_addr |= FIELD_PREP(AN8855_CL45_ADDR, regnum * 4);

	return regmap_write(priv->regmap, pbus_addr, value);
}

static int an8855_mdio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct an8855_mdio_priv *priv;
	struct mii_bus *bus;
	int ret;

	bus = devm_mdiobus_alloc_size(dev, sizeof(*priv));
	if (!bus)
		return -ENOMEM;

	priv = bus->priv;
	priv->regmap = dev_get_regmap(dev->parent, NULL);
	if (!priv->regmap)
		return -ENOENT;

	ret = of_property_read_u32(dev->parent->of_node, "reg",
				   &priv->base_addr);
	if (ret)
		return -EINVAL;

	bus->name = "an8855_mdio_bus";
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s-gphy-pbus", dev_name(dev));
	bus->parent = dev;
	bus->read = an8855_mdio_read;
	bus->write = an8855_mdio_write;
	bus->read_c45 = an8855_mdio_cl45_read;
	bus->write_c45 = an8855_mdio_cl45_write;

	ret = devm_of_mdiobus_register(dev, bus, dev->of_node);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register MDIO bus\n");

	return 0;
}

static const struct of_device_id an8855_mdio_of_match[] = {
	{ .compatible = "airoha,an8855-mdio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, an8855_mdio_of_match);

static struct platform_driver an8855_mdio_driver = {
	.probe	= an8855_mdio_probe,
	.driver = {
		.name = "an8855-mdio",
		.of_match_table = an8855_mdio_of_match,
	},
};
module_platform_driver(an8855_mdio_driver);

MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
MODULE_DESCRIPTION("Driver for AN8855 MDIO passthrough");
MODULE_LICENSE("GPL");
