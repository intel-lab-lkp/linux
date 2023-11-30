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
		 const char *msg, size_t len),

	TP_ARGS(desc, dev, msg, len),

	TP_STRUCT__entry(
		__dynamic_array(char, s, len+1)
	    ),

	TP_fast_assign(
		/*
		 * Each trace entry is printed in a new line.
		 * If the msg finishes with '\n', cut it off
		 * to avoid blank lines in the trace.
		 */
		if (len > 0 && (msg[len-1] == '\n'))
			len -= 1;

		memcpy(__get_str(s), msg, len);
		__get_str(s)[len] = 0;
	    ),

	TP_printk("%s", __get_str(s))
);

/* captures pr_debug() callsites */
DEFINE_EVENT(dyndbg_template, prdbg,

	TP_PROTO(const struct _ddebug *desc, const struct device *dev,
		 const char *msg, size_t len),

	TP_ARGS(desc, dev, msg, len)
);

/* captures dev_dbg() callsites */
DEFINE_EVENT(dyndbg_template, devdbg,

	TP_PROTO(const struct _ddebug *desc, const struct device *dev,
		 const char *msg, size_t len),

	TP_ARGS(desc, dev, msg, len)
);

#endif /* _TRACE_DYNDBG_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
