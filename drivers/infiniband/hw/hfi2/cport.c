// SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

/*
 * Implementation details of CPORT communications.
 */

#include <linux/semaphore.h>
#include <linux/io.h>

#include "hfi2.h"
#include "chip_jkr.h"
#include "cport.h"

static uint cport_ping_to;

uint hfi2_cport_adm_to = 1;

static bool cport_mctxt_recovery = true;

static void cport_send_req_fn(struct work_struct *work);
static void cport_send_rsp_fn(struct work_struct *work);

#undef CPORT_XA_DEBUG /* every tid assigned from xarray */
#undef CPORT_RCV_DEBUG /* every message (header) received, and outbox empty */
#undef CPORT_SND_DEBUG /* every message (header) sent */
#undef CPORT_INT_DEBUG /* every interrupt processed, and status in timeouts */
#define LOST_INT_DEBUG /* print if lost intr detected */

static unsigned int cport_lost_int = 1 * HZ;

/*
 * This limit needs to balance memory consuption against
 * the need to ensure tids don't repeat during periods of
 * CPORT stall and message timeout.
 *
 * Also note that the limit parameter passed to xa_alloc*()
 * gets modified, so we cannot use a static structure here.
 */
#define cport_tid_limit XA_LIMIT(0, 255)

/*
 * The "header" (first qword) of a message to/from CPORT.
 */
union cport_header {
	struct {
		u64 len : 15;
		u64 _resv1 : 1;
		u64 is_req : 1;
		u64 no_rsp : 1;
		u64 seq_no : 6;
		u64 sts : 8;
		u64 tid : 16;
		u64 sideband : 8;
		u64 op_code : 8;
	};
	u64 qw;
};

#define CPORT_SEQNO_MASK 0x3f

#define CPORT_HDR_DEF 0x0000007ec0000000ul
#define CPORT_HDR_LEN 48 /* bit position of length in CPORT_HDR_DEF */
#define CPORT_HDR_SEQ 28 /* bit position of SEQ_NO */
#define CPORT_HDR_EOM 30 /* bit position of EOM */
#define CPORT_HDR_SOM 31 /* bit position of SOM */

#define CPORT_IN_SCRATCH (JKR_ASIC_CFG_SCRATCH + 0)
#define CPORT_OUT_SCRATCH (JKR_ASIC_CFG_SCRATCH + sizeof(u64))

/*
 * Assignment of bits in JKR_MCTXT_CPORT_INT_STATUS and
 * JKR_MCTXT_PF0_INT_STATUS.
 */
#define JKR_MCTXT_INT_OUTBOX_EMPTY 0b00000001
#define JKR_MCTXT_INT_INBOX_FULL 0b00000010

/*
 * The remainder is private to the host driver.
 * CPORT firmware has no need of this interface.
 */

/*
 * How the MCTXT CSRs are interpreted.
 */
union mctxt_mem {
	union cport_header hdr;
	u64 qw[JKR_C_MCTXT_MEM_SIZE_IN_QWORDS];
};

/*
 * The common structure used to implement CPORT messages in hfi2.
 */
struct cport_work {
	struct work_struct work;
	struct kref kref;
	int flags;
	u8 n_mctxts; /* len of mctxt_mem req/rsp arrays */
	u8 mctxts_done; /* number of mctxs sent/received */
	long timeout;
	struct semaphore *sem; /* only valid in send, if request w/response */
	struct hfi2_devdata *dd; /* only valid in recv context */
	union mctxt_mem *req;
	union mctxt_mem *rsp; /* only used for request w/response */
};

#define CW_FLAG_SEND 0x01 /* struct originated in send */
#define CW_FLAG_RECV 0x02 /* struct originated in receive */

/*
 * Suspected lost interrupt, try to recover if possible.
 */
static void lost_mctxt_intr(struct hfi2_devdata *dd)
{
	unsigned long flags;
	u64 ints;

	spin_lock_irqsave(&dd->irq_src_lock, flags);
	ints = hfi2_read_csr(dd, JKR_MCTXT_PF0_INT_STATUS_ENABLED);
	/* if bits set, assume lost interrupt and attempt recovery. */
	if (ints) {
#ifdef LOST_INT_DEBUG
		dd_dev_info(dd, "%s forcing int for %llx\n", __func__, ints);
#endif
		hfi2_force_intr(dd, JKR_MCTXT_CPORT_TO_PCIE_INT);
	}
	spin_unlock_irqrestore(&dd->irq_src_lock, flags);
}

