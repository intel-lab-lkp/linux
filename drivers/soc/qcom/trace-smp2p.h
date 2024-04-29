/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM qcom_smp2p

#if !defined(__QCOM_SMP2P_TRACE_H__) || defined(TRACE_HEADER_MULTI_READ)
#define __QCOM_SMP2P_TRACE_H__

#include <linux/tracepoint.h>

TRACE_EVENT(smp2p_ssr_ack,
	TP_PROTO(unsigned int remote_pid),
	TP_ARGS(remote_pid),
	TP_STRUCT__entry(
		__field(u32, remote_pid)
	),
	TP_fast_assign(
		__entry->remote_pid = remote_pid;
	),
	TP_printk("%d: SSR detected, doing SSR Handshake",
		__entry->remote_pid
	)
);

TRACE_EVENT(smp2p_negotiate,
	TP_PROTO(unsigned int remote_pid, bool ssr_ack_enabled),
	TP_ARGS(remote_pid, ssr_ack_enabled),
	TP_STRUCT__entry(
		__field(u32, remote_pid)
		__field(bool, ssr_ack_enabled)
	),
	TP_fast_assign(
		__entry->remote_pid = remote_pid;
		__entry->ssr_ack_enabled = ssr_ack_enabled;
	),
	TP_printk("%d: state=open ssr_ack=%d",
		__entry->remote_pid,
		__entry->ssr_ack_enabled
	)
);

TRACE_EVENT(smp2p_notify_in,
	TP_PROTO(unsigned int remote_pid, const char *name, unsigned long status, u32 val),
	TP_ARGS(remote_pid, name, status, val),
	TP_STRUCT__entry(
		__field(u32, remote_pid)
		__string(name, name)
		__field(unsigned long, status)
		__field(u32, val)
	),
	TP_fast_assign(
		__entry->remote_pid = remote_pid;
		__assign_str(name, name);
		__entry->status = status;
		__entry->val = val;
	),
	TP_printk("%d: %s: status:0x%0lx val:0x%0x",
		__entry->remote_pid,
		__get_str(name),
		__entry->status,
		__entry->val
	)
);

TRACE_EVENT(smp2p_update_bits,
	TP_PROTO(unsigned int remote_pid, const char *name, u32 orig, u32 val),
	TP_ARGS(remote_pid, name, orig, val),
	TP_STRUCT__entry(
		__field(u32, remote_pid)
		__string(name, name)
		__field(u32, orig)
		__field(u32, val)
	),
	TP_fast_assign(
		__entry->remote_pid = remote_pid;
		__assign_str(name, name);
		__entry->orig = orig;
		__entry->val = val;
	),
	TP_printk("%d: %s: orig:0x%0x new:0x%0x",
		__entry->remote_pid,
		__get_str(name),
		__entry->orig,
		__entry->val
	)
);

#endif /* __QCOM_SMP2P_TRACE_H__ */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace-smp2p

#include <trace/define_trace.h>
