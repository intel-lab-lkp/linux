// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux I2C core OF component prober code
 *
 * Copyright (C) 2024 Google LLC
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
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
 * - Support I2C muxes
 */

/*
 * While 8 seems like a small number, especially when probing many component
 * options, in practice all the options will have the same resources. The
 * code getting the resources below does deduplication to avoid conflicts.
 */
#define RESOURCE_MAX 8

struct i2c_of_probe_data {
	struct of_phandle_args gpio_phandles[RESOURCE_MAX];
	unsigned int gpio_phandles_num;
	struct gpio_desc *gpiods[RESOURCE_MAX];
	unsigned int gpiods_num;
	struct regulator *regulators[RESOURCE_MAX];
	unsigned int regulators_num;
};

#define REGULATOR_SUFFIX "-supply"

/* Returns 1 if regulator found for property, 0 if not, or error. */
static int i2c_of_probe_get_regulator(struct device_node *node, struct property *prop,
				      struct i2c_of_probe_data *data)
{
	struct regulator *regulator = NULL;
	char con[32]; /* 32 is max size of property name */
	char *p;

	p = strstr(prop->name, REGULATOR_SUFFIX);
	if (!p)
		return 0;

	if (strcmp(p, REGULATOR_SUFFIX))
		return 0;

	strscpy(con, prop->name, p - prop->name + 1);
	regulator = regulator_of_get_optional(node, con);
	/* DT lookup should never return -ENODEV */
	if (IS_ERR(regulator))
		return PTR_ERR(regulator);

	for (int i = 0; i < data->regulators_num; i++)
		if (regulator_is_equal(regulator, data->regulators[i])) {
			regulator_put(regulator);
			regulator = NULL;
			break;
		}

	if (!regulator)
		return 1;

	if (data->regulators_num == ARRAY_SIZE(data->regulators)) {
		regulator_put(regulator);
		return -ENOMEM;
	}

	data->regulators[data->regulators_num++] = regulator;

	return 1;
}

#define GPIO_SUFFIX "-gpio"

/* Returns 1 if GPIO found for property, 0 if not, or error. */
static int i2c_of_probe_get_gpiod(struct device_node *node, struct property *prop,
				  struct i2c_of_probe_data *data)
{
	struct fwnode_handle *fwnode = of_fwnode_handle(node);
	struct gpio_desc *gpiod = NULL;
	char con[32]; /* 32 is max size of property name */
	char *con_id = NULL;
	char *p;
	struct of_phandle_args phargs;
	int ret;
	bool duplicate_found;

	p = strstr(prop->name, GPIO_SUFFIX);
	if (p) {
		strscpy(con, prop->name, p - prop->name + 1);
		con_id = con;
	} else if (strcmp(prop->name, "gpio") && strcmp(prop->name, "gpios")) {
		return 0;
	}

	ret = of_parse_phandle_with_args_map(node, prop->name, "gpio", 0, &phargs);
	if (ret)
		return ret;

	/*
	 * GPIO descriptors are not reference counted. GPIOD_FLAGS_BIT_NONEXCLUSIVE
	 * can't differentiate between GPIOs shared between devices to be probed and
	 * other devices (which is incorrect). Instead we check the parsed phandle
	 * for duplicates. Ignore the flags (the last arg) in this case.
	 */
	phargs.args[phargs.args_count - 1] = 0;
	duplicate_found = false;
	for (int i = 0; i < data->gpio_phandles_num; i++)
		if (of_phandle_args_equal(&phargs, &data->gpio_phandles[i])) {
			duplicate_found = true;
			break;
		}

	if (duplicate_found) {
		of_node_put(phargs.np);
		return 1;
	}

	gpiod = fwnode_gpiod_get_index(fwnode, con_id, 0, GPIOD_ASIS, "i2c-of-prober");
	if (IS_ERR(gpiod)) {
		of_node_put(phargs.np);
		return PTR_ERR(gpiod);
	}

	if (data->gpiods_num == ARRAY_SIZE(data->gpiods)) {
		of_node_put(phargs.np);
		gpiod_put(gpiod);
		return -ENOMEM;
	}

	memcpy(&data->gpio_phandles[data->gpio_phandles_num++], &phargs, sizeof(phargs));
	data->gpiods[data->gpiods_num++] = gpiod;

	return 1;
}

/*
 * This is split into two functions because in the normal flow the GPIOs
 * have to be released before the actual driver probes so that the latter
 * can acquire them.
 */
