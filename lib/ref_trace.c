// SPDX-License-Identifier: GPL-2.0
#define CREATE_TRACE_POINTS
#include <trace/events/ref_trace.h>
#include <linux/ref_trace.h>

//Wrapper function for functions defined entirely in header files
void do_ref_trace_final_put(unsigned long caller, unsigned long ip, const void *obj)
{
	trace_call__ref_trace_final_put(caller, ip, obj);
}
EXPORT_SYMBOL_GPL(do_ref_trace_final_put);

EXPORT_TRACEPOINT_SYMBOL_GPL(ref_trace_final_put);
