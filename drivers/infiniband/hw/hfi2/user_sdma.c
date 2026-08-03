// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 * Copyright(c) 2015 - 2018 Intel Corporation.
 */

#include <linux/mm.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/dmapool.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/uio.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/mmu_context.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/string.h>

#include "hfi2.h"
#include "sdma.h"
#include "user_sdma.h"
#include "verbs.h" /* for the headers */
#include "common.h" /* for struct hfi2_tid_info */
#include "trace.h"

static uint hfi2_sdma_comp_ring_size = 128;

static unsigned int initial_pkt_count = 8;

static int user_sdma_send_pkts(struct user_sdma_request *req, u16 maxpkts);
static void user_sdma_txreq_cb(struct sdma_txreq *txreq, int status);
static inline void pq_update(struct hfi2_user_sdma_pkt_q *pq);
static void user_sdma_free_request(struct user_sdma_request *req);
static int check_header_template(struct user_sdma_request *req,
				 struct user_sdma_txreq *tx, u32 lrhlen,
				 u32 datalen);
static int set_txreq_header(struct user_sdma_request *req,
			    struct user_sdma_txreq *tx, u32 datalen);
static int set_txreq_header_ahg(struct user_sdma_request *req,
				struct user_sdma_txreq *tx, u32 len);
static inline void set_comp_state(struct hfi2_user_sdma_pkt_q *pq,
				  struct hfi2_user_sdma_comp_q *cq, u16 idx,
				  enum hfi2_sdma_comp_state state, int ret);
static inline u32 set_pkt_bth_psn(__be32 bthpsn, u8 expct, u32 frags);
static inline u32 get_lrh_len(struct user_sdma_request *req, u32 len);

static int defer_packet_queue(struct sdma_engine *sde, struct iowait_work *wait,
			      struct sdma_txreq *txreq, uint seq,
			      bool pkts_sent);
static void activate_packet_queue(struct iowait *wait, int reason);

static int defer_packet_queue(struct sdma_engine *sde, struct iowait_work *wait,
			      struct sdma_txreq *txreq, uint seq,
			      bool pkts_sent)
{
	struct hfi2_user_sdma_pkt_q *pq =
		container_of(wait->iow, struct hfi2_user_sdma_pkt_q, busy);

	write_seqlock(&sde->waitlock);
	trace_hfi2_usdma_defer(pq, sde, &pq->busy);
	if (sdma_progress(sde, seq, txreq))
		goto eagain;
	/*
	 * We are assuming that if the list is enqueued somewhere, it
	 * is to the dmawait list since that is the only place where
	 * it is supposed to be enqueued.
	 */
	xchg(&pq->state, SDMA_PKT_Q_DEFERRED);
	if (list_empty(&pq->busy.list)) {
		pq->busy.lock = &sde->waitlock;
		iowait_get_priority(&pq->busy);
		iowait_queue(pkts_sent, &pq->busy, &sde->dmawait);
	}
	write_sequnlock(&sde->waitlock);
	return -EBUSY;
eagain:
	write_sequnlock(&sde->waitlock);
	return -EAGAIN;
}

static void activate_packet_queue(struct iowait *wait, int reason)
{
	struct hfi2_user_sdma_pkt_q *pq =
		container_of(wait, struct hfi2_user_sdma_pkt_q, busy);

	trace_hfi2_usdma_activate(pq, wait, reason);
	xchg(&pq->state, SDMA_PKT_Q_ACTIVE);
	wake_up(&wait->wait_dma);
};

#define HEADER_ALIGN 256 /* memory alignment for header descriptors */

int hfi2_user_sdma_alloc_queues(struct hfi2_ctxtdata *uctxt,
				struct hfi2_filedata *fd)
{
	int ret = -ENOMEM;
	char buf[64];
	struct hfi2_devdata *dd;
	struct hfi2_user_sdma_comp_q *cq;
	struct hfi2_user_sdma_pkt_q *pq;

	if (!uctxt || !fd)
		return -EBADF;

	if (!hfi2_sdma_comp_ring_size)
		return -EINVAL;

	dd = uctxt->dd;

	pq = kzalloc(sizeof(*pq), GFP_KERNEL);
	if (!pq)
		return -ENOMEM;
	pq->dd = dd;
	pq->ctxt = uctxt->ctxt;
	pq->subctxt = fd->subctxt;
	pq->n_max_reqs = hfi2_sdma_comp_ring_size;
	atomic_set(&pq->n_reqs, 0);
	init_waitqueue_head(&pq->wait);
	atomic_set(&pq->n_locked, 0);

	hfi2_iowait_init(&pq->busy, 0, NULL, NULL, defer_packet_queue,
			 activate_packet_queue, NULL, NULL);

	pq->reqs = kcalloc(hfi2_sdma_comp_ring_size, sizeof(*pq->reqs),
			   GFP_KERNEL);
	if (!pq->reqs)
		goto pq_reqs_nomem;

	pq->req_in_use = bitmap_zalloc(hfi2_sdma_comp_ring_size, GFP_KERNEL);
	if (!pq->req_in_use)
		goto pq_reqs_no_in_use;

	snprintf(buf, 64, "txreq-kmem-cache-%u-%u-%u", dd->unit, uctxt->ctxt,
		 fd->subctxt);
	pq->txreq_cache = kmem_cache_create(buf, sizeof(struct user_sdma_txreq),
					    HEADER_ALIGN, SLAB_HWCACHE_ALIGN,
					    NULL);
	if (!pq->txreq_cache) {
		dd_dev_err(dd, "[%u] Failed to allocate TxReq cache\n",
			   uctxt->ctxt);
		goto pq_txreq_nomem;
	}

	cq = kzalloc(sizeof(*cq), GFP_KERNEL);
	if (!cq)
		goto cq_nomem;

	cq->comps = vmalloc_user(
		PAGE_ALIGN(sizeof(*cq->comps) * hfi2_sdma_comp_ring_size));
	if (!cq->comps)
		goto cq_comps_nomem;

	cq->nentries = hfi2_sdma_comp_ring_size;

	ret = hfi2_init_pinning_interfaces(pq);
	if (ret)
		goto pq_mmu_fail;

	rcu_assign_pointer(fd->pq, pq);
	fd->cq = cq;

	return 0;

pq_mmu_fail:
	vfree(cq->comps);
cq_comps_nomem:
	kfree(cq);
cq_nomem:
	kmem_cache_destroy(pq->txreq_cache);
pq_txreq_nomem:
	bitmap_free(pq->req_in_use);
pq_reqs_no_in_use:
	kfree(pq->reqs);
pq_reqs_nomem:
	kfree(pq);

	return ret;
}

static void flush_pq_iowait(struct hfi2_user_sdma_pkt_q *pq)
{
	unsigned long flags;
	seqlock_t *lock = pq->busy.lock;

	if (!lock)
		return;
	write_seqlock_irqsave(lock, flags);
	if (!list_empty(&pq->busy.list)) {
		list_del_init(&pq->busy.list);
		pq->busy.lock = NULL;
	}
	write_sequnlock_irqrestore(lock, flags);
}

int hfi2_user_sdma_free_queues(struct hfi2_filedata *fd,
			       struct hfi2_ctxtdata *uctxt)
{
	struct hfi2_user_sdma_pkt_q *pq;

	trace_hfi2_sdma_user_free_queues(uctxt->dd, uctxt->ctxt, fd->subctxt);

