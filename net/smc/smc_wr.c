// SPDX-License-Identifier: GPL-2.0
/*
 * Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 * Work Requests exploiting Infiniband API
 *
 * Work requests (WR) of type ib_post_send or ib_post_recv respectively
 * are submitted to either RC SQ or RC RQ respectively
 * (reliably connected send/receive queue)
 * and become work queue entries (WQEs).
 * While an SQ WR/WQE is pending, we track it until transmission completion.
 * Through a send or receive completion queue (CQ) respectively,
 * we get completion queue entries (CQEs) [aka work completions (WCs)].
 * Since the CQ callback is called from IRQ context, we split work by using
 * bottom halves implemented by tasklets.
 *
 * SMC uses this to exchange LLC (link layer control)
 * and CDC (connection data control) messages.
 *
 * Copyright IBM Corp. 2016
 *
 * Author(s):  Steffen Maier <maier@linux.vnet.ibm.com>
 */

#include <linux/atomic.h>
#include <linux/hashtable.h>
#include <linux/wait.h>
#include <rdma/ib_verbs.h>
#include <asm/div64.h>

#include "smc.h"
#include "smc_wr.h"

#define SMC_WR_RX_HASH_BITS 4
static DEFINE_HASHTABLE(smc_wr_rx_hash, SMC_WR_RX_HASH_BITS);
static DEFINE_SPINLOCK(smc_wr_rx_hash_lock);

struct smc_wr_tx_pend {	/* control data for a pending send request */
	smc_wr_tx_handler	handler;
	enum ib_wc_status	wc_status;	/* CQE status */
	struct smc_link		*link;
	u32			idx;
	struct smc_wr_tx_pend_priv priv;
	u8			compl_requested;
};

/******************************** send queue *********************************/

/*------------------------------- completion --------------------------------*/

/* returns true if at least one tx work request is pending on the given link */
static inline bool smc_wr_is_tx_pend(struct smc_link *link)
{
	return !bitmap_empty(link->wr_tx_mask, link->wr_tx_cnt);
}

/* wait till all pending tx work requests on the given link are completed */
void smc_wr_tx_wait_no_pending_sends(struct smc_link *link)
{
	wait_event(link->wr_tx_wait, !smc_wr_is_tx_pend(link));
}

static void smc_wr_tx_rdma_process_cqe(struct ib_cq *cq, struct ib_wc *wc)
{
	struct smc_link *link = wc->qp->qp_context;

	/* terminate link */
	if (wc->status)
		smcr_link_down_cond_sched(link);
}

static void smc_wr_reg_process_cqe(struct ib_cq *cq, struct ib_wc *wc)
{
	struct smc_link *link = wc->qp->qp_context;

	if (wc->status)
		link->wr_reg_state = FAILED;
	else
		link->wr_reg_state = CONFIRMED;
	smc_wr_wakeup_reg_wait(link);
}

static void smc_wr_tx_process_cqe(struct ib_cq *cq, struct ib_wc *wc)
{
	struct smc_wr_tx_pend *tx_pend, pnd_snd;
	struct smc_ib_send_wr *send_wr;
	struct smc_link *link;
	u32 pnd_snd_idx;

	/* ib_drain_qp() is called before link free, so link is safe here */
	link = wc->qp->qp_context;

	send_wr = container_of(wc->wr_cqe, struct smc_ib_send_wr, cqe);
	pnd_snd_idx = send_wr->idx;

	tx_pend = (pnd_snd_idx == link->wr_tx_cnt) ? link->wr_tx_v2_pend :
		&link->wr_tx_pends[pnd_snd_idx];

	tx_pend->wc_status = wc->status;
	memcpy(&pnd_snd, tx_pend, sizeof(pnd_snd));
	/* clear the full struct smc_wr_tx_pend including .priv */
	memset(tx_pend, 0, sizeof(*tx_pend));

	if (pnd_snd_idx == link->wr_tx_cnt) {
		memset(link->lgr->wr_tx_buf_v2, 0,
		       sizeof(*link->lgr->wr_tx_buf_v2));
	} else {
		if (pnd_snd.compl_requested)
			complete(&link->wr_tx_compl[pnd_snd_idx]);
		memset(&link->wr_tx_bufs[pnd_snd_idx], 0,
		       sizeof(link->wr_tx_bufs[pnd_snd_idx]));
		if (!test_and_clear_bit(pnd_snd_idx, link->wr_tx_mask))
			return;
	}

	/* terminate link */
	if (wc->status)
		smcr_link_down_cond_sched(link);
	if (pnd_snd.handler)
		pnd_snd.handler(&pnd_snd.priv, link, wc->status);
	wake_up(&link->wr_tx_wait);
}

/*---------------------------- request submission ---------------------------*/

static inline int smc_wr_tx_get_free_slot_index(struct smc_link *link, u32 *idx)
{
	*idx = link->wr_tx_cnt;
	if (!smc_link_sendable(link))
		return -ENOLINK;
	for_each_clear_bit(*idx, link->wr_tx_mask, link->wr_tx_cnt) {
		if (!test_and_set_bit(*idx, link->wr_tx_mask))
			return 0;
	}
	*idx = link->wr_tx_cnt;
	return -EBUSY;
}

