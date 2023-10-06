// SPDX-License-Identifier: GPL-2.0

#include <linux/instruction_pointer.h>
#include <linux/trace_portio.h>

#define CREATE_TRACE_POINTS
#include <trace/events/portio.h>

void do_trace_portio_read(u32 value, u16 port, char width)
{
	trace_portio_read(value, port, width, (void *)_RET_IP_);
}
EXPORT_SYMBOL_GPL(do_trace_portio_read);
EXPORT_TRACEPOINT_SYMBOL_GPL(portio_read);

void do_trace_portio_write(u32 value, u16 port, char width)
{
	trace_portio_write(value, port, width, (void *)_RET_IP_);
}
EXPORT_SYMBOL_GPL(do_trace_portio_write);
EXPORT_TRACEPOINT_SYMBOL_GPL(portio_write);
