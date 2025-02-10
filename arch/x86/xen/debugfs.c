// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/debugfs.h>
#include <linux/slab.h>

#include "xen-ops.h"

static struct debugfs_node *d_xen_debug;

struct debugfs_node * __init xen_init_debugfs(void)
{
	if (!d_xen_debug)
		d_xen_debug = debugfs_create_dir("xen", NULL);
	return d_xen_debug;
}

