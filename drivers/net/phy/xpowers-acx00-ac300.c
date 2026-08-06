// SPDX-License-Identifier: GPL-2.0-only
/*
 * X-Powers AC300 Ethernet PHY package backend
 *
 * Copyright (C) 2026 James Hilliard <james.hilliard1@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/regulator/consumer.h>

#include "phylib.h"
#include "xpowers-acx00.h"

#define AC300_EPHY_BGS_EFFUSE_OFFSET	3
#define AC300_SYS_CONTROL_REG			0x00
#define AC300_PACKAGE_STATUS_MASK		GENMASK(11, 8)
#define AC300_EPHY_CLK_SEL_MASK			GENMASK(7, 6)
#define AC300_EPHY_CLK_SEL_25_MHZ		FIELD_PREP(AC300_EPHY_CLK_SEL_MASK, 0)
#define AC300_EPHY_CLK_SEL_27_MHZ		FIELD_PREP(AC300_EPHY_CLK_SEL_MASK, 1)
#define AC300_EPHY_CLK_SEL_24_MHZ		FIELD_PREP(AC300_EPHY_CLK_SEL_MASK, 2)
#define AC300_EFUSE_CLK_ENABLE			BIT(5)
#define AC300_EPHY_REG_CLK_ENABLE		BIT(4)
#define AC300_MDIO_ERROR			BIT(3)
#define AC300_CLKIN_GATING_ENABLE		BIT(2)
#define AC300_EPHY_RESET_DEASSERT		BIT(1)
#define AC300_CHIP_RESET_DEASSERT		BIT(0)

#define AC300_PACKAGE_POR_INTERNAL_DLDO		BIT(3)
#define AC300_PACKAGE_PHY_ADDR_MASK		GENMASK(2, 0)

#define AC300_SYS_BIAS1_REG			0x02
#define AC300_INTERNAL_DLDO_ENABLE		BIT(15)

#define AC300_SYS_IO_REG			0x05
#define AC300_MDIO_DRV_MASK			GENMASK(15, 14)
#define AC300_MII_DRV_MASK			GENMASK(11, 10)
#define AC300_IO_DRV_LEVEL_2			2
#define AC300_CLKIN_PAD_ENABLE			BIT(4)
#define AC300_EPHY_MII_IO_ENABLE			BIT(0)

#define AC300_EPHY_CONFIG_REG			0x06
#define AC300_EPHY_BGS_EFFUSE_MASK		GENMASK(15, 12)
#define AC300_EPHY_RMII_SEL			BIT(11)
#define AC300_EPHY_SHUTDOWN			BIT(0)

#define AC300_SYS_CONTROL_ENABLE_BITS \
	(AC300_EFUSE_CLK_ENABLE | AC300_EPHY_REG_CLK_ENABLE | \
	 AC300_CLKIN_GATING_ENABLE | AC300_EPHY_RESET_DEASSERT | \
	 AC300_CHIP_RESET_DEASSERT)

#define AC300_SYS_IO_VALUE \
	(FIELD_PREP(AC300_MDIO_DRV_MASK, AC300_IO_DRV_LEVEL_2) | \
	 FIELD_PREP(AC300_MII_DRV_MASK, AC300_IO_DRV_LEVEL_2) | \
	 AC300_CLKIN_PAD_ENABLE | AC300_EPHY_MII_IO_ENABLE)

struct ac300_ephy_ctl {
	struct acx00_ephy_control control;
	struct phy_device *phydev;
	struct clk *clk;
	struct mutex lock; /* Serializes power sequencing and state. */
	u16 sys_control;
	u16 ephy_config;
	phy_interface_t interface;
	bool package_known;
	bool internal_dldo;
	bool powered;
};

static unsigned int
ac300_ephy_ctl_link_addr(const struct ac300_ephy_ctl *priv)
{
	return priv->phydev->mdio.addr;
}

static int ac300_ephy_ctl_read(struct ac300_ephy_ctl *priv, u32 regnum)
{
	int ret;

	phy_lock_mdio_bus(priv->phydev);
	ret = __phy_package_read(priv->phydev,
				 AC300_EPHY_CONTROL_ADDR_OFFSET, regnum);
	phy_unlock_mdio_bus(priv->phydev);

	return ret;
}

static int ac300_ephy_ctl_write(struct ac300_ephy_ctl *priv, u32 regnum,
				u16 val)
{
	int ret;

	phy_lock_mdio_bus(priv->phydev);
	ret = __phy_package_write(priv->phydev,
				  AC300_EPHY_CONTROL_ADDR_OFFSET, regnum, val);
	phy_unlock_mdio_bus(priv->phydev);

	return ret;
}

static int ac300_ephy_ctl_modify(struct ac300_ephy_ctl *priv, u32 regnum,
				 u16 mask, u16 set)
{
	int ret;

	phy_lock_mdio_bus(priv->phydev);
	ret = __phy_package_read(priv->phydev,
				 AC300_EPHY_CONTROL_ADDR_OFFSET, regnum);
	if (ret >= 0) {
		u16 val = (ret & ~mask) | (set & mask);

		ret = val == ret ? 0 :
			__phy_package_write(priv->phydev,
					    AC300_EPHY_CONTROL_ADDR_OFFSET,
					    regnum, val);
	}
	phy_unlock_mdio_bus(priv->phydev);

	return ret;
}

