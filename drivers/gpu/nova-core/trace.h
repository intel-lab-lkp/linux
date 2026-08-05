/* SPDX-License-Identifier: GPL-2.0 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM nova_core

#if !defined(_NOVA_CORE_TRACE_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _NOVA_CORE_TRACE_H_

#include <linux/tracepoint.h>

DECLARE_EVENT_CLASS(nova_core_trace_class,
	TP_PROTO(const char *dev, const char *message, size_t message_len),
	TP_ARGS(dev, message, message_len),
	TP_STRUCT__entry(
		__string(dev, dev)
		__string_len(message, message, message_len)
	),
	TP_fast_assign(
		__assign_str(dev);
		__assign_str(message);
	),
	TP_printk("%s %s", __get_str(dev), __get_str(message))
);

DEFINE_EVENT(nova_core_trace_class, nova_core_trace_driver,
	TP_PROTO(const char *dev, const char *message, size_t message_len),
	TP_ARGS(dev, message, message_len)
);

DEFINE_EVENT(nova_core_trace_class, nova_core_trace_fsp,
	TP_PROTO(const char *dev, const char *message, size_t message_len),
	TP_ARGS(dev, message, message_len)
);

DEFINE_EVENT(nova_core_trace_class, nova_core_trace_gsp,
	TP_PROTO(const char *dev, const char *message, size_t message_len),
	TP_ARGS(dev, message, message_len)
);

DEFINE_EVENT(nova_core_trace_class, nova_core_trace_vgpu,
	TP_PROTO(const char *dev, const char *message, size_t message_len),
	TP_ARGS(dev, message, message_len)
);

#endif /* _NOVA_CORE_TRACE_H_ */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ../../drivers/gpu/nova-core
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace

#include <trace/define_trace.h>