static void i2c_of_probe_free_gpios(struct i2c_of_probe_data *data)
{
	for (int i = data->gpio_phandles_num - 1; i >= 0; i--)
		of_node_put(data->gpio_phandles[i].np);
	data->gpio_phandles_num = 0;

	for (int i = data->gpiods_num - 1; i >= 0; i--)
		gpiod_put(data->gpiods[i]);
	data->gpiods_num = 0;
}

static void i2c_of_probe_free_res(struct i2c_of_probe_data *data)
{
	i2c_of_probe_free_gpios(data);

	for (int i = data->regulators_num; i >= 0; i--)
		regulator_put(data->regulators[i]);
	data->regulators_num = 0;
}

static int i2c_of_probe_get_res(struct device *dev, struct device_node *node,
				struct i2c_of_probe_data *data)
{
	struct property *prop;
	int ret;

	for_each_property_of_node(node, prop) {
		dev_dbg(dev, "Trying property %pOF/%s\n", node, prop->name);

		/* regulator supplies */
		ret = i2c_of_probe_get_regulator(node, prop, data);
		if (ret > 0)
			continue;
		if (ret < 0) {
			dev_err_probe(dev, ret, "Failed to get regulator supply from %pOF/%s\n",
				      node, prop->name);
			goto err_cleanup;
		}

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

static int i2c_of_probe_enable_res(struct device *dev, struct i2c_of_probe_data *data)
{
	int ret = 0;
	int reg_i, gpio_i;

	dev_dbg(dev, "Enabling resources\n");

	for (reg_i = 0; reg_i < data->regulators_num; reg_i++) {
		dev_dbg(dev, "Enabling regulator %d\n", reg_i);
		ret = regulator_enable(data->regulators[reg_i]);
		if (ret)
			goto disable_regulators;
	}

	/* largest post-power-on pre-reset-deassert delay seen among drivers */
	msleep(500);

	for (gpio_i = 0; gpio_i < data->gpiods_num; gpio_i++) {
		/*
		 * reset GPIOs normally have opposite polarity compared to
		 * enable GPIOs. Instead of parsing the flags again, simply
		 * set the raw value to high.
		 */
		dev_dbg(dev, "Setting GPIO %d\n", gpio_i);
		ret = gpiod_direction_output_raw(data->gpiods[gpio_i], 1);
		if (ret)
			goto disable_gpios;
	}

	/* largest post-reset-deassert delay seen in tree for Elan I2C HID */
	msleep(300);

	return 0;

disable_gpios:
	for (gpio_i--; gpio_i >= 0; gpio_i--)
		gpiod_set_raw_value_cansleep(data->gpiods[gpio_i], 0);
disable_regulators:
	for (reg_i--; reg_i >= 0; reg_i--)
		regulator_disable(data->regulators[reg_i]);

	return ret;
}

static void i2c_of_probe_disable_regulators(struct i2c_of_probe_data *data)
{
	for (int i = data->regulators_num - 1; i >= 0; i--)
		regulator_disable(data->regulators[i]);
}

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
	struct i2c_of_probe_data data = {0};
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

	/* Grab resources */
	for_each_child_of_node_scoped(i2c_node, node) {
		u32 addr;

		if (!of_node_name_prefix(node, type))
			continue;
		if (of_property_read_u32(node, "reg", &addr))
			continue;

		dev_dbg(dev, "Requesting resources for %pOF\n", node);
		ret = i2c_of_probe_get_res(dev, node, &data);
		if (ret) {
			of_node_put(i2c_node);
			return ret;
		}
	}

	dev_dbg(dev, "Resources: # of GPIOs = %d, # of regulator supplies = %d\n",
		data.gpiods_num, data.regulators_num);

	/* Enable resources */
	ret = i2c_of_probe_enable_res(dev, &data);
	if (ret) {
		i2c_of_probe_free_res(&data);
		of_node_put(i2c_node);
		return dev_err_probe(dev, ret, "Failed to enable resources\n");
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

	i2c_of_probe_free_gpios(&data);

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

	i2c_of_probe_disable_regulators(&data);
	i2c_of_probe_free_res(&data);
	i2c_put_adapter(i2c);
	of_node_put(i2c_node);

	return 0;

err_destroy_ocs:
	of_changeset_destroy(ocs);
err_free_ocs:
	kfree(ocs);
err_put_node:
	of_node_put(node);
	i2c_of_probe_disable_regulators(&data);
	i2c_of_probe_free_res(&data);
	i2c_put_adapter(i2c);
	of_node_put(i2c_node);
	return ret;
}
EXPORT_SYMBOL_GPL(i2c_of_probe_component);
