// SPDX-License-Identifier: GPL-2.0
/*
 * Synopsys DesignWare XPCS regmap helpers
 *
 * Copyright (C) 2026 RISCstar Solutions.
 * Copyright (C) 2024 Serge Semin
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mdio.h>
#include <linux/pcs/pcs-xpcs.h>
#include <linux/pcs/pcs-xpcs-regmap.h>
#include <linux/regmap.h>

#include "pcs-xpcs.h"

/* Page select register for the indirect MMIO CSRs access */
#define DW_VR_CSR_VIEWPORT		0xff

static ptrdiff_t xpcs_regmap_addr_format(int dev, int reg)
{
	return FIELD_PREP(0x1f0000, dev) | FIELD_PREP(0xffff, reg);
}

static u16 xpcs_regmap_addr_page(ptrdiff_t csr)
{
	return FIELD_GET(0x1fff00, csr);
}

static ptrdiff_t xpcs_regmap_addr_offset(ptrdiff_t csr)
{
	return FIELD_GET(0xff, csr);
}

static int
xpcs_regmap_read_reg_indirect(struct regmap *regmap, int dev, int reg)
{
	ptrdiff_t csr, ofs;
	unsigned int val;
	u16 page;
	int res;

	csr = xpcs_regmap_addr_format(dev, reg);
	page = xpcs_regmap_addr_page(csr);
	ofs = xpcs_regmap_addr_offset(csr);

	res = regmap_write(regmap, DW_VR_CSR_VIEWPORT, page);
	if (res < 0)
		return res;

	res = regmap_read(regmap, ofs, &val);
	if (res < 0)
		return res;

	return val & 0xffff;
}

static int
xpcs_regmap_write_reg_indirect(struct regmap *regmap, int dev, int reg, u16 val)
{
	ptrdiff_t csr, ofs;
	u16 page;
	int res;

	csr = xpcs_regmap_addr_format(dev, reg);
	page = xpcs_regmap_addr_page(csr);
	ofs = xpcs_regmap_addr_offset(csr);

	res = regmap_write(regmap, DW_VR_CSR_VIEWPORT, page);
	if (res < 0)
		return res;

	return regmap_write(regmap, ofs, val);
}

static int xpcs_regmap_read_c22(struct mii_bus *bus, int addr, int reg)
{
	struct regmap *regmap = bus->priv;

	if (addr != 0)
		return -ENODEV;

	return xpcs_regmap_read_reg_indirect(regmap, MDIO_MMD_VEND2, reg);
}

static int
xpcs_regmap_write_c22(struct mii_bus *bus, int addr, int reg, u16 val)
{
	struct regmap *regmap = bus->priv;

	if (addr != 0)
		return -ENODEV;

	return xpcs_regmap_write_reg_indirect(regmap, MDIO_MMD_VEND2, reg, val);
}

static int xpcs_regmap_read_c45(struct mii_bus *bus, int addr, int dev, int reg)
{
	struct regmap *regmap = bus->priv;

	if (addr != 0)
		return -ENODEV;

	return xpcs_regmap_read_reg_indirect(regmap, dev, reg);
}

static int xpcs_regmap_write_c45(struct mii_bus *bus, int addr, int dev,
				 int reg, u16 val)
{
	struct regmap *regmap = bus->priv;

	if (addr != 0)
		return -ENODEV;

	return xpcs_regmap_write_reg_indirect(regmap, dev, reg, val);
}

static void devm_xpcs_regmap_destroy(void *data)
{
	struct dw_xpcs *xpcs = data;

	xpcs_destroy(xpcs);
}

struct dw_xpcs *devm_xpcs_regmap_register(struct device *dev,
					  struct regmap *regmap)
{
	static atomic_t id = ATOMIC_INIT(-1);
	struct dw_xpcs *xpcs;
	struct mii_bus *bus;
	int ret;

	bus = devm_mdiobus_alloc_size(dev, 0);
	if (!bus)
		return ERR_PTR(-ENOMEM);

	bus->name = "DW XPCS MCI/APB3";
	bus->read = xpcs_regmap_read_c22;
	bus->write = xpcs_regmap_write_c22;
	bus->read_c45 = xpcs_regmap_read_c45;
	bus->write_c45 = xpcs_regmap_write_c45;
	bus->phy_mask = ~0;
	bus->parent = dev;
	bus->priv = regmap;

	snprintf(bus->id, sizeof(bus->id), "dwxpcs-%x", atomic_inc_return(&id));

	/* MDIO-bus here serves as just a back-end engine abstracting out
	 * the MDIO and MCI/APB3 IO interfaces utilized for the DW XPCS CSRs
	 * access.
	 */
	ret = devm_mdiobus_register(dev, bus);
	if (ret) {
		dev_err(dev, "failed to create MDIO bus\n");
		return ERR_PTR(ret);
	}

	xpcs = xpcs_create_mdiodev(bus, 0);
	if (IS_ERR(xpcs))
		return xpcs;

	ret = devm_add_action_or_reset(dev, devm_xpcs_regmap_destroy, xpcs);
	if (ret)
		return ERR_PTR(ret);

	return xpcs;
}
EXPORT_SYMBOL_GPL(devm_xpcs_regmap_register);
