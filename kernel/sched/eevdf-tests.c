// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2025 Advanced Micro Devices, Inc
 *
 * Author: Dhaval Giani (AMD) <dhaval@gianis.ca>
 *
 * Basic functional tests for EEVDF - Invariants
 *
 * Use the debugfs triggers to run them
 *
 */

#include <linux/debugfs.h>
#include <linux/sched.h>

#include "sched.h"

#ifdef CONFIG_SCHED_EEVDF_TESTING

static struct dentry *debugfs_eevdf_testing;
void debugfs_eevdf_testing_init(struct dentry *debugfs_sched)
{
	debugfs_eevdf_testing = debugfs_create_dir("eevdf-testing", debugfs_sched);

}
#endif /* CONFIG_SCHED_EEVDF_TESTING */
