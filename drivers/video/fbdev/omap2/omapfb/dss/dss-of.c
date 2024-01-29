// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Texas Instruments
 * Author: Tomi Valkeinen <tomi.valkeinen@ti.com>
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/seq_file.h>

#include <video/omapfb_dss.h>

#include "dss.h"

struct device_node *dss_of_port_get_parent_device(struct device_node *port)
{
	struct device_node *np;
	int i;

	if (!port)
		return NULL;

	np = of_get_parent(port);

	for (i = 0; i < 2 && np; ++i) {
		struct property *prop;

		prop = of_find_property(np, "compatible", NULL);

		if (prop)
			return np;

		np = of_get_next_parent(np);
	}

	return NULL;
}

u32 dss_of_port_get_port_number(struct device_node *port)
{
	int r;
	u32 reg;

	r = of_property_read_u32(port, "reg", &reg);
	if (r)
		reg = 0;

	return reg;
}

struct device_node *
omapdss_of_get_first_endpoint(const struct device_node *parent)
{
	struct device_node *port, *ep;

	port = of_graph_get_next_port(parent, NULL);

	if (!port)
		return NULL;

	ep = of_graph_get_next_endpoint_raw(port, NULL);

	of_node_put(port);

	return ep;
}
EXPORT_SYMBOL_GPL(omapdss_of_get_first_endpoint);

struct omap_dss_device *
omapdss_of_find_source_for_first_ep(struct device_node *node)
{
	struct device_node *ep;
	struct device_node *src_port;
	struct omap_dss_device *src;

	ep = omapdss_of_get_first_endpoint(node);
	if (!ep)
		return ERR_PTR(-EINVAL);

	src_port = of_graph_get_remote_port(ep);
	if (!src_port) {
		of_node_put(ep);
		return ERR_PTR(-EINVAL);
	}

	of_node_put(ep);

	src = omap_dss_find_output_by_port_node(src_port);

	of_node_put(src_port);

	return src ? src : ERR_PTR(-EPROBE_DEFER);
}
EXPORT_SYMBOL_GPL(omapdss_of_find_source_for_first_ep);