static void cport_lost_int_chk(struct timer_list *t)
{
	struct hfi2_cport *cport = timer_container_of(cport, t, lost_int_timer);

	if (cport_lost_int) {
		lost_mctxt_intr(cport->dd);
		mod_timer(&cport->lost_int_timer, jiffies + cport_lost_int);
	}
}

/*
 * Acquire a reference to the message structure
 */
static inline void cwget(struct cport_work *cw)
{
	kref_get(&cw->kref);
}

/*
 * Locate and remove the 'id' message in the xarray and atomically
 * acquire a reference to the message structure.
 */
static inline struct cport_work *cwget_xa(struct hfi2_devdata *dd, u32 id)
{
	struct cport_work *cw;

	xa_lock(&dd->cport->tid_xa);
	cw = __xa_erase(&dd->cport->tid_xa, id);
	if (cw)
		cwget(cw);
	xa_unlock(&dd->cport->tid_xa);
	return cw;
}

static void cwrelease(struct kref *kref)
{
	struct cport_work *cw = container_of(kref, struct cport_work, kref);

	kfree(cw->req);
	kfree(cw->rsp);

	kfree(cw);
}

static void cwput(struct cport_work *cw)
{
	kref_put(&cw->kref, cwrelease);
}

static struct cport_work *cwalloc(int flag)
{
	struct cport_work *cw = kzalloc(sizeof(*cw), GFP_KERNEL);

	if (!cw)
		return NULL;

	cw->flags = flag;
	cw->n_mctxts = 1;
	cw->req = kzalloc_obj(*cw->req, GFP_KERNEL);
	if (!cw->req) {
		kfree(cw);
		return NULL;
	}

	cw->rsp = kzalloc(sizeof(*cw->rsp), GFP_KERNEL);
	if (!cw->rsp) {
		kfree(cw->req);
		kfree(cw);
		return NULL;
	}

	kref_init(&cw->kref);
	return cw;
}

/* set "external" (non-alloc) response payload */
static void pld_rsp_set(struct cport_work *cw, void *pld, int len)
{
	memcpy(&cw->rsp->qw[1], pld, len);
	cw->rsp->hdr.len = len + sizeof(cw->rsp->hdr);
}

static int cw_pad_size(struct cport_work *msg, u32 size)
{
	union mctxt_mem *new_reqs, *new_rsps;
	u32 new_ctxts;

	/* already large enough */
	if (size <= msg->n_mctxts * (sizeof(*new_reqs)))
		return 0;

	/* round up to next mctxt buffer len */
	new_ctxts = (size + (sizeof(*new_reqs) - 1)) / sizeof(*new_reqs);
	if (new_ctxts > U8_MAX)
		return -EINVAL;
	new_reqs =
		krealloc(msg->req, new_ctxts * sizeof(*new_reqs), GFP_KERNEL);
	if (!new_reqs)
		return -ENOMEM;
	msg->req = new_reqs;

	new_rsps =
		krealloc(msg->rsp, new_ctxts * sizeof(*new_reqs), GFP_KERNEL);
	if (!new_rsps)
		return -ENOMEM;

	msg->rsp = new_rsps;
	msg->n_mctxts = new_ctxts;

	return 0;
}

static int cwcopy(struct cport_work *msg, void *from, u32 offset, u32 len,
		  bool req)
{
	union mctxt_mem *mc;
	int ret;

	ret = cw_pad_size(msg, offset + len);
	if (ret)
		return ret;

	mc = req ? msg->req : msg->rsp;
	memcpy(((u8 *)mc) + offset, from, len);

	return 0;
}

/*
 * Send request, non-blocking, with timeout for OUTBOX_EMPTY wait.
 * Caller must watch 'wait' semaphore for completion, and
 * then call hfi2_cport_send_comp() or hfi2_cport_send_cancel() to finalize everything.
 * Returns handle for hfi2_cport_send_comp()/_cancel(), or ERR_PTR().
 *
 * See hfi2_cport_send_req() for example usage.
 */
