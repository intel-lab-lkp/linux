// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright(c) 2015 - 2020 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#include <rdma/ib_mad.h>
#include <rdma/ib_user_verbs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/utsname.h>
#include <linux/rculist.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <rdma/opa_addr.h>
#include <linux/nospec.h>

#include "hfi2.h"
#include "common.h"
#include "trace.h"
#include "qp.h"
#include "verbs_txreq.h"
#include "debugfs.h"
#include "fault.h"
#include "affinity.h"
#include "ipoib.h"
#include "uverbs.h"
#include "sriov.h"
#include "chip_gen.h"

extern int hfi2_sriov_alloc_qpn(struct rvt_dev_info *rdi,
				struct rvt_qpn_table *qpt, enum ib_qp_type type,
				u32 port_num);

static unsigned int hfi2_lkey_table_size = 16;

static unsigned int hfi2_max_pds = 0xFFFF;

static unsigned int hfi2_max_ahs = 0xFFFF;

static unsigned int hfi2_max_cqes = 0x2FFFFF;

static unsigned int hfi2_max_cqs = 0x1FFFF;

static unsigned int hfi2_max_qp_wrs = 0x3FFF;

static unsigned int hfi2_max_qps = 32768;

static unsigned int hfi2_max_sges = 0x60;

static unsigned int hfi2_max_mcast_grps = 16384;

static unsigned int hfi2_max_mcast_qp_attached = 16;

static unsigned int hfi2_max_srqs = 1024;

static unsigned int hfi2_max_srq_sges = 128;

static unsigned int hfi2_max_srq_wrs = 0x1FFFF;

unsigned short hfi2_piothreshold = 256;

static unsigned int sge_copy_mode;

static void verbs_sdma_complete(struct sdma_txreq *cookie, int status);

static int pio_wait(struct rvt_qp *qp, struct send_context *sc,
		    struct hfi2_pkt_state *ps, u32 flag);

/* Length of buffer to create verbs txreq cache name */
#define TXREQ_NAME_LEN 24

static uint wss_threshold = 80;
static uint wss_clean_period = 256;

/*
 * Translate ib_wr_opcode into ib_wc_opcode.
 */
const enum ib_wc_opcode hfi2_ib_hfi2_wc_opcode[] = {
	[IB_WR_RDMA_WRITE] = IB_WC_RDMA_WRITE,
	[IB_WR_TID_RDMA_WRITE] = IB_WC_RDMA_WRITE,
	[IB_WR_RDMA_WRITE_WITH_IMM] = IB_WC_RDMA_WRITE,
	[IB_WR_SEND] = IB_WC_SEND,
	[IB_WR_SEND_WITH_IMM] = IB_WC_SEND,
	[IB_WR_RDMA_READ] = IB_WC_RDMA_READ,
	[IB_WR_TID_RDMA_READ] = IB_WC_RDMA_READ,
	[IB_WR_ATOMIC_CMP_AND_SWP] = IB_WC_COMP_SWAP,
	[IB_WR_ATOMIC_FETCH_AND_ADD] = IB_WC_FETCH_ADD,
	[IB_WR_SEND_WITH_INV] = IB_WC_SEND,
	[IB_WR_LOCAL_INV] = IB_WC_LOCAL_INV,
	[IB_WR_REG_MR] = IB_WC_REG_MR
};

/*
 * Length of header by opcode, 0 --> not supported
 */
const u8 hfi2_hdr_len_by_opcode[256] = {
	/* RC */
	[IB_OPCODE_RC_SEND_FIRST] = 12 + 8,
	[IB_OPCODE_RC_SEND_MIDDLE] = 12 + 8,
	[IB_OPCODE_RC_SEND_LAST] = 12 + 8,
	[IB_OPCODE_RC_SEND_LAST_WITH_IMMEDIATE] = 12 + 8 + 4,
	[IB_OPCODE_RC_SEND_ONLY] = 12 + 8,
	[IB_OPCODE_RC_SEND_ONLY_WITH_IMMEDIATE] = 12 + 8 + 4,
	[IB_OPCODE_RC_RDMA_WRITE_FIRST] = 12 + 8 + 16,
	[IB_OPCODE_RC_RDMA_WRITE_MIDDLE] = 12 + 8,
	[IB_OPCODE_RC_RDMA_WRITE_LAST] = 12 + 8,
	[IB_OPCODE_RC_RDMA_WRITE_LAST_WITH_IMMEDIATE] = 12 + 8 + 4,
	[IB_OPCODE_RC_RDMA_WRITE_ONLY] = 12 + 8 + 16,
	[IB_OPCODE_RC_RDMA_WRITE_ONLY_WITH_IMMEDIATE] = 12 + 8 + 20,
	[IB_OPCODE_RC_RDMA_READ_REQUEST] = 12 + 8 + 16,
	[IB_OPCODE_RC_RDMA_READ_RESPONSE_FIRST] = 12 + 8 + 4,
	[IB_OPCODE_RC_RDMA_READ_RESPONSE_MIDDLE] = 12 + 8,
	[IB_OPCODE_RC_RDMA_READ_RESPONSE_LAST] = 12 + 8 + 4,
	[IB_OPCODE_RC_RDMA_READ_RESPONSE_ONLY] = 12 + 8 + 4,
	[IB_OPCODE_RC_ACKNOWLEDGE] = 12 + 8 + 4,
	[IB_OPCODE_RC_ATOMIC_ACKNOWLEDGE] = 12 + 8 + 4 + 8,
	[IB_OPCODE_RC_COMPARE_SWAP] = 12 + 8 + 28,
	[IB_OPCODE_RC_FETCH_ADD] = 12 + 8 + 28,
	[IB_OPCODE_RC_SEND_LAST_WITH_INVALIDATE] = 12 + 8 + 4,
	[IB_OPCODE_RC_SEND_ONLY_WITH_INVALIDATE] = 12 + 8 + 4,
	[IB_OPCODE_TID_RDMA_READ_REQ] = 12 + 8 + 36,
	[IB_OPCODE_TID_RDMA_READ_RESP] = 12 + 8 + 36,
	[IB_OPCODE_TID_RDMA_WRITE_REQ] = 12 + 8 + 36,
	[IB_OPCODE_TID_RDMA_WRITE_RESP] = 12 + 8 + 36,
	[IB_OPCODE_TID_RDMA_WRITE_DATA] = 12 + 8 + 36,
	[IB_OPCODE_TID_RDMA_WRITE_DATA_LAST] = 12 + 8 + 36,
	[IB_OPCODE_TID_RDMA_ACK] = 12 + 8 + 36,
	[IB_OPCODE_TID_RDMA_RESYNC] = 12 + 8 + 36,
	/* UC */
	[IB_OPCODE_UC_SEND_FIRST] = 12 + 8,
	[IB_OPCODE_UC_SEND_MIDDLE] = 12 + 8,
	[IB_OPCODE_UC_SEND_LAST] = 12 + 8,
	[IB_OPCODE_UC_SEND_LAST_WITH_IMMEDIATE] = 12 + 8 + 4,
	[IB_OPCODE_UC_SEND_ONLY] = 12 + 8,
	[IB_OPCODE_UC_SEND_ONLY_WITH_IMMEDIATE] = 12 + 8 + 4,
	[IB_OPCODE_UC_RDMA_WRITE_FIRST] = 12 + 8 + 16,
	[IB_OPCODE_UC_RDMA_WRITE_MIDDLE] = 12 + 8,
	[IB_OPCODE_UC_RDMA_WRITE_LAST] = 12 + 8,
	[IB_OPCODE_UC_RDMA_WRITE_LAST_WITH_IMMEDIATE] = 12 + 8 + 4,
	[IB_OPCODE_UC_RDMA_WRITE_ONLY] = 12 + 8 + 16,
	[IB_OPCODE_UC_RDMA_WRITE_ONLY_WITH_IMMEDIATE] = 12 + 8 + 20,
	/* UD */
	[IB_OPCODE_UD_SEND_ONLY] = 12 + 8 + 8,
	[IB_OPCODE_UD_SEND_ONLY_WITH_IMMEDIATE] = 12 + 8 + 12
};

static const opcode_handler opcode_handler_tbl[256] = {
	/* RC */
	[IB_OPCODE_RC_SEND_FIRST] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_SEND_MIDDLE] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_SEND_LAST] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_SEND_LAST_WITH_IMMEDIATE] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_SEND_ONLY] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_SEND_ONLY_WITH_IMMEDIATE] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_RDMA_WRITE_FIRST] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_RDMA_WRITE_MIDDLE] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_RDMA_WRITE_LAST] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_RDMA_WRITE_LAST_WITH_IMMEDIATE] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_RDMA_WRITE_ONLY] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_RDMA_WRITE_ONLY_WITH_IMMEDIATE] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_RDMA_READ_REQUEST] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_RDMA_READ_RESPONSE_FIRST] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_RDMA_READ_RESPONSE_MIDDLE] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_RDMA_READ_RESPONSE_LAST] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_RDMA_READ_RESPONSE_ONLY] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_ACKNOWLEDGE] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_ATOMIC_ACKNOWLEDGE] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_COMPARE_SWAP] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_FETCH_ADD] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_SEND_LAST_WITH_INVALIDATE] = &hfi2_rc_rcv,
	[IB_OPCODE_RC_SEND_ONLY_WITH_INVALIDATE] = &hfi2_rc_rcv,

	/* TID RDMA has separate handlers for different opcodes.*/
	[IB_OPCODE_TID_RDMA_WRITE_REQ] = &hfi2_rc_rcv_tid_rdma_write_req,
	[IB_OPCODE_TID_RDMA_WRITE_RESP] = &hfi2_rc_rcv_tid_rdma_write_resp,
	[IB_OPCODE_TID_RDMA_WRITE_DATA] = &hfi2_rc_rcv_tid_rdma_write_data,
	[IB_OPCODE_TID_RDMA_WRITE_DATA_LAST] = &hfi2_rc_rcv_tid_rdma_write_data,
	[IB_OPCODE_TID_RDMA_READ_REQ] = &hfi2_rc_rcv_tid_rdma_read_req,
	[IB_OPCODE_TID_RDMA_READ_RESP] = &hfi2_rc_rcv_tid_rdma_read_resp,
	[IB_OPCODE_TID_RDMA_RESYNC] = &hfi2_rc_rcv_tid_rdma_resync,
	[IB_OPCODE_TID_RDMA_ACK] = &hfi2_rc_rcv_tid_rdma_ack,

	/* UC */
	[IB_OPCODE_UC_SEND_FIRST] = &hfi2_uc_rcv,
	[IB_OPCODE_UC_SEND_MIDDLE] = &hfi2_uc_rcv,
	[IB_OPCODE_UC_SEND_LAST] = &hfi2_uc_rcv,
	[IB_OPCODE_UC_SEND_LAST_WITH_IMMEDIATE] = &hfi2_uc_rcv,
	[IB_OPCODE_UC_SEND_ONLY] = &hfi2_uc_rcv,
	[IB_OPCODE_UC_SEND_ONLY_WITH_IMMEDIATE] = &hfi2_uc_rcv,
	[IB_OPCODE_UC_RDMA_WRITE_FIRST] = &hfi2_uc_rcv,
	[IB_OPCODE_UC_RDMA_WRITE_MIDDLE] = &hfi2_uc_rcv,
	[IB_OPCODE_UC_RDMA_WRITE_LAST] = &hfi2_uc_rcv,
	[IB_OPCODE_UC_RDMA_WRITE_LAST_WITH_IMMEDIATE] = &hfi2_uc_rcv,
	[IB_OPCODE_UC_RDMA_WRITE_ONLY] = &hfi2_uc_rcv,
	[IB_OPCODE_UC_RDMA_WRITE_ONLY_WITH_IMMEDIATE] = &hfi2_uc_rcv,
	/* UD */
	[IB_OPCODE_UD_SEND_ONLY] = &hfi2_ud_rcv,
	[IB_OPCODE_UD_SEND_ONLY_WITH_IMMEDIATE] = &hfi2_ud_rcv,
	/* CNP */
	[IB_OPCODE_CNP] = &hfi2_cnp_rcv
};

