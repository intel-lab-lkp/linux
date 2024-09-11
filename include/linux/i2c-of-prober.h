/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * i2c-of-prober.h - definitions for the Linux I2C OF component prober
 *
 * Copyright (C) 2024 Google LLC
 */

#ifndef _LINUX_I2C_OF_PROBER_H
#define _LINUX_I2C_OF_PROBER_H

#if IS_ENABLED(CONFIG_OF_DYNAMIC)

struct device;
struct device_node;

/**
 * struct i2c_of_probe_ops - I2C OF component prober callbacks
 *
 * A set of callbacks to be used by i2c_of_probe_component().
 *
 * All callbacks are optional. Callbacks are called only once per run, and are
 * used in the order they are defined in this structure.
 *
 * All callbacks that have return values shall return %0 on success,
 * or a negative error number on failure.
 *
 * The @dev parameter passed to the callbacks is the same as @dev passed to
 * i2c_of_probe_component(). It should only be used for dev_printk() calls
 * and nothing else, especially not managed device resource (devres) APIs.
 */
struct i2c_of_probe_ops {
	/** @get_resources: Retrieve resources for components. */
	int (*get_resources)(struct device *dev, struct device_node *bus_node, void *data);

	/** @free_resources_early: Release exclusive resources prior to enabling component. */
	void (*free_resources_early)(void *data);

	/**
	 * @enable: Enable resources so that the components respond to probes.
	 *
	 * Resources should be reverted to their initial state before returning if this fails.
	 */
	int (*enable)(struct device *dev, void *data);

	/**
	 * @cleanup: Opposite of @enable to balance refcounts after probing.
	 *
	 * Can not operate on resources already freed in @free_resources_early.
	 */
	int (*cleanup)(struct device *dev, void *data);

	/**
	 * @free_resources_late: Release all resources, including those that would have
	 *                       been released by @free_resources_early.
	 */
	void (*free_resources_late)(void *data);
};

/**
 * struct i2c_of_probe_cfg - I2C OF component prober configuration
 * @ops: Callbacks for the prober to use.
 * @type: A string to match the device node name prefix to probe for.
 */
struct i2c_of_probe_cfg {
	const struct i2c_of_probe_ops *ops;
	const char *type;
};

int i2c_of_probe_component(struct device *dev, const struct i2c_of_probe_cfg *cfg, void *ctx);

/**
 * DOC: I2C OF component prober simple helpers
 *
 * Components such as trackpads are commonly connected to a devices baseboard
 * with a 6-pin ribbon cable. That gives at most one voltage supply and one
 * GPIO besides the I2C bus, interrupt pin, and common ground. Touchscreens,
 * while integrated into the display panel's connection, typically have the
 * same set of connections.
 *
 * A simple set of helpers are provided here for use with the I2C OF component
 * prober. This implementation targets such components, allowing for at most
 * one regulator supply.
 *
 * The following helpers are provided:
 * * i2c_of_probe_simple_get_res()
 * * i2c_of_probe_simple_free_res_late()
 * * i2c_of_probe_simple_enable()
 * * i2c_of_probe_simple_cleanup()
 */

/**
 * struct i2c_of_probe_simple_opts - Options for simple I2C component prober callbacks
 * @res_node_compatible: Compatible string of device node to retrieve resources from.
 * @supply_name: Name of regulator supply.
 * @post_power_on_delay_ms: Delay in ms after regulators are powered on. Passed to msleep().
 */
struct i2c_of_probe_simple_opts {
	const char *res_node_compatible;
	const char *supply_name;
	unsigned int post_power_on_delay_ms;
};

struct regulator;

struct i2c_of_probe_simple_ctx {
	/* public: provided by user before helpers are used. */
	const struct i2c_of_probe_simple_opts *opts;
	/* private: internal fields for helpers. */
	struct regulator *supply;
};

int i2c_of_probe_simple_get_res(struct device *dev, struct device_node *bus_node, void *data);
void i2c_of_probe_simple_free_res_late(void *data);
int i2c_of_probe_simple_enable(struct device *dev, void *data);
int i2c_of_probe_simple_cleanup(struct device *dev, void *data);

extern struct i2c_of_probe_ops i2c_of_probe_simple_ops;

#endif /* IS_ENABLED(CONFIG_OF_DYNAMIC) */

#endif /* _LINUX_I2C_OF_PROBER_H */