void *hfi2_cport_send_req_nb(struct hfi2_devdata *dd, u8 op, u8 sideband,
			     void *payload, int len, struct semaphore *wait,
			     long timeout)
{
	int ret;
	struct cport_work *msg;
	u32 idx;

	if (!dd->cport)
		return ERR_PTR(-EINVAL);

	msg = cwalloc(CW_FLAG_SEND);
	if (!msg)
		return ERR_PTR(-ENOMEM);
	msg->dd = dd;
	msg->timeout = timeout;
	ret = cwcopy(msg, payload, sizeof(union cport_header), len, true);
	if (ret) {
		cwput(msg);
		return ERR_PTR(ret);
	}
	ret = xa_alloc_cyclic(&dd->cport->tid_xa, &idx, msg, cport_tid_limit,
			      &dd->cport->tid_next, GFP_KERNEL);
	if (ret < 0) {
		cwput(msg);
		return ERR_PTR(ret);
	}
#ifdef CPORT_XA_DEBUG
	dd_dev_info(dd, "CPORT tid is %04x\n", idx);
#endif
	msg->req->hdr.op_code = op;
	msg->req->hdr.sideband = sideband;
	msg->req->hdr.is_req = 1;
	msg->req->hdr.no_rsp = 0;
	msg->req->hdr.tid = idx;
	msg->req->hdr.len = len + sizeof(msg->req->hdr);
	msg->sem = wait;
	cwget(msg); /* extra ref so not freed after send */
	INIT_WORK(&msg->work, cport_send_req_fn);
	queue_work(dd->hfi2_wq, &msg->work);
	return msg;
}

/*
 * Extract results from response and drop (last) reference to structure.
 * Returns status from response header.
 * Must never be called twice for the same message (message has been freed).
 * Must never be called on a message that has not received response (up'ed wait).
 */
int hfi2_cport_send_comp(struct hfi2_devdata *dd, void *handle, void **rsp_pld,
			 int *rsp_len)
{
	struct cport_work *msg = handle;
	void *ptr;
	int ret;
	int len;

	*rsp_len = len = msg->rsp->hdr.len - sizeof(msg->rsp->hdr);
	if (rsp_pld) {
		ptr = kzalloc(len, GFP_KERNEL);
		if (!ptr)
			return -ENOMEM;
		memcpy(ptr, &msg->rsp[0].qw[1], len);
		*rsp_pld = ptr;
	}
	ret = msg->rsp->hdr.sts;
	cwput(msg);
	return ret;
}

/*
 * Cleanup aborted wait for response.
 * Only called for requests w/responses.
 * Must never be called twice for the same message (message has been freed).
 */
void hfi2_cport_send_cancel(struct hfi2_devdata *dd, void *handle)
{
	struct cport_work *msg = handle;

	cancel_work(&msg->work); /* cport_send() drops ref on all paths */
	xa_erase(&dd->cport->tid_xa, msg->req->hdr.tid);
	cwput(msg);
}

/*
 * Send request and wait for response, with timeout.
 * Caller must be able to sleep.
 * Returns status from response header, and response payload data, on success.
 * Response payload was from k[z]alloc() and caller must kfree().
 * On error, returns -errno (response status is always 0-15).
 */
int hfi2_cport_send_req(struct hfi2_devdata *dd, u8 op, u8 sideband,
			void *payload, int len, void **rsp_pld, int *rsp_len,
			long timeout)
{
	int ret;
	struct cport_work *msg;
	struct semaphore comp;

	sema_init(&comp, 0);

	might_sleep();

	msg = hfi2_cport_send_req_nb(dd, op, sideband, payload, len, &comp,
				     timeout);
	if (IS_ERR(msg))
		return PTR_ERR(msg);
	if (timeout > 0 && timeout != MAX_SCHEDULE_TIMEOUT)
		ret = down_timeout(&comp, timeout);
	else
		ret = down_killable(&comp);
	if (ret) {
#ifdef CPORT_INT_DEBUG
		u64 ints;

		ints = hfi2_read_csr(dd, JKR_MCTXT_PF0_INT_STATUS);
		dd_dev_err(
			dd,
			"CPORT request wait interrupted %016llx (%d) [%02llx]\n",
			msg->req->hdr.qw, ret, ints);
#else
		dd_dev_err(dd, "CPORT request wait interrupted %016llx (%d)\n",
			   msg->req->hdr.qw, ret);
#endif
		hfi2_cport_send_cancel(dd, msg);
		lost_mctxt_intr(dd); /* attempt recovery */
		return ret;
	}
	return hfi2_cport_send_comp(dd, msg, rsp_pld, rsp_len);
}

int hfi2_cport_send_notif(struct hfi2_devdata *dd, u8 op, u8 sideband,
			  void *payload, int len, long timeout)
{
	struct cport_work *msg;
	int ret;

	if (!dd->cport)
		return -EINVAL;

	msg = cwalloc(CW_FLAG_SEND);
	if (!msg)
		return -ENOMEM;
	msg->dd = dd;
	msg->timeout = timeout;
	ret = cwcopy(msg, payload, sizeof(union cport_header), len, true);
	if (ret) {
		cwput(msg);
		return ret;
	}
	msg->req->hdr.len = len + sizeof(msg->req->hdr);
	msg->req->hdr.op_code = op;
	msg->req->hdr.sideband = sideband;
	msg->req->hdr.is_req = 1;
	msg->req->hdr.no_rsp = 1;
	INIT_WORK(&msg->work, cport_send_req_fn);
	queue_work(dd->hfi2_wq, &msg->work);
	return 0;
}

