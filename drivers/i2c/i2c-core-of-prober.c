// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux I2C core OF component prober code
 *
 * Copyright (C) 2024 Google LLC
 */

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/i2c-of-prober.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>

/*
 * Some devices, such as Google Hana Chromebooks, are produced by multiple
 * vendors each using their preferred components. Such components are all
 * in the device tree. Instead of having all of them enabled and having each
 * driver separately try and probe its device while fighting over shared
 * resources, they can be marked as "fail-needs-probe" and have a prober
 * figure out which one is actually used beforehand.
 *
 * This prober assumes such drop-in parts are on the same I2C bus, have
 * non-conflicting addresses, and can be directly probed by seeing which
 * address responds.
 *
 * TODO:
 * - Support handling common regulators.
 * - Support handling common GPIOs.
 * - Support I2C muxes
 */

static struct device_node *i2c_of_probe_get_i2c_node(struct device *dev, const char *type)
{
	struct device_node *node __free(device_node) = of_find_node_by_name(NULL, type);
	if (!node)
		return dev_err_ptr_probe(dev, -ENODEV, "Could not find %s device node\n", type);

	struct device_node *i2c_node __free(device_node) = of_get_parent(node);
	if (!of_node_name_eq(i2c_node, "i2c"))
		return dev_err_ptr_probe(dev, -EINVAL, "%s device isn't on I2C bus\n", type);

	if (!of_device_is_available(i2c_node))
		return dev_err_ptr_probe(dev, -ENODEV, "I2C controller not available\n");

	return no_free_ptr(i2c_node);
}

static int i2c_of_probe_enable_node(struct device *dev, struct device_node *node)
{
	int ret;

	dev_info(dev, "Enabling %pOF\n", node);

	struct of_changeset *ocs __free(kfree) = kzalloc(sizeof(*ocs), GFP_KERNEL);
	if (!ocs)
		return -ENOMEM;

	of_changeset_init(ocs);
	ret = of_changeset_update_prop_string(ocs, node, "status", "okay");
	if (ret)
		return ret;

	ret = of_changeset_apply(ocs);
	if (ret) {
		/* ocs needs to be explicitly cleaned up before being freed. */
		of_changeset_destroy(ocs);
	} else {
		/*
		 * ocs is intentionally kept around as it needs to
		 * exist as long as the change is applied.
		 */
		void *ptr __always_unused = no_free_ptr(ocs);
	}

	return ret;
}

static const struct i2c_of_probe_ops i2c_of_probe_dummy_ops;

/**
 * i2c_of_probe_component() - probe for devices of "type" on the same i2c bus
 * @dev: Pointer to the &struct device of the caller, only used for dev_printk() messages.
 * @cfg: Pointer to the &struct i2c_of_probe_cfg containing callbacks and other options
 *       for the prober.
 * @ctx: Context data for callbacks.
 *
 * Probe for possible I2C components of the same "type" (&i2c_of_probe_cfg->type)
 * on the same I2C bus that have their status marked as "fail".
 *
 * Assumes that across the entire device tree the only instances of nodes
 * prefixed with "type" are the ones that need handling for second source
 * components. In other words, if "type" is "touchscreen", then all device
 * nodes named "touchscreen*" are the ones that need probing. There cannot
 * be another "touchscreen" node that is already enabled.
 *
 * Assumes that for each "type" of component, only one actually exists. In
 * other words, only one matching and existing device will be enabled.
 *
 * Context: Process context only. Does non-atomic I2C transfers.
 *          Should only be used from a driver probe function, as the function
 *          can return -EPROBE_DEFER if the I2C adapter or other resources
 *          are unavailable.
 * Return: 0 on success or no-op, error code otherwise.
 *         A no-op can happen when it seems like the device tree already
 *         has components of the type to be probed already enabled. This
 *         can happen when the device tree had not been updated to mark
 *         the status of the to-be-probed components as "fail". Or this
 *         function was already run with the same parameters and succeeded
 *         in enabling a component. The latter could happen if the user
 *         had multiple types of components to probe, and one of them down
 *         the list caused a deferred probe. This is expected behavior.
 */
int i2c_of_probe_component(struct device *dev, const struct i2c_of_probe_cfg *cfg, void *ctx)
{
	const struct i2c_of_probe_ops *ops;
	const char *type;
	struct device_node *i2c_node;
	struct i2c_adapter *i2c;
	int ret;

	if (!cfg)
		return -EINVAL;

	ops = cfg->ops ?: &i2c_of_probe_dummy_ops;
	type = cfg->type;

	i2c_node = i2c_of_probe_get_i2c_node(dev, type);
	if (IS_ERR(i2c_node))
		return PTR_ERR(i2c_node);

	for_each_child_of_node_with_prefix(i2c_node, node, type) {
		if (!of_device_is_available(node))
			continue;

		/*
		 * Device tree has component already enabled. Either the
		 * device tree isn't supported or we already probed once.
		 */
		ret = 0;
		goto out_put_i2c_node;
	}

	i2c = of_get_i2c_adapter_by_node(i2c_node);
	if (!i2c) {
		ret = dev_err_probe(dev, -EPROBE_DEFER, "Couldn't get I2C adapter\n");
		goto out_put_i2c_node;
	}

	/* Grab resources */
	ret = 0;
	if (ops->get_resources)
		ret = ops->get_resources(dev, i2c_node, ctx);
	if (ret)
		goto out_put_i2c_adapter;

	/* Enable resources */
	if (ops->enable)
		ret = ops->enable(dev, ctx);
	if (ret)
		goto out_release_resources;

	ret = 0;
	for_each_child_of_node_with_prefix(i2c_node, node, type) {
		union i2c_smbus_data data;
		u32 addr;

		if (of_property_read_u32(node, "reg", &addr))
			continue;
		if (i2c_smbus_xfer(i2c, addr, 0, I2C_SMBUS_READ, 0, I2C_SMBUS_BYTE, &data) < 0)
			continue;

		/* Found a device that is responding */
		if (ops->free_resources_early)
			ops->free_resources_early(ctx);
		ret = i2c_of_probe_enable_node(dev, node);
		break;
	}

	if (ops->cleanup)
		ops->cleanup(dev, ctx);
out_release_resources:
	if (ops->free_resources_late)
		ops->free_resources_late(ctx);
out_put_i2c_adapter:
	i2c_put_adapter(i2c);
out_put_i2c_node:
	of_node_put(i2c_node);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(i2c_of_probe_component, I2C_OF_PROBER);
