// SPDX-License-Identifier: GPL-2.0
#define CREATE_TRACE_POINTS
#include <trace/events/ref_trace.h>

//Wrapper function for functions defined entirely in header files
void do_ref_trace_final_put(unsigned long caller, const char *fn, const void *obj)
{
	trace_call__ref_trace_final_put(caller, fn, obj);
}
EXPORT_SYMBOL_GPL(do_ref_trace_final_put);

EXPORT_TRACEPOINT_SYMBOL_GPL(ref_trace_final_put);