	spin_lock(&fd->pq_rcu_lock);
	pq = srcu_dereference_check(fd->pq, &fd->pq_srcu,
				    lockdep_is_held(&fd->pq_rcu_lock));
	if (pq) {
		rcu_assign_pointer(fd->pq, NULL);
		spin_unlock(&fd->pq_rcu_lock);
		synchronize_srcu(&fd->pq_srcu);
		/* at this point there can be no more new requests */
		iowait_sdma_drain(&pq->busy);
		/* Wait until all requests have been freed. */
		wait_event(pq->wait, !atomic_read(&pq->n_reqs));
		kfree(pq->reqs);
		hfi2_free_pinning_interfaces(pq);
		bitmap_free(pq->req_in_use);
		kmem_cache_destroy(pq->txreq_cache);
		flush_pq_iowait(pq);
		kfree(pq);
	} else {
		spin_unlock(&fd->pq_rcu_lock);
	}
	if (fd->cq) {
		vfree(fd->cq->comps);
		kfree(fd->cq);
		fd->cq = NULL;
	}
	return 0;
}

static u8 dlid_to_selector(u16 dlid)
{
	static u8 mapping[256];
	static int initialized;
	static u8 next;
	int hash;

	if (!initialized) {
		memset(mapping, 0xFF, 256);
		initialized = 1;
	}

	hash = ((dlid >> 8) ^ dlid) & 0xFF;
	if (mapping[hash] == 0xFF) {
		mapping[hash] = next;
		next = (next + 1) & 0x7F;
	}

	return mapping[hash];
}

/* return the data length expressed in the template LRH */
static inline u32 template_data_len(struct user_sdma_request *req)
{
	u32 len;

	if (req->is16b) {
		/*
		 * The incoming LRH template length is:
		 *   lrh_len = header_len + data_len + ICRC
		 *   => data_len = lrh_len - header_len - ICRC
		 *   header_len = req->hsize - sizeof(PBC)
		 *   ICRC = 8 bytes (for 16B packets)
		 */
		len = req->lrh_len_bytes - (req->hsize - sizeof(req->h.pbc)) -
		      8;
	} else {
		/*
		 * The minimum representable packet data length in a
		 * header is 4 bytes, therefore, when the data length
		 * request is less than 4 bytes, there's only one
		 * packet, and the packet data length is equal to that
		 * of the request data length.
		 */
		if (req->data_len < sizeof(u32))
			len = req->data_len;
		else
			len = (req->lrh_len_bytes - (req->hsize - 4));
	}

	return len;
}

/*
 * Decide if the PBC is for 9B or 16B packets.
 * Expect the PBC to be endianized for the CPU.
 *
 * Return:
 * 0       - PBC is 9B or 16B, set is_16b accordingly
 * -EINVAL - PBC not 9B or 16B
 *
 */
static int check_pbc_16b(struct hfi2_devdata *dd, u64 pbc, bool *is_16b)
{
	u32 l2type;

	if (dd->params->chip_type == CHIP_WFR) {
		/* WFR: if bypass is set, it is 16B, else 9B */
		*is_16b = !!(pbc & PBC_PACKET_BYPASS);
		return 0;
	}

	/* JKR and beyond */
	l2type = (pbc >> PBC_L2_TYPE_SHIFT) & 0x3;
	if (l2type == PBC_L2_16B) {
		*is_16b = true;
		return 0;
	}
	if (l2type == PBC_L2_9B) {
		*is_16b = false;
		return 0;
	}
	/* unexpected l2 type */

	return -EINVAL;
}

/**
 * hfi2_user_sdma_process_request() - Process and start a user sdma request
 * @fd: valid file descriptor
 * @iovec: array of io vectors to process
 * @dim: overall iovec array size
 * @count: number of io vector array entries processed
 */
