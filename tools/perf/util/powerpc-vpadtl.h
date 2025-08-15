/* SPDX-License-Identifier: GPL-2.0 */
/*
 * VPA DTL PMU Support
 */

#ifndef INCLUDE__PERF_POWERPC_VPADTL_H__
#define INCLUDE__PERF_POWERPC_VPADTL_H__

#define POWERPC_VPADTL_NAME "powerpc_vpadtl_"

enum {
	POWERPC_VPADTL_TYPE,
	VPADTL_PER_CPU_MMAPS,
	VPADTL_AUXTRACE_PRIV_MAX,
};

#define VPADTL_AUXTRACE_PRIV_SIZE (VPADTL_AUXTRACE_PRIV_MAX * sizeof(u64))

union perf_event;
struct perf_session;
struct perf_pmu;

/*
 * The DTL entries are of below format
 */
struct dtl_entry {
	u8      dispatch_reason;
	u8      preempt_reason;
	u16     processor_id;
	u32     enqueue_to_dispatch_time;
	u32     ready_to_enqueue_time;
	u32     waiting_to_ready_time;
	u64     timebase;
	u64     fault_addr;
	u64     srr0;
	u64     srr1;
};

extern const char *dispatch_reasons[11];
extern const char *preempt_reasons[10];

int powerpc_vpadtl_process_auxtrace_info(union perf_event *event,
				  struct perf_session *session);

#endif