/*
 ********************************************************************
 * Internal routines.
 */

/*
 * Send a response to CPORT's request.
 *
 * Re-uses message structure from request. Queues to send workqueue.
 */
static int cport_send_rsp(struct cport_work *msg, int sts)
{
	struct hfi2_devdata *dd = msg->dd;

	/* rsp.hdr.len and rsp.qw[1..] already setup, also rsp.op_code/rsp.tid */
	msg->rsp->hdr.is_req = 0;
	msg->rsp->hdr.sts = sts;
	INIT_WORK(&msg->work, cport_send_rsp_fn);
	queue_work(dd->hfi2_wq, &msg->work);
	return 0;
}

static void cport_send(struct cport_work *msg, bool req)
{
	union mctxt_mem *mc = req ? msg->req : msg->rsp;
	struct hfi2_devdata *dd = msg->dd;
	u32 csr_i, tot_len, len_sent;
	u64 cport_hdr;
	u8 seq_no;
	u64 *ptr;
	int ret, mcxt_len;

	/* sleep until OutboxEmpty... */
	if (msg->timeout > 0 && msg->timeout != MAX_SCHEDULE_TIMEOUT)
		ret = down_timeout(&dd->cport->outbox, msg->timeout);
	else
		ret = down_killable(&dd->cport->outbox);
	if (ret) {
		dd_dev_err(dd, "CPORT Send OUTBOX_EMPTY killed %016llx (%d)\n",
			   msg->req->hdr.qw, ret);
		cwput(msg);
		lost_mctxt_intr(dd); /* attempt recovery */
		return; /* no way to report error to caller */
	}

	/* sequential under lock, check if there are unfinished messages */
	if (dd->cport->incomplete_mctxt_msg_tx &&
	    dd->cport->incomplete_mctxt_msg_tx != msg) {
		/* give up empty outbox, the incomplete msg must finish next */
		up(&dd->cport->outbox);
		/* reschedule this work until next time */
		queue_work(dd->hfi2_wq, &msg->work);
		return;
	}

#ifdef CPORT_SND_DEBUG
	dd_dev_info(dd, "MCTXT sent %016llx\n", mc->hdr.qw);
#endif
	tot_len = mc->hdr.len;
	len_sent = msg->mctxts_done * sizeof(union mctxt_mem);
	ptr = &mc[msg->mctxts_done].qw[0];
	seq_no = msg->mctxts_done;
	mcxt_len =
		min_t(int, (int)(tot_len - len_sent), sizeof(union mctxt_mem));
	/* NOTE: "CPORT IN" is our output */
	csr_i = JKR_MCTXT_CPORT_IN;
	cport_hdr = CPORT_HDR_DEF;
	cport_hdr |= ((u64)mcxt_len << CPORT_HDR_LEN);

	cport_hdr |= (((u64)seq_no & 0x3) << CPORT_HDR_SEQ);

	/* CPORT_HDR_DEF sets SOM/EOM, clear if not */
	if (msg->mctxts_done)
		cport_hdr &= ~((u64)1 << CPORT_HDR_SOM);
	else
		mc->hdr.seq_no = atomic_fetch_inc(&dd->cport->seqno);

	if (msg->mctxts_done + 1 < msg->n_mctxts)
		cport_hdr &= ~((u64)1 << CPORT_HDR_EOM);

	hfi2_write_csr(dd, CPORT_IN_SCRATCH, cport_hdr);
	while (mcxt_len > 0) {
		hfi2_write_csr(dd, csr_i, *ptr++);
		csr_i += sizeof(u64);
		mcxt_len -= sizeof(u64);
	}
	hfi2_write_csr(dd, JKR_MCTXT_CPORT_INT_STATUS,
		       JKR_MCTXT_INT_INBOX_FULL);

	/* did we complete the message? */
	if (++msg->mctxts_done < msg->n_mctxts) {
		dd->cport->incomplete_mctxt_msg_tx = msg;
		queue_work(dd->hfi2_wq,
			   &msg->work); /* reschedule the remaining sends */

		return;
	}

	dd->cport->incomplete_mctxt_msg_tx = NULL;
	/* reset mctxts_done for response parsing */
	if (req)
		msg->mctxts_done = 0;
	cwput(msg); /* may or may not free memory */
}

/*
 * Low-level send to CPORT via MCTXT.
 *
 * Interfaces with MCTXT.
 */