#define OPMASK 0x1f

static const u32 pio_opmask[BIT(3)] = {
	/* RC */
	[IB_OPCODE_RC >> 5] =
		BIT(RC_OP(SEND_ONLY) & OPMASK) |
		BIT(RC_OP(SEND_ONLY_WITH_IMMEDIATE) & OPMASK) |
		BIT(RC_OP(RDMA_WRITE_ONLY) & OPMASK) |
		BIT(RC_OP(RDMA_WRITE_ONLY_WITH_IMMEDIATE) & OPMASK) |
		BIT(RC_OP(RDMA_READ_REQUEST) & OPMASK) |
		BIT(RC_OP(ACKNOWLEDGE) & OPMASK) |
		BIT(RC_OP(ATOMIC_ACKNOWLEDGE) & OPMASK) |
		BIT(RC_OP(COMPARE_SWAP) & OPMASK) |
		BIT(RC_OP(FETCH_ADD) & OPMASK),
	/* UC */
	[IB_OPCODE_UC >> 5] =
		BIT(UC_OP(SEND_ONLY) & OPMASK) |
		BIT(UC_OP(SEND_ONLY_WITH_IMMEDIATE) & OPMASK) |
		BIT(UC_OP(RDMA_WRITE_ONLY) & OPMASK) |
		BIT(UC_OP(RDMA_WRITE_ONLY_WITH_IMMEDIATE) & OPMASK),
};

/*
 * System image GUID.
 */
__be64 hfi2_ib_hfi2_sys_image_guid;

/*
 * Make sure the QP is ready and able to accept the given opcode.
 */
static inline opcode_handler qp_ok(struct hfi2_packet *packet)
{
	if (!(ib_rvt_state_ops[packet->qp->state] & RVT_PROCESS_RECV_OK))
		return NULL;
	if (((packet->opcode & RVT_OPCODE_QP_MASK) ==
	     packet->qp->allowed_ops) ||
	    (packet->opcode == IB_OPCODE_CNP))
		return opcode_handler_tbl[packet->opcode];

	return NULL;
}

static u64 hfi2_fault_tx(struct rvt_qp *qp, u8 opcode, u64 pbc)
{
#ifdef CONFIG_FAULT_INJECTION
	if ((opcode & IB_OPCODE_MSP) == IB_OPCODE_MSP) {
		/*
		 * In order to drop non-IB traffic we
		 * set PbcInsertHrc to NONE (0x2).
		 * The packet will still be delivered
		 * to the receiving node but a
		 * KHdrHCRCErr (KDETH packet with a bad
		 * HCRC) will be triggered and the
		 * packet will not be delivered to the
		 * correct context.
		 */
		pbc &= ~PBC_INSERT_HCRC_SMASK;
		pbc |= (u64)PBC_IHCRC_NONE << PBC_INSERT_HCRC_SHIFT;
	} else {
		/*
		 * In order to drop regular verbs
		 * traffic we set the PbcTestEbp
		 * flag. The packet will still be
		 * delivered to the receiving node but
		 * a 'late ebp error' will be
		 * triggered and will be dropped.
		 */
		pbc |= PBC_TEST_EBP;
	}
#endif
	return pbc;
}

static opcode_handler tid_qp_ok(int opcode, struct hfi2_packet *packet)
{
	if (packet->qp->ibqp.qp_type != IB_QPT_RC ||
	    !(ib_rvt_state_ops[packet->qp->state] & RVT_PROCESS_RECV_OK))
		return NULL;
	if ((opcode & RVT_OPCODE_QP_MASK) == IB_OPCODE_TID_RDMA)
		return opcode_handler_tbl[opcode];
	return NULL;
}

void hfi2_kdeth_eager_rcv(struct hfi2_packet *packet)
{
	struct hfi2_ctxtdata *rcd = packet->rcd;
	struct ib_header *hdr = packet->hdr;
	u32 tlen = packet->tlen;
	struct hfi2_pportdata *ppd = rcd->ppd;
	struct hfi2_ibport *ibp = &ppd->ibport_data;
	struct rvt_dev_info *rdi = &ppd->dd->verbs_dev.rdi;
	opcode_handler opcode_handler;
	unsigned long flags;
	u32 qp_num;
	int lnh;
	u8 opcode;

	/* DW == LRH (2) + BTH (3) + KDETH (9) + CRC (1) */
	if (unlikely(tlen < 15 * sizeof(u32)))
		goto drop;

	lnh = be16_to_cpu(hdr->lrh[0]) & 3;
	if (lnh != HFI2_LRH_BTH)
		goto drop;

	packet->ohdr = &hdr->u.oth;
	trace_input_ibhdr(rcd->dd, packet, packet->sc4);

	opcode = (be32_to_cpu(packet->ohdr->bth[0]) >> 24);
	inc_opstats(tlen, &rcd->opstats->stats[opcode]);

	/* verbs_qp can be picked up from any tid_rdma header struct */
	qp_num = be32_to_cpu(packet->ohdr->u.tid_rdma.r_req.verbs_qp) &
		 RVT_QPN_MASK;

	rcu_read_lock();
	packet->qp = rvt_lookup_qpn(rdi, &ibp->rvp, qp_num);
	if (!packet->qp)
		goto drop_rcu;
	spin_lock_irqsave(&packet->qp->r_lock, flags);
	opcode_handler = tid_qp_ok(opcode, packet);
	if (likely(opcode_handler))
		opcode_handler(packet);
	else
		goto drop_unlock;
	spin_unlock_irqrestore(&packet->qp->r_lock, flags);
	rcu_read_unlock();

	return;
drop_unlock:
	spin_unlock_irqrestore(&packet->qp->r_lock, flags);
drop_rcu:
	rcu_read_unlock();
drop:
	ibp->rvp.n_pkt_drops++;
}

void hfi2_kdeth_expected_rcv(struct hfi2_packet *packet)
{
	struct hfi2_ctxtdata *rcd = packet->rcd;
	struct ib_header *hdr = packet->hdr;
	u32 tlen = packet->tlen;
	struct hfi2_pportdata *ppd = rcd->ppd;
	struct hfi2_ibport *ibp = &ppd->ibport_data;
	struct rvt_dev_info *rdi = &ppd->dd->verbs_dev.rdi;
	opcode_handler opcode_handler;
	unsigned long flags;
	u32 qp_num;
	int lnh;
	u8 opcode;

	/* DW == LRH (2) + BTH (3) + KDETH (9) + CRC (1) */
	if (unlikely(tlen < 15 * sizeof(u32)))
		goto drop;

	lnh = be16_to_cpu(hdr->lrh[0]) & 3;
	if (lnh != HFI2_LRH_BTH)
		goto drop;

	packet->ohdr = &hdr->u.oth;
	trace_input_ibhdr(rcd->dd, packet, packet->sc4);

	opcode = (be32_to_cpu(packet->ohdr->bth[0]) >> 24);
	inc_opstats(tlen, &rcd->opstats->stats[opcode]);

	/* verbs_qp can be picked up from any tid_rdma header struct */
	qp_num = be32_to_cpu(packet->ohdr->u.tid_rdma.r_rsp.verbs_qp) &
		 RVT_QPN_MASK;

	rcu_read_lock();
	packet->qp = rvt_lookup_qpn(rdi, &ibp->rvp, qp_num);
	if (!packet->qp)
		goto drop_rcu;
	spin_lock_irqsave(&packet->qp->r_lock, flags);
	opcode_handler = tid_qp_ok(opcode, packet);
	if (likely(opcode_handler))
		opcode_handler(packet);
	else
		goto drop_unlock;
	spin_unlock_irqrestore(&packet->qp->r_lock, flags);
	rcu_read_unlock();

	return;
drop_unlock:
	spin_unlock_irqrestore(&packet->qp->r_lock, flags);
drop_rcu:
	rcu_read_unlock();
drop:
	ibp->rvp.n_pkt_drops++;
}

static int hfi2_do_pkey_check(struct hfi2_packet *packet)
{
	struct hfi2_ctxtdata *rcd = packet->rcd;
	struct hfi2_pportdata *ppd = rcd->ppd;
	struct hfi2_16b_header *hdr = packet->hdr;
	u16 pkey;

	/* Pkey check needed only for bypass packets */
	if (packet->etype != RHF_RCV_TYPE_BYPASS)
		return 0;

	/* Perform pkey check */
	pkey = hfi2_16B_get_pkey(hdr);
	return ingress_pkey_check(ppd, pkey, packet->sc,
				  packet->qp->s_pkey_index, packet->slid, true);
}

static inline void hfi2_handle_packet(struct hfi2_packet *packet, bool is_mcast)
{
	u32 qp_num;
	struct hfi2_ctxtdata *rcd = packet->rcd;
	struct hfi2_pportdata *ppd = rcd->ppd;
	struct hfi2_ibport *ibp = rcd_to_iport(rcd);
	struct rvt_dev_info *rdi = &ppd->dd->verbs_dev.rdi;
	opcode_handler packet_handler;
	unsigned long flags;

	inc_opstats(packet->tlen, &rcd->opstats->stats[packet->opcode]);

	if (unlikely(is_mcast)) {
		struct rvt_mcast *mcast;
		struct rvt_mcast_qp *p;

		if (!packet->grh)
			goto drop;
		mcast = rvt_mcast_find(&ibp->rvp, &packet->grh->dgid,
				       opa_get_lid(packet->dlid, 9B));
		if (!mcast)
			goto drop;
		rcu_read_lock();
		list_for_each_entry_rcu(p, &mcast->qp_list, list) {
			packet->qp = p->qp;
			if (hfi2_do_pkey_check(packet))
				goto unlock_drop;
			spin_lock_irqsave(&packet->qp->r_lock, flags);
			packet_handler = qp_ok(packet);
			if (likely(packet_handler))
				packet_handler(packet);
			else
				ibp->rvp.n_pkt_drops++;
			spin_unlock_irqrestore(&packet->qp->r_lock, flags);
		}
		rcu_read_unlock();
		/*
		 * Notify rvt_multicast_detach() if it is waiting for us
		 * to finish.
		 */
		if (atomic_dec_return(&mcast->refcount) <= 1)
			wake_up(&mcast->wait);
	} else {
		/* Get the destination QP number. */
		if (packet->etype == RHF_RCV_TYPE_BYPASS &&
		    hfi2_16B_get_l4(packet->hdr) == OPA_16B_L4_FM)
			qp_num = hfi2_16B_get_dest_qpn(packet->mgmt);
		else
			qp_num = ib_bth_get_qpn(packet->ohdr);

		rcu_read_lock();
		packet->qp = rvt_lookup_qpn(rdi, &ibp->rvp, qp_num);
		if (!packet->qp)
			goto unlock_drop;

		if (hfi2_do_pkey_check(packet))
			goto unlock_drop;

		spin_lock_irqsave(&packet->qp->r_lock, flags);
		packet_handler = qp_ok(packet);
		if (likely(packet_handler))
			packet_handler(packet);
		else
			ibp->rvp.n_pkt_drops++;
		spin_unlock_irqrestore(&packet->qp->r_lock, flags);
		rcu_read_unlock();
	}
	return;
unlock_drop:
	rcu_read_unlock();
drop:
	ibp->rvp.n_pkt_drops++;
}

