/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM goog_mba

#if !defined(_GOOG_MBA_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _GOOG_MBA_TRACE_H

#include <linux/tracepoint.h>

#include "goog-mba-priv.h"

TRACE_EVENT(
	goog_mba_process_nq_txdone,

	TP_PROTO(const struct goog_mbox_info *goog_mbox),

	TP_ARGS(goog_mbox),

	TP_STRUCT__entry(
		__string(dev_name, dev_name(goog_mbox->mba->dev))
		__string(name, goog_mbox->np->full_name)
	),

	TP_fast_assign(
		__assign_str(dev_name);
		__assign_str(name);
	),

	TP_printk("%s %s", __get_str(dev_name), __get_str(name))
);

TRACE_EVENT(
	goog_mba_process_q_txdone,

	TP_PROTO(const struct goog_mbox_info *goog_mbox, u32 reqs_completed, u32 outstanding_msgs),

	TP_ARGS(goog_mbox, reqs_completed, outstanding_msgs),

	TP_STRUCT__entry(
		__string(dev_name, dev_name(goog_mbox->mba->dev))
		__string(name, goog_mbox->np->full_name)
		__field(u32, outstanding_msgs)
		__field(u32, reqs_completed)
	),

	TP_fast_assign(
		__assign_str(dev_name);
		__assign_str(name);
		__entry->outstanding_msgs = outstanding_msgs;
		__entry->reqs_completed = reqs_completed;
	),

	TP_printk("%s %s: reqs_completed=%u outstanding_msgs=%u",
		  __get_str(dev_name), __get_str(name), __entry->reqs_completed,
		  __entry->outstanding_msgs)
);

TRACE_EVENT(
	goog_mba_send_data_nq,

	TP_PROTO(const struct goog_mbox_info *goog_mbox, const u32 *payload,
		 unsigned int payload_words),

	TP_ARGS(goog_mbox, payload, payload_words),

	TP_STRUCT__entry(
		__string(dev_name, dev_name(goog_mbox->mba->dev))
		__string(name, goog_mbox->np->full_name)
		__field(u32, payload_words)
		__dynamic_array(u32, payload, payload_words)
	),

	TP_fast_assign(
		__assign_str(dev_name);
		__assign_str(name);
		__entry->payload_words = payload_words;
		memcpy(__get_dynamic_array(payload), payload,
		       payload_words * sizeof(u32));
	),

	TP_printk("%s %s: data=%s",
		  __get_str(dev_name), __get_str(name),
		  __print_array(__get_dynamic_array(payload),
				__entry->payload_words, sizeof(u32)))
);

TRACE_EVENT(
	goog_mba_send_data_q,

	TP_PROTO(const struct goog_mbox_info *goog_mbox, const u32 *payload,
		 unsigned int payload_words),

	TP_ARGS(goog_mbox, payload, payload_words),

	TP_STRUCT__entry(
		__string(dev_name, dev_name(goog_mbox->mba->dev))
		__string(name, goog_mbox->np->full_name)
		__field(u32, tx_idx)
		__field(u32, payload_words)
		__dynamic_array(u32, payload, payload_words)
	),

	TP_fast_assign(
		__assign_str(dev_name);
		__assign_str(name);
		__entry->tx_idx = goog_mbox->tx_idx;
		__entry->payload_words = payload_words;
		memcpy(__get_dynamic_array(payload), payload,
		       payload_words * sizeof(u32));
	),

	TP_printk("%s %s: tx_idx=%u data=%s",
		  __get_str(dev_name), __get_str(name), __entry->tx_idx,
		  __print_array(__get_dynamic_array(payload),
				__entry->payload_words, sizeof(u32)))
);

TRACE_EVENT(
	goog_mba_process_nq_rx,

	TP_PROTO(const struct goog_mbox_info *goog_mbox),

	TP_ARGS(goog_mbox),

	TP_STRUCT__entry(
		__string(dev_name, dev_name(goog_mbox->mba->dev))
		__string(name, goog_mbox->np->full_name)
		__field(u32, rx_payload_words)
		__dynamic_array(u32, payload, goog_mbox->rx_payload_words)
	),

	TP_fast_assign(
		__assign_str(dev_name);
		__assign_str(name);
		__entry->rx_payload_words = goog_mbox->rx_payload_words;
		memcpy(__get_dynamic_array(payload), goog_mbox->rx_buffer,
		       goog_mbox->rx_payload_words * sizeof(u32));
	),

	TP_printk("%s %s: data=%s",
		  __get_str(dev_name), __get_str(name),
		  __print_array(__get_dynamic_array(payload),
				__entry->rx_payload_words, sizeof(u32)))
);

TRACE_EVENT(
	goog_mba_process_q_rx,

	TP_PROTO(const struct goog_mbox_info *goog_mbox),

	TP_ARGS(goog_mbox),

	TP_STRUCT__entry(
		__string(dev_name, dev_name(goog_mbox->mba->dev))
		__string(name, goog_mbox->np->full_name)
		__field(u32, rx_idx)
		__field(u32, rx_payload_words)
		__dynamic_array(u32, payload, goog_mbox->rx_payload_words)
	),

	TP_fast_assign(
		__assign_str(dev_name);
		__assign_str(name);
		__entry->rx_idx = goog_mbox->rx_idx;
		__entry->rx_payload_words = goog_mbox->rx_payload_words;
		memcpy(__get_dynamic_array(payload), goog_mbox->rx_buffer,
		       goog_mbox->rx_payload_words * sizeof(u32));
	),

	TP_printk("%s %s: rx_idx=%u data=%s",
		  __get_str(dev_name), __get_str(name), __entry->rx_idx,
		  __print_array(__get_dynamic_array(payload),
				__entry->rx_payload_words, sizeof(u32)))
);

#endif /* _GOOG_MBA_TRACE_H */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ../drivers/mailbox
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE goog-mba-trace
#include <trace/define_trace.h>