/**
 * smc_wr_tx_get_free_slot() - returns buffer for message assembly,
 *			and sets info for pending transmit tracking
 * @link:		Pointer to smc_link used to later send the message.
 * @handler:		Send completion handler function pointer.
 * @wr_buf:		Out value returns pointer to message buffer.
 * @wr_rdma_buf:	Out value returns pointer to rdma work request.
 * @wr_pend_priv:	Out value returns pointer serving as handler context.
 *
 * Return: 0 on success, or -errno on error.
 */
int smc_wr_tx_get_free_slot(struct smc_link *link,
			    smc_wr_tx_handler handler,
			    struct smc_wr_buf **wr_buf,
			    struct smc_rdma_wr **wr_rdma_buf,
			    struct smc_wr_tx_pend_priv **wr_pend_priv)
{
	struct smc_link_group *lgr = smc_get_lgr(link);
	struct smc_wr_tx_pend *wr_pend;
	u32 idx = link->wr_tx_cnt;
	int rc;

	*wr_buf = NULL;
	*wr_pend_priv = NULL;
	if (in_softirq() || lgr->terminating) {
		rc = smc_wr_tx_get_free_slot_index(link, &idx);
		if (rc)
			return rc;
	} else {
		rc = wait_event_interruptible_timeout(
			link->wr_tx_wait,
			!smc_link_sendable(link) ||
			lgr->terminating ||
			(smc_wr_tx_get_free_slot_index(link, &idx) != -EBUSY),
			SMC_WR_TX_WAIT_FREE_SLOT_TIME);
		if (!rc) {
			/* timeout - terminate link */
			smcr_link_down_cond_sched(link);
			return -EPIPE;
		}
		if (idx == link->wr_tx_cnt)
			return -EPIPE;
	}
	wr_pend = &link->wr_tx_pends[idx];
	wr_pend->handler = handler;
	wr_pend->link = link;
	wr_pend->idx = idx;
	*wr_buf = &link->wr_tx_bufs[idx];
	if (wr_rdma_buf)
		*wr_rdma_buf = &link->wr_tx_rdmas[idx];
	*wr_pend_priv = &wr_pend->priv;
	return 0;
}

int smc_wr_tx_get_v2_slot(struct smc_link *link,
			  smc_wr_tx_handler handler,
			  struct smc_wr_v2_buf **wr_buf,
			  struct smc_wr_tx_pend_priv **wr_pend_priv)
{
	struct smc_wr_tx_pend *wr_pend;

	if (link->wr_tx_v2_pend->idx == link->wr_tx_cnt)
		return -EBUSY;

	*wr_buf = NULL;
	*wr_pend_priv = NULL;
	wr_pend = link->wr_tx_v2_pend;
	wr_pend->handler = handler;
	wr_pend->link = link;
	wr_pend->idx = link->wr_tx_cnt;
	*wr_buf = link->lgr->wr_tx_buf_v2;
	*wr_pend_priv = &wr_pend->priv;
	return 0;
}

int smc_wr_tx_put_slot(struct smc_link *link,
		       struct smc_wr_tx_pend_priv *wr_pend_priv)
{
	struct smc_wr_tx_pend *pend;

	pend = container_of(wr_pend_priv, struct smc_wr_tx_pend, priv);
	if (pend->idx < link->wr_tx_cnt) {
		u32 idx = pend->idx;

		/* clear the full struct smc_wr_tx_pend including .priv */
		memset(&link->wr_tx_pends[idx], 0,
		       sizeof(link->wr_tx_pends[idx]));
		memset(&link->wr_tx_bufs[idx], 0,
		       sizeof(link->wr_tx_bufs[idx]));
		test_and_clear_bit(idx, link->wr_tx_mask);
		wake_up(&link->wr_tx_wait);
		return 1;
	} else if (link->lgr->smc_version == SMC_V2 &&
		   pend->idx == link->wr_tx_cnt) {
		/* Large v2 buffer */
		memset(link->wr_tx_v2_pend, 0,
		       sizeof(*link->wr_tx_v2_pend));
		memset(link->lgr->wr_tx_buf_v2, 0,
		       sizeof(*link->lgr->wr_tx_buf_v2));
		return 1;
	}

	return 0;
}

/* Send prepared WR slot via ib_post_send.
 * @priv: pointer to smc_wr_tx_pend_priv identifying prepared message buffer
 */
int smc_wr_tx_send(struct smc_link *link, struct smc_wr_tx_pend_priv *priv)
{
	struct smc_wr_tx_pend *pend;
	int rc;

	pend = container_of(priv, struct smc_wr_tx_pend, priv);
	rc = ib_post_send(link->roce_qp, &link->wr_tx_ibs[pend->idx].wr, NULL);
	if (rc) {
		smc_wr_tx_put_slot(link, priv);
		smcr_link_down_cond_sched(link);
	}
	return rc;
}