/**
 * hfi2_ib_rcv - process an incoming packet
 * @packet: data packet information
 *
 * This is called to process an incoming packet at interrupt level.
 */
void hfi2_ib_rcv(struct hfi2_packet *packet)
{
	struct hfi2_ctxtdata *rcd = packet->rcd;

	trace_input_ibhdr(rcd->dd, packet, packet->sc4);
	hfi2_handle_packet(packet, hfi2_check_mcast(packet->dlid));
}

void hfi2_16B_rcv(struct hfi2_packet *packet)
{
	struct hfi2_ctxtdata *rcd = packet->rcd;

	trace_input_ibhdr(rcd->dd, packet, false);
	hfi2_handle_packet(packet, hfi2_check_mcast(packet->dlid));
}

/*
 * This is called from a timer to check for QPs
 * which need kernel memory in order to send a packet.
 */
static void mem_timer(struct timer_list *t)
{
	struct hfi2_ibdev *dev = timer_container_of(dev, t, mem_timer);
	struct list_head *list = &dev->memwait;
	struct rvt_qp *qp = NULL;
	struct iowait *wait;
	unsigned long flags;
	struct hfi2_qp_priv *priv;

	write_seqlock_irqsave(&dev->iowait_lock, flags);
	if (!list_empty(list)) {
		wait = list_first_entry(list, struct iowait, list);
		qp = iowait_to_qp(wait);
		priv = qp->priv;
		list_del_init(&priv->s_iowait.list);
		priv->s_iowait.lock = NULL;
		/* refcount held until actual wake up */
		if (!list_empty(list))
			mod_timer(&dev->mem_timer, jiffies + 1);
	}
	write_sequnlock_irqrestore(&dev->iowait_lock, flags);

	if (qp)
		hfi2_qp_wakeup(qp, RVT_S_WAIT_KMEM);
}

/*
 * This is called with progress side lock held.
 */
/* New API */
static void verbs_sdma_complete(struct sdma_txreq *cookie, int status)
{
	struct verbs_txreq *tx =
		container_of(cookie, struct verbs_txreq, txreq);
	struct rvt_qp *qp = tx->qp;
	enum ib_wc_status ib_status =
		status == SDMA_TXREQ_S_OK ? IB_WC_SUCCESS : IB_WC_GENERAL_ERR;
	unsigned long flags;

	spin_lock_irqsave(&qp->s_lock, flags);
	if (tx->wqe) {
		rvt_send_complete(qp, tx->wqe, ib_status);
	} else if (qp->ibqp.qp_type == IB_QPT_RC) {
		struct hfi2_opa_header *hdr;

		hdr = &tx->phdr.hdr;
		if (unlikely(status != SDMA_TXREQ_S_OK))
			hfi2_rc_verbs_aborted(qp, hdr);
		hfi2_rc_send_complete(qp, hdr);
	}
	spin_unlock_irqrestore(&qp->s_lock, flags);

	hfi2_put_txreq(tx);
}

void hfi2_wait_kmem(struct rvt_qp *qp)
{
	struct hfi2_qp_priv *priv = qp->priv;
	struct ib_qp *ibqp = &qp->ibqp;
	struct ib_device *ibdev = ibqp->device;
	struct hfi2_ibdev *dev = to_idev(ibdev);

	if (list_empty(&priv->s_iowait.list)) {
		if (list_empty(&dev->memwait))
			mod_timer(&dev->mem_timer, jiffies + 1);
		qp->s_flags |= RVT_S_WAIT_KMEM;
		list_add_tail(&priv->s_iowait.list, &dev->memwait);
		priv->s_iowait.lock = &dev->iowait_lock;
		trace_hfi2_qpsleep(qp, RVT_S_WAIT_KMEM);
		rvt_get_qp(qp);
	}
}

/*
 * This routine calls txadds for each sg entry.
 *
 * Add failures will revert the sge cursor
 */
static noinline int build_verbs_ulp_payload(struct sdma_engine *sde, u32 length,
					    struct verbs_txreq *tx)
{
	struct rvt_sge_state *ss = tx->ss;
	struct rvt_sge *sg_list = ss->sg_list;
	struct rvt_sge sge = ss->sge;
	u8 num_sge = ss->num_sge;
	u32 len;
	int ret = 0;

	while (length) {
		len = rvt_get_sge_length(&ss->sge, length);
		WARN_ON_ONCE(len == 0);
		ret = sdma_txadd_kvaddr(sde->dd, &tx->txreq, ss->sge.vaddr,
					len);
		if (ret)
			goto bail_txadd;
		rvt_update_sge(ss, len, false);
		length -= len;
	}
	return ret;
bail_txadd:
	/* unwind cursor */
	ss->sge = sge;
	ss->num_sge = num_sge;
	ss->sg_list = sg_list;
	return ret;
}

/**
 * update_tx_opstats - record stats by opcode
 * @qp: the qp
 * @ps: transmit packet state
 * @plen: the plen in dwords
 *
 * This is a routine to record the tx opstats after a
 * packet has been presented to the egress mechanism.
 */
static void update_tx_opstats(struct rvt_qp *qp, struct hfi2_pkt_state *ps,
			      u32 plen)
{
#ifdef CONFIG_DEBUG_FS
	struct hfi2_devdata *dd = dd_from_ibdev(qp->ibqp.device);
	struct hfi2_opcode_stats_perctx *s = get_cpu_ptr(dd->tx_opstats);

	inc_opstats(plen * 4, &s->stats[ps->opcode]);
	put_cpu_ptr(s);
#endif
}

/*
 * Build the number of DMA descriptors needed to send length bytes of data.
 *
 * NOTE: DMA mapping is held in the tx until completed in the ring or
 *       the tx desc is freed without having been submitted to the ring
 *
 * This routine ensures all the helper routine calls succeed.
 */
/* New API */
static int build_verbs_tx_desc(struct sdma_engine *sde, u32 length,
			       struct verbs_txreq *tx,
			       struct hfi2_ahg_info *ahg_info, u64 pbc)
{
	int ret = 0;
	struct hfi2_sdma_header *phdr = &tx->phdr;
	u16 hdrbytes = (tx->hdr_dwords + sizeof(pbc) / 4) << 2;
	u8 extra_bytes = 0;

	if (tx->phdr.hdr.hdr_type) {
		/*
		 * hdrbytes accounts for PBC. Need to subtract 8 bytes
		 * before calculating padding.
		 */
		if (sde->dd->params->chip_type == CHIP_WFR) {
			extra_bytes =
				hfi2_get_16b_padding(hdrbytes - 8, length) +
				(SIZE_OF_CRC << 2) + SIZE_OF_LT;
		} else {
			/* add ICRC QW */
			extra_bytes = hfi2_pad8(hdrbytes - 8 + length) + 8;
		}
	}
	if (!ahg_info->ahgcount) {
		ret = sdma_txinit_ahg(sde->dd, &tx->txreq, ahg_info->tx_flags,
				      hdrbytes + length + extra_bytes,
				      ahg_info->ahgidx, 0, NULL, 0,
				      verbs_sdma_complete);
		if (ret)
			goto bail_txadd;
		phdr->pbc = cpu_to_le64(pbc);
		ret = sdma_txadd_kvaddr(sde->dd, &tx->txreq, phdr, hdrbytes);
		if (ret)
			goto bail_txadd;
	} else {
		ret = sdma_txinit_ahg(sde->dd, &tx->txreq, ahg_info->tx_flags,
				      length + extra_bytes, ahg_info->ahgidx,
				      ahg_info->ahgcount, ahg_info->ahgdesc,
				      hdrbytes, verbs_sdma_complete);
		if (ret)
			goto bail_txadd;
	}
	/* add the ulp payload - if any. tx->ss can be NULL for acks */
	if (tx->ss) {
		ret = build_verbs_ulp_payload(sde, length, tx);
		if (ret)
			goto bail_txadd;
	}

	/* add icrc, lt byte, and padding to flit */
	if (extra_bytes)
		ret = sdma_txadd_daddr(sde->dd, &tx->txreq,
				       sde->dd->sdma_pad_phys, extra_bytes);

bail_txadd:
	return ret;
}

static u64 update_hcrc(u8 opcode, u64 pbc)
{
	if ((opcode & IB_OPCODE_TID_RDMA) == IB_OPCODE_TID_RDMA) {
		pbc &= ~PBC_INSERT_HCRC_SMASK;
		pbc |= (u64)PBC_IHCRC_LKDETH << PBC_INSERT_HCRC_SHIFT;
	}
	return pbc;
}