int hfi2_user_sdma_process_request(struct hfi2_filedata *fd,
				   struct iovec *iovec, unsigned long dim,
				   unsigned long *count)
{
	int ret = 0, i;
	struct hfi2_ctxtdata *uctxt = fd->uctxt;
	struct hfi2_pportdata *ppd = uctxt->ppd;
	struct hfi2_user_sdma_pkt_q *pq =
		srcu_dereference(fd->pq, &fd->pq_srcu);
	struct hfi2_user_sdma_comp_q *cq = fd->cq;
	struct hfi2_devdata *dd = pq->dd;
	unsigned long idx = 0;
	u8 pcount = initial_pkt_count;
	struct sdma_req_info info;
	u64 cpu_pbc;
	unsigned long rsize;
	void *rhdr; /* header remainder */
	__be32 *bth;
	struct hfi2_kdeth_header *kdeth;
	bool lrh_grh;

	/*
	 * For PSM2-CUDA backwards compatibility.
	 * Start by assuming u16 .flags not present.
	 * Add sizeof(u16) back once determined that .flags is present.
	 */
	int infosz = sizeof(struct sdma_req_info);
	struct user_sdma_request *req;
	size_t header_offset;
	u8 opcode, sc, vl;
	u16 pkey;
	u32 slid;
	u16 dlid;
	u32 selector;

	if (iovec[idx].iov_len < infosz) {
		hfi2_cdbg(
			SDMA,
			"[%u:%u:%u] First vector not big enough for info %lu/%u",
			dd->unit, uctxt->ctxt, fd->subctxt, iovec[idx].iov_len,
			infosz);
		return -EINVAL;
	}
	ret = copy_from_user(&info, iovec[idx].iov_base, sizeof(info));
	if (ret) {
		hfi2_cdbg(SDMA, "[%u:%u:%u] Failed to copy info QW (%d)",
			  dd->unit, uctxt->ctxt, fd->subctxt, ret);
		return -EFAULT;
	}

	trace_hfi2_sdma_user_reqinfo(dd, uctxt->ctxt, fd->subctxt,
				     (u16 *)&info);
	if (info.comp_idx >= hfi2_sdma_comp_ring_size) {
		hfi2_cdbg(SDMA, "[%u:%u:%u:%u] Invalid comp index", dd->unit,
			  uctxt->ctxt, fd->subctxt, info.comp_idx);
		return -EINVAL;
	}

	/*
	 * Sanity check the header io vector count.  Need at least 1 vector
	 * (header) and cannot be larger than the actual io vector count.
	 */
	if (req_iovcnt(info.ctrl) < 1 || req_iovcnt(info.ctrl) > dim) {
		hfi2_cdbg(SDMA, "[%u:%u:%u:%u] Invalid iov count %d, dim %ld",
			  dd->unit, uctxt->ctxt, fd->subctxt, info.comp_idx,
			  req_iovcnt(info.ctrl), dim);
		return -EINVAL;
	}

	if (!info.fragsize) {
		hfi2_cdbg(SDMA,
			  "[%u:%u:%u:%u] Request does not specify fragsize",
			  dd->unit, uctxt->ctxt, fd->subctxt, info.comp_idx);
		return -EINVAL;
	}

	/* Try to claim the request. */
	if (test_and_set_bit(info.comp_idx, pq->req_in_use)) {
		hfi2_cdbg(SDMA, "[%u:%u:%u] Entry %u is in use", dd->unit,
			  uctxt->ctxt, fd->subctxt, info.comp_idx);
		return -EBADSLT;
	}
	/*
	 * All safety checks have been done and this request has been claimed.
	 */
	trace_hfi2_sdma_user_process_request(dd, uctxt->ctxt, fd->subctxt,
					     info.comp_idx);
	req = pq->reqs + info.comp_idx;
	req->data_iovs = req_iovcnt(info.ctrl) - 1; /* subtract header vector */
	req->data_len = 0;
	req->pq = pq;
	req->cq = cq;
	req->ahg_idx = -1;
	req->iov_idx = 0;
	req->sent = 0;
	req->seqnum = 0;
	req->seqcomp = 0;
	req->seqsubmitted = 0;
	req->tids = NULL;
	req->has_error = 0;
	req->hsize = 0; /* set below */
	req->lrh_len_bytes = 0; /* set below */
	req->pad_mask = 0; /* set below */
	req->tailsize = 0; /* set below */
	req->is16b = false; /* set below */
	INIT_LIST_HEAD(&req->txps);
	req->n_pinrefs = 0;
	req->pinref_seqnum = 0;

	memcpy(&req->info, &info, sizeof(info));

	/* The request is initialized, count it */
	atomic_inc(&pq->n_reqs);

	if (req_opcode(info.ctrl) == EXPECTED) {
		/* expected must have a TID info and at least one data vector */
		if (req->data_iovs < 2) {
			SDMA_DBG(
				req,
				"Not enough vectors for expected request: 0x%x",
				info.ctrl);
			ret = -EINVAL;
			goto free_req;
		}
		req->data_iovs--;
	}

	if (!info.npkts || req->data_iovs > ARRAY_SIZE(req->iovs)) {
		SDMA_DBG(req, "Too many vectors (%u/%u)", req->data_iovs,
			 (u32)ARRAY_SIZE(req->iovs));
		ret = -EINVAL;
		goto free_req;
	}

	if (req_has_meminfo(info.ctrl)) {
		/* Copy the meminfo from the user buffer */
		if (iovec[idx].iov_len < infosz + sizeof(req->meminfo)) {
			SDMA_DBG(
				req,
				"First vector not big enough for meminfo %lu/%lu",
				iovec[idx].iov_len,
				infosz + sizeof(req->meminfo));
			ret = -EINVAL;
			goto free_req;
		}
		ret = copy_from_user(&req->meminfo,
				     iovec[idx].iov_base + sizeof(info),
				     sizeof(req->meminfo));
		if (ret) {
			SDMA_DBG(req, "Failed to copy meminfo (%d)", ret);
			ret = -EFAULT;
			goto free_req;
		}
		header_offset = sizeof(info) + sizeof(req->meminfo);
	} else {
		req->meminfo.types = 0;
		header_offset = sizeof(info);
	}

	/* copy in the PBC to find the packet type: 9B or 16B */
	if (iovec[idx].iov_len < header_offset + sizeof(req->h.pbc)) {
		SDMA_DBG(req, "First vector not big enough for pbc %lu/%lu",
			 iovec[idx].iov_len,
			 header_offset + sizeof(req->h.pbc));
		ret = -EINVAL;
		goto free_req;
	}
	ret = copy_from_user(&req->h.pbc, iovec[idx].iov_base + header_offset,
			     sizeof(req->h.pbc));
	if (ret) {
		SDMA_DBG(req, "Failed to copy header template pbc (%d)", ret);
		ret = -EFAULT;
		goto free_req;
	}
	header_offset += sizeof(req->h.pbc);

	cpu_pbc = le64_to_cpu(*(__le64 *)req->h.pbc);
	vl = (cpu_pbc >> PBC_VL_SHIFT) & PBC_VL_MASK;

	ret = check_pbc_16b(dd, cpu_pbc, &req->is16b);
	if (ret) {
		SDMA_DBG(req, "Bad header template PBC L2 type");
		goto free_req;
	}
	if (req->is16b) {
		/*
		 * 16B not supported for WFR expected - all code here assumes
		 * the ICRC QW does not land in memory.  Would require
		 * coordination between library and driver.
		 */
		if (dd->params->chip_type == CHIP_WFR &&
		    req_opcode(req->info.ctrl) == EXPECTED) {
			SDMA_DBG(req, "WFR expected not supported");
			ret = -EOPNOTSUPP;
			goto free_req;
		}

		/* extra appended by the driver */
		req->tailsize = 8; /* ICRC QW size */

		req->hsize = sizeof(req->h.hdr16b);
		rhdr = req->h.hdr16b.lrh; /* remainder of header */
		bth = req->h.hdr16b.bth;
		kdeth = &req->h.hdr16b.kdeth;
	} else {
		req->hsize = sizeof(req->h.hdr9b);
		rhdr = req->h.hdr9b.lrh; /* remainder of header */
		bth = req->h.hdr9b.bth;
		kdeth = &req->h.hdr9b.kdeth;
	}
	rsize = req->hsize - sizeof(req->h.pbc); /* header remainder size */

	/* copy in rest of header: LRH (9B or 16B), BTH, and KDETH */
	if (iovec[idx].iov_len < header_offset + rsize) {
		SDMA_DBG(req, "First vector not big enough for header %lu/%lu",
			 iovec[idx].iov_len, header_offset + rsize);
		ret = -EINVAL;
		goto free_req;
	}
	ret = copy_from_user(rhdr, iovec[idx].iov_base + header_offset, rsize);
	if (ret) {
		SDMA_DBG(req, "Failed to copy header template (%d)", ret);
		ret = -EFAULT;
		goto free_req;
	}

	if (req->is16b) {
		struct hfi2_16b_header *hdr = rhdr;

		if (hfi2_16B_get_l2(hdr) != PBC_L2_16B) {
			SDMA_DBG(req, "Non-matching L2 (%d)",
				 hfi2_16B_get_l2(hdr));
			ret = -EINVAL;
			goto free_req;
		}

		sc = hfi2_16B_get_sc(hdr);
		slid = hfi2_16B_get_slid(hdr);
		dlid = hfi2_16B_get_dlid(hdr);
		lrh_grh = hfi2_16B_get_l4(hdr) == OPA_16B_L4_IB_GLOBAL;
		req->lrh_len_bytes = hfi2_16B_get_len(hdr) << 3;
		req->pad_mask = 0x7; /* round up to 8 bytes */
	} else {
		struct ib_header *hdr = rhdr;
		bool sc4 = (le16_to_cpu(req->h.hdr9b.pbc[1]) >> 14) & 0x1;

		/* sanitize the pbc if no rate control */
		if (!HFI2_CAP_IS_USET(STATIC_RATE_CTRL))
			req->h.hdr9b.pbc[2] = 0;

		sc = hfi2_9B_get_sc5(hdr, sc4);
		slid = ib_get_slid(hdr);
		dlid = ib_get_dlid(hdr);
		lrh_grh = ib_get_lnh(hdr) == HFI2_LRH_GRH;
		req->lrh_len_bytes = ib_get_len(hdr) << 2;
		req->pad_mask = 0x3; /* round up to 4 bytes */
	}

	/* Validate the opcode. Do not trust packets from user space blindly. */
	opcode = (be32_to_cpu(bth[0]) >> 24) & 0xff;
	if ((opcode & USER_OPCODE_CHECK_MASK) != USER_OPCODE_CHECK_VAL) {
		SDMA_DBG(req, "Invalid opcode (%d)", opcode);
		ret = -EINVAL;
		goto free_req;
	}
	/*
	 * Validate the vl. Do not trust packets from user space blindly.
	 * VL comes from PBC, SC comes from LRH, and the VL needs to
	 * match the SC look up.
	 */
	if (vl >= ppd->vls_operational || vl != sc_to_vlt(ppd, sc)) {
		SDMA_DBG(req, "Invalid SC(%u)/VL(%u)", sc, vl);
		ret = -EINVAL;
		goto free_req;
	}

	/* Checking P_KEY for requests from user-space */
	pkey = (u16)be32_to_cpu(bth[0]);
	if (hfi2_egress_pkey_check(ppd, slid, pkey, sc, PKEY_CHECK_INVALID)) {
		ret = -EINVAL;
		SDMA_DBG(req, "P_KEY check failed\n");
		goto free_req;
	}

	/*
	 * Also should check the BTH.lnh. If it says the next header is GRH then
	 * the RXE parsing will be off and will land in the middle of the KDETH
	 * or miss it entirely.
	 */
	if (lrh_grh) {
		SDMA_DBG(req, "User tried to pass in a GRH");
		ret = -EINVAL;
		goto free_req;
	}

	req->koffset = le32_to_cpu(kdeth->swdata[6]);
	/*
	 * Calculate the initial TID offset based on the values of
	 * KDETH.OFFSET and KDETH.OM that are passed in.
	 */
	req->tidoffset = KDETH_GET(kdeth->ver_tid_offset, OFFSET) *
			 (KDETH_GET(kdeth->ver_tid_offset, OM) ?
				  KDETH_OM_LARGE :
				  KDETH_OM_SMALL);
	trace_hfi2_sdma_user_initial_tidoffset(dd, uctxt->ctxt, fd->subctxt,
					       info.comp_idx, req->tidoffset);
	idx++;

	/* Save all the IO vector structures */
	for (i = 0; i < req->data_iovs; i++) {
		req->iovs[i].type =
			HFI2_MEMINFO_TYPE_ENTRY_GET(req->meminfo.types, i);
		if (!pinning_type_supported(req->iovs[i].type)) {
			SDMA_DBG(req, "Pinning type not supported: %u\n",
				 req->iovs[i].type);
			req->data_iovs = i;
			ret = -EINVAL;
			goto free_req;
		}
		req->iovs[i].context = req->meminfo.context[i];
		req->iovs[i].offset = 0;
		memcpy(&req->iovs[i].iov, iovec + idx++,
		       sizeof(req->iovs[i].iov));
		if (req->iovs[i].iov.iov_len == 0) {
			ret = -EINVAL;
			goto free_req;
		}
		req->data_len += req->iovs[i].iov.iov_len;
	}
	trace_hfi2_sdma_user_data_length(dd, uctxt->ctxt, fd->subctxt,
					 info.comp_idx, req->data_len);
	/* reject if not enough data provided for the template */
	if (req->data_len < template_data_len(req)) {
		ret = -EINVAL;
		goto free_req;
	}
	if (pcount > req->info.npkts)
		pcount = req->info.npkts;
	/*
	 * Copy any TID info
	 * User space will provide the TID info only when the
	 * request type is EXPECTED. This is true even if there is
	 * only one packet in the request and the header is already
	 * setup. The reason for the singular TID case is that the
	 * driver needs to perform safety checks.
	 */
	if (req_opcode(req->info.ctrl) == EXPECTED) {
		u16 ntids = iovec[idx].iov_len / sizeof(*req->tids);
		u32 *tmp;

		if (!ntids || ntids > MAX_TID_PAIR_ENTRIES) {
			ret = -EINVAL;
			goto free_req;
		}

		/*
		 * We have to copy all of the tids because they may vary
		 * in size and, therefore, the TID count might not be
		 * equal to the pkt count. However, there is no way to
		 * tell at this point.
		 */
		tmp = memdup_array_user(iovec[idx].iov_base, ntids,
					sizeof(*req->tids));
		if (IS_ERR(tmp)) {
			ret = PTR_ERR(tmp);
			SDMA_DBG(req, "Failed to copy %d TIDs (%pe)", ntids,
				 tmp);
			goto free_req;
		}
		req->tids = tmp;
		req->n_tids = ntids;
		req->tididx = 0;
		idx++;
	}

	selector = dlid_to_selector(dlid);
	selector += uctxt->ctxt + fd->subctxt;
	req->sde = hfi2_sdma_select_user_engine(ppd, selector, vl);

	if (!req->sde || !sdma_running(req->sde)) {
		ret = -ECOMM;
		goto free_req;
	}

	/* We don't need an AHG entry if the request contains only one packet */
	if (req->info.npkts > 1 && HFI2_CAP_IS_USET(SDMA_AHG))
		req->ahg_idx = hfi2_sdma_ahg_alloc(req->sde);

	set_comp_state(pq, cq, info.comp_idx, QUEUED, 0);
	pq->state = SDMA_PKT_Q_ACTIVE;

	/*
	 * This is a somewhat blocking send implementation.
	 * The driver will block the caller until all packets of the
	 * request have been submitted to the SDMA engine. However, it
	 * will not wait for send completions.
	 */
	while (req->seqsubmitted != req->info.npkts) {
		ret = user_sdma_send_pkts(req, pcount);
		if (ret < 0) {
			int we_ret;

			if (ret != -EBUSY)
				goto free_req;
			we_ret = wait_event_interruptible_timeout(
				pq->busy.wait_dma,
				pq->state == SDMA_PKT_Q_ACTIVE,
				msecs_to_jiffies(SDMA_IOWAIT_TIMEOUT));
			trace_hfi2_usdma_we(pq, we_ret);
			if (we_ret <= 0)
				flush_pq_iowait(pq);
		}
	}
	*count += idx;
	return 0;
free_req:
	/*
	 * If the submitted seqsubmitted == npkts, the completion routine
	 * controls the final state.  If sequbmitted < npkts, wait for any
	 * outstanding packets to finish before cleaning up.
	 */
	if (req->seqsubmitted < req->info.npkts) {
		if (req->seqsubmitted)
			wait_event(pq->busy.wait_dma,
				   (req->seqcomp == req->seqsubmitted - 1));
		user_sdma_free_request(req);
		pq_update(pq);
		set_comp_state(pq, cq, info.comp_idx, ERROR, ret);
	}
	return ret;
}