int smc_wr_tx_v2_send(struct smc_link *link, struct smc_wr_tx_pend_priv *priv,
		      int len)
{
	int rc;

	link->wr_tx_v2_ib->wr.sg_list[0].length = len;
	rc = ib_post_send(link->roce_qp, &link->wr_tx_v2_ib->wr, NULL);
	if (rc) {
		smc_wr_tx_put_slot(link, priv);
		smcr_link_down_cond_sched(link);
	}
	return rc;
}

/* Send prepared WR slot via ib_post_send and wait for send completion
 * notification.
 * @priv: pointer to smc_wr_tx_pend_priv identifying prepared message buffer
 */
int smc_wr_tx_send_wait(struct smc_link *link, struct smc_wr_tx_pend_priv *priv,
			unsigned long timeout)
{
	struct smc_wr_tx_pend *pend;
	u32 pnd_idx;
	int rc;

	pend = container_of(priv, struct smc_wr_tx_pend, priv);
	pend->compl_requested = 1;
	pnd_idx = pend->idx;
	init_completion(&link->wr_tx_compl[pnd_idx]);

	rc = smc_wr_tx_send(link, priv);
	if (rc)
		return rc;
	/* wait for completion by smc_wr_tx_process_cqe() */
	rc = wait_for_completion_interruptible_timeout(
					&link->wr_tx_compl[pnd_idx], timeout);
	if (rc <= 0)
		rc = -ENODATA;
	if (rc > 0)
		rc = 0;
	return rc;
}

/* Register a memory region and wait for result. */
int smc_wr_reg_send(struct smc_link *link, struct ib_mr *mr)
{
	int rc;

	link->wr_reg_state = POSTED;
	link->wr_reg.mr = mr;
	link->wr_reg.key = mr->rkey;
	rc = ib_post_send(link->roce_qp, &link->wr_reg.wr, NULL);
	if (rc)
		return rc;

	if (!percpu_ref_tryget_live(&link->wr_reg_refs))
		return -EPIPE;

	rc = wait_event_interruptible_timeout(link->wr_reg_wait,
					      (link->wr_reg_state != POSTED),
					      SMC_WR_REG_MR_WAIT_TIME);
	percpu_ref_put(&link->wr_reg_refs);
	if (!rc) {
		/* timeout - terminate link */
		smcr_link_down_cond_sched(link);
		return -EPIPE;
	}
	if (rc == -ERESTARTSYS)
		return -EINTR;
	switch (link->wr_reg_state) {
	case CONFIRMED:
		rc = 0;
		break;
	case FAILED:
		rc = -EIO;
		break;
	case POSTED:
		rc = -EPIPE;
		break;
	}
	return rc;
}

/****************************** receive queue ********************************/

int smc_wr_rx_register_handler(struct smc_wr_rx_handler *handler)
{
	struct smc_wr_rx_handler *h_iter;
	int rc = 0;

	spin_lock(&smc_wr_rx_hash_lock);
	hash_for_each_possible(smc_wr_rx_hash, h_iter, list, handler->type) {
		if (h_iter->type == handler->type) {
			rc = -EEXIST;
			goto out_unlock;
		}
	}
	hash_add(smc_wr_rx_hash, &handler->list, handler->type);
out_unlock:
	spin_unlock(&smc_wr_rx_hash_lock);
	return rc;
}

/* Demultiplex a received work request based on the message type to its handler.
 * Relies on smc_wr_rx_hash having been completely filled before any IB WRs,
 * and not being modified any more afterwards so we don't need to lock it.
 */
static inline void smc_wr_rx_demultiplex(struct ib_wc *wc)
{
	struct smc_link *link = (struct smc_link *)wc->qp->qp_context;
	struct smc_wr_rx_handler *handler;
	struct smc_ib_recv_wr *recv_wr;
	struct smc_wr_rx_hdr *wr_rx;

	if (wc->byte_len < sizeof(*wr_rx))
		return; /* short message */

	recv_wr = container_of(wc->wr_cqe, struct smc_ib_recv_wr, cqe);

	wr_rx = (struct smc_wr_rx_hdr *)(link->wr_rx_bufs + recv_wr->idx * link->wr_rx_buflen);
	hash_for_each_possible(smc_wr_rx_hash, handler, list, wr_rx->type) {
		if (handler->type == wr_rx->type)
			handler->handler(wc, wr_rx);
	}
}

/* Repost a receive WR unless the link is being torn down. Holding wr_rx_refs
 * across the post serializes against smc_wr_stop_link(), which kills the ref and
 * waits before ib_drain_qp() posts its drain WR. This guarantees no recv WR is
 * posted after the drain WR, preventing a use-after-free of wr_rx_ibs when
 * flush CQEs are polled after teardown.
 */
