// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux I2C core OF component prober code
 *
 * Copyright (C) 2024 Google LLC
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
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
 * - Support handling common regulators and GPIOs.
 * - Support I2C muxes
 */

/**
 * i2c_of_probe_component() - probe for devices of "type" on the same i2c bus
 * @dev: &struct device of the caller, only used for dev_* printk messages
 * @type: a string to match the device node name prefix to probe for
 *
 * Probe for possible I2C components of the same "type" on the same I2C bus
 * that have their status marked as "fail".
 *
 * Assumes that across the entire device tree the only instances of nodes
 * prefixed with "type" are the ones that need handling for second source
 * components. In other words, if type is "touchscreen", then all device
 * nodes named "touchscreen*" are the ones that need probing. There cannot
 * be another "touchscreen" node that is already enabled.
 *
 * Assumes that for each "type" of component, only one actually exists. In
 * other words, only one matching and existing device will be enabled.
 *
 * Context: Process context only. Does non-atomic I2C transfers.
 *          Should only be used from a driver probe function, as the function
 *          can return -EPROBE_DEFER if the I2C adapter is unavailable.
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
int i2c_of_probe_component(struct device *dev, const char *type)
{
	struct device_node *node, *i2c_node;
	struct i2c_adapter *i2c;
	struct of_changeset *ocs = NULL;
	int ret;

	node = of_find_node_by_name(NULL, type);
	if (!node)
		return dev_err_probe(dev, -ENODEV, "Could not find %s device node\n", type);

	i2c_node = of_get_next_parent(node);
	if (!of_node_name_eq(i2c_node, "i2c")) {
		of_node_put(i2c_node);
		return dev_err_probe(dev, -EINVAL, "%s device isn't on I2C bus\n", type);
	}

	if (!of_device_is_available(i2c_node)) {
		of_node_put(i2c_node);
		return dev_err_probe(dev, -ENODEV, "I2C controller not available\n");
	}

	for_each_child_of_node(i2c_node, node) {
		if (!of_node_name_prefix(node, type))
			continue;
		if (of_device_is_available(node)) {
			/*
			 * Device tree has component already enabled. Either the
			 * device tree isn't supported or we already probed once.
			 */
			of_node_put(node);
			of_node_put(i2c_node);
			return 0;
		}
	}

	i2c = of_get_i2c_adapter_by_node(i2c_node);
	if (!i2c) {
		of_node_put(i2c_node);
		return dev_err_probe(dev, -EPROBE_DEFER, "Couldn't get I2C adapter\n");
	}

	ret = 0;
	for_each_child_of_node(i2c_node, node) {
		union i2c_smbus_data data;
		u32 addr;

		if (!of_node_name_prefix(node, type))
			continue;
		if (of_property_read_u32(node, "reg", &addr))
			continue;
		if (i2c_smbus_xfer(i2c, addr, 0, I2C_SMBUS_READ, 0, I2C_SMBUS_BYTE, &data) < 0)
			continue;

		break;
	}

	/* Found a device that is responding */
	if (node) {
		dev_info(dev, "Enabling %pOF\n", node);

		ocs = kzalloc(sizeof(*ocs), GFP_KERNEL);
		if (!ocs) {
			ret = -ENOMEM;
			goto err_put_node;
		}

		of_changeset_init(ocs);
		ret = of_changeset_update_prop_string(ocs, node, "status", "okay");
		if (ret)
			goto err_free_ocs;
		ret = of_changeset_apply(ocs);
		if (ret)
			goto err_destroy_ocs;

		of_node_put(node);
	}

	i2c_put_adapter(i2c);
	of_node_put(i2c_node);

	return 0;

err_destroy_ocs:
	of_changeset_destroy(ocs);
err_free_ocs:
	kfree(ocs);
err_put_node:
	of_node_put(node);
	i2c_put_adapter(i2c);
	of_node_put(i2c_node);
	return ret;
}
EXPORT_SYMBOL_GPL(i2c_of_probe_component);