/*
 * Determine the proper size of the packet data.
 *
 * The size of the data of the first packet is in the header template.
 *
 * The size of the remaining packets is the minimum of the frag size (MTU)
 * or remaining data in the request.
 */
static inline u32 compute_data_length(struct user_sdma_request *req)
{
	u32 len;

	if (!req->seqnum) {
		len = template_data_len(req);
	} else if (req_opcode(req->info.ctrl) == EXPECTED) {
		u32 tidlen = EXP_TID_GET(req->tids[req->tididx], LEN) *
			     EXP_TID_ADDR_SIZE;
		/*
		 * Get the data length based on the remaining space in the
		 * TID pair.
		 */
		len = min(tidlen - req->tidoffset, (u32)req->info.fragsize);
		/* If we've filled up the TID pair, move to the next one. */
		if (unlikely(!len) && ++req->tididx < req->n_tids &&
		    req->tids[req->tididx]) {
			tidlen = EXP_TID_GET(req->tids[req->tididx], LEN) *
				 EXP_TID_ADDR_SIZE;
			req->tidoffset = 0;
			len = min_t(u32, tidlen, req->info.fragsize);
		}
		/*
		 * Since the TID pairs map entire pages, make sure that we
		 * are not going to try to send more data that we have
		 * remaining.
		 */
		len = min(len, req->data_len - req->sent);
	} else {
		len = min(req->data_len - req->sent, (u32)req->info.fragsize);
	}
	trace_hfi2_sdma_user_compute_length(req->pq->dd, req->pq->ctxt,
					    req->pq->subctxt,
					    req->info.comp_idx, len);
	return len;
}

static inline u32 pad_len(struct user_sdma_request *req, u32 len)
{
	return (len + req->pad_mask) & ~req->pad_mask;
}

/*
 * Return in bytes the size to be put into the LRH
 */
static inline u32 get_lrh_len(struct user_sdma_request *req, u32 len)
{
	if (req->is16b) {
		/* header - PBC + data length + trailing ICRC QW */
		return req->hsize - sizeof(u64) + len + 8;
	}

	/* (Size of complete header - size of PBC) + 4B ICRC + data length */
	return req->hsize - sizeof(u64) + 4 + len;
}

/*
 * Convert a PBC length (DW) to an LRH length (bytes).  Important: incoming
 * PBC length is whole bottom of PBC, be sure to mask.
 */
static inline u16 pbc2lrh(struct user_sdma_request *req, u16 pbclen)
{
	if (req->is16b) {
		/*
		 * 16B: Both PBC and LRH include QW ICRC, just subtract off PBC.
		 */
		return ((pbclen & 0xfff) - 2) << 2;
	}
	/* 9B: len - PBC + ICRC */
	return ((pbclen & 0xfff) - 2 + 1) << 2;
}

