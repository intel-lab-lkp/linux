/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * Copyright(c) 2015, 2016, 2017 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#if !defined(__HFI2_TRACE_RC_H) || defined(TRACE_HEADER_MULTI_READ)
#define __HFI2_TRACE_RC_H

#include <linux/tracepoint.h>
#include <linux/trace_seq.h>

#include "hfi2.h"

#undef TRACE_SYSTEM
#define TRACE_SYSTEM hfi2_rc

DECLARE_EVENT_CLASS(hfi2_rc_template,
		    TP_PROTO(struct rvt_qp *qp, u32 psn),
		    TP_ARGS(qp, psn),
		    TP_STRUCT__entry(
			DD_DEV_ENTRY(dd_from_ibdev(qp->ibqp.device))
			__field(u32, qpn)
			__field(u32, s_flags)
			__field(u32, psn)
			__field(u32, s_psn)
			__field(u32, s_next_psn)
			__field(u32, s_sending_psn)
			__field(u32, s_sending_hpsn)
			__field(u32, r_psn)
			),
		    TP_fast_assign(
			DD_DEV_ASSIGN(dd_from_ibdev(qp->ibqp.device));
			__entry->qpn = qp->ibqp.qp_num;
			__entry->s_flags = qp->s_flags;
			__entry->psn = psn;
			__entry->s_psn = qp->s_psn;
			__entry->s_next_psn = qp->s_next_psn;
			__entry->s_sending_psn = qp->s_sending_psn;
			__entry->s_sending_hpsn = qp->s_sending_hpsn;
			__entry->r_psn = qp->r_psn;
			),
		    TP_printk(
			"[%s] qpn 0x%x s_flags 0x%x psn 0x%x s_psn 0x%x s_next_psn 0x%x s_sending_psn 0x%x sending_hpsn 0x%x r_psn 0x%x",
			__get_str(dev),
			__entry->qpn,
			__entry->s_flags,
			__entry->psn,
			__entry->s_psn,
			__entry->s_next_psn,
			__entry->s_sending_psn,
			__entry->s_sending_hpsn,
			__entry->r_psn
			)
);

DEFINE_EVENT(hfi2_rc_template, hfi2_sendcomplete,
	     TP_PROTO(struct rvt_qp *qp, u32 psn),
	     TP_ARGS(qp, psn)
);

DEFINE_EVENT(hfi2_rc_template, hfi2_ack,
	     TP_PROTO(struct rvt_qp *qp, u32 psn),
	     TP_ARGS(qp, psn)
);

DEFINE_EVENT(hfi2_rc_template, hfi2_rcv_error,
	     TP_PROTO(struct rvt_qp *qp, u32 psn),
	     TP_ARGS(qp, psn)
);

DEFINE_EVENT(/* event */
	hfi2_rc_template, hfi2_rc_completion,
	TP_PROTO(struct rvt_qp *qp, u32 psn),
	TP_ARGS(qp, psn)
);

DECLARE_EVENT_CLASS(/* rc_ack */
	hfi2_rc_ack_template,
	TP_PROTO(struct rvt_qp *qp, u32 aeth, u32 psn,
		 struct rvt_swqe *wqe),
	TP_ARGS(qp, aeth, psn, wqe),
	TP_STRUCT__entry(/* entry */
		DD_DEV_ENTRY(dd_from_ibdev(qp->ibqp.device))
		__field(u32, qpn)
		__field(u32, aeth)
		__field(u32, psn)
		__field(u8, opcode)
		__field(u32, spsn)
		__field(u32, lpsn)
	),
	TP_fast_assign(/* assign */
		DD_DEV_ASSIGN(dd_from_ibdev(qp->ibqp.device));
		__entry->qpn = qp->ibqp.qp_num;
		__entry->aeth = aeth;
		__entry->psn = psn;
		__entry->opcode = wqe->wr.opcode;
		__entry->spsn = wqe->psn;
		__entry->lpsn = wqe->lpsn;
	),
	TP_printk(/* print */
		"[%s] qpn 0x%x aeth 0x%x psn 0x%x opcode 0x%x spsn 0x%x lpsn 0x%x",
		__get_str(dev),
		__entry->qpn,
		__entry->aeth,
		__entry->psn,
		__entry->opcode,
		__entry->spsn,
		__entry->lpsn
	)
);

DEFINE_EVENT(/* hfi2_do_rc_ack */
	hfi2_rc_ack_template, hfi2_rc_ack_do,
	TP_PROTO(struct rvt_qp *qp, u32 aeth, u32 psn,
		 struct rvt_swqe *wqe),
	TP_ARGS(qp, aeth, psn, wqe)
);

#endif /* __HFI2_TRACE_RC_H */

#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE trace_rc
#include <trace/define_trace.h>
