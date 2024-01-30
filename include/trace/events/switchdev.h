/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM	switchdev

#if !defined(_TRACE_SWITCHDEV_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SWITCHDEV_H

#include <linux/tracepoint.h>
#include <net/switchdev.h>

#define SWITCHDEV_TRACE_MSG_MAX 128

TRACE_EVENT(switchdev_defer,
	TP_PROTO(unsigned long val,
		 const struct switchdev_notifier_info *info),

	TP_ARGS(val, info),

	TP_STRUCT__entry(
		__field(unsigned long, val)
		__string(dev, info->dev ? netdev_name(info->dev) : "(null)")
		__field(const struct switchdev_notifier_info *, info)
		__array(char, msg, SWITCHDEV_TRACE_MSG_MAX)
	),

	TP_fast_assign(
		__entry->val = val;
		__assign_str(dev, info->dev ? netdev_name(info->dev) : "(null)");
		__entry->info = info;
		switchdev_notifier_str(val, info, __entry->msg, SWITCHDEV_TRACE_MSG_MAX);
	),

	TP_printk("dev %s %s", __get_str(dev), __entry->msg)
);

DECLARE_EVENT_CLASS(switchdev_call,
	TP_PROTO(unsigned long val,
		 const struct switchdev_notifier_info *info,
		 int err),

	TP_ARGS(val, info, err),

	TP_STRUCT__entry(
		__field(unsigned long, val)
		__string(dev, info->dev ? netdev_name(info->dev) : "(null)")
		__field(const struct switchdev_notifier_info *, info)
		__field(int, err)
		__array(char, msg, SWITCHDEV_TRACE_MSG_MAX)
	),

	TP_fast_assign(
		__entry->val = val;
		__assign_str(dev, info->dev ? netdev_name(info->dev) : "(null)");
		__entry->info = info;
		__entry->err = err;
		switchdev_notifier_str(val, info, __entry->msg, SWITCHDEV_TRACE_MSG_MAX);
	),

	TP_printk("dev %s %s -> %d", __get_str(dev), __entry->msg, __entry->err)
);

DEFINE_EVENT(switchdev_call, switchdev_call_atomic,
	TP_PROTO(unsigned long val,
		 const struct switchdev_notifier_info *info,
		 int err),

	TP_ARGS(val, info, err)
);

DEFINE_EVENT(switchdev_call, switchdev_call_blocking,
	TP_PROTO(unsigned long val,
		 const struct switchdev_notifier_info *info,
		 int err),

	TP_ARGS(val, info, err)
);

DEFINE_EVENT(switchdev_call, switchdev_call_replay,
	TP_PROTO(unsigned long val,
		 const struct switchdev_notifier_info *info,
		 int err),

	TP_ARGS(val, info, err)
);

#endif /* _TRACE_SWITCHDEV_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