static void smc_wr_rx_refill(struct smc_link *link, struct ib_cqe *cqe)
{
	if (!percpu_ref_tryget_live(&link->wr_rx_refs))
		return;
	smc_wr_rx_post(link, cqe); /* refill WR RX */
	percpu_ref_put(&link->wr_rx_refs);
}

static void smc_wr_rx_process_cqe(struct ib_cq *cq, struct ib_wc *wc)
{
	struct smc_link *link = wc->qp->qp_context;

	if (wc->status == IB_WC_SUCCESS) {
		link->wr_rx_tstamp = jiffies;
		smc_wr_rx_demultiplex(wc);
		smc_wr_rx_refill(link, wc->wr_cqe);
	} else {
		/* handle status errors */
		switch (wc->status) {
		case IB_WC_RETRY_EXC_ERR:
		case IB_WC_RNR_RETRY_EXC_ERR:
		case IB_WC_WR_FLUSH_ERR:
			smcr_link_down_cond_sched(link);
			break;
		default:
			smc_wr_rx_refill(link, wc->wr_cqe);
			break;
		}
	}
}

void smc_wr_stop_link(struct smc_link *lnk)
{
	if (lnk->state == SMC_LNK_UNUSED)
		return;

	percpu_ref_kill(&lnk->wr_reg_refs);
	percpu_ref_kill(&lnk->wr_tx_refs);
	percpu_ref_kill(&lnk->wr_rx_refs);

	/* quick wakeup */
	smc_wr_wakeup_reg_wait(lnk);
	smc_wr_wakeup_tx_wait(lnk);

	wait_for_completion(&lnk->reg_ref_comp);
	wait_for_completion(&lnk->tx_ref_comp);
	wait_for_completion(&lnk->rx_ref_comp);
}

int smc_wr_rx_post_init(struct smc_link *link)
{
	int i, rc = 0;

	for (i = 0; i < link->wr_rx_cnt; i++)
		rc = smc_wr_rx_post(link, &link->wr_rx_ibs[i].cqe);
	return rc;
}

/***************************** init, exit, misc ******************************/

static void smc_wr_reg_init_cqe(struct ib_cqe *cqe)
{
	cqe->done = smc_wr_reg_process_cqe;
}

static void smc_wr_tx_init_cqe(struct ib_cqe *cqe)
{
	cqe->done = smc_wr_tx_process_cqe;
}

static void smc_wr_rx_init_cqe(struct ib_cqe *cqe)
{
	cqe->done = smc_wr_rx_process_cqe;
}

static void smc_wr_tx_rdma_init_cqe(struct ib_cqe *cqe)
{
	cqe->done = smc_wr_tx_rdma_process_cqe;
}

void smc_wr_remember_qp_attr(struct smc_link *lnk)
{
	struct ib_qp_attr *attr = &lnk->qp_attr;
	struct ib_qp_init_attr init_attr;

	memset(attr, 0, sizeof(*attr));
	memset(&init_attr, 0, sizeof(init_attr));
	ib_query_qp(lnk->roce_qp, attr,
		    IB_QP_STATE |
		    IB_QP_CUR_STATE |
		    IB_QP_PKEY_INDEX |
		    IB_QP_PORT |
		    IB_QP_QKEY |
		    IB_QP_AV |
		    IB_QP_PATH_MTU |
		    IB_QP_TIMEOUT |
		    IB_QP_RETRY_CNT |
		    IB_QP_RNR_RETRY |
		    IB_QP_RQ_PSN |
		    IB_QP_ALT_PATH |
		    IB_QP_MIN_RNR_TIMER |
		    IB_QP_SQ_PSN |
		    IB_QP_PATH_MIG_STATE |
		    IB_QP_CAP |
		    IB_QP_DEST_QPN,
		    &init_attr);

	lnk->wr_tx_cnt = min_t(size_t, lnk->max_send_wr,
			       lnk->qp_attr.cap.max_send_wr);
	lnk->wr_rx_cnt = min_t(size_t, lnk->max_recv_wr,
			       lnk->qp_attr.cap.max_recv_wr - 1);	/* -1 for ib_drain_rq() */
}

