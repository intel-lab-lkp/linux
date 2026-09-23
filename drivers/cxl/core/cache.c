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
