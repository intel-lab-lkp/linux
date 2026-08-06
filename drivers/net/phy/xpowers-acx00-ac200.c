// SPDX-License-Identifier: GPL-2.0-only
/*
 * X-Powers AC200 Ethernet PHY package backend
 *
 * Copyright (c) 2022 Arm Ltd. (Andre Przywara <andre.przywara@arm.com>)
 * Copyright (C) 2026 James Hilliard <james.hilliard1@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "xpowers-acx00.h"

#define AC200_EPHY_BPS_EFFUSE_OFFSET	3

#define AC200_SYS_EPHY_CTL0_REG			0x0014
#define AC200_EPHY_RESET_DEASSERT		BIT(0)
#define AC200_EPHY_SYSCLK_ENABLE			BIT(1)

#define AC200_SYS_EPHY_CTL1_REG			0x0016
#define AC200_EPHY_MII_IO_ENABLE			BIT(0)

/* AC200-internal copy of the Ethernet PHY calibration eFuse. */
#define AC200_EFUSE_EPHY_REG			0x8004

#define AC200_EPHY_CTL_REG			0x6000
#define AC200_EPHY_SHUTDOWN			BIT(0)
#define AC200_EPHY_CLK_SEL_24_MHZ		BIT(2)
#define AC200_EPHY_PHY_ADDR_MASK			GENMASK(8, 4)
#define AC200_EPHY_RMII_SEL			BIT(11)
#define AC200_EPHY_BPS_EFFUSE_MASK		GENMASK(15, 12)

struct ac200_ephy_ctl {
	struct acx00_ephy_control control;
	struct regmap *regmap;
	struct mutex lock; /* Serializes power sequencing and state. */
	u16 ephy_ctl;
	unsigned int phy_addr;
	phy_interface_t interface;
	bool powered;
};

#if IS_ENABLED(CONFIG_OF_DYNAMIC)
static bool ac200_ephy_node_needs_probe(struct device_node *node)
{
	const char *status;

	return !of_property_read_string(node, "status", &status) &&
	       !strcmp(status, "fail-needs-probe");
}

static int ac200_ephy_add_enable_path(struct device *dev,
				      struct of_changeset *ocs,
				      struct device_node *node,
				      bool *changed)
{
	struct device_node *parent;
	int ret;

	parent = of_get_parent(node);
	if (parent) {
		ret = ac200_ephy_add_enable_path(dev, ocs, parent, changed);
		of_node_put(parent);
		if (ret)
			return ret;
	}

	if (of_device_is_available(node))
		return 0;

	if (!ac200_ephy_node_needs_probe(node))
		return dev_err_probe(dev, -ENODEV,
				     "%pOF is unavailable without fail-needs-probe\n",
				     node);

	ret = of_changeset_update_prop_string(ocs, node, "status", "okay");
	if (!ret)
		*changed = true;

	return ret;
}

static int ac200_ephy_enable_path(struct device *dev,
				  struct device_node *node)
{
	struct of_changeset *ocs;
	bool changed = false;
	int ret;

	ocs = kzalloc_obj(*ocs);
	if (!ocs)
		return -ENOMEM;

	of_changeset_init(ocs);
	ret = ac200_ephy_add_enable_path(dev, ocs, node, &changed);
	if (ret || !changed)
		goto out_destroy;

	ret = of_changeset_apply(ocs);
	if (ret)
		goto out_destroy;

	/* Keep the applied status properties alive for the lifetime of the DT. */
	return 0;

out_destroy:
	of_changeset_destroy(ocs);
	kfree(ocs);
	return ret;
}
#endif

static u16 ac200_ephy_ctl_config(const struct ac200_ephy_ctl *priv)
{
	return priv->ephy_ctl |
		(priv->interface == PHY_INTERFACE_MODE_RMII ?
		 AC200_EPHY_RMII_SEL : 0) |
		FIELD_PREP(AC200_EPHY_PHY_ADDR_MASK, priv->phy_addr);
}