int hfi2_verbs_send_dma(struct rvt_qp *qp, struct hfi2_pkt_state *ps, u64 pbc)
{
	struct hfi2_qp_priv *priv = qp->priv;
	struct hfi2_ahg_info *ahg_info = priv->s_ahg;
	u32 hdrwords = ps->s_txreq->hdr_dwords;
	u32 len = ps->s_txreq->s_cur_size;
	u32 plen;
	struct hfi2_pportdata *ppd = ps->ppd;
	struct hfi2_devdata *dd = ppd->dd;
	struct verbs_txreq *tx;
	u8 sc5 = priv->s_sc;
	int ret;
	u32 dwords;

	if (ps->s_txreq->phdr.hdr.hdr_type) {
		u8 extra_bytes;

		if (dd->params->chip_type == CHIP_WFR) {
			extra_bytes =
				hfi2_get_16b_padding((hdrwords << 2), len);
			dwords = (len + extra_bytes + (SIZE_OF_CRC << 2) +
				  SIZE_OF_LT) >>
				 2;
		} else {
			/* round up to multiple of 8 */
			extra_bytes = hfi2_pad8((hdrwords << 2) + len);
			/* add ICRC QW */
			dwords = (len + extra_bytes + 8) >> 2;
		}
	} else {
		dwords = (len + 3) >> 2;
	}
	plen = hdrwords + dwords + sizeof(pbc) / 4;

	tx = ps->s_txreq;
	if (!sdma_txreq_built(&tx->txreq)) {
		if (likely(pbc == 0)) {
			u32 vl = sc_to_vlt(ppd, sc5);
			u32 l2;
			u32 dlid;

			/* No vl15 here */
			/* set PBC_DC_INFO bit (aka SC[4]) in pbc */
			if (ps->s_txreq->phdr.hdr.hdr_type) {
				l2 = PBC_L2_16B;
				dlid = hfi2_16B_get_dlid(&tx->phdr.hdr.opah);
			} else {
				pbc |= pbc_sc4_flag(sc5);
				l2 = PBC_L2_9B;
				dlid = ib_get_dlid(&tx->phdr.hdr.ibh);
			}

			pbc = dd->params->create_pbc(
				ppd, ps->hfi2_loopback, pbc, qp->srate_mbps, vl,
				plen, l2, dlid,
				priv->s_sendcontext->hw_context);

			if (unlikely(hfi2_dbg_should_fault_tx(qp, ps->opcode)))
				pbc = hfi2_fault_tx(qp, ps->opcode, pbc);
			else
				/* Update HCRC based on packet opcode */
				pbc = update_hcrc(ps->opcode, pbc);
		}
		tx->wqe = qp->s_wqe;
		ret = build_verbs_tx_desc(tx->sde, len, tx, ahg_info, pbc);
		if (unlikely(ret)) {
			/*
			 * Drop this transmit with an error.  Expect that
			 * whatever caused the build error will occur again,
			 * so do not attempt to retry.  If a retry is attempted,
			 * then tx would need to be cleaned first - tx may not
			 * be fully constructed.  tx will be cleaned when
			 * deallocated.
			 */
			verbs_sdma_complete(&tx->txreq, SDMA_TXREQ_S_SENDERROR);
			/* verbs_sdma_complete already freed tx via hfi2_put_txreq */
			ps->s_txreq = NULL;
			/* return 0 so the next transmit is attempted */
			ret = 0;
			goto bail_build;
		}
	}
	/*
	 * hfi2_sdma_send_txreq() does not know about verbs_txreq and may transfer
	 * ownership of tx's ref.  Take an extra reference here so that tx
	 * remains valid for the trace that follows the hfi2_sdma_send_txreq() call.
	 *
	 * Reference ownership will have been transferred if the return value
	 * is 0, -ECOMM, or -EIOCBQUEUED.
	 */
	hfi2_get_txreq(tx);
	ret = hfi2_sdma_send_txreq(tx->sde, ps->wait, &tx->txreq,
				   ps->pkts_sent);
	trace_sdma_output_ibhdr(dd, &ps->s_txreq->phdr.hdr, ib_is_sc5(sc5),
				ret);
	if (unlikely(ret < 0))
		goto bail_ecomm;
	/* ret == 0 => the extra reference has been transferred */

	update_tx_opstats(qp, ps, plen);
	/* put ps's reference */
	hfi2_put_txreq(tx);
	ps->s_txreq = NULL;
	return 0;

bail_ecomm:
	if (ret == -EIOCBQUEUED) {
		/*
		 * The SDMA engine descriptor queue is full.  The txreq (and
		 * its reference) has been added to the wait list.
		 *
		 * Return an error so pending transmit attempts cease until
		 * the engine is ready.
		 */
	} else if (ret == -ECOMM) {
		/*
		 * The SDMA engine has stopped.  The txreq (and its reference)
		 * has been transferred to a flush list.
		 *
		 * Return 0 so all pending transmits also drop like this one.
		 */
		ret = 0;
	} else {
		/*
		 * An unexpected error has occurred.  Complete the tx with an
		 * error.  The complete will remove the reference.  A call to
		 * sdma_txclean() will occur when the structure is deallocated.
		 *
		 * Return 0 so the next transmit is attempted.
		 */
		tx->txreq.complete(&tx->txreq, SDMA_TXREQ_S_SENDERROR);
		ret = 0;
	}
bail_build:
	/* put ps's reference if not already freed */
	if (ps->s_txreq) {
		hfi2_put_txreq(ps->s_txreq);
		ps->s_txreq = NULL;
	}
	return ret;
}

/*
 * If we are now in the error state, return zero to flush the
 * send work request.
 */
static int pio_wait(struct rvt_qp *qp, struct send_context *sc,
		    struct hfi2_pkt_state *ps, u32 flag)
{
	struct hfi2_qp_priv *priv = qp->priv;
	struct hfi2_devdata *dd = sc->dd;
	unsigned long flags;
	int ret = 0;

	/*
	 * Note that as soon as want_buffer() is called and
	 * possibly before it returns, sc_piobufavail()
	 * could be called. Therefore, put QP on the I/O wait list before
	 * enabling the PIO avail interrupt.
	 */
	spin_lock_irqsave(&qp->s_lock, flags);
	if (ib_rvt_state_ops[qp->state] & RVT_PROCESS_RECV_OK) {
		/* take a new txreq reference */
		hfi2_get_txreq(ps->s_txreq);
		write_seqlock(&sc->waitlock);
		list_add_tail(&ps->s_txreq->txreq.list, &ps->wait->tx_head);
		if (list_empty(&priv->s_iowait.list)) {
			struct hfi2_ibdev *dev = &dd->verbs_dev;
			int was_empty;

			dev->n_piowait += !!(flag & RVT_S_WAIT_PIO);
			dev->n_piodrain += !!(flag & HFI2_S_WAIT_PIO_DRAIN);
			qp->s_flags |= flag;
			was_empty = list_empty(&sc->piowait);
			iowait_get_priority(&priv->s_iowait);
			iowait_queue(ps->pkts_sent, &priv->s_iowait,
				     &sc->piowait);
			priv->s_iowait.lock = &sc->waitlock;
			trace_hfi2_qpsleep(qp, RVT_S_WAIT_PIO);
			rvt_get_qp(qp);
			/* counting: only call wantpiobuf_intr if first user */
			if (was_empty)
				hfi2_sc_wantpiobuf_intr(sc, 1);
		}
		write_sequnlock(&sc->waitlock);
		hfi2_qp_unbusy(qp, ps->wait);
		ret = -EBUSY;
	}
	spin_unlock_irqrestore(&qp->s_lock, flags);
	return ret;
}

static void verbs_pio_complete(void *arg, int code)
{
	struct rvt_qp *qp = (struct rvt_qp *)arg;
	struct hfi2_qp_priv *priv = qp->priv;

	if (iowait_pio_dec(&priv->s_iowait))
		iowait_drain_wakeup(&priv->s_iowait);
}

static void hfi2_verbs_complete_tx(struct rvt_qp *qp, struct hfi2_pkt_state *ps,
				   int wc_status)
{
	unsigned long flags;

	spin_lock_irqsave(&qp->s_lock, flags);
	if (qp->s_wqe) {
		rvt_send_complete(qp, qp->s_wqe, wc_status);
	} else if (qp->ibqp.qp_type == IB_QPT_RC) {
		if (unlikely(wc_status != IB_WC_SUCCESS))
			hfi2_rc_verbs_aborted(qp, &ps->s_txreq->phdr.hdr);
		hfi2_rc_send_complete(qp, &ps->s_txreq->phdr.hdr);
	}
	spin_unlock_irqrestore(&qp->s_lock, flags);
}

int hfi2_verbs_send_pio(struct rvt_qp *qp, struct hfi2_pkt_state *ps, u64 pbc)
{
	struct hfi2_qp_priv *priv = qp->priv;
	u32 hdrwords = ps->s_txreq->hdr_dwords;
	struct rvt_sge_state *ss = ps->s_txreq->ss;
	u32 len = ps->s_txreq->s_cur_size;
	u32 dwords;
	u32 plen;
	struct hfi2_pportdata *ppd = ps->ppd;
	struct hfi2_devdata *dd = ppd->dd;
	u32 *hdr;
	u8 sc5;
	struct send_context *sc;
	struct pio_buf *pbuf;
	int wc_status = IB_WC_SUCCESS;
	int ret = 0;
	pio_release_cb cb = NULL;
	u8 extra_bytes = 0;

	if (ps->s_txreq->phdr.hdr.hdr_type) {
		if (dd->params->chip_type == CHIP_WFR) {
			u8 pad_size =
				hfi2_get_16b_padding((hdrwords << 2), len);

			extra_bytes =
				pad_size + (SIZE_OF_CRC << 2) + SIZE_OF_LT;
		} else {
			/* round up to next multiple of 8, add ICRC */
			extra_bytes = hfi2_pad8((hdrwords << 2) + len) + 8;
		}
		dwords = (len + extra_bytes) >> 2;
		hdr = (u32 *)&ps->s_txreq->phdr.hdr.opah;
	} else {
		dwords = (len + 3) >> 2;
		hdr = (u32 *)&ps->s_txreq->phdr.hdr.ibh;
	}
	plen = hdrwords + dwords + sizeof(pbc) / 4;

	/* only RC/UC use complete */
	switch (qp->ibqp.qp_type) {
	case IB_QPT_RC:
	case IB_QPT_UC:
		cb = verbs_pio_complete;
		break;
	default:
		break;
	}

	/* vl15 special case taken care of in ud.c */
	sc5 = priv->s_sc;
	sc = ps->s_txreq->psc;

	if (likely(pbc == 0)) {
		u8 vl = sc_to_vlt(ppd, sc5);
		u32 l2;
		u32 dlid;

		/* set PBC_DC_INFO bit (aka SC[4]) in pbc */
		if (ps->s_txreq->phdr.hdr.hdr_type) {
			l2 = PBC_L2_16B;
			dlid = hfi2_16B_get_dlid(&ps->s_txreq->phdr.hdr.opah);
		} else {
			pbc |= pbc_sc4_flag(sc5);
			l2 = PBC_L2_9B;
			dlid = ib_get_dlid(&ps->s_txreq->phdr.hdr.ibh);
		}

		pbc = dd->params->create_pbc(ppd, ps->hfi2_loopback, pbc,
					     qp->srate_mbps, vl, plen, l2, dlid,
					     priv->s_sendcontext->hw_context);
		if (unlikely(hfi2_dbg_should_fault_tx(qp, ps->opcode)))
			pbc = hfi2_fault_tx(qp, ps->opcode, pbc);
		else
			/* Update HCRC based on packet opcode */
			pbc = update_hcrc(ps->opcode, pbc);
	}
	if (cb)
		iowait_pio_inc(&priv->s_iowait);
	pbuf = hfi2_sc_buffer_alloc(sc, plen, cb, qp);
	if (IS_ERR_OR_NULL(pbuf)) {
		if (cb)
			verbs_pio_complete(qp, 0);
		if (IS_ERR(pbuf)) {
			/*
			 * If we have filled the PIO buffers to capacity and are
			 * not in an active state this request is not going to
			 * go out to so just complete it with an error or else a
			 * ULP or the core may be stuck waiting.
			 */
			hfi2_cdbg(PIO, "alloc failed with error, completing");
			wc_status = IB_WC_GENERAL_ERR;
			goto pio_bail;
		}

		/*
		 * This is a normal occurrence. The PIO buffers are all busy.
		 * Queue the tx request.
		 */
		ret = pio_wait(qp, sc, ps, RVT_S_WAIT_PIO);
		if (ret == 0) {
			/* not able to queue - complete tx with an error */
			wc_status = IB_WC_GENERAL_ERR;
			hfi2_cdbg(PIO,
				  "alloc failed, unable to queue, completing");
			goto pio_bail;
		}
		/* tx is queued - return an error to stop processing */
		hfi2_cdbg(PIO, "alloc failed, queued");
		goto bail;
	}

	if (dwords == 0) {
		hfi2_pio_copy(dd, pbuf, pbc, hdr, hdrwords);
	} else {
		hfi2_seg_pio_copy_start(pbuf, pbc, hdr, hdrwords * 4);
		if (ss) {
			while (len) {
				void *addr = ss->sge.vaddr;
				u32 slen = rvt_get_sge_length(&ss->sge, len);

				rvt_update_sge(ss, slen, false);
				hfi2_seg_pio_copy_mid(pbuf, addr, slen);
				len -= slen;
			}
		}
		/* add icrc, lt byte, and padding to flit */
		if (extra_bytes)
			hfi2_seg_pio_copy_mid(pbuf, dd->sdma_pad_dma,
					      extra_bytes);

		hfi2_seg_pio_copy_end(pbuf);
	}

	update_tx_opstats(qp, ps, plen);
	trace_pio_output_ibhdr(dd, &ps->s_txreq->phdr.hdr, ib_is_sc5(sc5), 0);

pio_bail:
	hfi2_verbs_complete_tx(qp, ps, wc_status);
	ret = 0;

bail:
	/* release ps hold on s_txreq */
	hfi2_put_txreq(ps->s_txreq);
	ps->s_txreq = NULL;
	return ret;
}

