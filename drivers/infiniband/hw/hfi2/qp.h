/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * Copyright(c) 2015 - 2018 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#ifndef _QP_H
#define _QP_H
#include <linux/hash.h>
#include <rdma/rdmavt_qp.h>
#include "verbs.h"
#include "sdma.h"
#include "verbs_txreq.h"

extern unsigned int hfi2_qp_table_size;

extern const struct rvt_operation_params hfi2_post_parms[];

/*
 * Driver specific s_flags starting at bit 31 down to HFI2_S_MIN_BIT_MASK
 *
 * HFI2_S_AHG_VALID - ahg header valid on chip
 * HFI2_S_AHG_CLEAR - have send engine clear ahg state
 * HFI2_S_WAIT_PIO_DRAIN - qp waiting for PIOs to drain
 * HFI2_S_WAIT_TID_SPACE - a QP is waiting for TID resource
 * HFI2_S_WAIT_TID_RESP - waiting for a TID RDMA WRITE response
 * HFI2_S_WAIT_HALT - halt the first leg send engine
 * HFI2_S_MIN_BIT_MASK - the lowest bit that can be used by hfi2
 */
#define HFI2_S_AHG_VALID         0x80000000
#define HFI2_S_AHG_CLEAR         0x40000000
#define HFI2_S_WAIT_PIO_DRAIN    0x20000000
#define HFI2_S_WAIT_TID_SPACE    0x10000000
#define HFI2_S_WAIT_TID_RESP     0x08000000
#define HFI2_S_WAIT_HALT         0x04000000
#define HFI2_S_MIN_BIT_MASK      0x01000000

/*
 * overload wait defines
 */

#define HFI2_S_ANY_WAIT_IO (RVT_S_ANY_WAIT_IO | HFI2_S_WAIT_PIO_DRAIN)
#define HFI2_S_ANY_WAIT (HFI2_S_ANY_WAIT_IO | RVT_S_ANY_WAIT_SEND)
#define HFI2_S_ANY_TID_WAIT_SEND (RVT_S_WAIT_SSN_CREDIT | RVT_S_WAIT_DMA)

/*
 * Send if not busy or waiting for I/O and either
 * a RC response is pending or we can process send work requests.
 */
static inline int hfi2_send_ok(struct rvt_qp *qp)
{
	struct hfi2_qp_priv *priv = qp->priv;

	return !(qp->s_flags & (RVT_S_BUSY | HFI2_S_ANY_WAIT_IO)) &&
		(verbs_txreq_queued(iowait_get_ib_work(&priv->s_iowait)) ||
		(qp->s_flags & RVT_S_RESP_PENDING) ||
		 !(qp->s_flags & RVT_S_ANY_WAIT_SEND));
}

/*
 * free_ahg - clear ahg from QP
 */
static inline void clear_ahg(struct rvt_qp *qp)
{
	struct hfi2_qp_priv *priv = qp->priv;

	priv->s_ahg->ahgcount = 0;
	qp->s_flags &= ~(HFI2_S_AHG_VALID | HFI2_S_AHG_CLEAR);
	if (priv->s_sde && qp->s_ahgidx >= 0)
		hfi2_sdma_ahg_free(priv->s_sde, qp->s_ahgidx);
	qp->s_ahgidx = -1;
}

/**
 * hfi2_qp_wakeup - wake up on the indicated event
 * @qp: the QP
 * @flag: flag the qp on which the qp is stalled
 */
void hfi2_qp_wakeup(struct rvt_qp *qp, u32 flag);

struct sdma_engine *hfi2_qp_to_sdma_engine(struct rvt_qp *qp, u8 sc5);
struct send_context *hfi2_qp_to_send_context(struct rvt_qp *qp, u8 sc5);

void hfi2_qp_iter_print(struct seq_file *s, struct rvt_qp_iter *iter);

bool _hfi2_schedule_send(struct rvt_qp *qp);
bool hfi2_schedule_send(struct rvt_qp *qp);

void hfi2_migrate_qp(struct rvt_qp *qp);
int hfi2_valid_qp(struct hfi2_pportdata *ppd, u32 qpn);

/*
 * Functions provided by hfi2 driver for rdmavt to use
 */
void *hfi2_qp_priv_alloc(struct rvt_dev_info *rdi, struct rvt_qp *qp);
void hfi2_qp_priv_free(struct rvt_dev_info *rdi, struct rvt_qp *qp);
unsigned int hfi2_free_all_qps(struct rvt_dev_info *rdi);
void hfi2_notify_qp_reset(struct rvt_qp *qp);
int hfi2_get_pmtu_from_attr(struct rvt_dev_info *rdi, struct rvt_qp *qp,
		       struct ib_qp_attr *attr);
void hfi2_flush_qp_waiters(struct rvt_qp *qp);
void hfi2_notify_error_qp(struct rvt_qp *qp);
void hfi2_stop_send_queue(struct rvt_qp *qp);
void hfi2_quiesce_qp(struct rvt_qp *qp);
u32 hfi2_mtu_from_qp(struct rvt_dev_info *rdi, struct rvt_qp *qp, u32 pmtu);
int hfi2_mtu_to_path_mtu(u32 mtu);
void hfi2_error_port_qps(struct hfi2_ibport *ibp, u8 sl);
void hfi2_qp_unbusy(struct rvt_qp *qp, struct iowait_work *wait);
int hfi2_sriov_alloc_qpn(struct rvt_dev_info *rdi, struct rvt_qpn_table *qpt,
		    enum ib_qp_type type, u32 port_num);

#endif /* _QP_H */
