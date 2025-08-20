/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM ti_sci

#if !defined(_TRACE_TI_SCI_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_TI_SCI_H

#include <linux/tracepoint.h>

#define show_ti_sci_msg_type(x) \
	__print_symbolic(x, \
		{ TI_SCI_MSG_ENABLE_WDT, "ENABLE_WDT" }, \
		{ TI_SCI_MSG_WAKE_RESET, "WAKE_RESET" }, \
		{ TI_SCI_MSG_VERSION, "VERSION" }, \
		{ TI_SCI_MSG_WAKE_REASON, "WAKE_REASON" }, \
		{ TI_SCI_MSG_GOODBYE, "GOODBYE" }, \
		{ TI_SCI_MSG_SYS_RESET, "SYS_RESET" }, \
		{ TI_SCI_MSG_QUERY_FW_CAPS, "QUERY_FW_CAPS" }, \
		{ TI_SCI_MSG_SET_DEVICE_STATE, "SET_DEVICE_STATE" }, \
		{ TI_SCI_MSG_GET_DEVICE_STATE, "GET_DEVICE_STATE" }, \
		{ TI_SCI_MSG_SET_DEVICE_RESETS, "SET_DEVICE_RESETS" }, \
		{ TI_SCI_MSG_SET_CLOCK_STATE, "SET_CLOCK_STATE" }, \
		{ TI_SCI_MSG_GET_CLOCK_STATE, "GET_CLOCK_STATE" }, \
		{ TI_SCI_MSG_SET_CLOCK_PARENT, "SET_CLOCK_PARENT" }, \
		{ TI_SCI_MSG_GET_CLOCK_PARENT, "GET_CLOCK_PARENT" }, \
		{ TI_SCI_MSG_GET_NUM_CLOCK_PARENTS, "GET_NUM_CLOCK_PARENTS" }, \
		{ TI_SCI_MSG_SET_CLOCK_FREQ, "SET_CLOCK_FREQ" }, \
		{ TI_SCI_MSG_QUERY_CLOCK_FREQ, "QUERY_CLOCK_FREQ" }, \
		{ TI_SCI_MSG_GET_CLOCK_FREQ, "GET_CLOCK_FREQ" }, \
		{ TI_SCI_MSG_PREPARE_SLEEP, "PREPARE_SLEEP" }, \
		{ TI_SCI_MSG_LPM_WAKE_REASON, "LPM_WAKE_REASON" }, \
		{ TI_SCI_MSG_SET_IO_ISOLATION, "SET_IO_ISOLATION" }, \
		{ TI_SCI_MSG_LPM_SET_DEVICE_CONSTRAINT, "LPM_SET_DEVICE_CONSTRAINT" }, \
		{ TI_SCI_MSG_LPM_SET_LATENCY_CONSTRAINT, "LPM_SET_LATENCY_CONSTRAINT" }, \
		{ TI_SCI_MSG_GET_RESOURCE_RANGE, "GET_RESOURCE_RANGE" }, \
		{ TI_SCI_MSG_SET_IRQ, "SET_IRQ" }, \
		{ TI_SCI_MSG_FREE_IRQ, "FREE_IRQ" }, \
		{ TI_SCI_MSG_RM_RING_ALLOCATE, "RM_RING_ALLOCATE" }, \
		{ TI_SCI_MSG_RM_RING_FREE, "RM_RING_FREE" }, \
		{ TI_SCI_MSG_RM_RING_RECONFIG, "RM_RING_RECONFIG" }, \
		{ TI_SCI_MSG_RM_RING_RESET, "RM_RING_RESET" }, \
		{ TI_SCI_MSG_RM_RING_CFG, "RM_RING_CFG" }, \
		{ TI_SCI_MSG_RM_PSIL_PAIR, "RM_PSIL_PAIR" }, \
		{ TI_SCI_MSG_RM_PSIL_UNPAIR, "RM_PSIL_UNPAIR" }, \
		{ TI_SCI_MSG_RM_UDMAP_TX_ALLOC, "RM_UDMAP_TX_ALLOC" }, \
		{ TI_SCI_MSG_RM_UDMAP_TX_FREE, "RM_UDMAP_TX_FREE" }, \
		{ TI_SCI_MSG_RM_UDMAP_RX_ALLOC, "RM_UDMAP_RX_ALLOC" }, \
		{ TI_SCI_MSG_RM_UDMAP_RX_FREE, "RM_UDMAP_RX_FREE" }, \
		{ TI_SCI_MSG_RM_UDMAP_FLOW_CFG, "RM_UDMAP_FLOW_CFG" }, \
		{ TI_SCI_MSG_RM_UDMAP_OPT_FLOW_CFG, "RM_UDMAP_OPT_FLOW_CFG" }, \
		{ TISCI_MSG_RM_UDMAP_TX_CH_CFG, "RM_UDMAP_TX_CH_CFG" }, \
		{ TISCI_MSG_RM_UDMAP_TX_CH_GET_CFG, "RM_UDMAP_TX_CH_GET_CFG" }, \
		{ TISCI_MSG_RM_UDMAP_RX_CH_CFG, "RM_UDMAP_RX_CH_CFG" }, \
		{ TISCI_MSG_RM_UDMAP_RX_CH_GET_CFG, "RM_UDMAP_RX_CH_GET_CFG" }, \
		{ TISCI_MSG_RM_UDMAP_FLOW_CFG, "RM_UDMAP_FLOW_CFG" }, \
		{ TISCI_MSG_RM_UDMAP_FLOW_SIZE_THRESH_CFG, "RM_UDMAP_FLOW_SIZE_THRESH_CFG" }, \
		{ TISCI_MSG_RM_UDMAP_FLOW_GET_CFG, "RM_UDMAP_FLOW_GET_CFG" }, \
		{ TISCI_MSG_RM_UDMAP_FLOW_SIZE_THRESH_GET_CFG, "RM_UDMAP_FLOW_SIZE_THRESH_GET_CFG" }, \
		{ TI_SCI_MSG_PROC_REQUEST, "PROC_REQUEST" }, \
		{ TI_SCI_MSG_PROC_RELEASE, "PROC_RELEASE" }, \
		{ TI_SCI_MSG_PROC_HANDOVER, "PROC_HANDOVER" }, \
		{ TI_SCI_MSG_SET_CONFIG, "SET_CONFIG" }, \
		{ TI_SCI_MSG_SET_CTRL, "SET_CTRL" }, \
		{ TI_SCI_MSG_GET_STATUS, "GET_STATUS" } \
	)


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

	TP_printk("type=%s host=%02X seq=%02X flags=%08X status=%d",
		show_ti_sci_msg_type(__entry->type), __entry->host,
		__entry->seq, __entry->flags, __entry->status)
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

	TP_printk("type=%s host=%02X seq=%02X flags=%08X data=%s",
		show_ti_sci_msg_type(__entry->type), __entry->host,
		__entry->seq, __entry->flags,
		__print_hex(__get_dynamic_array(cmd), __entry->len))
);
#endif /* _TRACE_TI_SCI_H */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE ti_sci_trace
#include <trace/define_trace.h>