/* convert a LRH length (bytes) to a PBC length (DW) */
static inline u16 lrh2pbc(struct user_sdma_request *req, u16 lrhlen)
{
	if (req->is16b) {
		/* 16B: to DW, ICRC QW already included, add 2 for PBC */
		return ((lrhlen >> 2) + 2) & 0xfff;
	}
	/*
	 * 9B: to DW, len includes ICRC use that for half of PBC, add 1 for
	 * second half of PBC
	 */
	return ((lrhlen >> 2) + 1) & 0xfff;
}

static int user_sdma_txadd_ahg(struct user_sdma_request *req,
			       struct user_sdma_txreq *tx, u32 datalen)
{
	int ret;
	u16 pbclen = le16_to_cpu(req->h.pbc[0]);
	u32 lrhlen = get_lrh_len(req, pad_len(req, datalen));
	struct hfi2_user_sdma_pkt_q *pq = req->pq;

	/*
	 * Copy the request header into the tx header
	 * because the HW needs a cacheline-aligned
	 * address.
	 * This copy can be optimized out if the hdr
	 * member of user_sdma_request were also
	 * cacheline aligned.
	 */
	memcpy(&tx->h, &req->h, req->hsize);
	if (pbc2lrh(req, pbclen) != lrhlen) {
		pbclen = (pbclen & 0xf000) | lrh2pbc(req, lrhlen);
		tx->h.pbc[0] = cpu_to_le16(pbclen);
	}
	ret = check_header_template(req, tx, lrhlen, datalen);
	if (ret)
		return ret;
	ret = sdma_txinit_ahg(pq->dd, &tx->txreq, SDMA_TXREQ_F_AHG_COPY,
			      req->hsize + datalen, req->ahg_idx, 0, NULL, 0,
			      user_sdma_txreq_cb);
	if (ret)
		return ret;
	ret = sdma_txadd_kvaddr(pq->dd, &tx->txreq, &tx->h, req->hsize);
	if (ret)
		sdma_txclean(pq->dd, &tx->txreq);
	return ret;
}

static void free_pinref(struct user_sdma_pinref *d)
{
	hfi2_pinning_interfaces[d->memtype].put(d->ptr);
}

static void free_pinrefs(struct user_sdma_pinref *pinrefs, u16 n_pinrefs)
{
	u16 i;

	if (!pinrefs)
		return;
	for (i = 0; i < n_pinrefs; i++)
		free_pinref(&pinrefs[i]);
}

/*
 * Evict + free a pinref last-used by a now-completed packet (user_sdma_txreq).
 */
static inline int evict_complete_pinref(struct user_sdma_request *req)
{
	u16 i;

	for (i = 0; i < req->n_pinrefs; i++) {
		if (req->pinrefs[i].req_seqnum > req->seqcomp)
			continue;
		free_pinref(&req->pinrefs[i]);
		req->pinrefs[i].ptr = NULL;
		req->n_pinrefs--;
		return i;
	}

	return -ENOMEM;
}

/*
 * @return:
 * * 0 on success
 * * -EINVAL or -ENOMEM on error
 */
static int request_add_ref(struct user_sdma_request *req, void *ptr,
			   u16 memtype)
{
	int ret;
	u16 i;

	if (req->n_pinrefs >= ARRAY_SIZE(req->pinrefs)) {
		ret = evict_complete_pinref(req);
		if (ret < 0)
			return ret;
		i = (u16)ret;
	} else {
		i = req->n_pinrefs;
	}
	req->n_pinrefs++;
	req->pinrefs[i].ptr = ptr;
	req->pinrefs[i].memtype = memtype;
	req->pinrefs[i].req_seqnum = req->seqnum;
	req->pinrefs[i].pinref_seqnum = req->pinref_seqnum++;
	return 0;
}

/*
 * Add pinref made from (@ptr,@put) to @tx->pinrefs.
 */
static int txreq_add_ref(struct user_sdma_txreq *tx, void *ptr, u16 memtype)
{
	u16 i;

	/* Should not happen; internal error, check just in case */
	if (WARN_ON_ONCE(tx->n_pinrefs >= MAX_DESC))
		return -ENOMEM;
	if (!tx->pinrefs) {
		size_t bytes;

		bytes = array_size(MAX_DESC, sizeof(tx->pinrefs[0]));
		tx->pinrefs = kmalloc(bytes, GFP_KERNEL);
		if (!tx->pinrefs)
			return -ENOMEM;
	}
	i = tx->n_pinrefs++;
	tx->pinrefs[i].ptr = ptr;
	tx->pinrefs[i].memtype = memtype;
	/* .req_seqnum and .pinref_seqnum are N/A for pinrefs in tx->pinrefs */
	return 0;
}

/**
 * hfi2_user_sdma_add_ref - Try adding @ptr to @tx->req->pinrefs first but
 * add to @tx->pinrefs if there is no space in @tx->req->pinrefs.
 * @tx: user SDMA transmit request
 * @ptr: pinned memory reference to add
 * @memtype: memory type of @ptr
 */
int hfi2_user_sdma_add_ref(struct user_sdma_txreq *tx, void *ptr, u16 memtype)
{
	int ret;

	/* Should not happen; internal error, check just in case */
	if (WARN_ON_ONCE(!ptr || !pinning_type_supported(memtype)))
		return -EINVAL;
	if (WARN_ON_ONCE(!hfi2_pinning_interfaces[memtype].put))
		return -EINVAL;
	ret = request_add_ref(tx->req, ptr, memtype);
	if (!ret)
		return 0;
	if (ret != -ENOMEM)
		return ret;
	/* No space in req->pinrefs and no entries could be evicted */
	return txreq_add_ref(tx, ptr, memtype);
}

/**
 * hfi2_user_sdma_mru_ref - Get most-recently used pinref for @memtype from
 * @tx->req. Most-recent determined by &user_sdma_pinref->pinref_seqnum.
 * @tx: user SDMA transmit request
 * @memtype: memory type to search for
 *
 * Return: most-recently used pinref for @memtype, or NULL if none found.
 */
struct user_sdma_pinref *hfi2_user_sdma_mru_ref(struct user_sdma_txreq *tx,
						u16 memtype)
{
	struct user_sdma_request *req = tx->req;
	struct user_sdma_pinref *mru = NULL;
	u16 i;

	for (i = 0; i < req->n_pinrefs; i++) {
		if (req->pinrefs[i].memtype != memtype)
			continue;
		if (!mru || req->pinrefs[i].pinref_seqnum > mru->pinref_seqnum)
			mru = &req->pinrefs[i];
	}

	return mru;
}

/*
 * Update @d->req_seqnum and @d->pinref_seqnum from @tx->req.
 */
void hfi2_user_sdma_touch_ref(struct user_sdma_txreq *tx,
			      struct user_sdma_pinref *d)
{
	d->req_seqnum = tx->req->seqnum;
	d->pinref_seqnum = tx->req->pinref_seqnum++;
}

static void user_sdma_free_txreq(struct user_sdma_txreq *tx)
{
	free_pinrefs(tx->pinrefs, tx->n_pinrefs);
	kfree(tx->pinrefs);
	kmem_cache_free(tx->req->pq->txreq_cache, tx);
}

