// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver debug filesystem support
 *
 * Copyright (c) 2026 Microchip Technology Inc. and its subsidiaries.
 */

#include <linux/debugfs.h>

#include "sparx5_main.h"
#include "vcap_api_debugfs.h"

void sparx5_debugfs(struct sparx5 *sparx5)
{
	const struct sparx5_consts *consts = sparx5->data->consts;
	struct vcap_control *ctrl = sparx5->vcap_ctrl;
	struct dentry *dir;
	int idx;

	sparx5->debugfs_root = debugfs_create_dir("sparx5", NULL);

	dir = vcap_debugfs(sparx5->dev, sparx5->debugfs_root, ctrl);
	for (idx = 0; idx < consts->n_ports; ++idx)
		if (sparx5->ports[idx])
			vcap_port_debugfs(sparx5->dev, dir, ctrl,
					  sparx5->ports[idx]->ndev);
}
