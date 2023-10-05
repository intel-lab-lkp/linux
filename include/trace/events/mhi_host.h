/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM mhi_host

#if !defined(_TRACE_EVENT_MHI_HOST_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_EVENT_MHI_HOST_H

#include <linux/tracepoint.h>

TRACE_EVENT(mhi_gen_tre,

	TP_PROTO(const char *name, int ch_num, u64 wp, u64 tre_ptr, int dword0, int dword1),

	TP_ARGS(name, ch_num, wp, tre_ptr, dword0, dword1),

	TP_STRUCT__entry(
		__string(name, name)
		__field(int, ch_num)
		__field(u64, wp)
		__field(u64, tre_ptr)
		__field(int, dword0)
		__field(int, dword1)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->ch_num = ch_num;
		__entry->wp = wp;
		__entry->tre_ptr = tre_ptr;
		__entry->dword0 = dword0;
		__entry->dword1 = dword1;
	),

	TP_printk("%s: Chan: %d WP: 0x%llx TRE: 0x%llx 0x%08x 0x%08x\n",
		  __get_str(name), __entry->ch_num, __entry->wp, __entry->tre_ptr,
		  __entry->dword0, __entry->dword1)
);

TRACE_EVENT(mhi_intvec_threaded_handler,

	TP_PROTO(const char *name, const char *local_ee, const char *state, const char *dev_ee,
		 const char *dev_state),

	TP_ARGS(name, local_ee, state, dev_ee, dev_state),

	TP_STRUCT__entry(
		__string(name, name)
		__string(local_ee, local_ee)
		__string(state, state)
		__string(dev_ee, dev_ee)
		__string(dev_state, dev_state)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__assign_str(local_ee, local_ee);
		__assign_str(state, state);
		__assign_str(dev_ee, dev_ee);
		__assign_str(dev_state, dev_state);
	),

	TP_printk("%s: local ee: %s state: %s device ee: %s state: %s\n",
		  __get_str(name), __get_str(local_ee), __get_str(state), __get_str(dev_ee),
		  __get_str(dev_state))
);

TRACE_EVENT(mhi_tryset_pm_state,

	TP_PROTO(const char *name, const char *pm_state),

	TP_ARGS(name, pm_state),

	TP_STRUCT__entry(
		__string(name, name)
		__string(pm_state, pm_state)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__assign_str(pm_state, pm_state);
	),

	TP_printk("%s: PM state: %s\n", __get_str(name), __get_str(pm_state))
);

TRACE_EVENT(mhi_process_data_event_ring,

	TP_PROTO(const char *name, u64 ptr, int dword0, int dword1),

	TP_ARGS(name, ptr, dword0, dword1),

	TP_STRUCT__entry(
		__string(name, name)
		__field(u64, ptr)
		__field(int, dword0)
		__field(int, dword1)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->ptr = ptr;
		__entry->dword0 = dword0;
		__entry->dword1 = dword1;
	),

	TP_printk("%s: Processing Event:0x%llx 0x%08x 0x%08x\n",
		  __get_str(name), __entry->ptr, __entry->dword0, __entry->dword1)
);

TRACE_EVENT(mhi_process_ctrl_ev_ring,

	TP_PROTO(const char *name, u64 rp, u64 ptr, int dword0, int dword1, const char *state),

	TP_ARGS(name, rp, ptr, dword0, dword1, state),

	TP_STRUCT__entry(
		__string(name, name)
		__field(u64, rp)
		__field(u64, ptr)
		__field(int, dword0)
		__field(int, dword1)
		__string(state, state)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->rp = rp;
		__entry->ptr = ptr;
		__entry->dword0 = dword0;
		__entry->dword1 = dword1;
		__assign_str(state, state);
	),

	TP_printk("%s: RP:0x%llx Processing Event:0x%llx 0x%08x 0x%08x state:%s\n",
		  __get_str(name), __entry->rp, __entry->ptr, __entry->dword0,
		  __entry->dword1, __get_str(state))
);

TRACE_EVENT(mhi_update_channel_state_start,

	TP_PROTO(const char *name, int ch_num, const char *state),

	TP_ARGS(name, ch_num, state),

	TP_STRUCT__entry(
		__string(name, name)
		__field(int, ch_num)
		__string(state, state)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->ch_num = ch_num;
		__assign_str(state, state);
	),

	TP_printk("%s: ch%d: Updating state to: %s\n",
		  __get_str(name), __entry->ch_num,  __get_str(state))
);

TRACE_EVENT(mhi_update_channel_state_end,

	TP_PROTO(const char *name, int ch_num, const char *state),

	TP_ARGS(name, ch_num, state),

	TP_STRUCT__entry(
		__string(name, name)
		__field(int, ch_num)
		__string(state, state)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->ch_num = ch_num;
		__assign_str(state, state);
	),

	TP_printk("%s: ch%d: Updated state to: %s\n",
		  __get_str(name), __entry->ch_num,  __get_str(state))
);

TRACE_EVENT(mhi_pm_st_worker,

	TP_PROTO(const char *name, const char *state),

	TP_ARGS(name, state),

	TP_STRUCT__entry(
		__string(name, name)
		__string(state, state)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__assign_str(state, state);
	),

	TP_printk("%s: Handling state transition: %s\n", __get_str(name), __get_str(state))
);

#endif
#include <trace/define_trace.h>
