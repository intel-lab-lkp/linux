/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM dyndbg

#if !defined(_TRACE_DYNDBG_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_DYNDBG_H

#include <linux/tracepoint.h>

/*
 * template for dynamic debug events
 * captures debug log message and uses its length, it also
 * accepts _ddebug and dev structures for future extensions
 */
DECLARE_EVENT_CLASS(dyndbg_template,

	TP_PROTO(const struct _ddebug *desc, const struct device *dev,
		 const char *msg),

	TP_ARGS(desc, dev, msg),

	TP_STRUCT__entry(
		__string(s, msg)
	    ),

	TP_fast_assign(
		__assign_str(s, msg);
	    ),

	TP_printk("%s", __get_str_strip_nl(s))
);

/* captures pr_debug() callsites */
DEFINE_EVENT(dyndbg_template, prdbg,

	TP_PROTO(const struct _ddebug *desc, const struct device *dev,
		 const char *msg),

	TP_ARGS(desc, dev, msg)
);

/* captures dev_dbg() callsites */
DEFINE_EVENT(dyndbg_template, devdbg,

	TP_PROTO(const struct _ddebug *desc, const struct device *dev,
		 const char *msg),

	TP_ARGS(desc, dev, msg)
);

#endif /* _TRACE_DYNDBG_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