static int user_sdma_send_pkts(struct user_sdma_request *req, u16 maxpkts)
{
	int ret = 0;
	u16 count;
	unsigned npkts = 0;
	struct user_sdma_txreq *tx = NULL;
	struct hfi2_user_sdma_pkt_q *pq;
	struct hfi2_devdata *dd;
	struct user_sdma_iovec *iovec = NULL;

	if (!req->pq)
		return -EINVAL;

	pq = req->pq;
	dd = pq->dd;

	/* If tx completion has reported an error, we are done. */
	if (READ_ONCE(req->has_error))
		return -EFAULT;

	/*
	 * Check if we might have sent the entire request already
	 */
	if (unlikely(req->seqnum == req->info.npkts)) {
		if (!list_empty(&req->txps))
			goto dosend;
		return ret;
	}

	if (!maxpkts || maxpkts > req->info.npkts - req->seqnum)
		maxpkts = req->info.npkts - req->seqnum;

	while (npkts < maxpkts) {
		u32 datalen = 0;

		/*
		 * Check whether any of the completions have come back
		 * with errors. If so, we are not going to process any
		 * more packets from this request.
		 */
		if (READ_ONCE(req->has_error))
			return -EFAULT;

		tx = kmem_cache_alloc(pq->txreq_cache, GFP_KERNEL);
		if (!tx)
			return -ENOMEM;
		tx->pinrefs = NULL;
		tx->n_pinrefs = 0;
		tx->flags = 0;
		tx->req = req;
		/*
		 * For the last packet set the ACK request
		 * and disable header suppression.
		 */
		if (req->seqnum == req->info.npkts - 1)
			tx->flags |= (TXREQ_FLAGS_REQ_ACK |
				      TXREQ_FLAGS_REQ_DISABLE_SH);

		/*
		 * Calculate the payload size - this is min of the fragment
		 * (MTU) size or the remaining bytes in the request but only
		 * if we have payload data.
		 */
		if (req->data_len) {
			iovec = &req->iovs[req->iov_idx];
			if (READ_ONCE(iovec->offset) == iovec->iov.iov_len) {
				if (++req->iov_idx == req->data_iovs) {
					ret = -EFAULT;
					goto free_tx;
				}
				iovec = &req->iovs[req->iov_idx];
				WARN_ON(iovec->offset);
			}

			datalen = compute_data_length(req);

			/*
			 * Disable header suppression for the payload <= 8DWS.
			 * If there is an uncorrectable error in the receive
			 * data FIFO when the received payload size is less than
			 * or equal to 8DWS then the RxDmaDataFifoRdUncErr is
			 * not reported.There is set RHF.EccErr if the header
			 * is not suppressed.
			 */
			if (!datalen) {
				SDMA_DBG(req,
					 "Request has data but pkt len is 0");
				ret = -EFAULT;
				goto free_tx;
			} else if (datalen <= 32) {
				tx->flags |= TXREQ_FLAGS_REQ_DISABLE_SH;
			}
		}

		/*
		 * protocol decision: provider must give QWORD aligned data
		 * for 16B (as opposed to driver padding missing bytes)
		 */
		if (req->is16b && (datalen & req->pad_mask)) {
			SDMA_DBG(req, "16B packet size %u not QWORD aligned\n",
				 datalen);
			ret = -EINVAL;
			goto free_tx;
		}

		if (req->ahg_idx >= 0) {
			if (!req->seqnum) {
				ret = user_sdma_txadd_ahg(req, tx, datalen);
				if (ret)
					goto free_tx;
			} else {
				int changes;

				changes =
					set_txreq_header_ahg(req, tx, datalen);
				if (changes < 0) {
					ret = changes;
					goto free_tx;
				}
			}
		} else {
			ret = sdma_txinit(dd, &tx->txreq, 0,
					  req->hsize + datalen + req->tailsize,
					  user_sdma_txreq_cb);
			if (ret)
				goto free_tx;
			/*
			 * Modify the header for this packet. This only needs
			 * to be done if we are not going to use AHG. Otherwise,
			 * the HW will do it based on the changes we gave it
			 * during sdma_txinit_ahg().
			 */
			ret = set_txreq_header(req, tx, datalen);
			if (ret)
				goto free_txreq;
		}

		req->koffset += datalen;
		if (req_opcode(req->info.ctrl) == EXPECTED)
			req->tidoffset += datalen;
		req->sent += datalen;
		while (datalen) {
			ret = add_to_sdma_packet(iovec->type, req, tx, iovec,
						 &datalen);
			if (ret)
				goto free_txreq;
			iovec = &req->iovs[req->iov_idx];
		}
		/* 16B requests need to have the ICRC QW added */
		if (req->is16b) {
			ret = sdma_txadd_daddr(dd, &tx->txreq,
					       dd->sdma_pad_phys, 8);
			if (ret)
				goto free_txreq;
		}
		list_add_tail(&tx->txreq.list, &req->txps);
		/*
		 * It is important to increment this here as it is used to
		 * generate the BTH.PSN and, therefore, can't be bulk-updated
		 * outside of the loop.
		 */
		tx->seqnum = req->seqnum++;
		npkts++;
	}
dosend:
	ret = hfi2_sdma_send_txlist(req->sde, iowait_get_ib_work(&pq->busy),
				    &req->txps, &count);
	req->seqsubmitted += count;
	if (req->seqsubmitted == req->info.npkts) {
		/*
		 * The txreq has already been submitted to the HW queue
		 * so we can free the AHG entry now. Corruption will not
		 * happen due to the sequential manner in which
		 * descriptors are processed.
		 */
		if (req->ahg_idx >= 0)
			hfi2_sdma_ahg_free(req->sde, req->ahg_idx);
	}
	return ret;

free_txreq:
	sdma_txclean(dd, &tx->txreq);
free_tx:
	user_sdma_free_txreq(tx);
	return ret;
}

static int check_header_template(struct user_sdma_request *req,
				 struct user_sdma_txreq *tx, u32 lrhlen,
				 u32 datalen)
{
	/*
	 * Perform safety checks for any type of packet:
	 *    - transfer size is multiple of 64bytes
	 *    - packet length is multiple of pad_mask+1 (4 or 8) bytes
	 *    - packet length is not larger than MTU size
	 *
	 * These checks are only done for the first packet of the
	 * transfer since the header is "given" to us by user space.
	 * For the remainder of the packets we compute the values.
	 */
	if (req->info.fragsize % PIO_BLOCK_SIZE || lrhlen & req->pad_mask ||
	    lrhlen > get_lrh_len(req, req->info.fragsize))
		return -EINVAL;

	if (req_opcode(req->info.ctrl) == EXPECTED) {
		/*
		 * The header is checked only on the first packet. Furthermore,
		 * we ensure that at least one TID entry is copied when the
		 * request is submitted. Therefore, we don't have to verify that
		 * tididx points to something sane.
		 */
		u32 tidval = req->tids[req->tididx],
		    tidlen = EXP_TID_GET(tidval, LEN) * EXP_TID_ADDR_SIZE,
		    tididx = EXP_TID_GET(tidval, IDX),
		    tidctrl = EXP_TID_GET(tidval, CTRL), tidoff;
		__le32 kval;
		struct hfi2_kdeth_header *kdeth;

		if (req->is16b) {
			kval = tx->h.hdr16b.kdeth.ver_tid_offset;
			kdeth = &req->h.hdr16b.kdeth;
		} else {
			kval = tx->h.hdr9b.kdeth.ver_tid_offset;
			kdeth = &req->h.hdr9b.kdeth;
		}
		tidoff = KDETH_GET(kval, OFFSET) *
			 (KDETH_GET(kdeth->ver_tid_offset, OM) ?
				  KDETH_OM_LARGE :
				  KDETH_OM_SMALL);
		/*
		 * Expected receive packets have the following
		 * additional checks:
		 *     - offset is not larger than the TID size
		 *     - TIDCtrl values match between header and TID array
		 *     - TID indexes match between header and TID array
		 */
		if ((tidoff + datalen > tidlen) ||
		    KDETH_GET(kval, TIDCTRL) != tidctrl ||
		    KDETH_GET(kval, TID) != tididx)
			return -EINVAL;
	}
	return 0;
}