static u16 ac300_ephy_ctl_config(const struct ac300_ephy_ctl *priv)
{
	return priv->ephy_config |
		(priv->interface == PHY_INTERFACE_MODE_RMII ?
		 AC300_EPHY_RMII_SEL : 0);
}

static int ac300_ephy_ctl_power_off_locked(struct ac300_ephy_ctl *priv)
{
	int err;
	int ret;

	if (!priv->powered)
		return 0;

	ret = ac300_ephy_ctl_write(priv, AC300_EPHY_CONFIG_REG,
				   ac300_ephy_ctl_config(priv) |
				  AC300_EPHY_SHUTDOWN);
	err = ac300_ephy_ctl_write(priv, AC300_SYS_IO_REG, 0);
	if (!ret)
		ret = err;
	err = ac300_ephy_ctl_write(priv, AC300_SYS_CONTROL_REG,
				   priv->package_known && !priv->internal_dldo ?
				  AC300_CHIP_RESET_DEASSERT : 0);
	if (!ret)
		ret = err;

	clk_disable_unprepare(priv->clk);
	priv->powered = false;

	return ret;
}

static int ac300_ephy_ctl_power_off(struct acx00_ephy_control *control)
{
	struct ac300_ephy_ctl *priv =
		container_of(control, struct ac300_ephy_ctl, control);
	int ret;

	mutex_lock(&priv->lock);
	ret = ac300_ephy_ctl_power_off_locked(priv);
	mutex_unlock(&priv->lock);

	return ret;
}

static int
ac300_ephy_ctl_set_interface(struct acx00_ephy_control *control,
			     phy_interface_t interface)
{
	struct ac300_ephy_ctl *priv =
		container_of(control, struct ac300_ephy_ctl, control);
	u16 value;
	int ret = 0;

	switch (interface) {
	case PHY_INTERFACE_MODE_MII:
		value = 0;
		break;
	case PHY_INTERFACE_MODE_RMII:
		value = AC300_EPHY_RMII_SEL;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&priv->lock);
	if (priv->interface == interface)
		goto out_unlock;

	if (priv->powered)
		ret = ac300_ephy_ctl_modify(priv, AC300_EPHY_CONFIG_REG,
					    AC300_EPHY_RMII_SEL, value);
	if (!ret)
		priv->interface = interface;

out_unlock:
	mutex_unlock(&priv->lock);

	return ret;
}

static int ac300_ephy_ctl_power_on(struct acx00_ephy_control *control,
				   unsigned int phy_addr)
{
	struct ac300_ephy_ctl *priv =
		container_of(control, struct ac300_ephy_ctl, control);
	u8 package_status;
	u16 reset_value;
	int sys_control;
	int ret;

	if (phy_addr != ac300_ephy_ctl_link_addr(priv))
		return -EINVAL;

	mutex_lock(&priv->lock);
	if (priv->powered) {
		ret = 0;
		goto out_unlock;
	}

	ret = clk_prepare_enable(priv->clk);
	if (ret)
		goto out_unlock;
	priv->powered = true;

	/* Keep the external-supply configuration across subsequent resets. */
	reset_value = priv->package_known && !priv->internal_dldo ?
		      AC300_CHIP_RESET_DEASSERT : 0;
	ret = ac300_ephy_ctl_write(priv, AC300_SYS_CONTROL_REG, reset_value);
	if (ret)
		goto err_power_off;

	/* The manual requires both resets to be released before the clocks. */
	ret = ac300_ephy_ctl_write(priv, AC300_SYS_CONTROL_REG,
				   AC300_EPHY_RESET_DEASSERT |
				  AC300_CHIP_RESET_DEASSERT);
	if (ret)
		goto err_power_off;

	/* Retain the vendor clock-enable defaults, including the eFuse clock. */
	ret = ac300_ephy_ctl_write(priv, AC300_SYS_CONTROL_REG,
				   priv->sys_control);
	if (ret)
		goto err_power_off;

	sys_control = ac300_ephy_ctl_read(priv, AC300_SYS_CONTROL_REG);
	if (sys_control < 0) {
		ret = sys_control;
		goto err_power_off;
	}
	if (sys_control & AC300_MDIO_ERROR) {
		ret = ac300_ephy_ctl_write(priv, AC300_SYS_CONTROL_REG,
					   priv->sys_control | AC300_MDIO_ERROR);
		if (ret)
			goto err_power_off;

		sys_control = ac300_ephy_ctl_read(priv, AC300_SYS_CONTROL_REG);
		if (sys_control < 0) {
			ret = sys_control;
			goto err_power_off;
		}
		if (sys_control & AC300_MDIO_ERROR) {
			ret = -EIO;
			goto err_power_off;
		}
	}

	package_status = FIELD_GET(AC300_PACKAGE_STATUS_MASK, sys_control);
	if ((~package_status & AC300_PACKAGE_PHY_ADDR_MASK) !=
	    ac300_ephy_ctl_link_addr(priv)) {
		ret = -EINVAL;
		goto err_power_off;
	}

	priv->internal_dldo = package_status & AC300_PACKAGE_POR_INTERNAL_DLDO;
	priv->package_known = true;
	ret = ac300_ephy_ctl_modify(priv, AC300_SYS_BIAS1_REG,
				    AC300_INTERNAL_DLDO_ENABLE,
				   priv->internal_dldo ?
				   AC300_INTERNAL_DLDO_ENABLE : 0);
	if (ret)
		goto err_power_off;

	/* Keep the documented default drive level and leave the IRQ disabled. */
	ret = ac300_ephy_ctl_write(priv, AC300_SYS_IO_REG,
				   AC300_SYS_IO_VALUE);
	if (ret)
		goto err_power_off;

	fsleep(10000);

	ret = ac300_ephy_ctl_write(priv, AC300_EPHY_CONFIG_REG,
				   ac300_ephy_ctl_config(priv) |
				  AC300_EPHY_SHUTDOWN);
	if (ret)
		goto err_power_off;

	fsleep(10000);

	ret = ac300_ephy_ctl_write(priv, AC300_EPHY_CONFIG_REG,
				   ac300_ephy_ctl_config(priv));
	if (ret)
		goto err_power_off;

	goto out_unlock;

err_power_off:
	ac300_ephy_ctl_power_off_locked(priv);
out_unlock:
	mutex_unlock(&priv->lock);

	return ret;
}

