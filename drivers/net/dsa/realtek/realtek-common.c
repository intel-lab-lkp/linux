// SPDX-License-Identifier: GPL-2.0+

#include <linux/module.h>
#include <linux/of_device.h>

#include "realtek.h"
#include "realtek-common.h"

static LIST_HEAD(realtek_variants_list);
static DEFINE_MUTEX(realtek_variants_lock);

void realtek_variant_register(struct realtek_variant_entry *variant_entry)
{
	mutex_lock(&realtek_variants_lock);
	list_add_tail(&variant_entry->list, &realtek_variants_list);
	mutex_unlock(&realtek_variants_lock);
}
EXPORT_SYMBOL_GPL(realtek_variant_register);

void realtek_variant_unregister(struct realtek_variant_entry *variant_entry)
{
	mutex_lock(&realtek_variants_lock);
	list_del(&variant_entry->list);
	mutex_unlock(&realtek_variants_lock);
}
EXPORT_SYMBOL_GPL(realtek_variant_unregister);

const struct realtek_variant *realtek_variant_get(const char *compatible)
{
	const struct realtek_variant *variant = ERR_PTR(-ENOENT);
	struct realtek_variant_entry *variant_entry;

	request_module(compatible);

	mutex_lock(&realtek_variants_lock);
	list_for_each_entry(variant_entry, &realtek_variants_list, list) {
		if (strcmp(compatible, variant_entry->compatible))
			continue;

		if (!try_module_get(variant_entry->owner))
			break;

		variant = variant_entry->variant;
		break;
	}
	mutex_unlock(&realtek_variants_lock);

	return variant;
}
EXPORT_SYMBOL_GPL(realtek_variant_get);

void realtek_variant_put(const struct realtek_variant *var)
{
	struct realtek_variant_entry *variant_entry;

	mutex_lock(&realtek_variants_lock);
	list_for_each_entry(variant_entry, &realtek_variants_list, list) {
		if (variant_entry->variant != var)
			continue;

		if (variant_entry->owner)
			module_put(variant_entry->owner);

		break;
	}
	mutex_unlock(&realtek_variants_lock);
}
EXPORT_SYMBOL_GPL(realtek_variant_put);

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
	const struct of_device_id *match;
	struct realtek_priv *priv;
	struct device_node *np;
	int ret;

	match = of_match_device(dev->driver->of_match_table, dev);
	if (!match)
		return ERR_PTR(-EINVAL);

	var = realtek_variant_get(match->compatible);
	if (IS_ERR(var)) {
		ret = PTR_ERR(var);
		dev_err_probe(dev, ret,
			      "failed to get module for alias '%s'",
			      match->compatible);
		goto err_variant_put;
	}

	priv = devm_kzalloc(dev, size_add(sizeof(*priv), var->chip_data_sz),
			    GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto err_variant_put;
	}

	mutex_init(&priv->map_lock);

	rc.lock_arg = priv;
	priv->map = devm_regmap_init(dev, NULL, priv, &rc);
	if (IS_ERR(priv->map)) {
		ret = PTR_ERR(priv->map);
		dev_err(dev, "regmap init failed: %d\n", ret);
		goto err_variant_put;
	}

	priv->map_nolock = devm_regmap_init(dev, NULL, priv, &rc_nolock);
	if (IS_ERR(priv->map_nolock)) {
		ret = PTR_ERR(priv->map_nolock);
		dev_err(dev, "regmap init failed: %d\n", ret);
		goto err_variant_put;
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
		ret = PTR_ERR(priv->reset);
		dev_err(dev, "failed to get RESET GPIO\n");
		goto err_variant_put;
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
	if (!priv->ds) {
		ret = -ENOMEM;
		goto err_variant_put;
	}

	priv->ds->dev = dev;
	priv->ds->priv = priv;

	return priv;

err_variant_put:
	realtek_variant_put(var);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL(realtek_common_probe);

void realtek_common_remove(struct realtek_priv *priv)
{
	if (!priv)
		return;

	dsa_unregister_switch(priv->ds);
	if (priv->user_mii_bus)
		of_node_put(priv->user_mii_bus->dev.of_node);

	realtek_variant_put(priv->variant);

	/* leave the device reset asserted */
	if (priv->reset)
		gpiod_set_value(priv->reset, 1);
}
EXPORT_SYMBOL(realtek_common_remove);

const struct of_device_id realtek_common_of_match[] = {
#if IS_ENABLED(CONFIG_NET_DSA_REALTEK_RTL8366RB)
	{ .compatible = "realtek,rtl8366rb", },
#endif
#if IS_ENABLED(CONFIG_NET_DSA_REALTEK_RTL8365MB)
	{ .compatible = "realtek,rtl8365mb", },
#endif
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, realtek_common_of_match);
EXPORT_SYMBOL_GPL(realtek_common_of_match);

MODULE_AUTHOR("Luiz Angelo Daros de Luca <luizluca@gmail.com>");
MODULE_DESCRIPTION("Realtek DSA switches common module");
MODULE_LICENSE("GPL");