/*
 * Correctly set the BTH.PSN field based on type of
 * transfer - eager packets can just increment the PSN but
 * expected packets encode generation and sequence in the
 * BTH.PSN field so just incrementing will result in errors.
 */
static inline u32 set_pkt_bth_psn(__be32 bthpsn, u8 expct, u32 frags)
{
	u32 val = be32_to_cpu(bthpsn),
	    mask = (HFI2_CAP_IS_KSET(EXTENDED_PSN) ? 0x7fffffffull :
						     0xffffffull),
	    psn = val & mask;
	if (expct)
		psn = (psn & ~HFI2_KDETH_BTH_SEQ_MASK) |
		      ((psn + frags) & HFI2_KDETH_BTH_SEQ_MASK);
	else
		psn = psn + frags;
	return psn & mask;
}

/* set the length field of a 16B LRH header */
static inline void hfi2_16B_set_len(__le32 *lrh, u32 len)
{
	u32 value;

	value = le32_to_cpu(lrh[0]);
	value &= ~OPA_16B_LEN_MASK;
	value |= OPA_16B_LEN_MASK & (len << OPA_16B_LEN_SHIFT);
	lrh[0] = cpu_to_le32(value);
}

static int set_txreq_header(struct user_sdma_request *req,
			    struct user_sdma_txreq *tx, u32 datalen)
{
	struct hfi2_user_sdma_pkt_q *pq = req->pq;
	struct hfi2_kdeth_header *kdeth;
	__be32 *bth;
	u8 omfactor; /* KDETH.OM */
	u16 pbclen;
	int ret;
	u32 tidval = 0, lrhlen = get_lrh_len(req, pad_len(req, datalen));

	/* Copy the header template to the request before modification */
	memcpy(&tx->h, &req->h, req->hsize);

	/*
	 * Check if the PBC and LRH length are mismatched. If so
	 * adjust both in the header.
	 */
	pbclen = le16_to_cpu(tx->h.pbc[0]);
	if (pbc2lrh(req, pbclen) != lrhlen) {
		pbclen = (pbclen & 0xf000) | lrh2pbc(req, lrhlen);
		tx->h.pbc[0] = cpu_to_le16(pbclen);
		if (req->is16b)
			hfi2_16B_set_len(tx->h.hdr16b.lrh, lrhlen >> 3);
		else
			tx->h.hdr9b.lrh[2] = cpu_to_be16(lrhlen >> 2);
		/*
		 * Third packet
		 * This is the first packet in the sequence that has
		 * a "static" size that can be used for the rest of
		 * the packets (besides the last one).
		 */
		if (unlikely(req->seqnum == 2)) {
			/*
			 * From this point on the lengths in both the
			 * PBC and LRH are the same until the last
			 * packet.
			 * Adjust the template so we don't have to update
			 * every packet
			 */
			req->h.pbc[0] = tx->h.pbc[0];
			if (req->is16b)
				hfi2_16B_set_len(req->h.hdr16b.lrh,
						 lrhlen >> 3);
			else
				req->h.hdr9b.lrh[2] = tx->h.hdr9b.lrh[2];
		}
	}
	/*
	 * We only have to modify the header if this is not the
	 * first packet in the request. Otherwise, we use the
	 * header given to us.
	 */
	if (unlikely(!req->seqnum)) {
		ret = check_header_template(req, tx, lrhlen, datalen);
		if (ret)
			return ret;
		goto done;
	}

	if (req->is16b) {
		bth = tx->h.hdr16b.bth;
		kdeth = &tx->h.hdr16b.kdeth;
	} else {
		bth = tx->h.hdr9b.bth;
		kdeth = &tx->h.hdr9b.kdeth;
	}

	bth[2] = cpu_to_be32(set_pkt_bth_psn(
		bth[2], (req_opcode(req->info.ctrl) == EXPECTED), req->seqnum));

	/* Set ACK request on last packet */
	if (unlikely(tx->flags & TXREQ_FLAGS_REQ_ACK))
		bth[2] |= cpu_to_be32(1UL << 31);

	/* Set the new offset */
	kdeth->swdata[6] = cpu_to_le32(req->koffset);
	/* Expected packets have to fill in the new TID information */
	if (req_opcode(req->info.ctrl) == EXPECTED) {
		tidval = req->tids[req->tididx];
		/*
		 * If the offset puts us at the end of the current TID,
		 * advance everything.
		 */
		if ((req->tidoffset) ==
		    (EXP_TID_GET(tidval, LEN) * EXP_TID_ADDR_SIZE)) {
			req->tidoffset = 0;
			/*
			 * Since we don't copy all the TIDs, all at once,
			 * we have to check again.
			 */
			if (++req->tididx > req->n_tids - 1 ||
			    !req->tids[req->tididx]) {
				return -EINVAL;
			}
			tidval = req->tids[req->tididx];
		}
		omfactor = EXP_TID_GET(tidval, LEN) * EXP_TID_ADDR_SIZE >=
					   KDETH_OM_MAX_SIZE ?
				   KDETH_OM_LARGE_SHIFT :
				   KDETH_OM_SMALL_SHIFT;
		/* Set KDETH.TIDCtrl based on value for this TID. */
		KDETH_SET(kdeth->ver_tid_offset, TIDCTRL,
			  EXP_TID_GET(tidval, CTRL));
		/* Set KDETH.TID based on value for this TID */
		KDETH_SET(kdeth->ver_tid_offset, TID, EXP_TID_GET(tidval, IDX));
		/* Clear KDETH.SH when DISABLE_SH flag is set */
		if (unlikely(tx->flags & TXREQ_FLAGS_REQ_DISABLE_SH))
			KDETH_SET(kdeth->ver_tid_offset, SH, 0);
		/*
		 * Set the KDETH.OFFSET and KDETH.OM based on size of
		 * transfer.
		 */
		trace_hfi2_sdma_user_tid_info(pq->dd, pq->ctxt, pq->subctxt,
					      req->info.comp_idx,
					      req->tidoffset,
					      req->tidoffset >> omfactor,
					      omfactor != KDETH_OM_SMALL_SHIFT);
		KDETH_SET(kdeth->ver_tid_offset, OFFSET,
			  req->tidoffset >> omfactor);
		KDETH_SET(kdeth->ver_tid_offset, OM,
			  omfactor != KDETH_OM_SMALL_SHIFT);
	}
done:
	if (req->is16b) {
		trace_hfi2_sdma_user_header16b(pq->dd, pq->ctxt, pq->subctxt,
					       req->info.comp_idx,
					       &tx->h.hdr16b, tidval);
	} else {
		trace_hfi2_sdma_user_header(pq->dd, pq->ctxt, pq->subctxt,
					    req->info.comp_idx, &tx->h.hdr9b,
					    tidval);
	}
	return sdma_txadd_kvaddr(pq->dd, &tx->txreq, &tx->h, req->hsize);
}

static int set_txreq_header_ahg(struct user_sdma_request *req,
				struct user_sdma_txreq *tx, u32 datalen)
{
	u32 ahg[AHG_KDETH_ARRAY_SIZE];
	int idx = 0;
	u8 omfactor; /* KDETH.OM */
	u8 off;
	struct hfi2_user_sdma_pkt_q *pq = req->pq;
	__be32 *bth;
	struct hfi2_kdeth_header *kdeth;
	u16 pbclen = le16_to_cpu(req->h.pbc[0]);
	u32 val32, tidval = 0, lrhlen = get_lrh_len(req, pad_len(req, datalen));
	size_t array_size = ARRAY_SIZE(ahg);

