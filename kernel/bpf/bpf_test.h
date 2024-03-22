/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM bpf_test

#if !defined(_TRACE_BPF_TEST_H) || defined(TRACE_HEADER_MULTI_READ)

#define _TRACE_BPF_TEST_H

#include <linux/tracepoint.h>

TRACE_EVENT(bpf_test,

	TP_PROTO(int nonce),

	TP_ARGS(nonce),

	TP_STRUCT__entry(
		__field(int, nonce)
	),

	TP_fast_assign(
		__entry->nonce = nonce;
	),

	TP_printk("nonce %d", __entry->nonce)
);

#endif /* _TRACE_BPF_TEST_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE bpf_test

#include <trace/define_trace.h>