/*
 * egress_pkey_matches_entry - return 1 if the pkey matches ent (ent
 * being an entry from the partition key table), return 0
 * otherwise. Use the matching criteria for egress partition keys
 * specified in the OPAv1 spec., section 9.1l.7.
 */
static inline int egress_pkey_matches_entry(u16 pkey, u16 ent)
{
	u16 mkey = pkey & PKEY_LOW_15_MASK;
	u16 mentry = ent & PKEY_LOW_15_MASK;

	if (mkey == mentry) {
		/*
		 * If pkey[15] is set (full partition member),
		 * is bit 15 in the corresponding table element
		 * clear (limited member)?
		 */
		if (pkey & PKEY_MEMBER_MASK)
			return !!(ent & PKEY_MEMBER_MASK);
		return 1;
	}
	return 0;
}

/**
 * hfi2_egress_pkey_check - check P_KEY of a packet
 * @ppd:  Physical IB port data
 * @slid: SLID for packet
 * @pkey: PKEY for header
 * @sc5:  SC for packet
 * @s_pkey_index: It will be used for look up optimization for kernel contexts
 * only. If it is negative value, then it means user contexts is calling this
 * function.
 *
 * It checks if hdr's pkey is valid.
 *
 * Return: 0 on success, otherwise, 1
 */
int hfi2_egress_pkey_check(struct hfi2_pportdata *ppd, u32 slid, u16 pkey,
			   u8 sc5, int8_t s_pkey_index)
{
	struct hfi2_devdata *dd = ppd->dd;
	int i;
	int is_user_ctxt_mechanism = (s_pkey_index < 0);

	if (!(ppd->part_enforce & HFI2_PART_ENFORCE_OUT))
		return 0;

	/* If SC15, pkey[0:14] must be 0x7fff */
	if ((sc5 == 0xf) && ((pkey & PKEY_LOW_15_MASK) != PKEY_LOW_15_MASK))
		goto bad;

	/* Is the pkey = 0x0, or 0x8000? */
	if ((pkey & PKEY_LOW_15_MASK) == 0)
		goto bad;

	/*
	 * For the kernel contexts only, if a qp is passed into the function,
	 * the most likely matching pkey has index qp->s_pkey_index
	 */
	if (!is_user_ctxt_mechanism &&
	    egress_pkey_matches_entry(pkey, ppd->pkeys[s_pkey_index])) {
		return 0;
	}

	for (i = 0; i < dd->params->pkey_table_size; i++) {
		if (egress_pkey_matches_entry(pkey, ppd->pkeys[i]))
			return 0;
	}
bad:
	/*
	 * For the user-context mechanism, the P_KEY check would only happen
	 * once per SDMA request, not once per packet.  Therefore, there's no
	 * need to increment the counter for the user-context mechanism.
	 */
	if (!is_user_ctxt_mechanism) {
		incr_cntr64(&ppd->port_xmit_constraint_errors);
		if (!(dd->err_info_xmit_constraint.status &
		      OPA_EI_STATUS_SMASK)) {
			dd->err_info_xmit_constraint.status |=
				OPA_EI_STATUS_SMASK;
			dd->err_info_xmit_constraint.slid = slid;
			dd->err_info_xmit_constraint.pkey = pkey;
		}
	}
	return 1;
}

/*
 * get_send_routine - choose an egress routine
 *
 * Choose an egress routine based on QP type
 * and size
 */
static inline send_routine get_send_routine(struct rvt_qp *qp,
					    struct hfi2_pkt_state *ps)
{
	struct hfi2_devdata *dd = dd_from_ibdev(qp->ibqp.device);
	struct hfi2_qp_priv *priv = qp->priv;
	struct verbs_txreq *tx = ps->s_txreq;

	if (unlikely(!(dd->flags & HFI2_HAS_SEND_DMA)))
		return dd->process_pio_send;
	switch (qp->ibqp.qp_type) {
	case IB_QPT_SMI:
		return dd->process_pio_send;
	case IB_QPT_GSI:
	case IB_QPT_UD:
		break;
	case IB_QPT_UC:
	case IB_QPT_RC:
		priv->s_running_pkt_size =
			(tx->s_cur_size + priv->s_running_pkt_size) / 2;
		if (hfi2_piothreshold &&
		    priv->s_running_pkt_size <=
			    min(hfi2_piothreshold, qp->pmtu) &&
		    (BIT(ps->opcode & OPMASK) & pio_opmask[ps->opcode >> 5]) &&
		    iowait_sdma_pending(&priv->s_iowait) == 0 &&
		    !sdma_txreq_built(&tx->txreq))
			return dd->process_pio_send;
		break;
	default:
		break;
	}
	return dd->process_dma_send;
}

/**
 * hfi2_verbs_send - send a packet
 * @qp: the QP to send on
 * @ps: the state of the packet to send
 *
 * Return zero if packet is sent or queued OK.
 * Return non-zero and clear qp->s_flags RVT_S_BUSY otherwise.
 */
int hfi2_verbs_send(struct rvt_qp *qp, struct hfi2_pkt_state *ps)
{
	struct hfi2_devdata *dd = dd_from_ibdev(qp->ibqp.device);
	struct hfi2_qp_priv *priv = qp->priv;
	struct hfi2_ibport *ibp = to_iport(qp->ibqp.device, qp->port_num);
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);
	struct ib_other_headers *ohdr = NULL;
	send_routine sr;
	int ret;
	u16 pkey;
	u32 slid;
	u8 l4 = 0;

	/* locate the pkey within the headers */
	if (ps->s_txreq->phdr.hdr.hdr_type) {
		struct hfi2_16b_header *hdr = &ps->s_txreq->phdr.hdr.opah;

		l4 = hfi2_16B_get_l4(hdr);
		if (l4 == OPA_16B_L4_IB_LOCAL)
			ohdr = &hdr->u.oth;
		else if (l4 == OPA_16B_L4_IB_GLOBAL)
			ohdr = &hdr->u.l.oth;

		slid = hfi2_16B_get_slid(hdr);
		pkey = hfi2_16B_get_pkey(hdr);
	} else {
		struct ib_header *hdr = &ps->s_txreq->phdr.hdr.ibh;
		u8 lnh = ib_get_lnh(hdr);

		if (lnh == HFI2_LRH_GRH)
			ohdr = &hdr->u.l.oth;
		else
			ohdr = &hdr->u.oth;
		slid = ib_get_slid(hdr);
		pkey = ib_bth_get_pkey(ohdr);
	}

	if (likely(l4 != OPA_16B_L4_FM))
		ps->opcode = ib_bth_get_opcode(ohdr);
	else
		ps->opcode = IB_OPCODE_UD_SEND_ONLY;

	sr = get_send_routine(qp, ps);
	ret = hfi2_egress_pkey_check(ppd, slid, pkey, priv->s_sc,
				     qp->s_pkey_index);
	if (unlikely(ret)) {
		/*
		 * The value we are returning here does not get propagated to
		 * the verbs caller. Thus we need to complete the request with
		 * error otherwise the caller could be sitting waiting on the
		 * completion event. Only do this for PIO. SDMA has its own
		 * mechanism for handling the errors. So for SDMA we can just
		 * return.
		 */
		if (sr == dd->process_pio_send) {
			unsigned long flags;

			hfi2_cdbg(PIO, "%s() Failed. Completing with err",
				  __func__);
			spin_lock_irqsave(&qp->s_lock, flags);
			rvt_send_complete(qp, qp->s_wqe, IB_WC_GENERAL_ERR);
			spin_unlock_irqrestore(&qp->s_lock, flags);
		} else {
			hfi2_put_txreq(ps->s_txreq);
			ps->s_txreq = NULL;
		}
		return -EINVAL;
	}
	if (sr == dd->process_dma_send && iowait_pio_pending(&priv->s_iowait)) {
		ret = pio_wait(qp, ps->s_txreq->psc, ps, HFI2_S_WAIT_PIO_DRAIN);
		if (ret == 0) {
			/* not able to queue - complete tx with an error */
			hfi2_verbs_complete_tx(qp, ps, IB_WC_GENERAL_ERR);
		}
		/* queued or not, ps is done with s_txreq */
		hfi2_put_txreq(ps->s_txreq);
		ps->s_txreq = NULL;
		return ret;
	}
	return sr(qp, ps, 0);
}

/**
 * hfi2_fill_device_attr - Fill in rvt dev info device attributes.
 * @dd: the device data structure
 */