static void cport_send_req_fn(struct work_struct *work)
{
	struct cport_work *msg = container_of(work, struct cport_work, work);

	cport_send(msg, true);
}

static void cport_send_rsp_fn(struct work_struct *work)
{
	struct cport_work *msg = container_of(work, struct cport_work, work);

	cport_send(msg, false);
}

static int echo_req(struct hfi2_devdata *dd, u8 op, u8 sideband, void *pld,
		    int pll, void *handle)
{
	struct cport_work *msg = handle;

	dd_dev_info(msg->dd, "cport ping %02x (%d)\n", sideband, pll);
	/* Leave payload in-tact (echo). */
	pld_rsp_set(msg, pld, pll);
	return MSG_RSP_STATUS_OK;
}

static int inval_req(struct hfi2_devdata *dd, u8 op, u8 sideband, void *pld,
		     int pll, void *handle)
{
	return MSG_RSP_STATUS_OPCODE_UNSUPPORTED;
}

/*
 * Process a request from CPORT.
 *
 * May be dispatched to external function from handlers[].
 */
static void cport_req_fn(struct work_struct *work)
{
	struct cport_work *msg = container_of(work, struct cport_work, work);
	cport_handler func;
	int ret = MSG_RSP_STATUS_OK;
	void *pld;
	int pll;

	pll = msg->req->hdr.len - sizeof(msg->req->hdr);
	pld = &msg->req->qw[1];
	msg->rsp->hdr.qw = msg->req->hdr.qw;
	/* default to no payload in response (if any) */
	msg->rsp->hdr.len = sizeof(msg->req->hdr);
	func = msg->dd->cport->handlers[msg->req->hdr.op_code];
	if (func)
		ret = func(msg->dd, msg->req->hdr.op_code,
			   msg->req->hdr.sideband, pld, pll, msg);
	else
		ret = inval_req(msg->dd, msg->req->hdr.op_code,
				msg->req->hdr.sideband, pld, pll, msg);
	if (msg->req->hdr.no_rsp) {
		cwput(msg);
		if (ret)
			dd_dev_err(msg->dd, "Op %d %02x failed (%d)\n",
				   msg->req->hdr.op_code,
				   msg->req->hdr.sideband, ret);
	} else {
		/* msg->rsp->qw[*] and msg->rsp->hdr.len have been updated */
		ret = cport_send_rsp(msg, ret);
		if (ret)
			dd_dev_err(msg->dd, "Response send failed (%d)\n", ret);
	}
}

/*
 * Handler for MCTXT Inbox Full interrupt.
 *
 * Only one can be queued/run until JKR_MCTXT_INT_OUTBOX_EMPTY is cleared.
 * Run in a workqueue (not interrupt context).
 */
