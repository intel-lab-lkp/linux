/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* Copyright (c) 2014 Raspberry Pi (Trading) Ltd. All rights reserved. */

#ifndef VCHIQ_DEBUGFS_H
#define VCHIQ_DEBUGFS_H

#include "vchiq_core.h"

struct vchiq_debugfs_node {
	struct dentry *dentry;
};

#endif /* VCHIQ_DEBUGFS_H */