static void smc_wr_init_sge(struct smc_link *lnk)
{
	bool send_inline = (lnk->qp_attr.cap.max_inline_data > SMC_WR_TX_SIZE);
	u32 i;

	for (i = 0; i < lnk->wr_tx_cnt; i++) {
		lnk->wr_tx_sges[i].addr = send_inline ? (uintptr_t)(&lnk->wr_tx_bufs[i]) :
			lnk->wr_tx_dma_addr + i * SMC_WR_BUF_SIZE;
		lnk->wr_tx_sges[i].length = SMC_WR_TX_SIZE;
		lnk->wr_tx_sges[i].lkey = lnk->roce_pd->local_dma_lkey;
		lnk->wr_tx_rdma_sges[i].tx_rdma_sge[0].wr_tx_rdma_sge[0].lkey =
			lnk->roce_pd->local_dma_lkey;
		lnk->wr_tx_rdma_sges[i].tx_rdma_sge[0].wr_tx_rdma_sge[1].lkey =
			lnk->roce_pd->local_dma_lkey;
		lnk->wr_tx_rdma_sges[i].tx_rdma_sge[1].wr_tx_rdma_sge[0].lkey =
			lnk->roce_pd->local_dma_lkey;
		lnk->wr_tx_rdma_sges[i].tx_rdma_sge[1].wr_tx_rdma_sge[1].lkey =
			lnk->roce_pd->local_dma_lkey;
		lnk->wr_tx_ibs[i].wr.next = NULL;
		lnk->wr_tx_ibs[i].wr.sg_list = &lnk->wr_tx_sges[i];
		lnk->wr_tx_ibs[i].wr.num_sge = 1;
		lnk->wr_tx_ibs[i].wr.opcode = IB_WR_SEND;
		lnk->wr_tx_ibs[i].wr.send_flags =
			IB_SEND_SIGNALED | IB_SEND_SOLICITED;
		if (send_inline)
			lnk->wr_tx_ibs[i].wr.send_flags |= IB_SEND_INLINE;
		lnk->wr_tx_rdmas[i].wr_tx_rdma[0].wr.opcode = IB_WR_RDMA_WRITE;
		lnk->wr_tx_rdmas[i].wr_tx_rdma[1].wr.opcode = IB_WR_RDMA_WRITE;
		lnk->wr_tx_rdmas[i].wr_tx_rdma[0].wr.sg_list =
			lnk->wr_tx_rdma_sges[i].tx_rdma_sge[0].wr_tx_rdma_sge;
		lnk->wr_tx_rdmas[i].wr_tx_rdma[1].wr.sg_list =
			lnk->wr_tx_rdma_sges[i].tx_rdma_sge[1].wr_tx_rdma_sge;
	}

	if (lnk->lgr->smc_version == SMC_V2) {
		lnk->wr_tx_v2_sge->addr = lnk->wr_tx_v2_dma_addr;
		lnk->wr_tx_v2_sge->length = SMC_WR_BUF_V2_SIZE;
		lnk->wr_tx_v2_sge->lkey = lnk->roce_pd->local_dma_lkey;

		lnk->wr_tx_v2_ib->wr.next = NULL;
		lnk->wr_tx_v2_ib->wr.sg_list = lnk->wr_tx_v2_sge;
		lnk->wr_tx_v2_ib->wr.num_sge = 1;
		lnk->wr_tx_v2_ib->wr.opcode = IB_WR_SEND;
		lnk->wr_tx_v2_ib->wr.send_flags =
			IB_SEND_SIGNALED | IB_SEND_SOLICITED;
	}

	/* With SMC-Rv2 there can be messages larger than SMC_WR_TX_SIZE.
	 * Each ib_recv_wr gets 2 sges, the second one is a spillover buffer
	 * and the same buffer for all sges. When a larger message arrived then
	 * the content of the first small sge is copied to the beginning of
	 * the larger spillover buffer, allowing easy data mapping.
	 */
	for (i = 0; i < lnk->wr_rx_cnt; i++) {
		int x = i * lnk->wr_rx_sge_cnt;

		lnk->wr_rx_sges[x].addr =
			lnk->wr_rx_dma_addr + i * lnk->wr_rx_buflen;
		lnk->wr_rx_sges[x].length = smc_link_shared_v2_rxbuf(lnk) ?
			SMC_WR_TX_SIZE : lnk->wr_rx_buflen;
		lnk->wr_rx_sges[x].lkey = lnk->roce_pd->local_dma_lkey;
		if (lnk->lgr->smc_version == SMC_V2 && smc_link_shared_v2_rxbuf(lnk)) {
			lnk->wr_rx_sges[x + 1].addr =
					lnk->wr_rx_v2_dma_addr + SMC_WR_TX_SIZE;
			lnk->wr_rx_sges[x + 1].length =
					SMC_WR_BUF_V2_SIZE - SMC_WR_TX_SIZE;
			lnk->wr_rx_sges[x + 1].lkey =
					lnk->roce_pd->local_dma_lkey;
		}
		lnk->wr_rx_ibs[i].wr.next = NULL;
		lnk->wr_rx_ibs[i].wr.sg_list = &lnk->wr_rx_sges[x];
		lnk->wr_rx_ibs[i].wr.num_sge = lnk->wr_rx_sge_cnt;
	}

	lnk->wr_reg.wr.next = NULL;
	lnk->wr_reg.wr.num_sge = 0;
	lnk->wr_reg.wr.send_flags = IB_SEND_SIGNALED;
	lnk->wr_reg.wr.opcode = IB_WR_REG_MR;
	lnk->wr_reg.access = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_WRITE;
}

