/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM dma

#if !defined(_TRACE_DMA_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_DMA_H

#include <linux/tracepoint.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>

TRACE_DEFINE_ENUM(DMA_BIDIRECTIONAL);
TRACE_DEFINE_ENUM(DMA_TO_DEVICE);
TRACE_DEFINE_ENUM(DMA_FROM_DEVICE);
TRACE_DEFINE_ENUM(DMA_NONE);

#define decode_dma_data_direction(dir) \
	__print_symbolic(dir, \
		{ DMA_BIDIRECTIONAL, "BIDIRECTIONAL" }, \
		{ DMA_TO_DEVICE, "TO_DEVICE" }, \
		{ DMA_FROM_DEVICE, "FROM_DEVICE" }, \
		{ DMA_NONE, "NONE" })

#define decode_dma_attrs(attrs) \
	__print_flags(attrs, "|", \
		{ DMA_ATTR_WEAK_ORDERING, "WEAK_ORDERING" }, \
		{ DMA_ATTR_WRITE_COMBINE, "WRITE_COMBINE" }, \
		{ DMA_ATTR_NO_KERNEL_MAPPING, "NO_KERNEL_MAPPING" }, \
		{ DMA_ATTR_SKIP_CPU_SYNC, "SKIP_CPU_SYNC" }, \
		{ DMA_ATTR_FORCE_CONTIGUOUS, "FORCE_CONTIGUOUS" }, \
		{ DMA_ATTR_ALLOC_SINGLE_PAGES, "ALLOC_SINGLE_PAGES" }, \
		{ DMA_ATTR_NO_WARN, "NO_WARN" }, \
		{ DMA_ATTR_PRIVILEGED, "PRIVILEGED" })

TRACE_EVENT(dma_map_page,
	TP_PROTO(struct device *dev, const struct page *page, size_t offset,
		 size_t size, enum dma_data_direction dir, dma_addr_t addr,
		 unsigned long attrs),
	TP_ARGS(dev, page, offset, size, dir, addr, attrs),

	TP_STRUCT__entry(
		__string(device, dev_name(dev))
		__field(const struct page *, page)
		__field(size_t, offset)
		__field(size_t, size)
		__field(enum dma_data_direction, dir)
		__field(dma_addr_t, addr)
		__field(unsigned long, attrs)
	),

	TP_fast_assign(
		__assign_str(device);
		__entry->page = page;
		__entry->offset = offset;
		__entry->size = size;
		__entry->dir = dir;
		__entry->addr = addr;
		__entry->attrs = attrs;
	),

	TP_printk("%s page=%p offset=%zu size=%zu dir=%s addr=%llx attrs=%s",
		__get_str(device),
		__entry->page,
		__entry->offset,
		__entry->size,
		decode_dma_data_direction(__entry->dir),
		(unsigned long long)__entry->addr,
		decode_dma_attrs(__entry->attrs))
);

TRACE_EVENT(dma_unmap_page,
	TP_PROTO(struct device *dev, dma_addr_t addr, size_t size,
		 enum dma_data_direction dir, unsigned long attrs),
	TP_ARGS(dev, addr, size, dir, attrs),

	TP_STRUCT__entry(
		__string(device, dev_name(dev))
		__field(dma_addr_t, addr)
		__field(size_t, size)
		__field(enum dma_data_direction, dir)
		__field(unsigned long, attrs)
	),

	TP_fast_assign(
		__assign_str(device);
		__entry->addr = addr;
		__entry->size = size;
		__entry->dir = dir;
		__entry->attrs = attrs;
	),

	TP_printk("%s addr=%llx size=%zu dir=%s attrs=%s",
		__get_str(device),
		(unsigned long long)__entry->addr,
		__entry->size,
		decode_dma_data_direction(__entry->dir),
		decode_dma_attrs(__entry->attrs))
);

#endif /*  _TRACE_DMA_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