static void cport_mctxt_fn(struct work_struct *work)
{
	struct hfi2_cport *cport =
		container_of(work, struct hfi2_cport, mctxt_work);
	struct hfi2_devdata *dd = cport->dd;
	int ret = 0;
	u8 is_start, is_end;
	int len;
	u64 *ptr;
	u64 mhdr;
	u32 i;
	struct cport_work *msg;
	union cport_header hdr;

	/*
	 * CPORT output MCTXT is our input.
	 */
	i = JKR_MCTXT_CPORT_OUT;

	mhdr = hfi2_read_csr(dd, CPORT_OUT_SCRATCH);
	len = (mhdr >> CPORT_HDR_LEN) & 0xfff;
	len = min_t(int, len, sizeof(union mctxt_mem));
	is_start = (mhdr >> CPORT_HDR_SOM) & 1;
	is_end = (mhdr >> CPORT_HDR_EOM) & 1;
	if (is_start) {
		hdr.qw = hfi2_read_csr(dd, i);
	} else if (cport->incomplete_mctxt_msg_rx) {
		msg = cport->incomplete_mctxt_msg_rx;
		if (msg->flags & CW_FLAG_RECV) {
			ptr = &msg->req[msg->mctxts_done++].qw[0];
			hdr.qw = msg->req->hdr.qw;
		} else {
			ptr = &msg->rsp[msg->mctxts_done++].qw[0];
			hdr.qw = msg->rsp->hdr.qw;
		}
		dd_dev_info(dd, "%s: got continuation of %u\n", __func__,
			    hdr.seq_no);
		/* skip all the start-message parsing/allocation */
		goto copy;
	} else {
		/* this is bad, just skip the message */
		dd_dev_warn(dd, "cport sent unexpected non-start packet\n");
		goto fail;
	}
#ifdef CPORT_RCV_DEBUG
	dd_dev_info(dd, "%s() %016llx\n", __func__, hdr.qw);
#endif
	if (hdr.len < sizeof(hdr)) {
		/* assume message is invalid  - cannot be processed */
		ret = -EDOM;
		goto fail;
	}
	i += sizeof(u64);
	/* No need for atomics here, we are single threaded */
	if (hdr.seq_no != cport->rseqno) {
		dd_dev_info(dd, "Recv out of sequence: %d -> %d\n",
			    cport->rseqno, hdr.seq_no);
		cport->rseqno = hdr.seq_no;
	}
	cport->rseqno = (cport->rseqno + 1) & CPORT_SEQNO_MASK;
	if (hdr.is_req) {
		/* Request from CPORT, has no existing message context */
		msg = cwalloc(CW_FLAG_RECV);
		if (!msg) {
			ret = -ENOMEM;
			goto fail; /* drop message, with error */
		}
		ret = cw_pad_size(msg, hdr.len);
		if (ret)
			goto fail;
		ptr = &msg->req->qw[msg->mctxts_done++];
	} else {
		/*
		 * Responses already have a 'msg', extra ref was already taken.
		 * Take an additional ref against possible race with timeout
		 * (hfi2_cport_send_cancel()) between here and the up().
		 */
		msg = cwget_xa(dd, hdr.tid);
		if (!msg) {
			ret = -ESRCH;
			goto fail; /* drop message, with error */
		}
		ret = cw_pad_size(msg, hdr.len);
		if (ret)
			goto fail;
		ptr = &msg->rsp->qw[msg->mctxts_done++];
		/* assert msg->req->hdr ~= hdr */
	}
	*ptr++ = hdr.qw;
	len -= sizeof(hdr);
copy:
	/* now copy payload into chosen buffer */
	while (len > 0) {
		*ptr++ = hfi2_read_csr(dd, i);
		i += sizeof(u64);
		len -= sizeof(u64);
	}
	/*
	 * We are finished with the dd->cport->mctxt_work struct,
	 * and the MCTXT, so it can all be re-used now.
	 */
	hfi2_write_csr(dd, JKR_MCTXT_CPORT_INT_STATUS,
		       JKR_MCTXT_INT_OUTBOX_EMPTY);
#ifdef CPORT_RCV_DEBUG
	dd_dev_info(dd, "%s() set CPORT OUTBOX_EMPTY\n", __func__);
#endif
	/*
	 * if we do not contain the end of the message then we need to wait
	 * until next mailbox
	 */
	if (!is_end) {
		cport->incomplete_mctxt_msg_rx = msg;
		dd_dev_info(dd, "%s: expecting continuation of %u\n", __func__,
			    hdr.seq_no);
		return;
	}
	cport->incomplete_mctxt_msg_rx = NULL;

	/* responses don't require any more work here - just wakeup requester */
	if (!hdr.is_req) {
		up(msg->sem);
		cwput(msg);
		return;
	}
	msg->dd = dd;

	/* reset mctxts for response parsing */
	msg->mctxts_done = 0;

	/* dispatch 'msg' request */
	INIT_WORK(&msg->work, cport_req_fn);
	/* don't care about locality */
	queue_work(dd->hfi2_wq, &msg->work);
	return;
fail:
	hfi2_write_csr(dd, JKR_MCTXT_CPORT_INT_STATUS,
		       JKR_MCTXT_INT_OUTBOX_EMPTY);
	dd_dev_err(dd, "Dropping incoming CPORT message %016llx (%d)\n", hdr.qw,
		   ret);
}

/*
 * Handler for PF0 MCTXT interrupts.
 *
 * Called when one of the enabled MCTXT_PF0 conditions occurs.
 * 'source' is always 0. Called in interrupt context.
 *
 * Since this interrupt is exclusive to MCTXT, there is no doubt
 * about which transport to use (always is MCTXT).
 */
void hfi2_is_cport_int(struct hfi2_devdata *dd, unsigned int source)
{
	u64 ints;
	const int limit = 100; /* arbitrary */
	int count;

	if (!dd->cport)
		return;

	/*
	 * MctxtCportToPcieInt is a "one shot" merged interrupt.  To ensure
	 * nothing is missed, ensure that its source, MctxtPf0IntStatusEnabled,
	 * is cleared and reads as zero.
	 */
	ints = 0;
	for (count = 0; count < limit; count++) {
		u64 temp;

		temp = hfi2_read_csr(dd, JKR_MCTXT_PF0_INT_STATUS_ENABLED);
		if (temp == 0)
			break;
		ints |= temp;
		hfi2_write_csr(dd, JKR_MCTXT_PF0_INT_ACK, temp);
	}
	if (count == limit)
		dd_dev_warn(dd, "MCTXT interrupt too many loops\n");
	if (!ints) {
		dd_dev_warn(dd, "MCTXT interrupt, but no status bits set\n");
		return;
	}

#ifdef CPORT_INT_DEBUG
	dd_dev_info(dd, "%s() %02llx\n", __func__, ints);
#endif
	if (ints & JKR_MCTXT_INT_INBOX_FULL)
		queue_work(dd->hfi2_wq, &dd->cport->mctxt_work);
	if (ints & JKR_MCTXT_INT_OUTBOX_EMPTY)
		up(&dd->cport->outbox);
}