void smc_wr_free_link(struct smc_link *lnk)
{
	struct ib_device *ibdev;

	if (!lnk->smcibdev)
		return;
	ibdev = lnk->smcibdev->ibdev;

	percpu_ref_exit(&lnk->wr_reg_refs);
	percpu_ref_exit(&lnk->wr_tx_refs);
	percpu_ref_exit(&lnk->wr_rx_refs);

	if (lnk->wr_rx_dma_addr) {
		ib_dma_unmap_single(ibdev, lnk->wr_rx_dma_addr,
				    lnk->wr_rx_buflen * lnk->wr_rx_cnt,
				    DMA_FROM_DEVICE);
		lnk->wr_rx_dma_addr = 0;
	}
	if (lnk->wr_rx_v2_dma_addr) {
		ib_dma_unmap_single(ibdev, lnk->wr_rx_v2_dma_addr,
				    SMC_WR_BUF_V2_SIZE,
				    DMA_FROM_DEVICE);
		lnk->wr_rx_v2_dma_addr = 0;
	}
	if (lnk->wr_tx_dma_addr) {
		ib_dma_unmap_single(ibdev, lnk->wr_tx_dma_addr,
				    SMC_WR_BUF_SIZE * lnk->wr_tx_cnt,
				    DMA_TO_DEVICE);
		lnk->wr_tx_dma_addr = 0;
	}
	if (lnk->wr_tx_v2_dma_addr) {
		ib_dma_unmap_single(ibdev, lnk->wr_tx_v2_dma_addr,
				    SMC_WR_BUF_V2_SIZE,
				    DMA_TO_DEVICE);
		lnk->wr_tx_v2_dma_addr = 0;
	}
}

void smc_wr_free_lgr_mem(struct smc_link_group *lgr)
{
	if (lgr->smc_version < SMC_V2)
		return;

	kfree(lgr->wr_rx_buf_v2);
	lgr->wr_rx_buf_v2 = NULL;
	kfree(lgr->wr_tx_buf_v2);
	lgr->wr_tx_buf_v2 = NULL;
}

void smc_wr_free_link_mem(struct smc_link *lnk)
{
	kfree(lnk->wr_tx_v2_ib);
	lnk->wr_tx_v2_ib = NULL;
	kfree(lnk->wr_tx_v2_sge);
	lnk->wr_tx_v2_sge = NULL;
	kfree(lnk->wr_tx_v2_pend);
	lnk->wr_tx_v2_pend = NULL;
	kfree(lnk->wr_tx_compl);
	lnk->wr_tx_compl = NULL;
	kfree(lnk->wr_tx_pends);
	lnk->wr_tx_pends = NULL;
	bitmap_free(lnk->wr_tx_mask);
	lnk->wr_tx_mask = NULL;
	kfree(lnk->wr_tx_sges);
	lnk->wr_tx_sges = NULL;
	kfree(lnk->wr_tx_rdma_sges);
	lnk->wr_tx_rdma_sges = NULL;
	kfree(lnk->wr_rx_sges);
	lnk->wr_rx_sges = NULL;
	kfree(lnk->wr_tx_rdmas);
	lnk->wr_tx_rdmas = NULL;
	kfree(lnk->wr_rx_ibs);
	lnk->wr_rx_ibs = NULL;
	kfree(lnk->wr_tx_ibs);
	lnk->wr_tx_ibs = NULL;
	kfree(lnk->wr_tx_bufs);
	lnk->wr_tx_bufs = NULL;
	kfree(lnk->wr_rx_bufs);
	lnk->wr_rx_bufs = NULL;
}

int smc_wr_alloc_lgr_mem(struct smc_link_group *lgr)
{
	if (lgr->smc_version < SMC_V2)
		return 0;

	lgr->wr_rx_buf_v2 = kzalloc(SMC_WR_BUF_V2_SIZE, GFP_KERNEL);
	if (!lgr->wr_rx_buf_v2)
		return -ENOMEM;
	lgr->wr_tx_buf_v2 = kzalloc(SMC_WR_BUF_V2_SIZE, GFP_KERNEL);
	if (!lgr->wr_tx_buf_v2) {
		kfree(lgr->wr_rx_buf_v2);
		return -ENOMEM;
	}
	return 0;
}

