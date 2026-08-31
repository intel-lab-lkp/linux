/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM qcom_gpi

#if !defined(_TRACE_QCOM_GPI_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_QCOM_GPI_H

#include <linux/tracepoint.h>

TRACE_EVENT(gpi_send_cmd,
	    TP_PROTO(struct device *dev, u32 chid, u32 cmd, const char *cmd_str),
	    TP_ARGS(dev, chid, cmd, cmd_str),

	    TP_STRUCT__entry(__string(name, dev_name(dev))
			     __string(cmd_str, cmd_str)
			     __field(u32, chid)
			     __field(u32, cmd)
	    ),

	    TP_fast_assign(__assign_str(name);
			   __assign_str(cmd_str);
			   __entry->chid = chid;
			   __entry->cmd = cmd;
	    ),

	    TP_printk("%s: chid=%u cmd=%s(%u)",
		      __get_str(name), __entry->chid, __get_str(cmd_str),
		      __entry->cmd)
);

TRACE_EVENT(gpi_irq_status,
	    TP_PROTO(struct device *dev, u32 gpii_id, u32 irq_type),
	    TP_ARGS(dev, gpii_id, irq_type),

	    TP_STRUCT__entry(__string(name, dev_name(dev))
			     __field(u32, gpii_id)
			     __field(u32, irq_type)
	    ),

	    TP_fast_assign(__assign_str(name);
			   __entry->gpii_id = gpii_id;
			   __entry->irq_type = irq_type;
	    ),

	    TP_printk("%s: gpii=%u irq_type=0x%08x",
		      __get_str(name), __entry->gpii_id, __entry->irq_type)
);

TRACE_EVENT(gpi_ch_ctrl_irq,
	    TP_PROTO(struct device *dev, u32 chid, u32 ch_state),
	    TP_ARGS(dev, chid, ch_state),

	    TP_STRUCT__entry(__string(name, dev_name(dev))
			     __field(u32, chid)
			     __field(u32, ch_state)
	    ),

	    TP_fast_assign(__assign_str(name);
			   __entry->chid = chid;
			   __entry->ch_state = ch_state;
	    ),

	    TP_printk("%s: chid=%u ch_state=%u",
		      __get_str(name), __entry->chid, __entry->ch_state)
);

TRACE_EVENT(gpi_ev_process,
	    TP_PROTO(struct device *dev, u32 chid, u32 ev_type, u8 code,
		     u16 status, u32 length),
	    TP_ARGS(dev, chid, ev_type, code, status, length),

	    TP_STRUCT__entry(__string(name, dev_name(dev))
			     __field(u32, chid)
			     __field(u32, ev_type)
			     __field(u8, code)
			     __field(u16, status)
			     __field(u32, length)
	    ),

	    TP_fast_assign(__assign_str(name);
			   __entry->chid = chid;
			   __entry->ev_type = ev_type;
			   __entry->code = code;
			   __entry->status = status;
			   __entry->length = length;
	    ),

	    TP_printk("%s: chid=%u ev_type=0x%02x code=%u status=%u length=%u",
		      __get_str(name), __entry->chid, __entry->ev_type,
		      __entry->code, __entry->status, __entry->length)
);

TRACE_EVENT(gpi_queue_xfer,
	    TP_PROTO(struct device *dev, u32 chid, u32 num_tre),
	    TP_ARGS(dev, chid, num_tre),

	    TP_STRUCT__entry(__string(name, dev_name(dev))
			     __field(u32, chid)
			     __field(u32, num_tre)
	    ),

	    TP_fast_assign(__assign_str(name);
			   __entry->chid = chid;
			   __entry->num_tre = num_tre;
	    ),

	    TP_printk("%s: chid=%u num_tre=%u",
		      __get_str(name), __entry->chid, __entry->num_tre)
);

