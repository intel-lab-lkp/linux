// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) Advanced Micro Device, Inc. */

#include <linux/iopoll.h>
#include <linux/pci.h>
#include <cxlcache.h>

#include "cxlpci.h"

#define CXL_CACHE_ID_COMMIT_MAXTMO_US (5 * USEC_PER_SEC)
#define CACHE_DECODER_MAX_CACHE_ID (15)

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

static unsigned long __cxl_cid_get_timeout_us(struct device *dev,
					      unsigned int scale,
					      unsigned int base)
{
	static const unsigned long scale_tbl[] = {
		1, 10, 100, 1000, 10000, 100000, 1000000, 10000000,
	};

	if (scale >= ARRAY_SIZE(scale_tbl) || !base) {
		dev_dbg(dev,
			"Invalid Cache ID commit timeout: scale=%u base=%u\n",
			scale, base);
		return CXL_CACHE_ID_COMMIT_MAXTMO_US;
	}

	return scale_tbl[scale] * base;
}

static int __cxl_cid_wait_commit(struct device *dev, void __iomem *status_reg,
				 u32 commit_bit, u32 err_bit,
				 unsigned int scale, unsigned int base)
{
	unsigned long tmo_us, poll_us;
	ktime_t start;
	u32 status;
	int rc;

	tmo_us = min_t(unsigned long, CXL_CACHE_ID_COMMIT_MAXTMO_US,
		       __cxl_cid_get_timeout_us(dev, scale, base));
	poll_us = max_t(unsigned long, tmo_us / 10, 1); /* ~10% */
	start = ktime_get();

	rc = readx_poll_timeout(readl, status_reg, status,
				status & (commit_bit | err_bit), poll_us,
				tmo_us);
	if (rc) {
		dev_err(dev, "Cache ID commit timed out\n");
		return rc;
	}

	if (status & err_bit) {
		dev_err(dev, "Cache ID commit rejected by hardware\n");
		return -EIO;
	}

	dev_dbg(dev, "Cache ID commit took %lluus\n",
		ktime_to_us(ktime_sub(ktime_get(), start)));
	return 0;
}

static int cxl_cid_commit_decoder(struct cxl_dport *dport)
{
	void __iomem *ciddc = dport->regs.ciddc;
	u32 cap, ctrl, status;
	u8 scale, base;
	int rc;

	cap = readl(ciddc + CXL_CACHE_ID_DC_CAP_OFFSET);
	if (!FIELD_GET(CXL_CACHE_ID_DC_CAP_COMMIT_REQ, cap))
		return 0;

	status = readl(ciddc + CXL_CACHE_ID_DC_STATUS_OFFSET);
	scale = FIELD_GET(CXL_CACHE_ID_DC_STATUS_TM_SCALE, status);
	base = FIELD_GET(CXL_CACHE_ID_DC_STATUS_TM_BASE, status);

	ctrl = readl(ciddc + CXL_CACHE_ID_DC_CTRL_OFFSET);
	if (FIELD_GET(CXL_CACHE_ID_DC_CTRL_COMMIT, ctrl)) {
		ctrl &= ~CXL_CACHE_ID_DC_CTRL_COMMIT;
		writel(ctrl, ciddc + CXL_CACHE_ID_DC_CTRL_OFFSET);
	}

	ctrl |= CXL_CACHE_ID_DC_CTRL_COMMIT;
	writel(ctrl, ciddc + CXL_CACHE_ID_DC_CTRL_OFFSET);

	rc = __cxl_cid_wait_commit(dport->dport_dev,
				   ciddc + CXL_CACHE_ID_DC_STATUS_OFFSET,
				   CXL_CACHE_ID_DC_STATUS_COMMITTED,
				   CXL_CACHE_ID_DC_STATUS_COMMIT_ERR, scale,
				   base);
	if (rc) {
		ctrl &= ~CXL_CACHE_ID_DC_CTRL_COMMIT;
		writel(ctrl, ciddc + CXL_CACHE_ID_DC_CTRL_OFFSET);
	}

	return rc;
}