int smc_wr_alloc_link_mem(struct smc_link *link)
{
	/* allocate link related memory */
	link->wr_tx_bufs = kcalloc(link->max_send_wr,
				   SMC_WR_BUF_SIZE, GFP_KERNEL);
	if (!link->wr_tx_bufs)
		goto no_mem;
	link->wr_rx_bufs = kcalloc(link->max_recv_wr, link->wr_rx_buflen,
				   GFP_KERNEL);
	if (!link->wr_rx_bufs)
		goto no_mem_wr_tx_bufs;
	link->wr_tx_ibs = kzalloc_objs(link->wr_tx_ibs[0], link->max_send_wr);
	if (!link->wr_tx_ibs)
		goto no_mem_wr_rx_bufs;
	link->wr_rx_ibs = kzalloc_objs(link->wr_rx_ibs[0], link->max_recv_wr);
	if (!link->wr_rx_ibs)
		goto no_mem_wr_tx_ibs;
	link->wr_tx_rdmas = kzalloc_objs(link->wr_tx_rdmas[0],
					 link->max_send_wr);
	if (!link->wr_tx_rdmas)
		goto no_mem_wr_rx_ibs;
	link->wr_tx_rdma_sges = kzalloc_objs(link->wr_tx_rdma_sges[0],
					     link->max_send_wr);
	if (!link->wr_tx_rdma_sges)
		goto no_mem_wr_tx_rdmas;
	link->wr_tx_sges = kzalloc_objs(link->wr_tx_sges[0], link->max_send_wr);
	if (!link->wr_tx_sges)
		goto no_mem_wr_tx_rdma_sges;
	link->wr_rx_sges = kcalloc(link->max_recv_wr,
				   sizeof(link->wr_rx_sges[0]) * link->wr_rx_sge_cnt,
				   GFP_KERNEL);
	if (!link->wr_rx_sges)
		goto no_mem_wr_tx_sges;
	link->wr_tx_mask = bitmap_zalloc(link->max_send_wr, GFP_KERNEL);
	if (!link->wr_tx_mask)
		goto no_mem_wr_rx_sges;
	link->wr_tx_pends = kzalloc_objs(link->wr_tx_pends[0],
					 link->max_send_wr);
	if (!link->wr_tx_pends)
		goto no_mem_wr_tx_mask;
	link->wr_tx_compl = kzalloc_objs(link->wr_tx_compl[0],
					 link->max_send_wr);
	if (!link->wr_tx_compl)
		goto no_mem_wr_tx_pends;

	if (link->lgr->smc_version == SMC_V2) {
		link->wr_tx_v2_ib = kzalloc_obj(*link->wr_tx_v2_ib);
		if (!link->wr_tx_v2_ib)
			goto no_mem_tx_compl;
		link->wr_tx_v2_sge = kzalloc_obj(*link->wr_tx_v2_sge);
		if (!link->wr_tx_v2_sge)
			goto no_mem_v2_ib;
		link->wr_tx_v2_pend = kzalloc_obj(*link->wr_tx_v2_pend);
		if (!link->wr_tx_v2_pend)
			goto no_mem_v2_sge;
	}
	return 0;

no_mem_v2_sge:
	kfree(link->wr_tx_v2_sge);
no_mem_v2_ib:
	kfree(link->wr_tx_v2_ib);
no_mem_tx_compl:
	kfree(link->wr_tx_compl);
no_mem_wr_tx_pends:
	kfree(link->wr_tx_pends);
no_mem_wr_tx_mask:
	kfree(link->wr_tx_mask);
no_mem_wr_rx_sges:
	kfree(link->wr_rx_sges);
no_mem_wr_tx_sges:
	kfree(link->wr_tx_sges);
no_mem_wr_tx_rdma_sges:
	kfree(link->wr_tx_rdma_sges);
no_mem_wr_tx_rdmas:
	kfree(link->wr_tx_rdmas);
no_mem_wr_rx_ibs:
	kfree(link->wr_rx_ibs);
no_mem_wr_tx_ibs:
	kfree(link->wr_tx_ibs);
no_mem_wr_rx_bufs:
	kfree(link->wr_rx_bufs);
no_mem_wr_tx_bufs:
	kfree(link->wr_tx_bufs);
no_mem:
	return -ENOMEM;
}

static void smcr_wr_tx_refs_free(struct percpu_ref *ref)
{
	struct smc_link *lnk = container_of(ref, struct smc_link, wr_tx_refs);

	complete(&lnk->tx_ref_comp);
}

static void smcr_wr_reg_refs_free(struct percpu_ref *ref)
{
	struct smc_link *lnk = container_of(ref, struct smc_link, wr_reg_refs);

	complete(&lnk->reg_ref_comp);
}

static void smcr_wr_rx_refs_free(struct percpu_ref *ref)
{
	struct smc_link *lnk = container_of(ref, struct smc_link, wr_rx_refs);

	complete(&lnk->rx_ref_comp);
}

