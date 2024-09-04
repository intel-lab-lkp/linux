// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux I2C core OF component prober code
 *
 * Copyright (C) 2024 Google LLC
 */

#include <linux/bitmap.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/types.h>

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
 * - Support inverted polarity GPIOs, such as electrical high to "disable".
 *   Seen on some OmniVision camera sensors.
 * - Support I2C muxes
 */

struct i2c_of_probe_data {
	const struct i2c_of_probe_opts *opts;
	struct gpio_descs *gpiods;
	struct regulator_bulk_data *regulators;
	unsigned int regulators_num;
};

/* Returns number of regulator supplies found for node, or error. */
static int i2c_of_probe_get_regulators(struct device *dev, struct device_node *node,
				       struct i2c_of_probe_data *data)
{
	struct regulator_bulk_data *tmp, *new_regulators;
	int ret;

	ret = of_regulator_bulk_get_all(dev, node, &tmp);
	if (ret < 0) {
		return ret;
	} else if (ret == 0) {
		/*
		 * It's entirely possible for a device node to not have
		 * regulator supplies. While it doesn't make sense from
		 * a hardware perspective, the supplies could be always
		 * on or otherwise not modeled in the device tree, but
		 * the device would still work.
		 */
		return ret;
	}

	if (!data->regulators) {
		data->regulators = tmp;
		data->regulators_num = ret;
		return ret;
	};

	new_regulators = krealloc_array(data->regulators, (data->regulators_num + ret),
					sizeof(*tmp), GFP_KERNEL);
	if (!new_regulators) {
		regulator_bulk_free(ret, tmp);
		return -ENOMEM;
	}

	data->regulators = new_regulators;
	memcpy(&data->regulators[data->regulators_num], tmp, sizeof(*tmp) * ret);
	data->regulators_num += ret;

	return ret;
}

static void i2c_of_probe_free_regulators(struct i2c_of_probe_data *data)
{
	regulator_bulk_free(data->regulators_num, data->regulators);
	data->regulators_num = 0;
	data->regulators = NULL;
};

/*
 * Returns 1 if property is GPIO and GPIO successfully requested,
 * 0 if not a GPIO property, or error if request for GPIO failed.
 */
static int i2c_of_probe_get_gpiod(struct device_node *node, struct property *prop,
				  struct i2c_of_probe_data *data)
{
	struct fwnode_handle *fwnode = of_fwnode_handle(node);
	struct gpio_descs *gpiods;
	struct gpio_desc *gpiod;
	char propname[32]; /* 32 is max size of property name */
	char *con_id = NULL;
	size_t new_size;
	int len, ret;

	len = gpio_get_property_name_length(prop->name);
	if (len < 0)
		return 0;

	ret = strscpy(propname, prop->name);
	if (ret < 0) {
		pr_err("%pOF: length of GPIO name \"%s\" exceeds current limit\n",
		       node, prop->name);
		return -EINVAL;
	}

	if (len > 0) {
		/* "len < ARRAY_SIZE(propname)" guaranteed by strscpy() above */
		propname[len] = '\0';
		con_id = propname;
	}

	/*
	 * GPIO descriptors are not reference counted. GPIOD_FLAGS_BIT_NONEXCLUSIVE
	 * can't differentiate between GPIOs shared between devices to be probed and
	 * other devices (which is incorrect). If the initial request fails with
	 * -EBUSY, retry with GPIOD_FLAGS_BIT_NONEXCLUSIVE and see if it matches
	 * any existing ones.
	 */
	gpiod = fwnode_gpiod_get_index(fwnode, con_id, 0, GPIOD_ASIS, "i2c-of-prober");
	if (IS_ERR(gpiod)) {
		if (PTR_ERR(gpiod) != -EBUSY || !data->gpiods)
			return PTR_ERR(gpiod);

		gpiod = fwnode_gpiod_get_index(fwnode, con_id, 0,
					       GPIOD_ASIS | GPIOD_FLAGS_BIT_NONEXCLUSIVE,
					       "i2c-of-prober");
		for (unsigned int i = 0; i < data->gpiods->ndescs; i++)
			if (gpiod == data->gpiods->desc[i])
				return 1;

		return -EBUSY;
	}

	new_size = struct_size(gpiods, desc, data->gpiods ? data->gpiods->ndescs + 1 : 1);
	gpiods = krealloc(data->gpiods, new_size, GFP_KERNEL);
	if (!gpiods) {
		gpiod_put(gpiod);
		return -ENOMEM;
	}

	data->gpiods = gpiods;
	data->gpiods->desc[data->gpiods->ndescs++] = gpiod;

	return 1;
}

/*
 * This is split into two functions because in the normal flow the GPIOs
 * have to be released before the actual driver probes so that the latter
 * can acquire them.
 */
static void i2c_of_probe_free_gpios(struct i2c_of_probe_data *data)
{
	if (data->gpiods)
		gpiod_put_array(data->gpiods);
	data->gpiods = NULL;
}

static void i2c_of_probe_free_res(struct i2c_of_probe_data *data)
{
	i2c_of_probe_free_gpios(data);
	i2c_of_probe_free_regulators(data);
}

static int i2c_of_probe_get_res(struct device *dev, struct device_node *node,
				struct i2c_of_probe_data *data)
{
	struct property *prop;
	int ret;

	ret = i2c_of_probe_get_regulators(dev, node, data);
	if (ret < 0) {
		dev_err_probe(dev, ret, "Failed to get regulator supplies from %pOF\n", node);
		goto err_cleanup;
	}

