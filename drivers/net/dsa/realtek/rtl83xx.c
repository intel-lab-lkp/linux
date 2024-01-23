// SPDX-License-Identifier: GPL-2.0+

#include <linux/module.h>

#include "realtek.h"
#include "rtl83xx.h"

/**
 * rtl83xx_lock() - Locks the mutex used by regmaps
 * @ctx: realtek_priv pointer
 *
 * This function is passed to regmap to be used as the lock function.
 * It is also used externally to block regmap before executing multiple
 * operations that must happen in sequence (which will use
 * realtek_priv.map_nolock instead).
 *
 * Context: Any context. Holds priv->map_lock lock.
 * Return: nothing
 */
void rtl83xx_lock(void *ctx)
{
	struct realtek_priv *priv = ctx;

	mutex_lock(&priv->map_lock);
}
EXPORT_SYMBOL_NS_GPL(rtl83xx_lock, REALTEK_DSA);

/**
 * rtl83xx_unlock() - Unlocks the mutex used by regmaps
 * @ctx: realtek_priv pointer
 *
 * This function unlocks the lock acquired by rtl83xx_lock.
 *
 * Context: Any context. Releases priv->map_lock lock
 * Return: nothing
 */
void rtl83xx_unlock(void *ctx)
{
	struct realtek_priv *priv = ctx;

	mutex_unlock(&priv->map_lock);
}
EXPORT_SYMBOL_NS_GPL(rtl83xx_unlock, REALTEK_DSA);

/**
 * rtl83xx_probe() - probe a Realtek switch
 * @dev: the device being probed
 * @interface_info: reg read/write methods for a specific management interface.
 *
 * This function initializes realtek_priv and read data from the device-tree
 * node. The switch is hard resetted if a method is provided.
 *
 * Context: Any context.
 * Return: Pointer to the realtek_priv or ERR_PTR() in case of failure.
 *
 * The realtek_priv pointer does not need to be freed as it is controlled by
 * devres.
 *
 */
struct realtek_priv *
rtl83xx_probe(struct device *dev,
	      const struct realtek_interface_info *interface_info)
{
	const struct realtek_variant *var;
	struct realtek_priv *priv;
	struct regmap_config rc = {
			.reg_bits = 10, /* A4..A0 R4..R0 */
			.val_bits = 16,
			.reg_stride = 1,
			.max_register = 0xffff,
			.reg_format_endian = REGMAP_ENDIAN_BIG,
			.reg_read = interface_info->reg_read,
			.reg_write = interface_info->reg_write,
			.cache_type = REGCACHE_NONE,
			.lock = rtl83xx_lock,
			.unlock = rtl83xx_unlock,
	};
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

	rc.disable_locking = true;
	priv->map_nolock = devm_regmap_init(dev, NULL, priv, &rc);
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
EXPORT_SYMBOL_NS_GPL(rtl83xx_probe, REALTEK_DSA);

/**
 * rtl83xx_register_switch() - detects and register a switch
 * @priv: realtek_priv pointer
 *
 * This function first checks the switch chip ID and register a DSA
 * switch.
 *
 * Context: Any context. Takes and releases priv->map_lock.
 * Return: 0 on success, negative value for failure.
 */
int rtl83xx_register_switch(struct realtek_priv *priv)
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
EXPORT_SYMBOL_NS_GPL(rtl83xx_register_switch, REALTEK_DSA);

/**
 * rtl83xx_remove() - Cleanup a realtek switch driver
 * @ctx: realtek_priv pointer
 *
 * If a method is provided, this function asserts the hard reset of the switch
 * in order to avoid leaking traffic when the driver is gone.
 *
 * Context: Any context.
 * Return: nothing
 */
void rtl83xx_remove(struct realtek_priv *priv)
{
	if (!priv)
		return;

	dsa_unregister_switch(priv->ds);

	/* leave the device reset asserted */
	if (priv->reset)
		gpiod_set_value(priv->reset, 1);
}
EXPORT_SYMBOL_NS_GPL(rtl83xx_remove, REALTEK_DSA);

MODULE_AUTHOR("Luiz Angelo Daros de Luca <luizluca@gmail.com>");
MODULE_AUTHOR("Linus Walleij <linus.walleij@linaro.org>");
MODULE_DESCRIPTION("Realtek DSA switches common module");
MODULE_LICENSE("GPL");
