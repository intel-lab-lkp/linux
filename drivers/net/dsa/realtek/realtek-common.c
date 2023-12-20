// SPDX-License-Identifier: GPL-2.0+

#include <linux/module.h>
#include <linux/of_mdio.h>

#include "realtek.h"
#include "realtek-common.h"

void realtek_common_lock(void *ctx)
{
	struct realtek_priv *priv = ctx;

	mutex_lock(&priv->map_lock);
}
EXPORT_SYMBOL_GPL(realtek_common_lock);

void realtek_common_unlock(void *ctx)
{
	struct realtek_priv *priv = ctx;

	mutex_unlock(&priv->map_lock);
}
EXPORT_SYMBOL_GPL(realtek_common_unlock);

static int realtek_common_user_mdio_read(struct mii_bus *bus, int addr,
					 int regnum)
{
	struct realtek_priv *priv = bus->priv;

	return priv->ops->phy_read(priv, addr, regnum);
}

static int realtek_common_user_mdio_write(struct mii_bus *bus, int addr,
					  int regnum, u16 val)
{
	struct realtek_priv *priv = bus->priv;

	return priv->ops->phy_write(priv, addr, regnum, val);
}

int realtek_common_setup_user_mdio(struct dsa_switch *ds)
{
	const char *compatible = "realtek,smi-mdio";
	struct realtek_priv *priv =  ds->priv;
	struct device_node *phy_node;
	struct device_node *mdio_np;
	struct dsa_port *dp;
	int ret;

	mdio_np = of_get_child_by_name(priv->dev->of_node, "mdio");
	if (!mdio_np) {
		mdio_np = of_get_compatible_child(priv->dev->of_node, compatible);
		if (!mdio_np) {
			dev_err(priv->dev, "no MDIO bus node\n");
			return -ENODEV;
		}
	}

	priv->user_mii_bus = devm_mdiobus_alloc(priv->dev);
	if (!priv->user_mii_bus) {
		ret = -ENOMEM;
		goto err_put_node;
	}
	priv->user_mii_bus->priv = priv;
	priv->user_mii_bus->name = "Realtek user MII";
	priv->user_mii_bus->read = realtek_common_user_mdio_read;
	priv->user_mii_bus->write = realtek_common_user_mdio_write;
	snprintf(priv->user_mii_bus->id, MII_BUS_ID_SIZE, "Realtek-%d",
		 ds->index);
	priv->user_mii_bus->parent = priv->dev;

	/* When OF describes the MDIO, connecting ports with phy-handle,
	 * ds->user_mii_bus should not be used *
	 */
	dsa_switch_for_each_user_port(dp, ds) {
		phy_node = of_parse_phandle(dp->dn, "phy-handle", 0);
		of_node_put(phy_node);
		if (phy_node)
			continue;

		dev_warn(priv->dev,
			 "DS user_mii_bus in use as '%s' is missing phy-handle",
			 dp->name);
		ds->user_mii_bus = priv->user_mii_bus;
		break;
	}

	ret = devm_of_mdiobus_register(priv->dev, priv->user_mii_bus, mdio_np);
	if (ret) {
		dev_err(priv->dev, "unable to register MDIO bus %s\n",
			priv->user_mii_bus->id);
		goto err_put_node;
	}

	return 0;

err_put_node:
	of_node_put(mdio_np);

	return ret;
}
EXPORT_SYMBOL_GPL(realtek_common_setup_user_mdio);

/* sets up driver private data struct, sets up regmaps, parse common device-tree
 * properties and finally issues a hardware reset.
 */
struct realtek_priv *
realtek_common_probe(struct device *dev, struct regmap_config rc,
		     struct regmap_config rc_nolock)
{
	const struct realtek_variant *var;
	struct realtek_priv *priv;
	int ret;

	var = of_device_get_match_data(dev);
	if (!var)
		return ERR_PTR(-EINVAL);

	priv = devm_kzalloc(dev, size_add(sizeof(*priv), var->chip_data_sz),
			    GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	mutex_init(&priv->map_lock);

	rc.lock_arg = priv;
	priv->map = devm_regmap_init(dev, NULL, priv, &rc);
	if (IS_ERR(priv->map)) {
		ret = PTR_ERR(priv->map);
		dev_err(dev, "regmap init failed: %d\n", ret);
		return ERR_PTR(ret);
	}

	priv->map_nolock = devm_regmap_init(dev, NULL, priv, &rc_nolock);
	if (IS_ERR(priv->map_nolock)) {
		ret = PTR_ERR(priv->map_nolock);
		dev_err(dev, "regmap init failed: %d\n", ret);
		return ERR_PTR(ret);
	}

	/* Link forward and backward */
	priv->dev = dev;
	priv->variant = var;
	priv->ops = var->ops;
	priv->chip_data = (void *)priv + sizeof(*priv);

	dev_set_drvdata(dev, priv);
	spin_lock_init(&priv->lock);

	priv->leds_disabled = of_property_read_bool(dev->of_node,
						    "realtek,disable-leds");

	/* TODO: if power is software controlled, set up any regulators here */

	priv->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(priv->reset)) {
		dev_err(dev, "failed to get RESET GPIO\n");
		return ERR_CAST(priv->reset);
	}
	if (priv->reset) {
		gpiod_set_value(priv->reset, 1);
		dev_dbg(dev, "asserted RESET\n");
		msleep(REALTEK_HW_STOP_DELAY);
		gpiod_set_value(priv->reset, 0);
		msleep(REALTEK_HW_START_DELAY);
		dev_dbg(dev, "deasserted RESET\n");
	}

	return priv;
}
EXPORT_SYMBOL(realtek_common_probe);

/* Detects the realtek switch id/version and registers the dsa switch.
 */
int realtek_common_register_switch(struct realtek_priv *priv)
{
	int ret;

	ret = priv->ops->detect(priv);
	if (ret) {
		dev_err_probe(priv->dev, ret, "unable to detect switch\n");
		return ret;
	}

	priv->ds.priv = priv;
	priv->ds.dev = priv->dev;
	priv->ds.ops = priv->variant->ds_ops;
	priv->ds.num_ports = priv->num_ports;

	ret = dsa_register_switch(&priv->ds);
	if (ret) {
		dev_err_probe(priv->dev, ret, "unable to register switch\n");
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(realtek_common_register_switch);

void realtek_common_remove(struct realtek_priv *priv)
{
	if (!priv)
		return;

	dsa_unregister_switch(&priv->ds);

	if (priv->user_mii_bus)
		of_node_put(priv->user_mii_bus->dev.of_node);

	/* leave the device reset asserted */
	if (priv->reset)
		gpiod_set_value(priv->reset, 1);
}
EXPORT_SYMBOL(realtek_common_remove);

MODULE_AUTHOR("Luiz Angelo Daros de Luca <luizluca@gmail.com>");
MODULE_AUTHOR("Linus Walleij <linus.walleij@linaro.org>");
MODULE_DESCRIPTION("Realtek DSA switches common module");
MODULE_LICENSE("GPL");
