/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM typec

#if !defined(_TRACE_TYPEC_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_TYPEC_H

#include <linux/usb/typec.h>
#include <linux/tracepoint.h>

TRACE_EVENT(typec_mode,

	TP_PROTO(struct typec_port *port, int mode, int err),

	TP_ARGS(port, mode, err),

	TP_STRUCT__entry(
		__string(device, dev_name(&port->dev))
		__field(int, mode)
		__field(int, err)
	),

	TP_fast_assign(
		__assign_str(device);
		__entry->mode = mode;
		__entry->err = err;
	),

	TP_printk("%s mode=%d (%d)",
		  __get_str(device), __entry->mode, __entry->err)
);

#endif /* if !defined(_TRACE_TYPEC_H) || defined(TRACE_HEADER_MULTI_READ) */

/* This part must be outside protection */
#include <trace/define_trace.h>