int smc_wr_create_link(struct smc_link *lnk)
{
	struct ib_device *ibdev = lnk->smcibdev->ibdev;
	int rc = 0;

	lnk->wr_rx_dma_addr = ib_dma_map_single(
		ibdev, lnk->wr_rx_bufs,	lnk->wr_rx_buflen * lnk->wr_rx_cnt,
		DMA_FROM_DEVICE);
	if (ib_dma_mapping_error(ibdev, lnk->wr_rx_dma_addr)) {
		lnk->wr_rx_dma_addr = 0;
		rc = -EIO;
		goto out;
	}
	if (lnk->lgr->smc_version == SMC_V2) {
		if (smc_link_shared_v2_rxbuf(lnk)) {
			lnk->wr_rx_v2_dma_addr =
				ib_dma_map_single(ibdev, lnk->lgr->wr_rx_buf_v2,
						  SMC_WR_BUF_V2_SIZE, DMA_FROM_DEVICE);
			if (ib_dma_mapping_error(ibdev, lnk->wr_rx_v2_dma_addr)) {
				lnk->wr_rx_v2_dma_addr = 0;
				rc = -EIO;
				goto dma_unmap;
			}
		}
		lnk->wr_tx_v2_dma_addr = ib_dma_map_single(ibdev,
			lnk->lgr->wr_tx_buf_v2, SMC_WR_BUF_V2_SIZE,
			DMA_TO_DEVICE);
		if (ib_dma_mapping_error(ibdev, lnk->wr_tx_v2_dma_addr)) {
			lnk->wr_tx_v2_dma_addr = 0;
			rc = -EIO;
			goto dma_unmap;
		}
	}
	lnk->wr_tx_dma_addr = ib_dma_map_single(
		ibdev, lnk->wr_tx_bufs,	SMC_WR_BUF_SIZE * lnk->wr_tx_cnt,
		DMA_TO_DEVICE);
	if (ib_dma_mapping_error(ibdev, lnk->wr_tx_dma_addr)) {
		rc = -EIO;
		goto dma_unmap;
	}
	smc_wr_init_sge(lnk);
	bitmap_zero(lnk->wr_tx_mask, lnk->max_send_wr);
	init_waitqueue_head(&lnk->wr_tx_wait);
	rc = percpu_ref_init(&lnk->wr_tx_refs, smcr_wr_tx_refs_free, 0, GFP_KERNEL);
	if (rc)
		goto dma_unmap;
	init_completion(&lnk->tx_ref_comp);
	init_waitqueue_head(&lnk->wr_reg_wait);
	rc = percpu_ref_init(&lnk->wr_reg_refs, smcr_wr_reg_refs_free, 0, GFP_KERNEL);
	if (rc)
		goto cancel_ref;
	init_completion(&lnk->reg_ref_comp);
	rc = percpu_ref_init(&lnk->wr_rx_refs, smcr_wr_rx_refs_free, 0, GFP_KERNEL);
	if (rc)
		goto cancel_reg_ref;
	init_completion(&lnk->rx_ref_comp);
	return rc;

cancel_reg_ref:
	percpu_ref_exit(&lnk->wr_reg_refs);
cancel_ref:
	percpu_ref_exit(&lnk->wr_tx_refs);
dma_unmap:
	if (lnk->wr_rx_v2_dma_addr) {
		ib_dma_unmap_single(ibdev, lnk->wr_rx_v2_dma_addr,
				    SMC_WR_BUF_V2_SIZE,
				    DMA_FROM_DEVICE);
		lnk->wr_rx_v2_dma_addr = 0;
	}
	if (lnk->wr_tx_v2_dma_addr) {
		ib_dma_unmap_single(ibdev, lnk->wr_tx_v2_dma_addr,
				    SMC_WR_BUF_V2_SIZE,
				    DMA_TO_DEVICE);
		lnk->wr_tx_v2_dma_addr = 0;
	}
	ib_dma_unmap_single(ibdev, lnk->wr_rx_dma_addr,
			    lnk->wr_rx_buflen * lnk->wr_rx_cnt,
			    DMA_FROM_DEVICE);
	lnk->wr_rx_dma_addr = 0;
out:
	return rc;
}

void smc_wr_init_cqes(struct smc_link *lnk)
{
	int i;

	/* init CQE for WR fast reg */
	smc_wr_reg_init_cqe(&lnk->wr_reg_cqe);
	lnk->wr_reg.wr.wr_cqe = &lnk->wr_reg_cqe;

	/* init CQE for WR WRITE */
	for (i = 0; i < lnk->wr_tx_cnt; i++) {
		int n;

		smc_wr_tx_rdma_init_cqe(&lnk->wr_tx_rdmas[i].cqe);
		for (n = 0; n < SMC_MAX_RDMA_WRITES; n++)
			lnk->wr_tx_rdmas[i].wr_tx_rdma[n].wr.wr_cqe = &lnk->wr_tx_rdmas[i].cqe;
	}

	/* init CQEs for WR RECV */
	for (i = 0; i < lnk->wr_rx_cnt; i++) {
		smc_wr_rx_init_cqe(&lnk->wr_rx_ibs[i].cqe);
		lnk->wr_rx_ibs[i].wr.wr_cqe = &lnk->wr_rx_ibs[i].cqe;
		lnk->wr_rx_ibs[i].idx = i;
	}

	/* init CQEs for WR SEND */
	for (i = 0; i < lnk->wr_tx_cnt; i++) {
		smc_wr_tx_init_cqe(&lnk->wr_tx_ibs[i].cqe);
		lnk->wr_tx_ibs[i].wr.wr_cqe = &lnk->wr_tx_ibs[i].cqe;
		lnk->wr_tx_ibs[i].idx = i;
	}

	/* init CQE for SMC-Rv2 WR SEND */
	if (lnk->lgr->smc_version == SMC_V2) {
		smc_wr_tx_init_cqe(&lnk->wr_tx_v2_ib->cqe);
		lnk->wr_tx_v2_ib->wr.wr_cqe = &lnk->wr_tx_v2_ib->cqe;
		lnk->wr_tx_v2_ib->idx = lnk->wr_tx_cnt;
	}
}
