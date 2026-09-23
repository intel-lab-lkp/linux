// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) Advanced Micro Device, Inc. */

#include <linux/pci.h>
#include <cxlcache.h>

#include "cxlpci.h"

int cxl_port_map_cache_id_rt(struct cxl_port *port)
{
	struct cxl_register_map *map = &port->reg_map;

	if (is_cxl_root(port) || is_cxl_cachedev(port->uport_dev))
		return -EINVAL;

	if (!dev_is_pci(port->uport_dev) ||
	    !cxl_pci_flit_256(to_pci_dev(port->uport_dev)))
		return -EINVAL;

	if (!map->component_map.cidrt.valid)
		return -ENXIO;

	return cxl_map_component_regs(map, &port->regs,
				      BIT(CXL_CM_CAP_CAP_ID_CACHE_ID_RT));
}
EXPORT_SYMBOL_FOR_MODULES(cxl_port_map_cache_id_rt, "cxl_port");

int cxl_dport_map_cache_id_dc(struct cxl_dport *dport)
{
	struct cxl_register_map *map = &dport->reg_map;
	struct cxl_port *port = dport->port;

	if (is_cxl_root(port) || is_cxl_cachedev(port->uport_dev))
		return -EINVAL;

	if (!map->component_map.ciddc.valid)
		return -ENXIO;

	return cxl_map_component_regs(map, &dport->regs.component,
				      BIT(CXL_CM_CAP_CAP_ID_CACHE_ID_DC));
}

static int cache_decoder_committed(struct cxl_dport *dport)
{
	u32 cap, stat;

	cap = readl(dport->regs.ciddc + CXL_CACHE_ID_DC_CAP_OFFSET);
	if (!FIELD_GET(CXL_CACHE_ID_DC_CAP_COMMIT_REQ, cap))
		return -ENXIO;

	stat = readl(dport->regs.ciddc + CXL_CACHE_ID_DC_STATUS_OFFSET);
	return FIELD_GET(CXL_CACHE_ID_DC_STATUS_COMMITTED, stat);
}

static int cache_decoder_get_id(struct cxl_dport *dport)
{
	u32 ctrl;

	ctrl = readl(dport->regs.ciddc + CXL_CACHE_ID_DC_CTRL_OFFSET);
	if (FIELD_GET(CXL_CACHE_ID_DC_CTRL_HDMD_PRESENT, ctrl))
		 return FIELD_GET(CXL_CACHE_ID_DC_CTRL_HDMD_ID, ctrl);

	return FIELD_GET(CXL_CACHE_ID_DC_CTRL_LOCAL_ID, ctrl);
}

static bool cache_decoder_valid(struct cxl_dport *dport, int id, bool endpoint)
{
	struct pci_dev *pdev = to_pci_dev(dport->dport_dev);
	bool flit_256b;
	u32 ctrl;

	if (!dev_is_pci(dport->dport_dev))
		return false;

	flit_256b = cxl_pci_flit_256(pdev);
	if (id && !flit_256b)
		return false;

	ctrl = readl(dport->regs.ciddc + CXL_CACHE_ID_DC_CTRL_OFFSET);
	if (endpoint || !flit_256b) {
		if (!FIELD_GET(CXL_CACHE_ID_DC_CTRL_ASGN_ID, ctrl))
			return false;
	} else if (!FIELD_GET(CXL_CACHE_ID_DC_CTRL_FWD_ID, ctrl)) {
		return false;
	}

	return cache_decoder_get_id(dport) == id;
}

static int cache_idrt_committed(struct cxl_port *port)
{
	u32 cap, stat;

	cap = readl(port->regs.cidrt + CXL_CACHE_ID_RT_CAP_OFFSET);
	if (!FIELD_GET(CXL_CACHE_ID_RT_CAP_COMMIT_REQ, cap))
		return true;

	stat = readl(port->regs.cidrt + CXL_CACHE_ID_RT_STATUS_OFFSET);
	return FIELD_GET(CXL_CACHE_ID_RT_STATUS_COMMITTED, stat);
}

static int cache_idrt_entry_valid(struct cxl_port *port, int id)
{
	u16 entry;
	u32 cap;

	cap = readl(port->regs.cidrt + CXL_CACHE_ID_RT_CAP_OFFSET);
	if (id >= FIELD_GET(CXL_CACHE_ID_RT_CAP_TARGET_CNT, cap))
		return false;

	entry = readw(port->regs.cidrt + CXL_CACHE_ID_RT_TARGETN_OFFSET(id));
	return FIELD_GET(CXL_CACHE_ID_RT_TARGETN_VALID, entry);
}

static struct ida *find_cache_id_ida(struct cxl_port *port)
{
	struct cxl_port *parent = parent_port_of(port);

	while (parent) {
		if (is_cxl_root(parent))
			return &port->cache_ida;

		port = parent;
		parent = parent_port_of(port);
	}

	return NULL;
}

int cxl_allocate_cache_id(struct cxl_cachedev *cxlcd)
{
	struct ida *ida = find_cache_id_ida(cxlcd->endpoint);
	int id;

	if (!ida)
		return -ENOSPC;

	id = ida_alloc_range(ida, cxlcd->cache_id, cxlcd->cache_id, GFP_KERNEL);
	if (id != cxlcd->cache_id)
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_FOR_MODULES(cxl_allocate_cache_id, "cxl_cache");

void cxl_free_cache_id(struct cxl_cachedev *cxlcd)
{
	struct ida *ida = find_cache_id_ida(cxlcd->endpoint);

	if (ida)
		ida_free(ida, cxlcd->cache_id);
}
EXPORT_SYMBOL_FOR_MODULES(cxl_free_cache_id, "cxl_cache");

bool cxl_cache_id_supported(struct cxl_cachedev *cxlcd)
{
	struct cxl_dport *dport = cxlcd->endpoint->parent_dport;
	struct cxl_port *port = dport->port;

	for (; !is_cxl_root(port);
	     dport = port->parent_dport, port = dport->port) {
		if (!port->regs.cidrt || !dport->regs.ciddc)
			return false;
	}

	return true;
}
EXPORT_SYMBOL_FOR_MODULES(cxl_cache_id_supported, "cxl_cache");

int cxl_cachedev_validate_cache_id(struct cxl_cachedev *cxlcd)
{
	struct cxl_dport *dport = cxlcd->endpoint->parent_dport;
	struct cxl_port *port = dport->port;
	int id = CXL_CACHE_ID_NO_ID;
	bool endpoint = true;

	cxlcd->cache_id = CXL_CACHE_ID_NO_ID;
	id = cache_decoder_get_id(dport);

	while (!is_cxl_root(port)) {
		if (!cache_decoder_valid(dport, id, endpoint) ||
		    !cache_decoder_committed(dport))
			return -EINVAL;

		endpoint = false;

		if (!cache_idrt_entry_valid(port, id) ||
		    !cache_idrt_committed(port))
			return -EINVAL;

		dport = port->parent_dport;
		port = dport->port;
	}

	cxlcd->cache_id = id;
	return 0;
}
EXPORT_SYMBOL_FOR_MODULES(cxl_cachedev_validate_cache_id, "cxl_cache");
>>>>>>> conflict 1 of 1 ends