static int ac200_ephy_ctl_power_off_locked(struct ac200_ephy_ctl *priv)
{
	int err;
	int ret;

	if (!priv->powered)
		return 0;

	ret = regmap_write(priv->regmap, AC200_EPHY_CTL_REG,
			   ac200_ephy_ctl_config(priv) | AC200_EPHY_SHUTDOWN);
	err = regmap_write(priv->regmap, AC200_SYS_EPHY_CTL1_REG, 0);
	if (!ret)
		ret = err;
	err = regmap_write(priv->regmap, AC200_SYS_EPHY_CTL0_REG, 0);
	if (!ret)
		ret = err;

	priv->powered = false;

	return ret;
}

static int ac200_ephy_ctl_power_off(struct acx00_ephy_control *control)
{
	struct ac200_ephy_ctl *priv =
		container_of(control, struct ac200_ephy_ctl, control);
	int ret;

	mutex_lock(&priv->lock);
	ret = ac200_ephy_ctl_power_off_locked(priv);
	mutex_unlock(&priv->lock);

	return ret;
}

static int
ac200_ephy_ctl_set_interface(struct acx00_ephy_control *control,
			     phy_interface_t interface)
{
	struct ac200_ephy_ctl *priv =
		container_of(control, struct ac200_ephy_ctl, control);
	u16 value;
	int ret = 0;

	switch (interface) {
	case PHY_INTERFACE_MODE_MII:
		value = 0;
		break;
	case PHY_INTERFACE_MODE_RMII:
		value = AC200_EPHY_RMII_SEL;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&priv->lock);
	if (priv->interface == interface)
		goto out_unlock;

	if (priv->powered)
		ret = regmap_update_bits(priv->regmap, AC200_EPHY_CTL_REG,
					 AC200_EPHY_RMII_SEL, value);
	if (!ret)
		priv->interface = interface;

out_unlock:
	mutex_unlock(&priv->lock);

	return ret;
}

static int ac200_ephy_ctl_power_on(struct acx00_ephy_control *control,
				   unsigned int phy_addr)
{
	struct ac200_ephy_ctl *priv =
		container_of(control, struct ac200_ephy_ctl, control);
	u16 ephy_ctl;
	int ret;

	if (phy_addr > FIELD_MAX(AC200_EPHY_PHY_ADDR_MASK))
		return -EINVAL;

	mutex_lock(&priv->lock);
	if (priv->powered && priv->phy_addr == phy_addr) {
		ret = 0;
		goto out_unlock;
	}
	if (priv->powered) {
		ret = ac200_ephy_ctl_power_off_locked(priv);
		if (ret)
			goto out_unlock;
	}
	priv->phy_addr = phy_addr;

	ephy_ctl = ac200_ephy_ctl_config(priv);

	/* Start from a disabled state before applying the configuration. */
	ret = regmap_write(priv->regmap, AC200_SYS_EPHY_CTL0_REG, 0);
	if (ret)
		goto err_disable;

	ret = regmap_write(priv->regmap, AC200_SYS_EPHY_CTL1_REG,
			   AC200_EPHY_MII_IO_ENABLE);
	if (ret)
		goto err_disable;

	ret = regmap_write(priv->regmap, AC200_EPHY_CTL_REG,
			   ephy_ctl | AC200_EPHY_SHUTDOWN);
	if (ret)
		goto err_disable;

	ret = regmap_write(priv->regmap, AC200_SYS_EPHY_CTL0_REG,
			   AC200_EPHY_RESET_DEASSERT |
			   AC200_EPHY_SYSCLK_ENABLE);
	if (ret)
		goto err_disable;

	fsleep(10000);

	ret = regmap_write(priv->regmap, AC200_EPHY_CTL_REG, ephy_ctl);
	if (ret)
		goto err_disable;

	priv->powered = true;
	goto out_unlock;

err_disable:
	/* Attempt every step of the shutdown sequence after a partial start. */
	priv->powered = true;
	ac200_ephy_ctl_power_off_locked(priv);
out_unlock:
	mutex_unlock(&priv->lock);

	return ret;
}

