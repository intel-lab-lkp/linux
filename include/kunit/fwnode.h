/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KUnit resource management helpers firmware nodes.
 *
 * Copyright (C) Qualcomm Technologies, Inc. and/or its subsidiaries
 */

#ifndef _KUNIT_FWNODE_H
#define _KUNIT_FWNODE_H

struct kunit;
struct fwnode_handle;
struct software_node;

struct fwnode_handle *
kunit_software_node_register(struct kunit *test,
			     const struct software_node *node);

#endif /* _KUNIT_FWNODE_H */