static void ac300_ephy_regulator_disable(void *data)
{
	regulator_disable(data);
}

static void ac300_ephy_clk_put(void *data)
{
	clk_put(data);
}

struct acx00_ephy_control *
ac300_ephy_ctl_create(struct phy_device *phydev,
		      struct device_node *package_node, u8 calibration)
{
	struct device *dev = &phydev->mdio.dev;
	struct ac300_ephy_ctl *priv;
	struct regulator *vcc1;
	unsigned long clk_rate;
	u8 bgs_effuse_code;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);
	if (phydev->mdio.addr > FIELD_MAX(AC300_PACKAGE_PHY_ADDR_MASK))
		return ERR_PTR(dev_err_probe(dev, -EINVAL,
					     "link address is outside the package range\n"));
	priv->phydev = phydev;
	mutex_init(&priv->lock);

	vcc1 = devm_of_regulator_get(dev, package_node, "vcc1");
	if (IS_ERR(vcc1))
		return ERR_PTR(dev_err_probe(dev, PTR_ERR(vcc1),
					     "failed to get VCC1 supply\n"));

	ret = regulator_enable(vcc1);
	if (ret)
		return ERR_PTR(dev_err_probe(dev, ret,
					     "failed to enable VCC1 supply\n"));

	ret = devm_add_action_or_reset(dev, ac300_ephy_regulator_disable,
				       vcc1);
	if (ret)
		return ERR_PTR(ret);

	/* Wait for the power-on reset interval specified by the manual. */
	fsleep(10000);

	priv->clk = of_clk_get(package_node, 0);
	if (IS_ERR(priv->clk))
		return ERR_PTR(dev_err_probe(dev, PTR_ERR(priv->clk),
					     "failed to get input clock\n"));

	ret = devm_add_action_or_reset(dev, ac300_ephy_clk_put, priv->clk);
	if (ret)
		return ERR_PTR(ret);

	ret = devm_clk_rate_exclusive_get(dev, priv->clk);
	if (ret)
		return ERR_PTR(dev_err_probe(dev, ret,
					     "failed to lock clock rate\n"));

	clk_rate = clk_get_rate(priv->clk);
	switch (clk_rate) {
	case 24000000:
		priv->sys_control = AC300_EPHY_CLK_SEL_24_MHZ;
		break;
	case 25000000:
		priv->sys_control = AC300_EPHY_CLK_SEL_25_MHZ;
		break;
	case 27000000:
		priv->sys_control = AC300_EPHY_CLK_SEL_27_MHZ;
		break;
	default:
		return ERR_PTR(dev_err_probe(dev, -EINVAL,
					     "unsupported input clock rate %lu Hz\n",
					     clk_rate));
	}
	priv->sys_control |= AC300_SYS_CONTROL_ENABLE_BITS;

	/* The vendor driver supplies no transfer function beyond this offset. */
	bgs_effuse_code = (calibration + AC300_EPHY_BGS_EFFUSE_OFFSET) &
			   FIELD_MAX(AC300_EPHY_BGS_EFFUSE_MASK);
	priv->ephy_config =
		FIELD_PREP(AC300_EPHY_BGS_EFFUSE_MASK, bgs_effuse_code);
	/* EPHY_MODE and BIST_CLK_EN stay clear for normal operation. */

	priv->control.power_on = ac300_ephy_ctl_power_on;
	priv->control.power_off = ac300_ephy_ctl_power_off;
	priv->control.set_interface = ac300_ephy_ctl_set_interface;
	/* MII is the reset default used until the MAC supplies its interface. */
	priv->interface = PHY_INTERFACE_MODE_MII;

	return &priv->control;
}