/***************************************************
 * API for handling notifications from CPORT
 */

int hfi2_cport_resp_set(void *handle, void *payload, int len)
{
	struct cport_work *msg = handle;

	if (!msg || !payload || len <= 0)
		return -EINVAL;
	if (len > (int)(sizeof(*msg->rsp) - sizeof(msg->rsp->hdr)))
		return -EINVAL;
	msg->rsp->hdr.len = len + sizeof(msg->rsp->hdr);
	memcpy(&msg->rsp->qw[1], payload, len);
	return 0;
}

int hfi2_cport_register_cb(struct hfi2_devdata *dd, u8 op_start, u8 op_end,
			   cport_handler func)
{
	int x;

	if (op_start > op_end)
		return -ERANGE;
	if (!dd->cport)
		return -EINVAL;

	/* 'func' may be NULL, to unregister */
	for (x = op_start; x <= op_end; ++x) {
		dd->cport->handlers[x] = func;
	}
	return 0;
}

static int cport_ping(void *data)
{
	struct hfi2_devdata *dd = data;
	char buf[16];
	int len;
	unsigned int num;
	void *rspbuf;
	int rsplen;
	int rc;

	while (!kthread_should_stop() &&
	       (num = atomic_read(&dd->cport->nping)) > 0) {
		len = snprintf(buf, sizeof(buf), "ping %u", num);
		rspbuf = NULL;
		rc = hfi2_cport_send_req(dd, CH_OP_PING, 0, buf, len, &rspbuf,
					 &rsplen,
					 cport_ping_to ? cport_ping_to * HZ :
							 MAX_SCHEDULE_TIMEOUT);
		if (rc < 0) {
			dd_dev_info(dd, "CPORT \"%s\" error %d\n", buf, rc);
			if (!cport_ping_to)
				break;
		} else {
			dd_dev_info(dd, "CPORT \"%s\" -> %d \"%.*s\"\n", buf,
				    rc, rsplen, (char *)rspbuf);
		}
		kfree(rspbuf);
		atomic_dec(&dd->cport->nping);
	}
	dd->cport->ping_th = NULL;
	atomic_set(&dd->cport->nping, 0);
	return 0;
}

int hfi2_cport_ping_start(struct hfi2_devdata *dd, unsigned int count)
{
	int rc;

	if (!count) {
		if (dd->cport->ping_th)
			kthread_stop(dd->cport->ping_th);
		/* kthread will zero count when exiting */
		else
			atomic_set(&dd->cport->nping, 0);
		return 0;
	}
	atomic_set(&dd->cport->nping, count);
	if (dd->cport->ping_th)
		return 0;

	dd->cport->ping_th =
		kthread_create_on_node(cport_ping, dd, dd->node, "cport_ping");
	if (IS_ERR(dd->cport->ping_th)) {
		rc = PTR_ERR(dd->cport->ping_th);
		dd->cport->ping_th = NULL;
		dd_dev_err(dd, "Failed to create CPORT ping thread %d\n", rc);
		return rc;
	}
	wake_up_process(dd->cport->ping_th);
	return 0;
}

#ifdef CONFIG_HFI_CPORT_POLLING
static int cport_poll(void *data)
{
	struct hfi2_devdata *dd = data;
	u64 v;

	while (!kthread_should_stop()) {
		v = hfi2_read_csr(dd, JKR_MCTXT_PF0_INT_STATUS);
		if (v & 0xff)
			hfi2_is_cport_int(dd, 0);
		fsleep(30);
	}
	return 0;
}
#endif

/*
 * Initialization/setup of MCTXT CPORT communications channel.
 */