TRACE_EVENT(gpi_gen_err_irq,
	    TP_PROTO(struct device *dev, u32 gpii_id, u32 irq_stts),
	    TP_ARGS(dev, gpii_id, irq_stts),

	    TP_STRUCT__entry(__string(name, dev_name(dev))
			     __field(u32, gpii_id)
			     __field(u32, irq_stts)
	    ),

	    TP_fast_assign(__assign_str(name);
			   __entry->gpii_id = gpii_id;
			   __entry->irq_stts = irq_stts;
	    ),

	    TP_printk("%s: gpii=%u irq_stts=0x%08x",
		      __get_str(name), __entry->gpii_id, __entry->irq_stts)
);

TRACE_EVENT(gpi_ev_ctrl_irq,
	    TP_PROTO(struct device *dev, u32 gpii_id, u32 ev_ch_irq, u32 ev_state),
	    TP_ARGS(dev, gpii_id, ev_ch_irq, ev_state),

	    TP_STRUCT__entry(__string(name, dev_name(dev))
			     __field(u32, gpii_id)
			     __field(u32, ev_ch_irq)
			     __field(u32, ev_state)
	    ),

	    TP_fast_assign(__assign_str(name);
			   __entry->gpii_id = gpii_id;
			   __entry->ev_ch_irq = ev_ch_irq;
			   __entry->ev_state = ev_state;
	    ),

	    TP_printk("%s: gpii=%u ev_ch_irq=0x%08x ev_state=%u",
		      __get_str(name), __entry->gpii_id, __entry->ev_ch_irq,
		      __entry->ev_state)
);

TRACE_EVENT(gpi_ev_no_desc,
	    TP_PROTO(struct device *dev, u32 chid, const u32 *ev_dword,
		     const u32 *tre_dword),
	    TP_ARGS(dev, chid, ev_dword, tre_dword),

	    TP_STRUCT__entry(__string(name, dev_name(dev))
			     __field(u32, chid)
			     __array(u32, ev_dword, 4)
			     __array(u32, tre_dword, 4)
	    ),

	    TP_fast_assign(__assign_str(name);
			   __entry->chid = chid;
			   memcpy(__entry->ev_dword, ev_dword, sizeof(__entry->ev_dword));
			   memcpy(__entry->tre_dword, tre_dword, sizeof(__entry->tre_dword));
	    ),

	    TP_printk("%s: chid=%u event=%08x:%08x:%08x:%08x pending_tre=%08x:%08x:%08x:%08x",
		      __get_str(name), __entry->chid,
		      __entry->ev_dword[0], __entry->ev_dword[1],
		      __entry->ev_dword[2], __entry->ev_dword[3],
		      __entry->tre_dword[0], __entry->tre_dword[1],
		      __entry->tre_dword[2], __entry->tre_dword[3])
);

TRACE_EVENT(gpi_xfer_result,
	    TP_PROTO(struct device *dev, u32 chid, int result, u32 residue),
	    TP_ARGS(dev, chid, result, residue),

	    TP_STRUCT__entry(__string(name, dev_name(dev))
			     __field(u32, chid)
			     __field(int, result)
			     __field(u32, residue)
	    ),

	    TP_fast_assign(__assign_str(name);
			   __entry->chid = chid;
			   __entry->result = result;
			   __entry->residue = residue;
	    ),

	    TP_printk("%s: chid=%u result=%d residue=%u",
		      __get_str(name), __entry->chid, __entry->result,
		      __entry->residue)
);

TRACE_EVENT(gpi_process_event,
	    TP_PROTO(struct device *dev, u32 chid, u32 type, const u32 *dword),
	    TP_ARGS(dev, chid, type, dword),

	    TP_STRUCT__entry(__string(name, dev_name(dev))
			     __field(u32, chid)
			     __field(u32, type)
			     __array(u32, dword, 4)
	    ),

	    TP_fast_assign(__assign_str(name);
			   __entry->chid = chid;
			   __entry->type = type;
			   memcpy(__entry->dword, dword, sizeof(__entry->dword));
	    ),

	    TP_printk("%s: chid=%u type=0x%02x %08x:%08x:%08x:%08x",
		      __get_str(name), __entry->chid, __entry->type,
		      __entry->dword[0], __entry->dword[1],
		      __entry->dword[2], __entry->dword[3])
);

