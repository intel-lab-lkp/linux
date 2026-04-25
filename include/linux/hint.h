/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Device hints sysfs interface
 */

#ifndef _HINT_H_
#define _HINT_H_

#include <linux/device.h>
#include <linux/bitops.h>

enum hint_idle_option {
	HINT_IDLE_ACTIVE,
	HINT_IDLE_INACTIVE,
	HINT_IDLE_SNOOZE,
	HINT_IDLE_RESUME,
	HINT_IDLE_LAST, /*must always be last */
};

/**
 * struct hint_ops - hint probes and get/set operations
 * @idle_probe:	Callback to setup idle hints available to the device.
 * @idle_get:	Will be called when showing the current idle hint in sysfs.
 * @idle_set:	Will be called when storing a new idle hint in sysfs.
 */
struct hint_ops {
	int (*idle_probe)(void *drvdata, unsigned long *choices);
	int (*idle_get)(struct device *dev, enum hint_idle_option *idle);
	int (*idle_set)(struct device *dev, enum hint_idle_option idle);
};

struct device *hint_register(struct device *dev, const char *name,
			     void *drvdata, const struct hint_ops *ops);
void hint_remove(struct device *dev);
struct device *devm_hint_register(struct device *dev, const char *name,
				  void *drvdata, const struct hint_ops *ops);

#endif  /*_HINT_H_*/