static void hfi2_fill_device_attr(struct hfi2_devdata *dd)
{
	struct rvt_dev_info *rdi = &dd->verbs_dev.rdi;
	u32 ver = dd->dc8051_ver;

	memset(&rdi->dparms.props, 0, sizeof(rdi->dparms.props));

	if (!dd->cport) {
		rdi->dparms.props.fw_ver = ((u64)(dc8051_ver_maj(ver)) << 32) |
					   ((u64)(dc8051_ver_min(ver)) << 16) |
					   (u64)dc8051_ver_patch(ver);
	} else {
		rdi->dparms.props.fw_ver = dd->cport_ver;
	}
	rdi->dparms.props.device_cap_flags =
		IB_DEVICE_BAD_PKEY_CNTR | IB_DEVICE_BAD_QKEY_CNTR |
		IB_DEVICE_SHUTDOWN_PORT | IB_DEVICE_SYS_IMAGE_GUID |
		IB_DEVICE_RC_RNR_NAK_GEN | IB_DEVICE_PORT_ACTIVE_EVENT |
		IB_DEVICE_SRQ_RESIZE | IB_DEVICE_MEM_MGT_EXTENSIONS;
	rdi->dparms.props.kernel_cap_flags = IBK_RDMA_NETDEV_OPA;
	rdi->dparms.props.page_size_cap = PAGE_SIZE;
	rdi->dparms.props.vendor_id = dd->oui1 << 16 | dd->oui2 << 8 | dd->oui3;
	rdi->dparms.props.vendor_part_id = dd->pcidev->device;
	rdi->dparms.props.hw_ver = dd->minrev;
	rdi->dparms.props.sys_image_guid = hfi2_ib_hfi2_sys_image_guid;
	rdi->dparms.props.max_mr_size = U64_MAX;
	rdi->dparms.props.max_fast_reg_page_list_len = UINT_MAX;
	rdi->dparms.props.max_qp = hfi2_max_qps;
	rdi->dparms.props.max_qp_wr = (hfi2_max_qp_wrs >= HFI2_QP_WQE_INVALID ?
					       HFI2_QP_WQE_INVALID - 1 :
					       hfi2_max_qp_wrs);
	rdi->dparms.props.max_send_sge = hfi2_max_sges;
	rdi->dparms.props.max_recv_sge = hfi2_max_sges;
	rdi->dparms.props.max_sge_rd = hfi2_max_sges;
	rdi->dparms.props.max_cq = hfi2_max_cqs;
	rdi->dparms.props.max_ah = hfi2_max_ahs;
	rdi->dparms.props.max_cqe = hfi2_max_cqes;
	rdi->dparms.props.max_pd = hfi2_max_pds;
	rdi->dparms.props.max_qp_rd_atom = HFI2_MAX_RDMA_ATOMIC;
	rdi->dparms.props.max_qp_init_rd_atom = 255;
	rdi->dparms.props.max_srq = hfi2_max_srqs;
	rdi->dparms.props.max_srq_wr = hfi2_max_srq_wrs;
	rdi->dparms.props.max_srq_sge = hfi2_max_srq_sges;
	rdi->dparms.props.atomic_cap = IB_ATOMIC_GLOB;
	rdi->dparms.props.max_pkeys = hfi2_get_npkeys(dd);
	rdi->dparms.props.max_mcast_grp = hfi2_max_mcast_grps;
	rdi->dparms.props.max_mcast_qp_attach = hfi2_max_mcast_qp_attached;
	rdi->dparms.props.max_total_mcast_qp_attach =
		rdi->dparms.props.max_mcast_qp_attach *
		rdi->dparms.props.max_mcast_grp;
}

static inline u16 opa_speed_to_ib(u16 in)
{
	u16 out = 0;

	if (in & OPA_LINK_SPEED_100G)
		out |= IB_SPEED_NDR;
	if (in & OPA_LINK_SPEED_50G)
		out |= IB_SPEED_HDR;
	if (in & OPA_LINK_SPEED_25G)
		out |= IB_SPEED_EDR;
	if (in & OPA_LINK_SPEED_12_5G)
		out |= IB_SPEED_FDR;

	return out;
}

/*
 * Convert a single OPA link width (no multiple flags) to an IB value.
 * A zero OPA link width means link down, which means the IB width value
 * is a don't care.
 */
static inline u16 opa_width_to_ib(u16 in)
{
	switch (in) {
	case OPA_LINK_WIDTH_1X:
	/* map 2x and 3x to 1x as they don't exist in IB */
	case OPA_LINK_WIDTH_2X:
	case OPA_LINK_WIDTH_3X:
		return IB_WIDTH_1X;
	default: /* link down or unknown, return our largest width */
	case OPA_LINK_WIDTH_4X:
		return IB_WIDTH_4X;
	}
}

static int query_port(struct rvt_dev_info *rdi, u32 port_num,
		      struct ib_port_attr *props)
{
	struct hfi2_ibdev *verbs_dev = dev_from_rdi(rdi);
	struct hfi2_devdata *dd = dd_from_dev(verbs_dev);
	struct hfi2_pportdata *ppd = &dd->pport[port_num - 1];
	u32 lid = ppd->lid;

	/* props being zeroed by the caller, avoid zeroing it here */
	props->lid = lid ? lid : 0;
	props->lmc = ppd->lmc;
	/* OPA logical states match IB logical states */
	props->state = hfi2_driver_lstate(ppd);
	props->phys_state = hfi2_driver_pstate(ppd);
	props->gid_tbl_len = HFI2_GUIDS_PER_PORT;
	props->active_width = (u8)opa_width_to_ib(ppd->link_width_active);
	/* see rate_show() in ib core/sysfs.c */
	props->active_speed = opa_speed_to_ib(ppd->link_speed_active);
	props->max_vl_num = ppd->vls_supported;

	/* Once we are a "first class" citizen and have added the OPA MTUs to
	 * the core we can advertise the larger MTU enum to the ULPs, for now
	 * advertise only 4K.
	 *
	 * Those applications which are either OPA aware or pass the MTU enum
	 * from the Path Records to us will get the new 8k MTU.  Those that
	 * attempt to process the MTU enum may fail in various ways.
	 */
	props->max_mtu = hfi2_mtu_to_enum(
		(!valid_ib_mtu(hfi2_max_mtu) ? 4096 : hfi2_max_mtu),
		IB_MTU_4096);
	props->active_mtu = !valid_ib_mtu(ppd->ibmtu) ?
				    props->max_mtu :
				    hfi2_mtu_to_enum(ppd->ibmtu, IB_MTU_4096);
	props->phys_mtu = hfi2_max_mtu;

	return 0;
}

static int modify_device(struct ib_device *device, int device_modify_mask,
			 struct ib_device_modify *device_modify)
{
	struct hfi2_devdata *dd = dd_from_ibdev(device);
	unsigned int i;
	int ret;

	if (device_modify_mask &
	    ~(IB_DEVICE_MODIFY_SYS_IMAGE_GUID | IB_DEVICE_MODIFY_NODE_DESC)) {
		ret = -EOPNOTSUPP;
		goto bail;
	}

	if (device_modify_mask & IB_DEVICE_MODIFY_NODE_DESC) {
		memcpy(device->node_desc, device_modify->node_desc,
		       IB_DEVICE_NODE_DESC_MAX);
		for (i = 0; i < dd->num_pports; i++) {
			struct hfi2_ibport *ibp = &dd->pport[i].ibport_data;

			hfi2_node_desc_chg(ibp);
		}
	}

	if (device_modify_mask & IB_DEVICE_MODIFY_SYS_IMAGE_GUID) {
		hfi2_ib_hfi2_sys_image_guid =
			cpu_to_be64(device_modify->sys_image_guid);
		for (i = 0; i < dd->num_pports; i++) {
			struct hfi2_ibport *ibp = &dd->pport[i].ibport_data;

			hfi2_sys_guid_chg(ibp);
		}
	}

	ret = 0;

bail:
	return ret;
}

static int shut_down_port(struct rvt_dev_info *rdi, u32 port_num)
{
	struct hfi2_ibdev *verbs_dev = dev_from_rdi(rdi);
	struct hfi2_devdata *dd = dd_from_dev(verbs_dev);
	struct hfi2_pportdata *ppd = &dd->pport[port_num - 1];

	hfi2_set_link_down_reason(ppd, OPA_LINKDOWN_REASON_UNKNOWN, 0,
				  OPA_LINKDOWN_REASON_UNKNOWN);
	return hfi2_set_link_state(ppd, HLS_DN_DOWNDEF);
}

static int hfi2_get_guid_be(struct rvt_dev_info *rdi, struct rvt_ibport *rvp,
			    int guid_index, __be64 *guid)
{
	struct hfi2_ibport *ibp = container_of(rvp, struct hfi2_ibport, rvp);

	if (guid_index >= HFI2_GUIDS_PER_PORT)
		return -EINVAL;

	*guid = get_sguid(ibp, guid_index);
	return 0;
}

/*
 * convert ah port,sl to sc
 */
u8 hfi2_ah_to_sc(struct ib_device *ibdev, struct rdma_ah_attr *ah)
{
	struct hfi2_ibport *ibp = to_iport(ibdev, rdma_ah_get_port_num(ah));

	return ibp->sl_to_sc[rdma_ah_get_sl(ah)];
}

static int hfi2_check_ah(struct ib_device *ibdev, struct rdma_ah_attr *ah_attr)
{
	struct hfi2_ibport *ibp;
	struct hfi2_pportdata *ppd;
	u8 sc5;
	u8 sl;
	u8 vl;

	if (hfi2_check_mcast(rdma_ah_get_dlid(ah_attr)) &&
	    !(rdma_ah_get_ah_flags(ah_attr) & IB_AH_GRH))
		return -EINVAL;

	/* test the mapping for validity */
	ibp = to_iport(ibdev, rdma_ah_get_port_num(ah_attr));
	ppd = ppd_from_ibp(ibp);

	sl = rdma_ah_get_sl(ah_attr);
	if (sl >= ARRAY_SIZE(ibp->sl_to_sc))
		return -EINVAL;
	sl = array_index_nospec(sl, ARRAY_SIZE(ibp->sl_to_sc));

	sc5 = ibp->sl_to_sc[sl];
	vl = sc_to_vlt(ppd, sc5);
	if (vl > hfi2_num_vls && vl != 0xf)
		return -EINVAL;
	return 0;
}

static void hfi2_notify_new_ah(struct ib_device *ibdev,
			       struct rdma_ah_attr *ah_attr, struct rvt_ah *ah)
{
	struct hfi2_ibport *ibp;
	struct hfi2_pportdata *ppd;
	u8 sc5;
	struct rdma_ah_attr *attr = &ah->attr;

	/*
	 * Do not trust reading anything from rvt_ah at this point as it is not
	 * done being setup. We can however modify things which we need to set.
	 */

	ibp = to_iport(ibdev, rdma_ah_get_port_num(ah_attr));
	ppd = ppd_from_ibp(ibp);
	sc5 = ibp->sl_to_sc[rdma_ah_get_sl(&ah->attr)];
	hfi2_update_ah_attr(ibdev, attr);
	hfi2_make_opa_lid(attr);
	ah->vl = sc_to_vlt(ppd, sc5);
	if (ah->vl < hfi2_num_vls || ah->vl == 15)
		ah->log_pmtu = ilog2(ppd->vld[ah->vl].mtu);
}

/**
 * hfi2_get_npkeys - return the size of the PKEY table for context 0
 * @dd: the hfi2_ib device
 */
unsigned int hfi2_get_npkeys(struct hfi2_devdata *dd)
{
	return dd->params->pkey_table_size;
}