	if (pbc2lrh(req, pbclen) != lrhlen) {
		/* PBC.PbcLengthDWs */
		idx = ahg_header_set(
			ahg, idx, array_size, 0, 0, 12,
			(__force u16)cpu_to_le16(lrh2pbc(req, lrhlen)));
		if (idx < 0)
			return idx;
		/* LRH.PktLen */
		if (req->is16b) {
			idx = ahg_header_set(ahg, idx, array_size, 3, 4, 11,
					     (__force u16)cpu_to_le16(lrhlen >>
								      3));
		} else {
			/* 9B: need the full 16 bits due to byte swap */
			idx = ahg_header_set(ahg, idx, array_size, 3, 0, 16,
					     (__force u16)cpu_to_be16(lrhlen >>
								      2));
		}
		if (idx < 0)
			return idx;
	}

	if (req->is16b) {
		bth = tx->h.hdr16b.bth;
		kdeth = &tx->h.hdr16b.kdeth;
		off = 2; /* BTH and KDETH are 2 DW further in */
	} else {
		bth = tx->h.hdr9b.bth;
		kdeth = &tx->h.hdr9b.kdeth;
		off = 0; /* no extra DW offset */
	}

	/*
	 * Do the common updates
	 */
	/* BTH.PSN and BTH.A */
	val32 = (be32_to_cpu(bth[2]) + req->seqnum) &
		(HFI2_CAP_IS_KSET(EXTENDED_PSN) ? 0x7fffffff : 0xffffff);
	if (unlikely(tx->flags & TXREQ_FLAGS_REQ_ACK))
		val32 |= 1UL << 31;
	idx = ahg_header_set(ahg, idx, array_size, 6 + off, 0, 16,
			     (__force u16)cpu_to_be16(val32 >> 16));
	if (idx < 0)
		return idx;
	idx = ahg_header_set(ahg, idx, array_size, 6 + off, 16, 16,
			     (__force u16)cpu_to_be16(val32 & 0xffff));
	if (idx < 0)
		return idx;
	/* KDETH.Offset */
	idx = ahg_header_set(ahg, idx, array_size, 15 + off, 0, 16,
			     (__force u16)cpu_to_le16(req->koffset & 0xffff));
	if (idx < 0)
		return idx;
	idx = ahg_header_set(ahg, idx, array_size, 15 + off, 16, 16,
			     (__force u16)cpu_to_le16(req->koffset >> 16));
	if (idx < 0)
		return idx;
	if (req_opcode(req->info.ctrl) == EXPECTED) {
		__le16 val;
		u16 tidoff;

		tidval = req->tids[req->tididx];

		/*
		 * If the offset puts us at the end of the current TID,
		 * advance everything.
		 */
		if ((req->tidoffset) ==
		    (EXP_TID_GET(tidval, LEN) * EXP_TID_ADDR_SIZE)) {
			req->tidoffset = 0;
			/*
			 * Since we don't copy all the TIDs, all at once,
			 * we have to check again.
			 */
			if (++req->tididx > req->n_tids - 1 ||
			    !req->tids[req->tididx])
				return -EINVAL;
			tidval = req->tids[req->tididx];
		}
		omfactor = ((EXP_TID_GET(tidval, LEN) * EXP_TID_ADDR_SIZE) >=
			    KDETH_OM_MAX_SIZE) ?
				   KDETH_OM_LARGE_SHIFT :
				   KDETH_OM_SMALL_SHIFT;
		/* KDETH.OM and KDETH.OFFSET (TID) */
		tidoff = ((!!(omfactor - KDETH_OM_SMALL_SHIFT)) << 15) |
			 ((req->tidoffset >> omfactor) & 0x7fff);
		idx = ahg_header_set(ahg, idx, array_size, 7 + off, 0, 16,
				     tidoff);
		if (idx < 0)
			return idx;
		/* KDETH.TIDCtrl, KDETH.TID, KDETH.Intr, KDETH.SH */
		val = cpu_to_le16(((EXP_TID_GET(tidval, CTRL) & 0x3) << 10) |
				  (EXP_TID_GET(tidval, IDX) & 0x3ff));

		if (unlikely(tx->flags & TXREQ_FLAGS_REQ_DISABLE_SH)) {
			val |= cpu_to_le16(
				(KDETH_GET(kdeth->ver_tid_offset, INTR)
				 << AHG_KDETH_INTR_SHIFT));
		} else {
			val |= KDETH_GET(kdeth->ver_tid_offset, SH) ?
				       cpu_to_le16(0x1 << AHG_KDETH_SH_SHIFT) :
				       cpu_to_le16(
					       (KDETH_GET(kdeth->ver_tid_offset,
							  INTR)
						<< AHG_KDETH_INTR_SHIFT));
		}

		idx = ahg_header_set(ahg, idx, array_size, 7 + off, 16, 14,
				     (__force u16)val);
		if (idx < 0)
			return idx;
	}

	trace_hfi2_sdma_user_header_ahg(pq->dd, pq->ctxt, pq->subctxt,
					req->info.comp_idx, req->sde->this_idx,
					req->ahg_idx, ahg, idx, tidval);
	sdma_txinit_ahg(pq->dd, &tx->txreq, SDMA_TXREQ_F_USE_AHG, datalen,
			req->ahg_idx, idx, ahg, req->hsize, user_sdma_txreq_cb);

	return idx;
}

/**
 * user_sdma_txreq_cb() - SDMA tx request completion callback.
 * @txreq: valid sdma tx request
 * @status: success/failure of request
 *
 * Called when the SDMA progress state machine gets notification that
 * the SDMA descriptors for this tx request have been processed by the
 * DMA engine. Called in interrupt context.
 * Only do work on completed sequences.
 */
static void user_sdma_txreq_cb(struct sdma_txreq *txreq, int status)
{
	struct user_sdma_txreq *tx =
		container_of(txreq, struct user_sdma_txreq, txreq);
	struct user_sdma_request *req;
	struct hfi2_user_sdma_pkt_q *pq;
	struct hfi2_user_sdma_comp_q *cq;
	enum hfi2_sdma_comp_state state = COMPLETE;

	if (!tx->req)
		return;

	req = tx->req;
	pq = req->pq;
	cq = req->cq;

	if (status != SDMA_TXREQ_S_OK) {
		SDMA_DBG(req, "SDMA completion with error %d", status);
		WRITE_ONCE(req->has_error, 1);
		state = ERROR;
	}

	req->seqcomp = tx->seqnum;
	user_sdma_free_txreq(tx);

	/* sequence isn't complete?  We are done */
	if (req->seqcomp != req->info.npkts - 1)
		return;

	user_sdma_free_request(req);
	set_comp_state(pq, cq, req->info.comp_idx, state, status);
	pq_update(pq);
}

static inline void pq_update(struct hfi2_user_sdma_pkt_q *pq)
{
	if (atomic_dec_and_test(&pq->n_reqs))
		wake_up(&pq->wait);
}

static void user_sdma_free_request(struct user_sdma_request *req)
{
	if (!list_empty(&req->txps)) {
		struct sdma_txreq *t, *p;

		list_for_each_entry_safe(t, p, &req->txps, list) {
			struct user_sdma_txreq *tx =
				container_of(t, struct user_sdma_txreq, txreq);
			list_del_init(&t->list);
			sdma_txclean(req->pq->dd, t);
			user_sdma_free_txreq(tx);
		}
	}

	free_pinrefs(req->pinrefs, req->n_pinrefs);
	kfree(req->tids);
	clear_bit(req->info.comp_idx, req->pq->req_in_use);
}

static inline void set_comp_state(struct hfi2_user_sdma_pkt_q *pq,
				  struct hfi2_user_sdma_comp_q *cq, u16 idx,
				  enum hfi2_sdma_comp_state state, int ret)
{
	if (state == ERROR)
		cq->comps[idx].errcode = -ret;
	smp_wmb(); /* make sure errcode is visible first */
	cq->comps[idx].status = state;
	trace_hfi2_sdma_user_completion(pq->dd, pq->ctxt, pq->subctxt, idx,
					state, ret);
}
