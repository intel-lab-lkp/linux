// SPDX-License-Identifier: GPL-2.0+

#include <linux/module.h>

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

	priv->ds = devm_kzalloc(priv->dev, sizeof(*priv->ds), GFP_KERNEL);
	if (!priv->ds)
		return -ENOMEM;

	priv->ds->priv = priv;
	priv->ds->dev = priv->dev;
	priv->ds->ops = priv->ds_ops;
	priv->ds->num_ports = priv->num_ports;

	ret = dsa_register_switch(priv->ds);
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

	/* leave the device reset asserted */
	if (priv->reset)
		gpiod_set_value(priv->reset, 1);
}
EXPORT_SYMBOL(realtek_common_remove);

MODULE_AUTHOR("Luiz Angelo Daros de Luca <luizluca@gmail.com>");
MODULE_DESCRIPTION("Realtek DSA switches common module");
MODULE_LICENSE("GPL");
