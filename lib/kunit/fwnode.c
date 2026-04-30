// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Qualcomm Technologies, Inc. and/or its subsidiaries
 */

#include <kunit/fwnode.h>
#include <kunit/test.h>

#include <linux/fwnode.h>
#include <linux/property.h>

static void kunit_software_node_unregister(void *data)
{
	const struct software_node *swnode = data;

	software_node_unregister(swnode);
}

/**
 * kunit_software_node_register() - Register a KUnit-managed software node
 * @test: test context
 * @swnode: Software node to register
 *
 * Register a test-managed software node and return its firmware node handle.
 * The software node is unregistered after the test case completes.
 *
 * Return: Firmware node handle of the registered software node or IS_ERR()
 * on failure.
 */
struct fwnode_handle *
kunit_software_node_register(struct kunit *test,
			     const struct software_node *swnode)
{
	struct fwnode_handle *fwnode;
	int ret;

	ret = software_node_register(swnode);
	if (ret)
		return ERR_PTR(ret);

	fwnode = software_node_fwnode(swnode);
	if (WARN_ON(!fwnode))
		return ERR_PTR(-ENOENT);

	ret = kunit_add_action_or_reset(test, kunit_software_node_unregister,
					(void *)swnode);
	if (ret)
		return ERR_PTR(ret);

	return fwnode;
}
EXPORT_SYMBOL_GPL(kunit_software_node_register);
