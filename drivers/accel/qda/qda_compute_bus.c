// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <linux/device.h>
#include <linux/init.h>

struct bus_type qda_cb_bus_type = {
	.name = "qda-compute-cb",
};
EXPORT_SYMBOL_GPL(qda_cb_bus_type);

static int __init qda_cb_bus_init(void)
{
	int err;

	err = bus_register(&qda_cb_bus_type);
	if (err < 0) {
		pr_err("qda-compute-cb bus registration failed: %d\n", err);
		return err;
	}
	return 0;
}

postcore_initcall(qda_cb_bus_init);
