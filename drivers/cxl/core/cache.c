// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) Advanced Micro Device, Inc. */

#include <linux/iopoll.h>
#include <linux/iommu.h>
#include <linux/pci.h>
#include <cxlcache.h>

#include "cxlpci.h"
#include "core.h"

#define CXL_CACHE_ID_COMMIT_MAXTMO_US (5 * USEC_PER_SEC)
#define CACHE_DECODER_MAX_CACHE_ID (15)

static DEFINE_XARRAY(snoop_filters);
static DECLARE_RWSEM(snoop_rwsem);

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
				   bool endpoint, bool hdmd)
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

	if (hdmd) {
		ctrl |= CXL_CACHE_ID_DC_CTRL_HDMD_PRESENT;
		FIELD_MODIFY(CXL_CACHE_ID_DC_CTRL_HDMD_ID, &ctrl, cid);
	} else {
		ctrl &= ~CXL_CACHE_ID_DC_CTRL_HDMD_PRESENT;
		FIELD_MODIFY(CXL_CACHE_ID_DC_CTRL_LOCAL_ID, &ctrl, cid);
	}

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
				       unsigned int dport_id, bool hdmd)
{
	void __iomem *cidrt = port->regs.cidrt;
	u8 target_cnt, hdmd_max, portn;
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

	if (is_cxl_root(parent_port_of(port)) && hdmd) {
		hdmd_max = FIELD_GET(CXL_CACHE_ID_RT_CAP_HDMD_MAX, cap);

		if (port->num_hdmd > hdmd_max) {
			dev_err(&port->dev,
				"Maximum number of devices using HDM-D reached\n");
			return -EINVAL;
		}
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

static void cxl_port_add_hdmd(struct cxl_port *endpoint, int val)
{
	struct cxl_port *parent = parent_port_of(endpoint);
	struct cxl_port *port = endpoint;

	if (!parent || !is_cxl_cachedev(endpoint->uport_dev))
		return;

	while (parent && !is_cxl_root(parent)) {
		port = parent;
		parent = parent_port_of(port);
	}

	port->num_hdmd += val;
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

	if (cxlcd->cxlds->hdmd)
		cxl_port_add_hdmd(cxlcd->endpoint, 1);

	cxlcd->cache_id = id;
	return 0;
}
EXPORT_SYMBOL_FOR_MODULES(cxl_allocate_cache_id, "cxl_cache");

void cxl_free_cache_id(struct cxl_cachedev *cxlcd)
{
	struct ida *ida = find_cache_id_ida(cxlcd->endpoint);

	if (ida)
		ida_free(ida, cxlcd->cache_id);

	if (cxlcd->cxlds->hdmd)
		cxl_port_add_hdmd(cxlcd->endpoint, -1);
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
	bool hdmd = cxlcd->cxlds->hdmd;
	bool endpoint = true;
	int rc;

	while (!is_cxl_root(port)) {
		rc = cxl_cid_program_decoder(dport, cxlcd->cache_id,
					     endpoint, hdmd);
		if (rc)
			goto err;

		rc = cxl_cid_program_table_entry(port, cxlcd->cache_id,
						 dport->port_id, hdmd);
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

struct snoop_allocation {
	struct cxl_cachedev *cxlcd;
	bool strict;
	u64 size;
	int id;
};

/**
 * struct cxl_snoop_filter - CXL snoop filter instance for tracking CXL.cache
 * devices below a dport
 *
 * @allocations: Allocations on this filter
 * @lock: Used for allocations
 * @strict: Number of allocations from devices that disallow oversubscription
 * @avail: Available capacity left in the filter
 * @size: Size of the filter
 * @id: Group ID of the filter
 */
struct cxl_snoop_filter {
	struct xarray allocations;
	struct mutex lock;
	u32 strict;
	u64 avail;
	u64 size;
	int id;
};

static struct cxl_snoop_filter *create_snoop_filter(u64 size, int id)
{
	struct cxl_snoop_filter *sf;

	sf = kzalloc_obj(*sf, GFP_KERNEL);
	if (!sf)
		return ERR_PTR(-ENOMEM);

	sf->id = id;
	sf->size = sf->avail = size;
	xa_init(&sf->allocations);
	mutex_init(&sf->lock);

	return sf;
}

static void free_snoop_capacity(void *);

static void __free_snoop_capacity(struct snoop_allocation *alloc)
{
	struct cxl_snoop_filter *sf;

	sf = xa_load(&snoop_filters, alloc->id);
	if (sf) {
		scoped_guard(mutex, &sf->lock) {
			sf->avail += alloc->size;

			if (alloc->strict)
				sf->strict--;

			xa_erase(&sf->allocations, (unsigned long)alloc);
		}
	}

	put_device(&alloc->cxlcd->dev);
	kfree(alloc);
}

static void destroy_snoop_filter(struct cxl_snoop_filter *sf)
{
	xa_destroy(&sf->allocations);
	mutex_destroy(&sf->lock);
	kfree(sf);
}

static struct cxl_snoop_filter *find_or_add_snoop_filter(u64 size, int id)
{
	struct cxl_snoop_filter *sf;
	int rc;

	guard(rwsem_write)(&snoop_rwsem);
	sf = xa_load(&snoop_filters, id);
	if (sf) {
		if (sf->size != size)
			pr_warn("Mismatched snoop filter (gid: %d) size: found %llu, expected %llu",
				id, sf->size, size);

		return sf;
	}

	sf = create_snoop_filter(size, id);
	if (IS_ERR_OR_NULL(sf))
		return sf;

	rc = xa_insert(&snoop_filters, id, sf, GFP_KERNEL);
	if (rc) {
		destroy_snoop_filter(sf);
		return ERR_PTR(rc);
	}

	return sf;
}

static struct snoop_allocation *
snoop_alloc_capacity(struct cxl_snoop_filter *sf, struct cxl_cachedev *cxlcd,
		     u64 size)
{
	struct cxl_cache_state *cstate = &cxlcd->cxlds->cstate;
	struct snoop_allocation *alloc;
	int rc;

	guard(mutex)(&sf->lock);
	if (sf->avail < size) {
		/*
		 * Either this device or a device already using the filter
		 * can't use an oversubscribed filter
		 */
		if (cstate->strict_snoop || sf->strict > 0)
			return ERR_PTR(-ENOSPC);

		size = sf->avail;
	}

	alloc = kmalloc_obj(*alloc);
	if (!alloc)
		return ERR_PTR(-ENOMEM);

	*alloc = (struct snoop_allocation) {
		.cxlcd = cxlcd,
		.size = size,
		.strict = cstate->strict_snoop,
		.id = sf->id,
	};

	rc = xa_insert(&sf->allocations, (unsigned long)alloc, alloc,
		       GFP_KERNEL);
	if (rc) {
		kfree(alloc);
		return ERR_PTR(rc);
	}

	if (cstate->strict_snoop)
		sf->strict++;

	sf->avail -= size;
	return alloc;
}

static void free_snoop_capacity(void *alloc)
{
	guard(rwsem_read)(&snoop_rwsem);
	__free_snoop_capacity(alloc);
}

/**
 * devm_cxl_cachedev_alloc_snoop_capacity - Allocate space in the system's snoop
 * filter for a given CXL.cache device
 * @cxlcd: Cache device to allocate capacity for
 * @size: Size of allocation
 *
 * NOTE: CXL accelerator drivers are expected to call this function before
 * using CXL.cache to access host memory. Failure to do so may result in
 * unpredictable or undesirable behavior depending on the host's snoop filter
 * implementation.
 */
int devm_cxl_cachedev_alloc_snoop_capacity(struct cxl_cachedev *cxlcd, u64 size)
{
	struct cxl_cache_state *cstate = &cxlcd->cxlds->cstate;
	struct snoop_allocation *alloc;
	struct cxl_snoop_filter *sf;

	if (cstate->gid < 0)
		return -EINVAL;

	scoped_guard(rwsem_read, &snoop_rwsem) {
		sf = xa_load(&snoop_filters, cstate->gid);
		if (!sf)
			return -ENODEV;

		alloc = snoop_alloc_capacity(sf, cxlcd, size);
		if (IS_ERR_OR_NULL(alloc))
			return !alloc ? -ENOMEM : PTR_ERR(alloc);

		get_device(&cxlcd->dev);
	}

	return devm_add_action_or_reset(&cxlcd->dev, free_snoop_capacity,
					alloc);
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_cachedev_alloc_snoop_capacity, "CXL");

int cxl_dport_probe_snoop_filter(struct cxl_dport *dport)
{
	struct cxl_snoop_filter *sf;
	u32 group, size;
	int id, rc;

	if (!dport->reg_map.component_map.snoop.valid) {
		dev_dbg(dport->dport_dev, "missing snoop filter capability\n");
		return 0;
	}

	rc = cxl_map_component_regs(&dport->reg_map, &dport->regs.component,
				    BIT(CXL_CM_CAP_CAP_ID_SNOOP));
	if (rc)
		return rc;

	group = readl(dport->regs.snoop + CXL_SNOOP_FILTER_GROUP_ID_OFFSET);
	id = FIELD_GET(CXL_SNOOP_FILTER_GROUP_ID_MASK, group);

	size = readl(dport->regs.snoop + CXL_SNOOP_FILTER_SIZE_OFFSET);
	if (!size) {
		dev_dbg(dport->dport_dev, "CXL snoop filter has no capacity\n");
		return 0;
	}

	sf = find_or_add_snoop_filter(size, id);
	if (IS_ERR_OR_NULL(sf)) {
		return PTR_ERR(sf);
	}

	dev_dbg(dport->dport_dev, "assigned snoop filter gid %d\n", sf->id);
	dport->snoop = id;
	return 0;
}
EXPORT_SYMBOL_NS_GPL(cxl_dport_probe_snoop_filter, "CXL");

void cxl_destroy_snoop_filters(void)
{
	struct cxl_snoop_filter *sf;
	unsigned long id;

	guard(rwsem_write)(&snoop_rwsem);
	xa_for_each(&snoop_filters, id, sf)
		destroy_snoop_filter(sf);

	xa_destroy(&snoop_filters);
}

/**
 * cxl_cache_configure_iommu() - Configure a device's IOMMU for CXL.cache
 * @cxlds: struct cxl_dev_state of a cxl_cachedev that has been through
 *	   CXL.cache probe
 *
 * Fails if the underlying PCI device supports ATS and IOMMU can't be
 * configured, or if the device doesn't support ATS and is not attached to an
 * identity IOMMU domain.
 */
int cxl_cache_configure_iommu(struct cxl_dev_state *cxlds)
{
	struct device *dev = cxlds->dev;
	struct iommu_domain *domain;
	int rc;

	lockdep_assert_held(&dev->mutex);

	if (!dev->iommu || !dev->iommu->iommu_dev)
		return 0;

	if (!device_iommu_capable(dev, IOMMU_CAP_PCI_ATS_SUPPORTED)) {
		domain = iommu_get_domain_for_dev(dev);
		if (!domain || domain->type != IOMMU_DOMAIN_IDENTITY)
			return -EINVAL;

		return 0;
	}

	rc = iommu_enable_cxl_ats(cxlds->dev);
	if (rc == -EOPNOTSUPP) {
		dev_warn(cxlds->dev,
			"IOMMU doesn't support enabling CXL ATS requests; CXL.cache may not function properly.");
		rc = 0;
	} else if (rc) {
		dev_err(cxlds->dev, "Failed to enable CXL ATS requests: %d\n",
			rc);
	}

	return rc;
}
EXPORT_SYMBOL_NS_GPL(cxl_cache_configure_iommu, "CXL");
