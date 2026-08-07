/* SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause */
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 * Copyright(c) 2017 Intel Corporation.
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * BSD LICENSE
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  - Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  - Neither the name of Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#if !defined(__HFI2_TRACE_PIN_H) || defined(TRACE_HEADER_MULTI_READ)
#define __HFI2_TRACE_PIN_H

#include <linux/tracepoint.h>
#include <linux/trace_seq.h>

const char *hfi2_memtype_str(unsigned int mt);

#undef TRACE_SYSTEM
#define TRACE_SYSTEM hfi2_pin

/*
 * @ctxt: pq, cache ptr, something that distinguishes the trace. Not necessarily
 *   an fd.
 * @memtype: HFI2_MEMINFO_* value
 * @va_addr: virtual address start
 * @va_len: "..." length
 * @ret signal: error with the pin operation
 * @from_cache: 0 or 1
 * @node: returned cache node; NULL if not found
 * @dma_addr: returned pinning physical or DMA address, depending
 *   on if kernel API has separate pin and DMA map operations for
 *   the memory type. 0 if not found.
 * @dma_len: 0 if not found.
 */
DECLARE_EVENT_CLASS(hfi2_pin_mem,
		    /* There are only a handful of memtypes; use u16 to minimize struct size */
		    TP_PROTO(void *ctxt, u16 memtype,
			     unsigned long va_addr, unsigned long va_len,
			     int ret, bool from_cache,
			     void *node,
			     dma_addr_t dma_addr, unsigned long dma_len),
		    TP_ARGS(ctxt, memtype, va_addr, va_len, ret, from_cache, node, dma_addr,
			    dma_len),
		    TP_STRUCT__entry(__field(void *, ctxt)
				     __field(u16, memtype)
				     __field(u16, from_cache)
				     __field(int, ret)
				     __field(unsigned long, va_addr)
				     __field(unsigned long, va_len)
				     __field(void *, node)
				     __field(dma_addr_t, dma_addr)
				     __field(unsigned long, dma_len)
			    ),
		    TP_fast_assign(__entry->ctxt = ctxt;
				   __entry->memtype = memtype;
				   __entry->va_addr = va_addr;
				   __entry->va_len = va_len;
				   __entry->ret = ret;
				   __entry->from_cache = from_cache;
				   __entry->node = node;
				   __entry->dma_addr = dma_addr;
				   __entry->dma_len = dma_len;
			    ),
		    TP_printk("ctxt %p %s (%u) VA start %p length %lu ret %d from_cache %u node %p phys/DMA start %pad length %lu",
			      __entry->ctxt,
			      hfi2_memtype_str(__entry->memtype),
			      __entry->memtype,
			      (void *)__entry->va_addr,
			      __entry->va_len,
			      __entry->ret,
			      __entry->from_cache,
			      __entry->node,
			      &__entry->dma_addr,
			      __entry->dma_len
			    )
);

DECLARE_EVENT_CLASS(hfi2_unpin_mem,
		    TP_PROTO(void *ctxt, unsigned int memtype,
			     int ret,
			     void *node,
			     unsigned long va_addr, unsigned long va_len,
			     dma_addr_t dma_addr, unsigned long dma_len),
		    TP_ARGS(ctxt, memtype, ret, node, va_addr, va_len, dma_addr, dma_len),
		    TP_STRUCT__entry(__field(void *, ctxt)
				    __field(unsigned int, memtype)
				    __field(int, ret)
				    __field(void *, node)
				    __field(unsigned long, va_addr)
				    __field(unsigned long, va_len)
				    __field(dma_addr_t, dma_addr)
				    __field(unsigned long, dma_len)
			    ),
		    TP_fast_assign(__entry->ctxt = ctxt;
				   __entry->memtype = memtype;
				   __entry->ret = ret;
				   __entry->node = node;
				   __entry->va_addr = va_addr;
				   __entry->va_len = va_len;
				   __entry->dma_addr = dma_addr;
				   __entry->dma_len = dma_len;
			    ),
		    TP_printk("ctxt %p %s (%u) VA start %p length %lu ret %u node %p phys/DMA start %pad length %lu",
			      __entry->ctxt,
			      hfi2_memtype_str(__entry->memtype),
			      __entry->memtype,
			      (void *)__entry->va_addr,
			      __entry->va_len,
			      __entry->ret,
			      __entry->node,
			      &__entry->dma_addr,
			      __entry->dma_len)
);

DEFINE_EVENT(hfi2_pin_mem, pin_sdma_mem,
	     TP_PROTO(void *ctxt, u16 memtype,
		      unsigned long va_start, unsigned long va_len,
		      int ret, bool from_cache,
		      void *node,
		      dma_addr_t dma_addr, unsigned long dma_len),
	     TP_ARGS(ctxt, memtype, va_start, va_len, ret, from_cache, node, dma_addr, dma_len));

DEFINE_EVENT(hfi2_unpin_mem, unpin_sdma_mem,
	     TP_PROTO(void *ctxt, unsigned int memtype,
		      int ret,
		      void *node,
		      unsigned long va_addr, unsigned long va_len,
		      dma_addr_t dma_addr, unsigned long dma_len),
	     TP_ARGS(ctxt, memtype, ret, node, va_addr, va_len, dma_addr, dma_len));

TRACE_EVENT(pin_stats,
	    TP_PROTO(void *ctxt, struct hfi2_pin_stats *s),
	    TP_ARGS(ctxt, s),
	    TP_STRUCT__entry(__field(void *, ctxt)
			     __field_struct(struct hfi2_pin_stats, s)),
	    TP_fast_assign(__entry->ctxt = ctxt;
			   __entry->s = *s;
	    ),
	    TP_printk("ctxt %p %s (%u) index %d id %llx cache_entries %llu total_refcounts %llu total_bytes %llu hits %llu misses %llu hint_hits %llu hint_misses %llu internal_evictions %llu external_evictions %llu",
		      __entry->ctxt,
		      hfi2_memtype_str(__entry->s.memtype),
		      __entry->s.memtype,
		      __entry->s.index,
		      __entry->s.id,
		      __entry->s.cache_entries,
		      __entry->s.total_refcounts,
		      __entry->s.total_bytes,
		      __entry->s.hits,
		      __entry->s.misses,
		      __entry->s.hint_hits,
		      __entry->s.hint_misses,
		      __entry->s.internal_evictions,
		      __entry->s.external_evictions)
);

#endif /* __HFI2_TRACE_PIN_H */

#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE trace_pin
#include <trace/define_trace.h>
