/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright 2023 NXP.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM dw_edma

#if !defined(__LINUX_DW_EDMA_TRACE) || defined(TRACE_HEADER_MULTI_READ)
#define __LINUX_DW_EDMA_TRACE

#include <linux/types.h>
#include <linux/tracepoint.h>

TRACE_DEFINE_ENUM(DW_EDMA_LL_EVENT_NONE);
TRACE_DEFINE_ENUM(DW_EDMA_LL_EVENT_PROGRESS);
TRACE_DEFINE_ENUM(DW_EDMA_LL_EVENT_STOP);

DECLARE_EVENT_CLASS(edma_desc_info,
	TP_PROTO(struct dw_edma_desc *desc),
	TP_ARGS(desc),
	TP_STRUCT__entry(
		__field(size_t, nburst)
		__field(size_t, start_burst)
		__field(dma_cookie_t, cookie)
		__field(u32, id)
		__field(u8, dir)
	),
	TP_fast_assign(
		__entry->nburst = desc->nburst;
		__entry->start_burst = desc->start_burst;
		__entry->id = desc->chan->id;
		__entry->dir = desc->chan->dir;
		__entry->cookie = desc->vd.tx.cookie;
	),
	TP_printk("chan %u%c desc %d nburst %zu start_burst %zu",
		__entry->id,
		__entry->dir ? 'R' : 'W',
		__entry->cookie,
		__entry->nburst,
		__entry->start_burst)
);

DEFINE_EVENT(edma_desc_info, edma_append_desc,
	TP_PROTO(struct dw_edma_desc *desc),
	TP_ARGS(desc)
);

DEFINE_EVENT(edma_desc_info, edma_complete_desc,
	TP_PROTO(struct dw_edma_desc *desc),
	TP_ARGS(desc)
);

TRACE_EVENT(edma_irq,
	TP_PROTO(struct dw_edma_chan *chan,
		 const struct dw_edma_ll_snapshot *snapshot),
	TP_ARGS(chan, snapshot),
	TP_STRUCT__entry(
		__field(u32, head)
		__field(u32, done)
		__field(u32, total)
		__field(int, index)
		__field(int, event)
		__field(dma_cookie_t, completed_cookie)
		__field(dma_cookie_t, cookie)
		__field(u32, id)
		__field(u8, dir)
	),
	TP_fast_assign(
		__entry->head = chan->ll_head;
		__entry->done = chan->ll_done;
		__entry->total = chan->ll_max;
		__entry->index = snapshot->idx;
		__entry->event = snapshot->event;
		__entry->completed_cookie = chan->vc.chan.completed_cookie;
		__entry->cookie = chan->vc.chan.cookie;
		__entry->id = chan->id;
		__entry->dir = chan->dir;
	),
	TP_printk("chan %u%c event %s head %u done %u total %u idx %d completed %d cookie %d",
		  __entry->id,
		  __entry->dir ? 'R' : 'W',
		  __print_symbolic(__entry->event,
				   { DW_EDMA_LL_EVENT_NONE, "none" },
				   { DW_EDMA_LL_EVENT_PROGRESS, "progress" },
				   { DW_EDMA_LL_EVENT_STOP, "stop" }),
		  __entry->head,
		  __entry->done,
		  __entry->total,
		  __entry->index,
		  __entry->completed_cookie,
		  __entry->cookie)
);

TRACE_EVENT(edma_engine_recovery,
	TP_PROTO(struct dw_edma_chan *chan, enum dma_status hw_status,
		 enum dw_edma_request request, enum dw_edma_status status,
		 u32 ll_head, u32 ll_done, u32 pending),
	TP_ARGS(chan, hw_status, request, status, ll_head, ll_done, pending),
	TP_STRUCT__entry(
		__field(u32, id)
		__field(u32, head)
		__field(u32, done)
		__field(u32, pending)
		__field(int, hw_status)
		__field(int, request)
		__field(int, status)
		__field(u8, dir)
	),
	TP_fast_assign(
		__entry->id = chan->id;
		__entry->dir = chan->dir;
		__entry->hw_status = hw_status;
		__entry->request = request;
		__entry->status = status;
		__entry->head = ll_head;
		__entry->done = ll_done;
		__entry->pending = pending;
	),
	TP_printk("chan %u%c hw %d request %d status %d head %u done %u pending %u",
		__entry->id,
		__entry->dir ? 'R' : 'W',
		__entry->hw_status,
		__entry->request,
		__entry->status,
		__entry->head,
		__entry->done,
		__entry->pending)
);

TRACE_EVENT(edma_fill_ll,
	TP_PROTO(struct dw_edma_chan *chan, u32 idx, dma_cookie_t cookie, u64 src,
		 u64 dest, u32 sz, bool flag),
	TP_ARGS(chan, idx, cookie, src, dest, sz, flag),
	TP_STRUCT__entry(
		__field(u32, idx)
		__field(u64, src)
		__field(u64, dest)
		__field(u32, sz)
		__field(u32, id)
		__field(dma_cookie_t, cookie)
		__field(bool, flag)
		__field(u8, dir)
	),
	TP_fast_assign(
		__entry->idx = idx;
		__entry->src = src;
		__entry->dest = dest;
		__entry->sz = sz;
		__entry->id = chan->id;
		__entry->dir = chan->dir;
		__entry->cookie = cookie;
		__entry->flag = flag;
	),
	TP_printk("chan %u%c %d [%u] %c src: %08llx dest: %08llx sz: %04x",
		__entry->id,
		__entry->dir ? 'R' : 'W',
		__entry->cookie,
		__entry->idx,
		__entry->flag ? 'C' : 'c',
		__entry->src,
		__entry->dest,
		__entry->sz)
);

#endif

/* This part must be outside the header guard. */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE dw-edma-trace

#include <trace/define_trace.h>
