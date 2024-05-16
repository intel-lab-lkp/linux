/* SPDX-License-Identifier: GPL-2.0 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM fsi

#if !defined(_TRACE_FSI_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_FSI_H

#include <linux/tracepoint.h>

TRACE_EVENT(fsi_master_xfer,
	TP_PROTO(int master_idx, int link, int id, uint32_t addr, size_t size, const void *data,
		 bool read),
	TP_ARGS(master_idx, link, id, addr, size, data, read),
	TP_STRUCT__entry(
		__field(int, master_idx)
		__field(int, link)
		__field(int, id)
		__field(uint32_t, addr)
		__field(int, size)
		__field(uint32_t, data)
		__field(bool, read)
	),
	TP_fast_assign(
		__entry->master_idx = master_idx;
		__entry->link = link;
		__entry->id = id;
		__entry->addr = addr;
		__entry->size = (int)size;
		__entry->data = 0;
		memcpy(&__entry->data, data, size);
		__entry->read = read;
	),
	TP_printk("fsi%d:%02d:%02d %s %08x {%*ph}", __entry->master_idx, __entry->link,
		  __entry->id, __entry->read ? "read" : "write", __entry->addr, __entry->size,
		  &__entry->data)
);

TRACE_EVENT(fsi_master_error,
	TP_PROTO(int master_idx, int link, int id, uint32_t addr, size_t size, const void *data,
		 int ret, bool read),
	TP_ARGS(master_idx, link, id, addr, size, data, ret, read),
	TP_STRUCT__entry(
		__field(int, master_idx)
		__field(int, link)
		__field(int, id)
		__field(uint32_t, addr)
		__field(int, size)
		__field(uint32_t, data)
		__field(int, ret)
		__field(bool, read)
	),
	TP_fast_assign(
		__entry->master_idx = master_idx;
		__entry->link = link;
		__entry->id = id;
		__entry->addr = addr;
		__entry->size = (int)size;
		__entry->data = 0;
		if (!read)
			memcpy(&__entry->data, data, size);
		__entry->ret = ret;
		__entry->read = read;
	),
	TP_printk("fsi%d:%02d:%02d %s %08x {%*ph} %d", __entry->master_idx, __entry->link,
		  __entry->id, __entry->read ? "read" : "write", __entry->addr, __entry->size,
		  &__entry->data, __entry->ret)
);

TRACE_EVENT(fsi_master_break,
	TP_PROTO(const struct fsi_master *master, int link),
	TP_ARGS(master, link),
	TP_STRUCT__entry(
		__field(int,	master_idx)
		__field(int,	link)
	),
	TP_fast_assign(
		__entry->master_idx = master->idx;
		__entry->link = link;
	),
	TP_printk("fsi%d:%d",
		__entry->master_idx,
		__entry->link
	)
);

TRACE_EVENT(fsi_master_scan,
	TP_PROTO(const struct fsi_master *master, bool scan),
	TP_ARGS(master, scan),
	TP_STRUCT__entry(
		__field(int,	master_idx)
		__field(int,	n_links)
		__field(bool,	scan)
	),
	TP_fast_assign(
		__entry->master_idx = master->idx;
		__entry->n_links = master->n_links;
		__entry->scan = scan;
	),
	TP_printk("fsi%d (%d links) %s", __entry->master_idx, __entry->n_links,
		  __entry->scan ? "scan" : "unscan")
);

TRACE_EVENT(fsi_master_unregister,
	TP_PROTO(const struct fsi_master *master),
	TP_ARGS(master),
	TP_STRUCT__entry(
		__field(int,	master_idx)
		__field(int,	n_links)
	),
	TP_fast_assign(
		__entry->master_idx = master->idx;
		__entry->n_links = master->n_links;
	),
	TP_printk("fsi%d (%d links)", __entry->master_idx, __entry->n_links)
);

TRACE_EVENT(fsi_slave_error,
	TP_PROTO(const struct fsi_slave *slave, uint32_t sisc, uint32_t sstat),
	TP_ARGS(slave, sisc, sstat),
	TP_STRUCT__entry(
		__field(int, master_idx)
		__field(int, link)
		__field(uint32_t, sisc)
		__field(uint32_t, sstat)
	),
	TP_fast_assign(
		__entry->master_idx = slave->master->idx;
		__entry->link = slave->link;
		__entry->sisc = sisc;
		__entry->sstat = sstat;
	),
	TP_printk("fsi%d:%02d sisc:%08x sstat:%08x", __entry->master_idx, __entry->link,
		  __entry->sisc, __entry->sstat)
);

TRACE_EVENT(fsi_slave_init,
	TP_PROTO(const struct fsi_slave *slave),
	TP_ARGS(slave),
	TP_STRUCT__entry(
		__field(int,	master_idx)
		__field(int,	master_n_links)
		__field(int,	idx)
		__field(int,	link)
		__field(int,	chip_id)
		__field(__u32,	cfam_id)
		__field(__u32,	size)
	),
	TP_fast_assign(
		__entry->master_idx = slave->master->idx;
		__entry->master_n_links = slave->master->n_links;
		__entry->idx = slave->cdev_idx;
		__entry->link = slave->link;
		__entry->chip_id = slave->chip_id;
		__entry->cfam_id = slave->cfam_id;
		__entry->size = slave->size;
	),
	TP_printk("fsi%d: idx:%d link:%d/%d cid:%d cfam:%08x %08x",
		__entry->master_idx,
		__entry->idx,
		__entry->link,
		__entry->master_n_links,
		__entry->chip_id,
		__entry->cfam_id,
		__entry->size
	)
);

TRACE_EVENT(fsi_slave_invalid_cfam,
	TP_PROTO(const struct fsi_master *master, int link, uint32_t cfam_id),
	TP_ARGS(master, link, cfam_id),
	TP_STRUCT__entry(
		__field(int,	master_idx)
		__field(int,	master_n_links)
		__field(int,	link)
		__field(__u32,	cfam_id)
	),
	TP_fast_assign(
		__entry->master_idx = master->idx;
		__entry->master_n_links = master->n_links;
		__entry->link = link;
		__entry->cfam_id = cfam_id;
	),
	TP_printk("fsi%d: cfam:%08x link:%d/%d",
		__entry->master_idx,
		__entry->cfam_id,
		__entry->link,
		__entry->master_n_links
	)
);

TRACE_EVENT(fsi_dev_init,
	TP_PROTO(const struct fsi_device *dev),
	TP_ARGS(dev),
	TP_STRUCT__entry(
		__field(int,	master_idx)
		__field(int,	link)
		__field(int,	type)
		__field(int,	unit)
		__field(int,	version)
		__field(__u32,	addr)
		__field(__u32,	size)
	),
	TP_fast_assign(
		__entry->master_idx = dev->slave->master->idx;
		__entry->link = dev->slave->link;
		__entry->type = dev->engine_type;
		__entry->unit = dev->unit;
		__entry->version = dev->version;
		__entry->addr = dev->addr;
		__entry->size = dev->size;
	),
	TP_printk("fsi%d: slv%d: t:%02x u:%02x v:%02x %08x@%08x",
		__entry->master_idx,
		__entry->link,
		__entry->type,
		__entry->unit,
		__entry->version,
		__entry->size,
		__entry->addr
	)
);

#endif /* _TRACE_FSI_H */

#include <trace/define_trace.h>