	for_each_property_of_node(node, prop) {
		dev_dbg(dev, "Trying property %pOF/%s\n", node, prop->name);

		/* GPIOs */
		ret = i2c_of_probe_get_gpiod(node, prop, data);
		if (ret < 0) {
			dev_err_probe(dev, ret, "Failed to get GPIO from %pOF/%s\n",
				      node, prop->name);
			goto err_cleanup;
		}
	}

	return 0;

err_cleanup:
	i2c_of_probe_free_res(data);
	return ret;
}

static int i2c_of_probe_enable_regulators(struct device *dev, struct i2c_of_probe_data *data)
{
	int ret;

	dev_dbg(dev, "Enabling regulator supplies\n");

	ret = regulator_bulk_enable(data->regulators_num, data->regulators);
	if (ret)
		return ret;

	msleep(data->opts->post_power_on_delay_ms);

	return 0;
}

static void i2c_of_probe_disable_regulators(struct i2c_of_probe_data *data)
{
	regulator_bulk_disable(data->regulators_num, data->regulators);
}

static int i2c_of_probe_set_gpios(struct device *dev, struct i2c_of_probe_data *data)
{
	int ret;
	int gpio_i;

	if (!data->gpiods)
		return 0;

	for (gpio_i = 0; gpio_i < data->gpiods->ndescs; gpio_i++) {
		/*
		 * "reset" GPIOs normally have opposite polarity compared to
		 * "enable" GPIOs. Instead of parsing the flags again, simply
		 * set the raw value to high.
		 */
		dev_dbg(dev, "Setting GPIO %d\n", gpio_i);
		ret = gpiod_direction_output_raw(data->gpiods->desc[gpio_i], 1);
		if (ret)
			goto disable_gpios;
	}

	msleep(data->opts->post_reset_deassert_delay_ms);

	return 0;

disable_gpios:
	for (gpio_i--; gpio_i >= 0; gpio_i--)
		gpiod_set_raw_value_cansleep(data->gpiods->desc[gpio_i], 0);

	return ret;
}

static int i2c_of_probe_enable_res(struct device *dev, struct i2c_of_probe_data *data)
{
	int ret;

	ret = i2c_of_probe_enable_regulators(dev, data);
	if (ret)
		return ret;

	ret = i2c_of_probe_set_gpios(dev, data);
	if (ret)
		goto err_disable_regulators;

	return 0;

err_disable_regulators:
	i2c_of_probe_disable_regulators(data);
	return ret;
}

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

static const struct i2c_of_probe_opts i2c_of_probe_opts_default = {
	/* largest post-power-on pre-reset-deassert delay seen among drivers */
	.post_power_on_delay_ms = 500,
	/* largest post-reset-deassert delay seen in tree for Elan I2C HID */
	.post_reset_deassert_delay_ms = 300,
};

/**
 * i2c_of_probe_component() - probe for devices of "type" on the same i2c bus
 * @dev: &struct device of the caller, only used for dev_* printk messages
 * @type: a string to match the device node name prefix to probe for
 * @opts: &struct i2c_of_probe_data containing tweakable options for the prober.
 *	  Defaults are used if this is %NULL.
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
int i2c_of_probe_component(struct device *dev, const char *type, const struct i2c_of_probe_opts *opts)
{
	struct i2c_of_probe_data probe_data = { .opts = opts ?: &i2c_of_probe_opts_default };
	struct i2c_adapter *i2c;
	int ret;

	struct device_node *i2c_node __free(device_node) = i2c_of_probe_get_i2c_node(dev, type);
	if (IS_ERR(i2c_node))
		return PTR_ERR(i2c_node);

	for_each_child_of_node_with_prefix_scoped(i2c_node, node, type) {
		if (!of_device_is_available(node))
			continue;

		/*
		 * Device tree has component already enabled. Either the
		 * device tree isn't supported or we already probed once.
		 */
		return 0;
	}

	i2c = of_get_i2c_adapter_by_node(i2c_node);
	if (!i2c)
		return dev_err_probe(dev, -EPROBE_DEFER, "Couldn't get I2C adapter\n");

	/* Grab resources */
	for_each_child_of_node_with_prefix_scoped(i2c_node, node, type) {
		u32 addr;

		if (of_property_read_u32(node, "reg", &addr))
			continue;

		dev_dbg(dev, "Requesting resources for %pOF\n", node);
		ret = i2c_of_probe_get_res(dev, node, &probe_data);
		if (ret)
			return ret;
	}

	dev_dbg(dev, "Resources: # of regulator supplies = %d\n", probe_data.regulators_num);
	dev_dbg(dev, "Resources: # of GPIOs = %d\n",
		probe_data.gpiods ? probe_data.gpiods->ndescs : 0);

	/* Enable resources */
	ret = i2c_of_probe_enable_res(dev, &probe_data);
	if (ret) {
		i2c_of_probe_free_res(&probe_data);
		return dev_err_probe(dev, ret, "Failed to enable resources\n");
	}

	ret = 0;
	for_each_child_of_node_with_prefix_scoped(i2c_node, node, type) {
		union i2c_smbus_data data;
		u32 addr;

		if (of_property_read_u32(node, "reg", &addr))
			continue;
		if (i2c_smbus_xfer(i2c, addr, 0, I2C_SMBUS_READ, 0, I2C_SMBUS_BYTE, &data) < 0)
			continue;

		/* Found a device that is responding */
		i2c_of_probe_free_gpios(&probe_data);
		ret = i2c_of_probe_enable_node(dev, node);
		break;
	}

	i2c_of_probe_disable_regulators(&probe_data);
	i2c_of_probe_free_res(&probe_data);
	i2c_put_adapter(i2c);

	return ret;
}
EXPORT_SYMBOL_GPL(i2c_of_probe_component);
