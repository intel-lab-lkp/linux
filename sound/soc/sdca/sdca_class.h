/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The MIPI SDCA specification is available for public downloads at
 * https://www.mipi.org/mipi-sdca-v1-0-download
 *
 * Copyright (C) 2025 Cirrus Logic, Inc. and
 *                    Cirrus Logic International Semiconductor Ltd.
 */

#ifndef __SDCA_CLASS_H__
#define __SDCA_CLASS_H__

#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

struct device;
struct dev_pm_ops;
struct regmap;
struct sdw_slave;
struct sdca_function_data;

/**
 * struct sdca_class_hw_ops - optional device-specific hardware callbacks
 * @hw_init: called during probe to enable supplies, toggle reset GPIO, etc.
 * @get_function_data: called when no DisCo/ACPI firmware node is available
 *             (e.g. DT/ARM platforms) to supply pre-populated static function
 *             data in place of sdca_parse_function().  Returns a pointer to
 *             an array of struct sdca_function_data and stores the number
 *             of entries in @num (must be > 0 and <= SDCA_MAX_FUNCTION_COUNT).
 *             May be NULL.
 *
 * Codec-specific SoundWire drivers pass a pointer to this struct to
 * sdca_class_probe() from their sdw_driver.probe.  Codec-specific
 * SoundWire slave property overrides live directly in the codec's
 * sdw_slave_ops.read_prop, which should call sdca_class_read_prop() to
 * fill the SDCA-common bits first.
 */
struct sdca_class_hw_ops {
	int  (*hw_init)(struct sdw_slave *slave);
	struct sdca_function_data *(*get_function_data)(unsigned int *num);
};

struct sdca_class_drv {
	struct device *dev;
	struct regmap *dev_regmap;
	struct sdw_slave *sdw;

	struct sdca_interrupt_info *irq_info;

	const struct sdca_class_hw_ops *hw_ops;

	struct mutex regmap_lock;
	/* Serialise function initialisations */
	struct mutex init_lock;
	struct work_struct boot_work;
};

/* Library helpers used by codec-specific SDCA SoundWire drivers. */
int sdca_class_read_prop(struct sdw_slave *sdw);
int sdca_class_probe(struct sdw_slave *sdw,
		     const struct sdca_class_hw_ops *hw_ops);
extern const struct dev_pm_ops sdca_class_pm_ops;

#endif /* __SDCA_CLASS_H__ */
