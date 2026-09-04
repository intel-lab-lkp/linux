/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#if !defined(_TRACE_SMD_RPM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SMD_RPM_H

#undef TRACE_SYSTEM
#define TRACE_SYSTEM rpm_smd

#include <linux/tracepoint.h>
#include <linux/soc/qcom/smd-rpm.h>

TRACE_EVENT(rpm_smd_ack_recvd,

	TP_PROTO(__le32 msg_id, int errno),

	TP_ARGS(msg_id, errno),

	TP_STRUCT__entry(
		__field(u32, msg_id)
		__field(int, errno)
	),

	TP_fast_assign(
		__entry->msg_id = le32_to_cpu(msg_id);
		__entry->errno = errno;
	),

	TP_printk("msg_id:%u errno:%08x",
		__entry->msg_id,
		__entry->errno)
);

TRACE_EVENT(rpm_smd_send_msg,

	TP_PROTO(__le32 msg_id, u32 state, u32 rsc_type, u32 rsc_id,
		 const struct clk_smd_rpm_req *req),

	TP_ARGS(msg_id, state, rsc_type, rsc_id, req),

	TP_STRUCT__entry(
		__field(u32, msg_id)
		__field(u32, state)
		__field(u32, rsc_type)
		__field(u32, rsc_id)
		__field(u32, key)
		__field(u32, nbytes)
		__field(u32, value)
	),

	TP_fast_assign(
		__entry->msg_id = le32_to_cpu(msg_id);
		__entry->state = state;
		__entry->rsc_type = rsc_type;
		__entry->rsc_id = rsc_id;
		__entry->key = le32_to_cpu(req->key);
		__entry->nbytes = le32_to_cpu(req->nbytes);
		__entry->value = le32_to_cpu(req->value);
	),

	TP_printk("msg_id:%u ctx:%s rsc_type:%.4s rsc_id:0x%x key:%.4s nbytes:%u value:%u",
		  __entry->msg_id,
		  __print_symbolic(__entry->state,
				   { QCOM_SMD_RPM_ACTIVE_STATE, "active" },
				   { QCOM_SMD_RPM_SLEEP_STATE, "sleep" }),
		  (const char *)&__entry->rsc_type,
		  __entry->rsc_id,
		  (const char *)&__entry->key,
		  __entry->nbytes,
		  __entry->value)
);

#endif /* _TRACE_SMD_RPM_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace-smd-rpm

#include <trace/define_trace.h>