static int cxl_cid_program_decoder(struct cxl_dport *dport, int cid,
				   bool endpoint)
{
	void __iomem *ciddc = dport->regs.ciddc;
	u32 ctrl;

	if (!ciddc)
		return -EINVAL;

	ctrl = readl(ciddc + CXL_CACHE_ID_DC_CTRL_OFFSET);

	/*
	 * The decoder may have been programmed before, so we zero out
	 * all the fields before writing to them
	 */
	ctrl &= ~(CXL_CACHE_ID_DC_CTRL_ASGN_ID | CXL_CACHE_ID_DC_CTRL_FWD_ID);
	if (endpoint) {
		ctrl |= CXL_CACHE_ID_DC_CTRL_ASGN_ID;
	} else {
		ctrl |= CXL_CACHE_ID_DC_CTRL_FWD_ID;
	}

	FIELD_MODIFY(CXL_CACHE_ID_DC_CTRL_LOCAL_ID, &ctrl, cid);
	writel(ctrl, ciddc + CXL_CACHE_ID_DC_CTRL_OFFSET);

	return cxl_cid_commit_decoder(dport);
}

static int cxl_cid_commit_table(struct cxl_port *port)
{
	void __iomem *cidrt = port->regs.cidrt;
	u32 cap, ctrl, status;
	u8 scale, base;
	int rc;

	cap = readl(cidrt + CXL_CACHE_ID_RT_CAP_OFFSET);
	if (!FIELD_GET(CXL_CACHE_ID_RT_CAP_COMMIT_REQ, cap))
		return 0;

	status = readl(cidrt + CXL_CACHE_ID_RT_STATUS_OFFSET);
	scale = FIELD_GET(CXL_CACHE_ID_RT_STATUS_TM_SCALE, status);
	base = FIELD_GET(CXL_CACHE_ID_RT_STATUS_TM_BASE, status);

	ctrl = readl(cidrt + CXL_CACHE_ID_RT_CTRL_OFFSET);
	if (FIELD_GET(CXL_CACHE_ID_RT_CTRL_COMMIT, ctrl)) {
		ctrl &= ~CXL_CACHE_ID_RT_CTRL_COMMIT;
		writel(ctrl, cidrt + CXL_CACHE_ID_RT_CTRL_OFFSET);
	}

	ctrl |= CXL_CACHE_ID_RT_CTRL_COMMIT;
	writel(ctrl, cidrt + CXL_CACHE_ID_RT_CTRL_OFFSET);

	rc = __cxl_cid_wait_commit(&port->dev,
				   cidrt + CXL_CACHE_ID_RT_STATUS_OFFSET,
				   CXL_CACHE_ID_RT_STATUS_COMMITTED,
				   CXL_CACHE_ID_RT_STATUS_COMMIT_ERR, scale,
				   base);
	if (rc) {
		ctrl &= ~CXL_CACHE_ID_RT_CTRL_COMMIT;
		writel(ctrl, cidrt + CXL_CACHE_ID_RT_CTRL_OFFSET);
	}

	return rc;
}

static int cxl_cid_program_table_entry(struct cxl_port *port, int cid,
				       unsigned int dport_id)
{
	void __iomem *cidrt = port->regs.cidrt;
	u8 target_cnt, portn;
	u16 target_n;
	u32 cap;

	if (!cidrt)
		return -EINVAL;

	cap = readl(cidrt + CXL_CACHE_ID_RT_CAP_OFFSET);
	target_cnt = FIELD_GET(CXL_CACHE_ID_RT_CAP_TARGET_CNT, cap);

	/* Shouldn't be possible, but better to be safe */
	if (cid >= target_cnt) {
		dev_err(&port->dev,
			"Tried to allocate cache ID (%d) larger than table size (%d)\n",
			cid, target_cnt);
		return -EINVAL;
	}

	target_n = readw(cidrt + CXL_CACHE_ID_RT_TARGETN_OFFSET(cid));
	if (FIELD_GET(CXL_CACHE_ID_RT_TARGETN_VALID, target_n)) {
		portn = FIELD_GET(CXL_CACHE_ID_RT_TARGETN_PORTN, target_n);
		return dport_id == portn ? 0 : -EINVAL;
	}

	target_n = CXL_CACHE_ID_RT_TARGETN_VALID;
	target_n |= FIELD_PREP(CXL_CACHE_ID_RT_TARGETN_PORTN, dport_id);
	writew(target_n, cidrt + CXL_CACHE_ID_RT_TARGETN_OFFSET(cid));

	return cxl_cid_commit_table(port);
}

