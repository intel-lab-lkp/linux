/* SPDX-License-Identifier: GPL-2.0-or-later */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM livedump

#if !defined(_TRACE_LIVEDUMP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_LIVEDUMP_H

#include <linux/blk_types.h>
#include <linux/tracepoint.h>

TRACE_EVENT(memdump_bio_submit,
	TP_PROTO(struct block_device *bdev, unsigned long pfn),
	TP_ARGS(bdev, pfn),
	TP_STRUCT__entry(
		__field(struct block_device *, bdev)
		__field(unsigned long, pfn)
	),
	TP_fast_assign(
		if (bdev != NULL)
			__entry->bdev = bdev;
		__entry->pfn = pfn;
	),
	TP_printk("bdev=%u, pfn=%lu", __entry->bdev->bd_dev, __entry->pfn)
);
#endif /* _TRACE_LIVEDUMP_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ../../kernel/livedump/
#define TRACE_INCLUDE_FILE memdump_trace
#include <trace/define_trace.h>
