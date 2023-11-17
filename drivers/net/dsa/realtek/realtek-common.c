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

struct realtek_priv *realtek_common_probe(struct device *dev,
		struct regmap_config rc, struct regmap_config rc_nolock)
{
	const struct realtek_variant *var;
	struct realtek_priv *priv;
	struct device_node *np;
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

	np = dev->of_node;

	priv->leds_disabled = of_property_read_bool(np, "realtek,disable-leds");

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

	priv->ds = devm_kzalloc(dev, sizeof(*priv->ds), GFP_KERNEL);
	if (!priv->ds)
		return ERR_PTR(-ENOMEM);

	priv->ds->dev = dev;
	priv->ds->priv = priv;

	return priv;
}
EXPORT_SYMBOL(realtek_common_probe);

void realtek_common_remove(struct realtek_priv *priv)
{
	if (!priv)
		return;

	dsa_unregister_switch(priv->ds);
	if (priv->user_mii_bus)
		of_node_put(priv->user_mii_bus->dev.of_node);

	/* leave the device reset asserted */
	if (priv->reset)
		gpiod_set_value(priv->reset, 1);
}
EXPORT_SYMBOL(realtek_common_remove);

const struct of_device_id realtek_common_of_match[] = {
#if IS_ENABLED(CONFIG_NET_DSA_REALTEK_RTL8366RB)
	{ .compatible = "realtek,rtl8366rb", .data = &rtl8366rb_variant, },
#endif
#if IS_ENABLED(CONFIG_NET_DSA_REALTEK_RTL8365MB)
	{ .compatible = "realtek,rtl8365mb", .data = &rtl8365mb_variant, },
#endif
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, realtek_common_of_match);
EXPORT_SYMBOL_GPL(realtek_common_of_match);

MODULE_AUTHOR("Luiz Angelo Daros de Luca <luizluca@gmail.com>");
MODULE_DESCRIPTION("Realtek DSA switches common module");
MODULE_LICENSE("GPL");
