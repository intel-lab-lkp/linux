/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2023 NXP.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM fsl_edma

#if !defined(__LINUX_FSL_EDMA_TRACE) || defined(TRACE_HEADER_MULTI_READ)
#define __LINUX_FSL_EDMA_TRACE

#include <linux/types.h>
#include <linux/tracepoint.h>

DECLARE_EVENT_CLASS(edma_log_io,
	TP_PROTO(struct fsl_edma_engine *edma, void __iomem *addr, u32 value),
	TP_ARGS(edma, addr, value),
	TP_STRUCT__entry(
		__field(struct fsl_edma_engine *, edma)
		__field(void __iomem *, addr)
		__field(u32, value)
	),
	TP_fast_assign(
		__entry->edma = edma;
		__entry->addr = addr;
		__entry->value = value;
	),
	TP_printk("offset %08x: value %08x",
		(u32)(__entry->addr - __entry->edma->membase), __entry->value)
);

DEFINE_EVENT(edma_log_io, edma_readl,
	TP_PROTO(struct fsl_edma_engine *edma, void __iomem *addr, u32 value),
	TP_ARGS(edma, addr, value)
);

DEFINE_EVENT(edma_log_io, edma_writel,
	TP_PROTO(struct fsl_edma_engine *edma, void __iomem *addr,  u32 value),
	TP_ARGS(edma, addr, value)
);

DEFINE_EVENT(edma_log_io, edma_readw,
	TP_PROTO(struct fsl_edma_engine *edma, void __iomem *addr, u32 value),
	TP_ARGS(edma, addr, value)
);

DEFINE_EVENT(edma_log_io, edma_writew,
	TP_PROTO(struct fsl_edma_engine *edma, void __iomem *addr,  u32 value),
	TP_ARGS(edma, addr, value)
);

DEFINE_EVENT(edma_log_io, edma_readb,
	TP_PROTO(struct fsl_edma_engine *edma, void __iomem *addr, u32 value),
	TP_ARGS(edma, addr, value)
);

DEFINE_EVENT(edma_log_io, edma_writeb,
	TP_PROTO(struct fsl_edma_engine *edma, void __iomem *addr,  u32 value),
	TP_ARGS(edma, addr, value)
);

DECLARE_EVENT_CLASS(edma_log_tcd,
	TP_PROTO(struct fsl_edma_engine *edma, struct fsl_edma_hw_tcd *tcd),
	TP_ARGS(edma, tcd),
	TP_STRUCT__entry(
		__field(struct fsl_edma_engine *, edma)
		__field(u32, saddr)
		__field(u16, soff)
		__field(u16, attr)
		__field(u32, nbytes)
		__field(u32, slast)
		__field(u32, daddr)
		__field(u16, doff)
		__field(u16, citer)
		__field(u32, dlast_sga)
		__field(u16, csr)
		__field(u16, biter)

	),
	TP_fast_assign(
		__entry->edma = edma;
		__entry->saddr = le32_to_cpu(tcd->saddr),
		__entry->soff = le16_to_cpu(tcd->soff),
		__entry->attr = le16_to_cpu(tcd->attr),
		__entry->nbytes = le32_to_cpu(tcd->nbytes),
		__entry->slast = le32_to_cpu(tcd->slast),
		__entry->daddr = le32_to_cpu(tcd->daddr),
		__entry->doff = le16_to_cpu(tcd->doff),
		__entry->citer = le16_to_cpu(tcd->citer),
		__entry->dlast_sga = le32_to_cpu(tcd->dlast_sga),
		__entry->csr = le16_to_cpu(tcd->csr),
		__entry->biter = le16_to_cpu(tcd->biter);
	),
	TP_printk("\n==== TCD =====\n"
		  "  saddr:  0x%08x\n"
		  "  soff:       0x%04x\n"
		  "  attr:       0x%04x\n"
		  "  nbytes: 0x%08x\n"
		  "  slast:  0x%08x\n"
		  "  daddr:  0x%08x\n"
		  "  doff:       0x%04x\n"
		  "  citer:      0x%04x\n"
		  "  dlast:  0x%08x\n"
		  "  csr:        0x%04x\n"
		  "  biter:      0x%04x\n",
		__entry->saddr,
		__entry->soff,
		__entry->attr,
		__entry->nbytes,
		__entry->slast,
		__entry->daddr,
		__entry->doff,
		__entry->citer,
		__entry->dlast_sga,
		__entry->csr,
		__entry->biter)
);

DEFINE_EVENT(edma_log_tcd, edma_fill_tcd,
	TP_PROTO(struct fsl_edma_engine *edma, struct fsl_edma_hw_tcd *tcd),
	TP_ARGS(edma, tcd)
);

#endif

/* this part must be outside header guard */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE fsl-edma-trace

#include <trace/define_trace.h>
