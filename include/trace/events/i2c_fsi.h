/* SPDX-License-Identifier: GPL-2.0 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM i2c_fsi

#if !defined(_TRACE_I2C_FSI_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_I2C_FSI_H

#include <linux/tracepoint.h>

TRACE_EVENT(i2c_fsi_irq,
	TP_PROTO(const struct fsi_i2c_port *port, uint32_t status),
	TP_ARGS(port, status),
	TP_STRUCT__entry(
		__field(int, bus)
		__field(int, msg_idx)
		__field(uint32_t, status)
	),
	TP_fast_assign(
		__entry->bus = port->adapter.nr;
		__entry->msg_idx = port->i;
		__entry->status = status;
	),
	TP_printk("i2c-%d status: %08x", __entry->bus, __entry->status)
);

TRACE_EVENT(i2c_fsi_start,
	TP_PROTO(const struct fsi_i2c_port *port, uint32_t command),
	TP_ARGS(port, command),
	TP_STRUCT__entry(
		__field(int, bus)
		__field(int, msg_idx)
		__field(uint32_t, command)
	),
	TP_fast_assign(
		__entry->bus = port->adapter.nr;
		__entry->msg_idx = port->i;
		__entry->command = command;
	),
	TP_printk("i2c-%d command: %08x", __entry->bus, __entry->command)
);

#endif

#include <trace/define_trace.h>
