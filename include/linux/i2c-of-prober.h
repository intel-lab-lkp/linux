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

#endif /* IS_ENABLED(CONFIG_OF_DYNAMIC) */

#endif /* _LINUX_I2C_OF_PROBER_H */