static void cxl_cid_invalidate_table_entry(struct cxl_port *port, int cid)
{
	void __iomem *cidrt = port->regs.cidrt;
	u8 target_cnt;
	u16 target_n;
	u32 cap;
	int rc;

	if (!cidrt)
		return;

	cap = readl(cidrt + CXL_CACHE_ID_RT_CAP_OFFSET);
	target_cnt = FIELD_GET(CXL_CACHE_ID_RT_CAP_TARGET_CNT, cap);

	/* Shouldn't be possible, but better to be safe */
	if (cid >= target_cnt) {
		dev_err(&port->dev,
			"Tried to free cache ID (%d) larger than table size (%d)\n",
			cid, target_cnt);
		return;
	}

	target_n = readw(cidrt + CXL_CACHE_ID_RT_TARGETN_OFFSET(cid));
	if (!FIELD_GET(CXL_CACHE_ID_RT_TARGETN_VALID, target_n))
		return;

	target_n &= ~CXL_CACHE_ID_RT_TARGETN_VALID;
	writew(target_n, cidrt + CXL_CACHE_ID_RT_TARGETN_OFFSET(cid));

	rc = cxl_cid_commit_table(port);
	if (rc)
		dev_warn(
			&port->dev,
			"Failed to commit invalidation of cache id table entry %d: %d\n",
			cid, rc);
}

static int get_max_cid(struct cxl_port *endpoint)
{
	struct cxl_port *port = parent_port_of(endpoint);
	void __iomem *cidrt;
	u32 cap;
	u8 cnt;

	if (!port)
		return -EINVAL;

	while (!is_cxl_root(port) && !is_cxl_root(parent_port_of(port)))
		port = parent_port_of(port);

	cidrt = port->regs.cidrt;
	if (!cidrt)
		return -EINVAL;

	cap = readl(cidrt + CXL_CACHE_ID_RT_CAP_OFFSET);
	cnt = FIELD_GET(CXL_CACHE_ID_RT_CAP_TARGET_CNT, cap);
	if (cnt == 0)
		return 0;

	/*
	 * Cache id decoders have 4 bits for the cache id, while target count
	 * is a 5 bit long field. Limit cache ids to the maximum value allowed
	 * by cache id decoders.
	 */
	return min(CACHE_DECODER_MAX_CACHE_ID, cnt - 1);
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
	int min, max, id;

	if (!ida)
		return -ENOSPC;

	max = get_max_cid(cxlcd->endpoint);
	if (max < 0 || cxlcd->cache_id > max)
		return -EINVAL;

	if (cxlcd->cache_id == CXL_CACHE_ID_NO_ID) {
		min = 0;
	} else {
		min = max = cxlcd->cache_id;
	}

	id = ida_alloc_range(ida, min, max, GFP_KERNEL);
	if (id < 0)
		return id;

	cxlcd->cache_id = id;
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

static void __deprogram_cache_id(struct cxl_cachedev *cxlcd,
				 struct cxl_port *stop)
{
	struct cxl_port *iter = cxlcd->endpoint->parent_dport->port;

	/*
	 * Leave cache id decoders programmed; the table entry being invalidated
	 * should be enough
	 */
	while (!is_cxl_root(iter)) {
		cxl_cid_invalidate_table_entry(iter, cxlcd->cache_id);

		if (iter == stop)
			return;

		iter = parent_port_of(iter);
	}
}

int cxl_cachedev_program_cache_id(struct cxl_cachedev *cxlcd)
{
	struct cxl_dport *dport = cxlcd->endpoint->parent_dport;
	struct cxl_port *port = dport->port;
	bool endpoint = true;
	int rc;

	while (!is_cxl_root(port)) {
		rc = cxl_cid_program_decoder(dport, cxlcd->cache_id,
					     endpoint);
		if (rc)
			goto err;

		rc = cxl_cid_program_table_entry(port, cxlcd->cache_id,
						 dport->port_id);
		if (rc)
			goto err;

		endpoint = false;
		dport = port->parent_dport;
		port = dport->port;
	}

	return 0;

err:
	__deprogram_cache_id(cxlcd, port);
	return rc;
}
EXPORT_SYMBOL_FOR_MODULES(cxl_cachedev_program_cache_id, "cxl_cache");

void cxl_cachedev_deprogram_cache_id(struct cxl_cachedev *cxlcd)
{
	struct cxl_root *root = find_cxl_root(cxlcd->endpoint);

	if (!root)
		return;

	__deprogram_cache_id(cxlcd, &root->port);
}
EXPORT_SYMBOL_FOR_MODULES(cxl_cachedev_deprogram_cache_id, "cxl_cache");
