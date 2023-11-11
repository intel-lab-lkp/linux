/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM mhi_host

#if !defined(_TRACE_EVENT_MHI_HOST_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_EVENT_MHI_HOST_H

#include <linux/tracepoint.h>
#include <linux/trace_seq.h>
#include "../common.h"
#include "internal.h"

TRACE_EVENT(mhi_gen_tre,

	TP_PROTO(const char *name, int ch_num, u64 wp, __le64 tre_ptr,
		 __le32 dword0, __le32 dword1),

	TP_ARGS(name, ch_num, wp, tre_ptr, dword0, dword1),

	TP_STRUCT__entry(
		__string(name, name)
		__field(int, ch_num)
		__field(u64, wp)
		__field(__le64, tre_ptr)
		__field(__le32, dword0)
		__field(__le32, dword1)
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

TRACE_EVENT(mhi_intvec_states,

	TP_PROTO(const char *name, int local_ee, int state, int dev_ee, int dev_state),

	TP_ARGS(name, local_ee, state, dev_ee, dev_state),

	TP_STRUCT__entry(
		__string(name, name)
		__field(int, local_ee)
		__field(int, state)
		__field(int, dev_ee)
		__field(int, dev_state)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->local_ee = local_ee;
		__entry->state = state;
		__entry->dev_ee = dev_ee;
		__entry->dev_state = dev_state;
	),

	TP_printk("%s: local ee: %s state: %s device ee: %s state: %s\n",
		  __get_str(name),
		  TO_MHI_EXEC_STR(__entry->local_ee),
		  mhi_state_str(__entry->state),
		  TO_MHI_EXEC_STR(__entry->dev_ee),
		  mhi_state_str(__entry->dev_state))
);

TRACE_EVENT(mhi_tryset_pm_state,

	TP_PROTO(const char *name, int pm_state),

	TP_ARGS(name, pm_state),

	TP_STRUCT__entry(
		__string(name, name)
		__field(int, pm_state)
	),

	TP_fast_assign(
		__assign_str(name, name);
		if (pm_state)
			pm_state = __fls(pm_state);
		__entry->pm_state = pm_state;
	),

	TP_printk("%s: PM state: %s\n", __get_str(name),
		  to_mhi_pm_state_str(__entry->pm_state))
);

TRACE_EVENT(mhi_process_data_event_ring,

	TP_PROTO(const char *name, __le64 ptr, __le32 dword0, __le32 dword1),

	TP_ARGS(name, ptr, dword0, dword1),

	TP_STRUCT__entry(
		__field(__le64, ptr)
		__string(name, name)
		__field(__le32, dword0)
		__field(__le32, dword1)
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

	TP_PROTO(const char *name, u64 rp, __le64 ptr, __le32 dword0, __le32 dword1),

	TP_ARGS(name, rp, ptr, dword0, dword1),

	TP_STRUCT__entry(
		__field(u64, rp)
		__field(__le64, ptr)
		__string(name, name)
		__field(__le32, dword0)
		__field(__le32, dword1)
		__field(int, state)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->rp = rp;
		__entry->ptr = ptr;
		__entry->dword0 = dword0;
		__entry->dword1 = dword1;
		__entry->state = MHI_TRE_GET_EV_STATE(rp);
	),

	TP_printk("%s: RP:0x%llx Processing Event:0x%llx 0x%08x 0x%08x state:%s\n",
		  __get_str(name), __entry->rp, __entry->ptr, __entry->dword0,
		  __entry->dword1, mhi_state_str(__entry->state))
);

TRACE_EVENT(mhi_update_channel_state_start,

	TP_PROTO(const char *name, int ch_num, int state),

	TP_ARGS(name, ch_num, state),

	TP_STRUCT__entry(
		__string(name, name)
		__field(int, ch_num)
		__field(int, state)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->ch_num = ch_num;
		__entry->state = state;
	),

	TP_printk("%s: ch%d: Updating state to: %s\n",
		  __get_str(name), __entry->ch_num,
		  TO_CH_STATE_TYPE_STR(__entry->state))
);

TRACE_EVENT(mhi_update_channel_state_end,

	TP_PROTO(const char *name, int ch_num, int state),

	TP_ARGS(name, ch_num, state),

	TP_STRUCT__entry(
		__string(name, name)
		__field(int, ch_num)
		__field(int, state)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->ch_num = ch_num;
		__entry->state = state;
	),

	TP_printk("%s: ch%d: Updated state to: %s\n",
		  __get_str(name), __entry->ch_num,
		  TO_CH_STATE_TYPE_STR(__entry->state))
);

TRACE_EVENT(mhi_pm_st_transition,

	TP_PROTO(const char *name, int state),

	TP_ARGS(name, state),

	TP_STRUCT__entry(
		__string(name, name)
		__field(int, state)
	),

	TP_fast_assign(
		__assign_str(name, name);
		__entry->state = state;
	),

	TP_printk("%s: Handling state transition: %s\n", __get_str(name),
		  TO_DEV_STATE_TRANS_STR(__entry->state))
);

#endif
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace

#include <trace/define_trace.h>