static void init_ibport(struct hfi2_pportdata *ppd)
{
	struct hfi2_ibport *ibp = &ppd->ibport_data;
	size_t sz = ARRAY_SIZE(ibp->sl_to_sc);
	int i;

	for (i = 0; i < sz; i++) {
		ibp->sl_to_sc[i] = i;
		ibp->sc_to_sl[i] = i;
	}

	for (i = 0; i < RVT_MAX_TRAP_LISTS; i++)
		INIT_LIST_HEAD(&ibp->rvp.trap_lists[i].list);
	timer_setup(&ibp->rvp.trap_timer, hfi2_handle_trap_timer, 0);

	spin_lock_init(&ibp->rvp.lock);
	/* Set the prefix to the default value (see ch. 4.1.1) */
	ibp->rvp.gid_prefix = IB_DEFAULT_GID_PREFIX;
	ibp->rvp.sm_lid = 0;
	/*
	 * Below should only set bits defined in OPA PortInfo.CapabilityMask
	 * and PortInfo.CapabilityMask3
	 */
	ibp->rvp.port_cap_flags = IB_PORT_AUTO_MIGR_SUP |
				  IB_PORT_CAP_MASK_NOTICE_SUP;
	ibp->rvp.port_cap3_flags = OPA_CAP_MASK3_IsSharedSpaceSupported;
	ibp->rvp.pma_counter_select[0] = IB_PMA_PORT_XMIT_DATA;
	ibp->rvp.pma_counter_select[1] = IB_PMA_PORT_RCV_DATA;
	ibp->rvp.pma_counter_select[2] = IB_PMA_PORT_XMIT_PKTS;
	ibp->rvp.pma_counter_select[3] = IB_PMA_PORT_RCV_PKTS;
	ibp->rvp.pma_counter_select[4] = IB_PMA_PORT_XMIT_WAIT;

	RCU_INIT_POINTER(ibp->rvp.qp[0], NULL);
	RCU_INIT_POINTER(ibp->rvp.qp[1], NULL);
}

/* ib_device_ops->get_dev_fw_str for wfr */
static void dc8051_get_dev_fw_str(struct ib_device *ibdev, char *str)
{
	struct rvt_dev_info *rdi = ib_to_rvt(ibdev);
	struct hfi2_ibdev *dev = dev_from_rdi(rdi);
	u32 ver = dd_from_dev(dev)->dc8051_ver;

	snprintf(str, IB_FW_VERSION_NAME_MAX, "%u.%u.%u", dc8051_ver_maj(ver),
		 dc8051_ver_min(ver), dc8051_ver_patch(ver));
}

/*
 * CPORT FW has defined the following mapping for WHO->VERSION_PATCH[4:7]
 * aka quality field
 */
static const char *const cport_ver_qlt_to_str_map[] = {
	"P0",  "P1",  "P2", /* 0-2: PowerOn[0-2] */
	"A0",  "A1",  "A2", /* 3-5: Alpha[0-2]   */
	"B0",  "B1",  "B2", /* 6-8: Beta[0-2]    */
	"RC0", "RC1", "RC2",
	"RC3", "RC4" /* 9-13: RC[0-4]     */
	/* 14: use patch field */
	/* 15: Reserved */
};

/* ib_device_ops->get_dev_fw_str for jkr and beyond */
void hfi2_cport_get_dev_fw_str(struct ib_device *ibdev, char *str)
{
	struct rvt_dev_info *rdi = ib_to_rvt(ibdev);
	struct hfi2_ibdev *dev = dev_from_rdi(rdi);
	u64 ver = dd_from_dev(dev)->cport_ver;
	u32 qlt = CPORT_FW_VER_QLT(ver);

	/* Does quality map to a special substring or do we just use patch */
	if (qlt < ARRAY_SIZE(cport_ver_qlt_to_str_map) && ver != 0)
		snprintf(str, IB_FW_VERSION_NAME_MAX, "%u.%u.%u.%s.%u",
			 CPORT_FW_VER_MAJ(ver), CPORT_FW_VER_MIN(ver),
			 CPORT_FW_VER_MNT(ver), cport_ver_qlt_to_str_map[qlt],
			 CPORT_FW_VER_BLD(ver));
	else
		snprintf(str, IB_FW_VERSION_NAME_MAX, "%u.%u.%u.%u.%u",
			 CPORT_FW_VER_MAJ(ver), CPORT_FW_VER_MIN(ver),
			 CPORT_FW_VER_MNT(ver), CPORT_FW_VER_PAT(ver),
			 CPORT_FW_VER_BLD(ver));
}

static const char *const driver_cntr_names[] = {
	/* must be element 0*/
	"DRIVER_KernIntr",  "DRIVER_ErrorIntr",	  "DRIVER_Tx_Errs",
	"DRIVER_Rcv_Errs",  "DRIVER_HW_Errs",	  "DRIVER_NoPIOBufs",
	"DRIVER_CtxtsOpen", "DRIVER_RcvLen_Errs", "DRIVER_EgrBufFull",
	"DRIVER_EgrHdrFull"
};

int hfi2_num_driver_cntrs = ARRAY_SIZE(driver_cntr_names);

/*
 * Convert a list of names separated by '\n' into an array of NULL terminated
 * strings. Optionally some entries can be reserved in the array to hold extra
 * external strings.
 */
static int init_cntr_names(const char *names_in, const size_t names_len,
			   int num_extra_names, int *num_cntrs,
			   struct rdma_stat_desc **cntr_descs)
{
	struct rdma_stat_desc *names_out;
	char *p;
	int i, n;

	n = 0;
	for (i = 0; i < names_len; i++)
		if (names_in[i] == '\n')
			n++;

	names_out =
		kzalloc((n + num_extra_names) * sizeof(*names_out) + names_len,
			GFP_KERNEL);
	if (!names_out) {
		*num_cntrs = 0;
		*cntr_descs = NULL;
		return -ENOMEM;
	}

	p = (char *)&names_out[n + num_extra_names];
	memcpy(p, names_in, names_len);

	for (i = 0; i < n; i++) {
		names_out[i].name = p;
		p = strchr(p, '\n');
		*p++ = '\0';
	}

	*num_cntrs = n;
	*cntr_descs = names_out;
	return 0;
}

static struct rdma_hw_stats *hfi2_alloc_hw_device_stats(struct ib_device *ibdev)
{
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);

	return rdma_alloc_hw_stats_struct(
		dd->dev_cntr_descs, dd->num_dev_cntrs + hfi2_num_driver_cntrs,
		RDMA_HW_STATS_DEFAULT_LIFESPAN);
}

static struct rdma_hw_stats *hfi_alloc_hw_port_stats(struct ib_device *ibdev,
						     u32 port_num)
{
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);

	return rdma_alloc_hw_stats_struct(dd->port_cntr_descs,
					  dd->num_port_cntrs,
					  RDMA_HW_STATS_DEFAULT_LIFESPAN);
}

static u64 hfi2_sps_ints(void)
{
	unsigned long index, flags;
	struct hfi2_devdata *dd;
	u64 sps_ints = 0;

	xa_lock_irqsave(&hfi2_dev_table, flags);
	xa_for_each(&hfi2_dev_table, index, dd) {
		sps_ints += hfi2_get_all_cpu_total(dd->int_counter);
	}
	xa_unlock_irqrestore(&hfi2_dev_table, flags);
	return sps_ints;
}

static int get_hw_stats(struct ib_device *ibdev, struct rdma_hw_stats *stats,
			u32 port, int index)
{
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	u64 *values;
	int count;

	if (!port) {
		u64 *stats = (u64 *)&hfi2_stats;
		int i;

		hfi2_read_cntrs(dd, NULL, &values);
		values[dd->num_dev_cntrs] = hfi2_sps_ints();
		for (i = 1; i < hfi2_num_driver_cntrs; i++)
			values[dd->num_dev_cntrs + i] = stats[i];
		count = dd->num_dev_cntrs + hfi2_num_driver_cntrs;
	} else {
		struct hfi2_ibport *ibp = to_iport(ibdev, port);

		hfi2_read_portcntrs(ppd_from_ibp(ibp), NULL, &values);
		count = dd->num_port_cntrs;
	}

	memcpy(stats->value, values, count * sizeof(u64));
	return count;
}

static const struct ib_device_ops hfi2_dev_ops = {
	.owner = THIS_MODULE,
	.driver_id = RDMA_DRIVER_HFI2,
	.uverbs_abi_ver = HFI2_UVERBS_ABI_VERSION,

	.alloc_hw_device_stats = hfi2_alloc_hw_device_stats,
	.alloc_hw_port_stats = hfi_alloc_hw_port_stats,
	.device_group = &hfi2_ib_hfi2_attr_group,
	.get_dev_fw_str = dc8051_get_dev_fw_str,
	.get_hw_stats = get_hw_stats,
	.modify_device = modify_device,
	.port_groups = hfi2_wfr_attr_port_groups,
	/* keep process mad in the driver */
	.process_mad = hfi2_process_mad,
	.rdma_netdev_get_params = hfi2_ipoib_rn_get_params,
};

static const struct ib_device_ops cport_dev_ops = {
	.owner = THIS_MODULE,
	.driver_id = RDMA_DRIVER_HFI2,
	.uverbs_abi_ver = HFI2_UVERBS_ABI_VERSION,

	.alloc_hw_device_stats = hfi2_alloc_hw_device_stats,
	.alloc_hw_port_stats = hfi_alloc_hw_port_stats,
	.device_group = &hfi2_ib_hfi2_attr_group,
	.get_dev_fw_str = hfi2_cport_get_dev_fw_str,
	.get_hw_stats = get_hw_stats,
	.modify_device = modify_device,
	.port_groups = hfi2_cport_attr_port_groups,
	/* keep process mad in the driver */
	.process_mad = hfi2_cport_process_mad,
	.rdma_netdev_get_params = hfi2_ipoib_rn_get_params,
};

static const struct ib_device_ops vf_dev_ops = {
	.owner = THIS_MODULE,
	.driver_id = RDMA_DRIVER_HFI2,
	.uverbs_abi_ver = HFI2_UVERBS_ABI_VERSION,

	.alloc_hw_device_stats = hfi2_alloc_hw_device_stats,
	.alloc_hw_port_stats = hfi_alloc_hw_port_stats,
	.device_group = &hfi2_ib_hfi2_attr_group,
	.get_dev_fw_str = hfi2_cport_get_dev_fw_str,
	.get_hw_stats = get_hw_stats,
	.modify_device = modify_device,
	.port_groups = hfi2_cport_attr_port_groups,
	/* keep process mad in the driver */
	.process_mad = hfi2_vf_process_mad,
	.rdma_netdev_get_params = hfi2_ipoib_rn_get_params,
};

/**
 * hfi2_register_ib_device - register our device with the infiniband core
 * @dd: the device data structure
 * Return 0 if successful, errno if unsuccessful.
 */
