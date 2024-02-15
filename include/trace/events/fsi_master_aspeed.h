/* SPDX-License-Identifier: GPL-2.0-or-later */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM fsi_master_aspeed

#if !defined(_TRACE_FSI_MASTER_ASPEED_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_FSI_MASTER_ASPEED_H

#include <linux/tracepoint.h>

TRACE_EVENT(fsi_master_aspeed_opb_xfer,
	TP_PROTO(uint32_t addr, uint32_t size, uint32_t data, bool read),
	TP_ARGS(addr, size, data, read),
	TP_STRUCT__entry(
		__field(uint32_t, addr)
		__field(uint32_t, size)
		__field(uint32_t, data)
		__field(bool, read)
	),
	TP_fast_assign(
		__entry->addr = addr;
		__entry->size = size;
		__entry->data = data;
		__entry->read = read;
	),
	TP_printk("%s addr %08x size %u data %08x", __entry->read ? "read" : "write",
		  __entry->addr, __entry->size, __entry->data)
);

TRACE_EVENT(fsi_master_aspeed_timeout,
	TP_PROTO(uint32_t irq, uint32_t status, bool read),
	TP_ARGS(irq, status, read),
	TP_STRUCT__entry(
		__field(uint32_t, irq)
		__field(uint32_t, status)
		__field(bool, read)
	),
	TP_fast_assign(
		__entry->irq = irq;
		__entry->status = status;
		__entry->read = read;
	),
	TP_printk("%s irq %08x status %08x", __entry->read ? "read" : "write", __entry->irq,
		  __entry->status)
);

TRACE_EVENT(fsi_master_aspeed_cfam_reset,
	TP_PROTO(bool start),
	TP_ARGS(start),
	TP_STRUCT__entry(
		__field(bool,	start)
	),
	TP_fast_assign(
		__entry->start = start;
	),
	TP_printk("%s", __entry->start ? "start" : "end")
);

#endif

#include <trace/define_trace.h>
