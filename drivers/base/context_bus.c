// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#include <linux/context_bus.h>
#include <linux/init.h>

const struct bus_type context_device_bus_type = {
	.name = "context-device",
};
EXPORT_SYMBOL_GPL(context_device_bus_type);

static int __init context_device_bus_init(void)
{
	int err;

	err = bus_register(&context_device_bus_type);
	if (err < 0) {
		pr_err("context-device bus registration failed: %d\n", err);
		return err;
	}

	return 0;
}
postcore_initcall(context_device_bus_init);
