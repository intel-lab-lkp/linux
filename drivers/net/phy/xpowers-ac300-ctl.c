// SPDX-License-Identifier: GPL-2.0-only
/*
 * X-Powers AC300 Ethernet PHY control driver
 *
 * Copyright (C) 2026 James Hilliard <james.hilliard1@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/phy.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#include "xpowers-acx00.h"

#define AC300_EPHY_BGS_EFFUSE_OFFSET	3
#define AC300_SYS_CONTROL_REG			0x00
#define AC300_CHIP_VERSION_MASK			GENMASK(15, 12)
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

#define AC300_MASK_VERSION_REG			0x04
#define AC300_MASK_VERSION_MASK			GENMASK(2, 0)

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
	struct mdio_device *mdiodev;
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
	return priv->mdiodev->addr - AC300_EPHY_CONTROL_ADDR_OFFSET;
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

	ret = mdiodev_write(priv->mdiodev, AC300_EPHY_CONFIG_REG,
			    ac300_ephy_ctl_config(priv) | AC300_EPHY_SHUTDOWN);
	err = mdiodev_write(priv->mdiodev, AC300_SYS_IO_REG, 0);
	if (!ret)
		ret = err;
	err = mdiodev_write(priv->mdiodev, AC300_SYS_CONTROL_REG,
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
		ret = mdiodev_modify(priv->mdiodev, AC300_EPHY_CONFIG_REG,
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
	ret = mdiodev_write(priv->mdiodev, AC300_SYS_CONTROL_REG, reset_value);
	if (ret)
		goto err_power_off;

	/* The manual requires both resets to be released before the clocks. */
	ret = mdiodev_write(priv->mdiodev, AC300_SYS_CONTROL_REG,
			    AC300_EPHY_RESET_DEASSERT |
			    AC300_CHIP_RESET_DEASSERT);
	if (ret)
		goto err_power_off;

	/* Retain the vendor clock-enable defaults, including the eFuse clock. */
	ret = mdiodev_write(priv->mdiodev, AC300_SYS_CONTROL_REG,
			    priv->sys_control);
	if (ret)
		goto err_power_off;

	sys_control = mdiodev_read(priv->mdiodev, AC300_SYS_CONTROL_REG);
	if (sys_control < 0) {
		ret = sys_control;
		goto err_power_off;
	}
	if (sys_control & AC300_MDIO_ERROR) {
		ret = mdiodev_write(priv->mdiodev, AC300_SYS_CONTROL_REG,
				    priv->sys_control | AC300_MDIO_ERROR);
		if (ret)
			goto err_power_off;

		sys_control = mdiodev_read(priv->mdiodev, AC300_SYS_CONTROL_REG);
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
	ret = mdiodev_modify(priv->mdiodev, AC300_SYS_BIAS1_REG,
			     AC300_INTERNAL_DLDO_ENABLE,
			     priv->internal_dldo ?
			     AC300_INTERNAL_DLDO_ENABLE : 0);
	if (ret)
		goto err_power_off;

	/* Keep the documented default drive level and leave the IRQ disabled. */
	ret = mdiodev_write(priv->mdiodev, AC300_SYS_IO_REG,
			    AC300_SYS_IO_VALUE);
	if (ret)
		goto err_power_off;

	fsleep(10000);

	ret = mdiodev_write(priv->mdiodev, AC300_EPHY_CONFIG_REG,
			    ac300_ephy_ctl_config(priv) | AC300_EPHY_SHUTDOWN);
	if (ret)
		goto err_power_off;

	fsleep(10000);

	ret = mdiodev_write(priv->mdiodev, AC300_EPHY_CONFIG_REG,
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

static int ac300_ephy_ctl_probe(struct mdio_device *mdiodev)
{
	struct device *dev = &mdiodev->dev;
	struct ac300_ephy_ctl *priv;
	unsigned long clk_rate;
	unsigned int phy_addr;
	u8 calibration;
	u8 bgs_effuse_code;
	u8 package_status;
	int mask_version;
	int sys_control;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	if (mdiodev->addr < AC300_EPHY_CONTROL_ADDR_OFFSET ||
	    mdiodev->addr > AC300_EPHY_CONTROL_ADDR_OFFSET +
			    FIELD_MAX(AC300_PACKAGE_PHY_ADDR_MASK))
		return dev_err_probe(dev, -EINVAL,
				     "control address is outside the package range\n");
	priv->mdiodev = mdiodev;
	mutex_init(&priv->lock);

	ret = devm_regulator_get_enable(dev, "vcc1");
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to enable VCC1 supply\n");

	/* Wait for the power-on reset interval specified by the manual. */
	fsleep(10000);

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk))
		return dev_err_probe(dev, PTR_ERR(priv->clk),
				     "failed to get input clock\n");

	ret = devm_clk_rate_exclusive_get(dev, priv->clk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to lock clock rate\n");

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
		return dev_err_probe(dev, -EINVAL,
				     "unsupported input clock rate %lu Hz\n",
				     clk_rate);
	}
	priv->sys_control |= AC300_SYS_CONTROL_ENABLE_BITS;

	ret = nvmem_cell_read_u8(dev, "calibration", &calibration);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read calibration data\n");

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
	mdiodev_set_drvdata(mdiodev, &priv->control);

	/* Validate the package while the control endpoint is known to respond. */
	phy_addr = ac300_ephy_ctl_link_addr(priv);
	ret = ac300_ephy_ctl_power_on(&priv->control, phy_addr);
	if (ret)
		return ret;

	sys_control = mdiodev_read(mdiodev, AC300_SYS_CONTROL_REG);
	if (sys_control < 0) {
		ret = sys_control;
		goto err_disable;
	}
	package_status = FIELD_GET(AC300_PACKAGE_STATUS_MASK, sys_control);

	mask_version = mdiodev_read(mdiodev, AC300_MASK_VERSION_REG);
	if (mask_version < 0) {
		ret = mask_version;
		goto err_disable;
	}

	dev_info(dev, "chip version %u, mask version %u, package %#x, %s supplies, PHY %u, %lu Hz clock\n",
		 (unsigned int)FIELD_GET(AC300_CHIP_VERSION_MASK, sys_control),
		 (unsigned int)FIELD_GET(AC300_MASK_VERSION_MASK, mask_version),
		 package_status,
		 package_status & AC300_PACKAGE_POR_INTERNAL_DLDO ?
			"POR/internal DLDO" : "reset pin/external VDD",
		 ac300_ephy_ctl_link_addr(priv), clk_rate);

	ret = ac300_ephy_ctl_power_off(&priv->control);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to quiesce control block\n");

	return 0;

err_disable:
	ac300_ephy_ctl_power_off(&priv->control);
	return ret;
}

static void ac300_ephy_ctl_remove(struct mdio_device *mdiodev)
{
	struct acx00_ephy_control *control = mdiodev_get_drvdata(mdiodev);

	control->power_off(control);
}

static const struct of_device_id ac300_ephy_ctl_of_match[] = {
	{ .compatible = "x-powers,ac300-ephy-ctl" },
	{ }
};
MODULE_DEVICE_TABLE(of, ac300_ephy_ctl_of_match);

static struct mdio_driver ac300_ephy_ctl_driver = {
	.probe = ac300_ephy_ctl_probe,
	.remove = ac300_ephy_ctl_remove,
	.shutdown = ac300_ephy_ctl_remove,
	.mdiodrv.driver = {
		.name = "ac300-ephy-ctl",
		.of_match_table = ac300_ephy_ctl_of_match,
	},
};

mdio_module_driver(ac300_ephy_ctl_driver);

MODULE_AUTHOR("James Hilliard <james.hilliard1@gmail.com>");
MODULE_DESCRIPTION("X-Powers AC300 Ethernet PHY control driver");
MODULE_LICENSE("GPL");
