// SPDX-License-Identifier: GPL-2.0
#define CREATE_TRACE_POINTS
#include <trace/events/refcount.h>
#include <linux/refcount_trace.h>

//Wrapper function for functions defined entirely in header files
void do_refcount_trace_final_put(unsigned long caller,
				 unsigned long ip,
				 const void *obj)
{
	trace_call__refcount_final_put(caller, ip, obj);
}
EXPORT_SYMBOL_GPL(do_refcount_trace_final_put);
EXPORT_TRACEPOINT_SYMBOL_GPL(refcount_final_put);