struct acx00_ephy_control *
ac200_ephy_ctl_create(struct phy_device *phydev,
		      struct device_node *package_node,
		      bool has_calibration, u8 calibration)
{
	struct device *dev = &phydev->mdio.dev;
	struct device_node *ac200_node;
	struct i2c_client *client;
	struct ac200_ephy_ctl *priv;
	unsigned long clk_rate;
	unsigned int internal_calibration;
	u8 bps_effuse_code;
	struct clk *clk;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);
	mutex_init(&priv->lock);

	ac200_node = of_parse_phandle(package_node, "x-powers,ac200", 0);
	if (!ac200_node)
		return ERR_PTR(dev_err_probe(dev, -EINVAL,
					     "missing x-powers,ac200 reference\n"));

#if IS_ENABLED(CONFIG_OF_DYNAMIC)
	ret = ac200_ephy_enable_path(dev, ac200_node);
	if (ret) {
		of_node_put(ac200_node);
		return ERR_PTR(ret);
	}
#endif

	client = of_find_i2c_device_by_node(ac200_node);
	of_node_put(ac200_node);
	if (!client) {
		ret = IS_ENABLED(CONFIG_I2C) ? -EPROBE_DEFER : -ENODEV;
		return ERR_PTR(dev_err_probe(dev, ret,
					     "AC200 device is not registered\n"));
	}

	if (!device_link_add(dev, &client->dev,
			     DL_FLAG_AUTOREMOVE_CONSUMER)) {
		ret = dev_err_probe(dev, -EINVAL,
				    "failed to link AC200 device\n");
		goto out_put_client;
	}

	device_lock(&client->dev);
	if (device_is_bound(&client->dev))
		priv->regmap = dev_get_regmap(&client->dev, NULL);
	device_unlock(&client->dev);
	if (!priv->regmap) {
		ret = dev_err_probe(dev, -EPROBE_DEFER,
				    "AC200 driver is not ready\n");
		goto out_error;
	}

	if (!has_calibration) {
		ret = regmap_read(priv->regmap, AC200_EFUSE_EPHY_REG,
				  &internal_calibration);
		if (ret)
			goto out_error;
		calibration = internal_calibration;
	}

	/* The vendor driver supplies no transfer function beyond this offset. */
	bps_effuse_code = (calibration + AC200_EPHY_BPS_EFFUSE_OFFSET) &
			   FIELD_MAX(AC200_EPHY_BPS_EFFUSE_MASK);
	priv->ephy_ctl =
		FIELD_PREP(AC200_EPHY_BPS_EFFUSE_MASK, bps_effuse_code);
	/* EPHY_MODE and BIST_CLK_EN stay clear for normal operation. */

	clk = clk_get(&client->dev, NULL);
	if (IS_ERR(clk)) {
		ret = PTR_ERR(clk);
		goto out_error;
	}

	clk_rate = clk_get_rate(clk);
	clk_put(clk);

	switch (clk_rate) {
	case 24000000:
		priv->ephy_ctl |= AC200_EPHY_CLK_SEL_24_MHZ;
		break;
	case 27000000:
		break;
	default:
		ret = dev_err_probe(dev, -EINVAL,
				    "unsupported AC200 clock rate %lu Hz\n",
				    clk_rate);
		goto out_put_client;
	}

	priv->control.power_on = ac200_ephy_ctl_power_on;
	priv->control.power_off = ac200_ephy_ctl_power_off;
	priv->control.set_interface = ac200_ephy_ctl_set_interface;
	/* MII is the reset default used until the MAC supplies its interface. */
	priv->interface = PHY_INTERFACE_MODE_MII;
	put_device(&client->dev);

	return &priv->control;

out_error:
	ret = dev_err_probe(dev, ret, "failed to initialize AC200 control\n");
out_put_client:
	put_device(&client->dev);
	return ERR_PTR(ret);
}
