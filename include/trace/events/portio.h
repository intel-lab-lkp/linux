/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM portio

#if !defined(_TRACE_PORTIO_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_PORTIO_H

#include <linux/tracepoint.h>

DECLARE_EVENT_CLASS(portio_class,
	TP_PROTO(u32 value, u16 port, char width, void *ip_addr),

	TP_ARGS(value, port, width, ip_addr),

	TP_STRUCT__entry(
		__field(u32, value)
		__field(u16, port)
		__field(char, width)
		__field(void *, ip_addr)
	),

	TP_fast_assign(
		__entry->value = value;
		__entry->port = port;
		__entry->width = width;
		__entry->ip_addr = ip_addr;
	),

	TP_printk("port=0x%04x value=0x%0*x %pS",
		__entry->port,
		__entry->width == 'b' ? 2 :
		__entry->width == 'w' ? 4 : 8,
		__entry->value, __entry->ip_addr)
);

DEFINE_EVENT(portio_class, portio_read,
	TP_PROTO(u32 value, u16 port, char width, void *ip_addr),
	TP_ARGS(value, port, width, ip_addr)
);

DEFINE_EVENT(portio_class, portio_write,
	TP_PROTO(u32 value, u16 port, char width, void *ip_addr),
	TP_ARGS(value, port, width, ip_addr)
);

#endif /* _TRACE_PORTIO_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