int hfi2_register_ib_device(struct hfi2_devdata *dd)
{
	struct hfi2_ibdev *dev = &dd->verbs_dev;
	struct ib_device *ibdev = &dev->rdi.ibdev;
	struct hfi2_pportdata *ppd;
	unsigned int i;
	int ret;
	u8 max_qos_shift;

	ret = init_cntr_names(dd->cntrnames, dd->cntrnameslen,
			      hfi2_num_driver_cntrs, &dd->num_dev_cntrs,
			      &dd->dev_cntr_descs);
	if (ret)
		goto err_cntr_descs;
	for (i = 0; i < hfi2_num_driver_cntrs; i++)
		dd->dev_cntr_descs[dd->num_dev_cntrs + i].name =
			driver_cntr_names[i];
	ret = init_cntr_names(dd->portcntrnames, dd->portcntrnameslen, 0,
			      &dd->num_port_cntrs, &dd->port_cntr_descs);
	if (ret)
		goto err_cntr_descs;

	/* rdmavt has only a single QPN space - use the largest QOS shift */
	max_qos_shift = 1; /* always shift by at least 1 */
	for (i = 0, ppd = dd->pport; i < dd->num_pports; i++, ppd++) {
		init_ibport(ppd);
		max_qos_shift = max_t(u8, max_qos_shift, ppd->qos_shift);
	}

	/* Only need to initialize non-zero fields. */

	timer_setup(&dev->mem_timer, mem_timer, 0);

	seqlock_init(&dev->iowait_lock);
	seqlock_init(&dev->txwait_lock);
	INIT_LIST_HEAD(&dev->txwait);
	INIT_LIST_HEAD(&dev->memwait);

	ret = hfi2_verbs_txreq_init(dev);
	if (ret)
		goto err_verbs_txreq;

	/* Use first-port GUID as node guid */
	ibdev->node_guid = get_sguid(&dd->pport[HFI2_PORT_IDX].ibport_data,
				     HFI2_PORT_GUID_INDEX);

	/*
	 * The system image GUID is supposed to be the same for all
	 * HFIs in a single system but since there can be other
	 * device types in the system, we can't be sure this is unique.
	 */
	if (!hfi2_ib_hfi2_sys_image_guid)
		hfi2_ib_hfi2_sys_image_guid = ibdev->node_guid;
	ibdev->phys_port_cnt = dd->num_pports;
	ibdev->dev.parent = &dd->pcidev->dev;

	if (dd->cport)
		ib_set_device_ops(ibdev, &cport_dev_ops);
	else if (dd->is_vf)
		ib_set_device_ops(ibdev, &vf_dev_ops);
	else
		ib_set_device_ops(ibdev, &hfi2_dev_ops);

	strscpy(ibdev->node_desc, init_utsname()->nodename,
		sizeof(ibdev->node_desc));

	if (IS_ENABLED(CONFIG_INFINIBAND_USER_ACCESS))
		ibdev->driver_def = hfi2_ib_defs;

	/*
	 * Fill in rvt info object.
	 */
	dd->verbs_dev.rdi.driver_f.get_pci_dev = hfi2_get_pci_dev;
	dd->verbs_dev.rdi.driver_f.check_ah = hfi2_check_ah;
	dd->verbs_dev.rdi.driver_f.notify_new_ah = hfi2_notify_new_ah;
	dd->verbs_dev.rdi.driver_f.get_guid_be = hfi2_get_guid_be;
	dd->verbs_dev.rdi.driver_f.query_port_state = query_port;
	dd->verbs_dev.rdi.driver_f.shut_down_port = shut_down_port;
	dd->verbs_dev.rdi.driver_f.cap_mask_chg = hfi2_cap_mask_chg;
	/*
	 * Fill in rvt info device attributes.
	 */
	hfi2_fill_device_attr(dd);

	/* queue pair */
	dd->verbs_dev.rdi.dparms.qp_table_size = hfi2_qp_table_size;
	dd->verbs_dev.rdi.dparms.qpn_start = 0;
	dd->verbs_dev.rdi.dparms.qpn_inc = 1;
	dd->verbs_dev.rdi.dparms.qos_shift = max_qos_shift;
	dd->verbs_dev.rdi.dparms.qpn_res_start = RVT_KDETH_QP_BASE;
	dd->verbs_dev.rdi.dparms.qpn_res_end = RVT_AIP_QP_MAX;
	dd->verbs_dev.rdi.dparms.max_rdma_atomic = HFI2_MAX_RDMA_ATOMIC;
	dd->verbs_dev.rdi.dparms.psn_mask = PSN_MASK;
	dd->verbs_dev.rdi.dparms.psn_shift = PSN_SHIFT;
	dd->verbs_dev.rdi.dparms.psn_modify_mask = PSN_MODIFY_MASK;
	dd->verbs_dev.rdi.dparms.core_cap_flags = RDMA_CORE_PORT_INTEL_OPA |
						  RDMA_CORE_CAP_OPA_AH;
	dd->verbs_dev.rdi.dparms.max_mad_size = OPA_MGMT_MAD_SIZE;

	if (dd->is_sriov) {
		dd->verbs_dev.rdi.driver_f.alloc_qpn = hfi2_sriov_alloc_qpn;
		dd->verbs_dev.rdi.dparms.qpn_start =
			(dd->rsrcs.c.first_rcv_context << 1) -
			(1 << max_qos_shift);
	}
	dd->verbs_dev.rdi.driver_f.qp_priv_alloc = hfi2_qp_priv_alloc;
	dd->verbs_dev.rdi.driver_f.qp_priv_init = hfi2_qp_priv_init;
	dd->verbs_dev.rdi.driver_f.qp_priv_free = hfi2_qp_priv_free;
	dd->verbs_dev.rdi.driver_f.free_all_qps = hfi2_free_all_qps;
	dd->verbs_dev.rdi.driver_f.notify_qp_reset = hfi2_notify_qp_reset;
	dd->verbs_dev.rdi.driver_f.do_send = hfi2_do_send_from_rvt;
	dd->verbs_dev.rdi.driver_f.schedule_send = hfi2_schedule_send;
	dd->verbs_dev.rdi.driver_f.schedule_send_no_lock = _hfi2_schedule_send;
	dd->verbs_dev.rdi.driver_f.get_pmtu_from_attr = hfi2_get_pmtu_from_attr;
	dd->verbs_dev.rdi.driver_f.notify_error_qp = hfi2_notify_error_qp;
	dd->verbs_dev.rdi.driver_f.flush_qp_waiters = hfi2_flush_qp_waiters;
	dd->verbs_dev.rdi.driver_f.stop_send_queue = hfi2_stop_send_queue;
	dd->verbs_dev.rdi.driver_f.quiesce_qp = hfi2_quiesce_qp;
	dd->verbs_dev.rdi.driver_f.mtu_from_qp = hfi2_mtu_from_qp;
	dd->verbs_dev.rdi.driver_f.mtu_to_path_mtu = hfi2_mtu_to_path_mtu;
	dd->verbs_dev.rdi.driver_f.check_modify_qp = hfi2_check_modify_qp;
	dd->verbs_dev.rdi.driver_f.modify_qp = hfi2_modify_qp;
	dd->verbs_dev.rdi.driver_f.notify_restart_rc = hfi2_restart_rc;
	dd->verbs_dev.rdi.driver_f.setup_wqe = hfi2_setup_wqe;
	dd->verbs_dev.rdi.driver_f.comp_vect_cpu_lookup =
		hfi2_comp_vect_mappings_lookup;
	dd->verbs_dev.rdi.driver_f.alloc_ucontext = hfi2_alloc_ucontext;
	dd->verbs_dev.rdi.driver_f.dealloc_ucontext = hfi2_dealloc_ucontext;
	dd->verbs_dev.rdi.driver_f.mmap = hfi2_rdma_mmap;

	/* completeion queue */
	dd->verbs_dev.rdi.ibdev.num_comp_vectors = dd->comp_vect_possible_cpus;
	dd->verbs_dev.rdi.dparms.node = dd->node;

	/* misc settings */
	dd->verbs_dev.rdi.flags = 0; /* Let rdmavt handle it all */
	dd->verbs_dev.rdi.dparms.lkey_table_size = hfi2_lkey_table_size;
	dd->verbs_dev.rdi.dparms.nports = dd->num_pports;
	dd->verbs_dev.rdi.dparms.npkeys = hfi2_get_npkeys(dd);
	dd->verbs_dev.rdi.dparms.sge_copy_mode = sge_copy_mode;
	dd->verbs_dev.rdi.dparms.wss_threshold = wss_threshold;
	dd->verbs_dev.rdi.dparms.wss_clean_period = wss_clean_period;
	dd->verbs_dev.rdi.dparms.reserved_operations = 1;
	dd->verbs_dev.rdi.dparms.extra_rdma_atomic = HFI2_TID_RDMA_WRITE_CNT;

	/* post send table */
	dd->verbs_dev.rdi.post_parms = hfi2_post_parms;

	/* opcode translation table */
	dd->verbs_dev.rdi.wc_opcode = hfi2_ib_hfi2_wc_opcode;

	for (i = 0, ppd = dd->pport; i < dd->num_pports; i++, ppd++)
		rvt_init_port(&dd->verbs_dev.rdi, &ppd->ibport_data.rvp, i,
			      ppd->pkeys);

	ret = rvt_register_device(&dd->verbs_dev.rdi);
	if (ret)
		goto err_verbs_txreq;

	ret = hfi2_verbs_register_sysfs(dd);
	if (ret)
		goto err_class;

	return ret;

err_class:
	rvt_unregister_device(&dd->verbs_dev.rdi);
err_verbs_txreq:
	hfi2_verbs_txreq_exit(dev);
err_cntr_descs:
	kfree(dd->port_cntr_descs);
	kfree(dd->dev_cntr_descs);
	dd_dev_err(dd, "cannot register verbs: %d!\n", -ret);
	return ret;
}

void hfi2_unregister_ib_device(struct hfi2_devdata *dd)
{
	struct hfi2_ibdev *dev = &dd->verbs_dev;

	hfi2_verbs_unregister_sysfs(dd);

	rvt_unregister_device(&dd->verbs_dev.rdi);

	if (!list_empty(&dev->txwait))
		dd_dev_err(dd, "txwait list not empty!\n");
	if (!list_empty(&dev->memwait))
		dd_dev_err(dd, "memwait list not empty!\n");

	timer_shutdown_sync(&dev->mem_timer);
	hfi2_verbs_txreq_exit(dev);

	kfree(dd->dev_cntr_descs);
	kfree(dd->port_cntr_descs);
}

void hfi2_cnp_rcv(struct hfi2_packet *packet)
{
	struct hfi2_ibport *ibp = rcd_to_iport(packet->rcd);
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);
	struct rvt_qp *qp = packet->qp;
	u32 lqpn, rqpn = 0;
	u16 rlid = 0;
	u8 sl, svc_type;

	switch (packet->qp->ibqp.qp_type) {
	case IB_QPT_UC:
		rlid = rdma_ah_get_dlid(&qp->remote_ah_attr);
		rqpn = qp->remote_qpn;
		svc_type = IB_CC_SVCTYPE_UC;
		break;
	case IB_QPT_RC:
		rlid = rdma_ah_get_dlid(&qp->remote_ah_attr);
		rqpn = qp->remote_qpn;
		svc_type = IB_CC_SVCTYPE_RC;
		break;
	case IB_QPT_SMI:
	case IB_QPT_GSI:
	case IB_QPT_UD:
		svc_type = IB_CC_SVCTYPE_UD;
		break;
	default:
		ibp->rvp.n_pkt_drops++;
		return;
	}

	sl = ibp->sc_to_sl[packet->sc];
	lqpn = qp->ibqp.qp_num;

	hfi2_process_becn(ppd, sl, rlid, lqpn, rqpn, svc_type);
}
