// SPDX-License-Identifier: GPL-2.0
#ifndef _HMEM_H
#define _HMEM_H

typedef int (*walk_hmem_fn)(struct device *dev, int target_nid,
			    const struct resource *res);
int walk_hmem_resources(struct device *dev, walk_hmem_fn fn);

extern struct platform_device *hmem_pdev;

#endif