TRACE_EVENT(gpi_alloc_ring,
	    TP_PROTO(struct device *dev, u32 elements, u32 el_size,
		     u32 req_len, u64 len, size_t alloc_size),
	    TP_ARGS(dev, elements, el_size, req_len, len, alloc_size),

	    TP_STRUCT__entry(__string(name, dev_name(dev))
			     __field(u32, elements)
			     __field(u32, el_size)
			     __field(u32, req_len)
			     __field(u64, len)
			     __field(size_t, alloc_size)
	    ),

	    TP_fast_assign(__assign_str(name);
			   __entry->elements = elements;
			   __entry->el_size = el_size;
			   __entry->req_len = req_len;
			   __entry->len = len;
			   __entry->alloc_size = alloc_size;
	    ),

	    TP_printk("%s: elements=%u el_size=%u req_len=%u len=%llu alloc_size=%zu",
		      __get_str(name), __entry->elements, __entry->el_size,
		      __entry->req_len, __entry->len, __entry->alloc_size)
);

TRACE_EVENT(gpi_ring_info,
	    TP_PROTO(struct device *dev, dma_addr_t dma_handle, phys_addr_t phys_addr,
		     u32 len, u32 el_size, u32 elements),
	    TP_ARGS(dev, dma_handle, phys_addr, len, el_size, elements),

	    TP_STRUCT__entry(__string(name, dev_name(dev))
			     __field(u64, dma_handle)
			     __field(u64, phys_addr)
			     __field(u32, len)
			     __field(u32, el_size)
			     __field(u32, elements)
	    ),

	    TP_fast_assign(__assign_str(name);
			   __entry->dma_handle = dma_handle;
			   __entry->phys_addr = phys_addr;
			   __entry->len = len;
			   __entry->el_size = el_size;
			   __entry->elements = elements;
	    ),

	    TP_printk("%s: dma_handle=%llx phys_addr=%llx len=%u el_size=%u elements=%u",
		      __get_str(name), __entry->dma_handle, __entry->phys_addr,
		      __entry->len, __entry->el_size, __entry->elements)
);

TRACE_EVENT(gpi_already_state,
	    TP_PROTO(struct device *dev, u32 gpii_id, u32 pm_state),
	    TP_ARGS(dev, gpii_id, pm_state),

	    TP_STRUCT__entry(__string(name, dev_name(dev))
			     __field(u32, gpii_id)
			     __field(u32, pm_state)
	    ),

	    TP_fast_assign(__assign_str(name);
			   __entry->gpii_id = gpii_id;
			   __entry->pm_state = pm_state;
	    ),

	    TP_printk("%s: gpii=%u already in pm_state=%u",
		      __get_str(name), __entry->gpii_id, __entry->pm_state)
);

TRACE_EVENT(gpi_tre,
	    TP_PROTO(struct device *dev, u32 chid, u32 idx, const u32 *dword),
	    TP_ARGS(dev, chid, idx, dword),

	    TP_STRUCT__entry(__string(name, dev_name(dev))
			     __field(u32, chid)
			     __field(u32, idx)
			     __array(u32, dword, 4)
	    ),

	    TP_fast_assign(__assign_str(name);
			   __entry->chid = chid;
			   __entry->idx = idx;
			   memcpy(__entry->dword, dword, sizeof(__entry->dword));
	    ),

	    TP_printk("%s: chid=%u tre[%u]=%08x:%08x:%08x:%08x",
		      __get_str(name), __entry->chid, __entry->idx,
		      __entry->dword[0], __entry->dword[1],
		      __entry->dword[2], __entry->dword[3])
);

#endif /* _TRACE_QCOM_GPI_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
