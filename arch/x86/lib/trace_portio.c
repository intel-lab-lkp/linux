// SPDX-License-Identifier: GPL-2.0+

#define CREATE_TRACE_POINTS
#include <trace/events/portio.h>

void do_trace_portio_read(u32 value, u16 port, char width, long ip_addr)
{
	trace_portio_read(value, port, width, ip_addr);
}
EXPORT_SYMBOL_GPL(do_trace_portio_read);
EXPORT_TRACEPOINT_SYMBOL_GPL(portio_read);

void do_trace_portio_write(u32 value, u16 port, char width, long ip_addr)
{
	trace_portio_write(value, port, width, ip_addr);
}
EXPORT_SYMBOL_GPL(do_trace_portio_write);
EXPORT_TRACEPOINT_SYMBOL_GPL(portio_write);