int hfi2_cport_init(struct hfi2_devdata *dd)
{
	struct hfi2_cport *cport;

	if (dd->params->chip_type == CHIP_WFR || dd->is_vf)
		return 0;

	cport = kzalloc_obj(*cport, GFP_KERNEL);
	if (!cport)
		goto err1;

	INIT_WORK(&cport->mctxt_work, cport_mctxt_fn);
	xa_init_flags(&cport->tid_xa, XA_FLAGS_ALLOC);
	xa_init_flags(&cport->trap_xa, XA_FLAGS_ALLOC);

	/*
	 * Setting initial state can be problematic.
	 * We require that CPORT set JKR_MCTXT_INT_OUTBOX_EMPTY in
	 * JKR_MCTXT_PF0_INT_STATUS or we will never start sending.
	 * We also require that CPORT never set JKR_MCTXT_INT_OUTBOX_EMPTY
	 * gratuitously, or we get a semaphore count > 1 and will
	 * start overrunning MCTXT. Essentially, CPORT must set this
	 * exactly once when entering the "ready to receive" state
	 * (initially and after processing each message).
	 */
	sema_init(&cport->outbox, 0);

	cport->dd = dd;
	dd->cport = cport;

	hfi2_cport_register_cb(dd, CH_OP_PING, CH_OP_PING, echo_req);

	if (cport_mctxt_recovery) {
		u64 is, ie;

		is = hfi2_read_csr(dd, JKR_MCTXT_PF0_INT_STATUS);
		ie = hfi2_read_csr(dd, JKR_MCTXT_PF0_INT_ENABLE);
		if (!(is & JKR_MCTXT_INT_OUTBOX_EMPTY) && ie) {
			dd_dev_warn(dd, "recovering CPORT MCTXT state\n");
			hfi2_write_csr(dd, JKR_MCTXT_PF0_INT_ENABLE, 0);
			hfi2_write_csr(dd, JKR_MCTXT_PF0_INT_STATUS,
				       JKR_MCTXT_INT_OUTBOX_EMPTY);
		}
	}

#ifdef CONFIG_HFI_CPORT_POLLING
	cport->poll_th =
		kthread_create_on_node(cport_poll, dd, dd->node, "cport_poll");
	if (!cport->poll_th)
		dd_dev_err(dd, "Failed to create CPORT polling thread\n");
	else
		wake_up_process(dd->cport->poll_th);
#else
	/* Enable intr source for MCTXT from CPORT (to PF0) */
	hfi2_write_csr(dd, JKR_MCTXT_PF0_INT_ENABLE,
		       JKR_MCTXT_INT_INBOX_FULL | JKR_MCTXT_INT_OUTBOX_EMPTY);
	hfi2_set_intr_bits(dd, JKR_MCTXT_CPORT_TO_PCIE_INT,
			   JKR_MCTXT_CPORT_TO_PCIE_INT, true);
	timer_setup(&cport->lost_int_timer, cport_lost_int_chk, 0);
	if (cport_lost_int)
		mod_timer(&cport->lost_int_timer, jiffies + cport_lost_int);
#endif

	/*
	 * Must reset/resync sequence numbers as CPORT is strictly enforcing
	 * sequence number order. Use a timeout to allow easier cleanup should
	 * init fail.
	 */
	hfi2_cport_send_notif(dd, CH_OP_PING, 0, NULL, 0,
			      hfi2_cport_adm_to * HZ);
	return 0;

err1:
	return -ENOMEM;
}

/*
 * Deinitialization of MCTXT CPORT communications channel.
 */
int hfi2_cport_exit(struct hfi2_devdata *dd)
{
	if (!dd->cport)
		return 0;

	timer_shutdown_sync(&dd->cport->lost_int_timer);
	/* flush all cport queued tasks (plus anything else on this queue) */
	flush_workqueue(dd->hfi2_wq);

	/* Disable intr source for MCTXT from CPORT (to PF0) */
	hfi2_set_intr_bits(dd, JKR_MCTXT_CPORT_TO_PCIE_INT,
			   JKR_MCTXT_CPORT_TO_PCIE_INT, false);
	hfi2_write_csr(dd, JKR_MCTXT_PF0_INT_ENABLE, 0);
	/* leave JKR_MCTXT_INT_OUTBOX_EMPTY set so that future users are ready-to-go */
	hfi2_write_csr(dd, JKR_MCTXT_PF0_INT_STATUS,
		       JKR_MCTXT_INT_OUTBOX_EMPTY);
#ifdef CONFIG_HFI_CPORT_POLLING
	if (dd->cport->poll_th)
		kthread_stop(dd->cport->poll_th);
#endif
	if (dd->cport->ping_th)
		kthread_stop(dd->cport->ping_th);

	cancel_work_sync(&dd->cport->mctxt_work);

	xa_destroy(&dd->cport->tid_xa);
	xa_destroy(&dd->cport->trap_xa);
	kfree(dd->cport);
	dd->cport = NULL;

	return 0;
}
