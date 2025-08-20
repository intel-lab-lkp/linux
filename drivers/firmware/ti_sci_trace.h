/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM ti_sci

#if !defined(_TRACE_TI_SCI_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_TI_SCI_H

#include <linux/tracepoint.h>


DECLARE_EVENT_CLASS(ti_sci_hdr_event_class,
	TP_PROTO(struct ti_sci_msg_hdr *hdr, int status),
	TP_ARGS(hdr, status),

	TP_STRUCT__entry(
		__field(u16, type)
		__field(u8, host)
		__field(u8, seq)
		__field(u32, flags)
		__field(int, status)
	),

	TP_fast_assign(
		__entry->type = hdr->type;
		__entry->host = hdr->host;
		__entry->seq = hdr->seq;
		__entry->flags = hdr->flags;
		__entry->status = status;
	),

	TP_printk("type=%04X host=%02X seq=%02X flags=%08X status=%d",
		__entry->type, __entry->host, __entry->seq, __entry->flags, __entry->status)
);

DEFINE_EVENT(ti_sci_hdr_event_class,
	ti_sci_xfer_begin,
	TP_PROTO(struct ti_sci_msg_hdr *hdr, int status),
	TP_ARGS(hdr, status)
);

DEFINE_EVENT(ti_sci_hdr_event_class,
	ti_sci_rx_callback,
	TP_PROTO(struct ti_sci_msg_hdr *hdr, int status),
	TP_ARGS(hdr, status)
);

DEFINE_EVENT(ti_sci_hdr_event_class,
	ti_sci_xfer_end,
	TP_PROTO(struct ti_sci_msg_hdr *hdr, int status),
	TP_ARGS(hdr, status)
);


TRACE_EVENT(ti_sci_msg_dump,
	TP_PROTO(struct ti_sci_msg_hdr *hdr, struct ti_sci_xfer *xfer),
	TP_ARGS(hdr, xfer),

	TP_STRUCT__entry(
		__field(u16, type)
		__field(u8, host)
		__field(u8, seq)
		__field(u32, flags)
		__field(size_t, len)
		__dynamic_array(unsigned char, cmd, xfer->rx_len)
	),

	TP_fast_assign(
		__entry->type = hdr->type;
		__entry->host = hdr->host;
		__entry->seq = hdr->seq;
		__entry->flags = hdr->flags;
		__entry->len = xfer->rx_len;
		memcpy(__get_dynamic_array(cmd), xfer->xfer_buf, __entry->len);
	),

	TP_printk("type=%04X host=%02X seq=%02X flags=%08X data=%s",
		__entry->type, __entry->host, __entry->seq, __entry->flags,
		__print_hex_str(__get_dynamic_array(cmd), __entry->len))
);
#endif /* _TRACE_TI_SCI_H */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE ti_sci_trace
#include <trace/define_trace.h>
