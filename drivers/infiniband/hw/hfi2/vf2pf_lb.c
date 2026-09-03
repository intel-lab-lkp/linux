// SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 *
 * SRIOV support for VFs making requests to PF0 over hfi2_loopback.
 */

#include "hfi2.h"
#include "chip_jkr.h"
#include "chip_gen.h"
#include "exp_rcv.h"
#include "sriov.h"
#include "vf2pf_int.h"
#include "vf2pf_lb.h"

static uint vf2pf_lb_port;

#undef VF2PF_LB_DEBUG
#define LB_RCV_CANT_SLEEP

#define LB_IN_INTR 0
#define LB_IN_THREAD 1
#define LB_IN_POLL 2
#define LB_IN_SHUTDOWN 3

#define LB_RHQ_ENT_SIZE 32 /* same as default for hfi2_hdrq_entsize */
#define LB_MAX_RCV_MSG PAGE_SIZE
#define LB_DEFAULT_RCVHDRSIZE 2 /* get split-point right */

static void lb_deinit(struct hfi2_devdata *dd, u8 si);
static struct vf2pf_hdr *lb_get_msg(struct hfi2_devdata *dd, void *buf);
static void *lb_msg_alloc(struct hfi2_devdata *dd, struct vf2pf_hdr **msg);

static struct vf2pf_devops vf2pf_lb_dev;

/*
 * Get vf2pf_lb context resources for specific SI.
 * These should be valid early, as soon as CSRs are accessible.
 * Only called by PF0.
 */
static void lb_set_si_ctxtrsrcs(struct hfi2_devdata *dd, int si,
				struct hfi2_ctxtrsrcs *lbr)
{
	int nctxt = vf2pf_lb_dev.num_ctxts;

	lbr->first_rcv_context = chip_rcv_contexts(dd) - nctxt + si;
	lbr->last_rcv_context = lbr->first_rcv_context + 1;
	lbr->first_send_context = chip_send_contexts(dd) - nctxt + si;
	lbr->last_send_context = lbr->first_send_context + 1;
	lbr->first_rcvarray_entry = chip_rcv_array_count(dd) -
				    HFI_MIN_PF0_RCVARY(nctxt) +
				    HFI_MIN_PF0_RCVARY(si);
	lbr->last_rcvarray_entry =
		lbr->first_rcvarray_entry + HFI_MIN_PF0_RCVARY(1);
	lbr->first_pio_block = chip_pio_mem_size(dd) / PIO_BLOCK_SIZE -
			       HFI_MIN_PF0_PIO(nctxt) + HFI_MIN_PF0_PIO(si);
	lbr->last_pio_block = lbr->first_pio_block + HFI_MIN_PF0_PIO(1);
}

static void lb_rcv_msg(struct work_struct *work)
{
	struct vf2pf_lb_msg *msg =
		container_of(work, struct vf2pf_lb_msg, pfx.work);
	struct hfi2_devdata *dd = msg->pfx.dd;

	hfi2_vf2pf_rcv_msg(dd, &msg->hdr, msg);
	kfree(msg);
}

/*
 * 'buf' is struct vf2pf_hdr plus payload, in h/w recv buffer (eager buf).
 * copy out and spawn to kworker thread.
 */
static void lb_queue_rcv_work(struct hfi2_devdata *dd, void *buf)
{
	struct vf2pf_hdr *hdr = buf;
	struct vf2pf_lb_msg *msg;
	void *out;

	out = lb_msg_alloc(dd, NULL);
	if (!out) {
		dd_dev_err(dd, "vf2pf no memory for rcv work\n");
		return;
	}
	msg = out;
	INIT_WORK(&msg->pfx.work, lb_rcv_msg);
	msg->pfx.dd = dd;
	msg->pfx.len = hdr->len + sizeof(*hdr);
	memcpy(out + offsetof(struct vf2pf_lb_msg, hdr), buf, msg->pfx.len);
	queue_work(dd->hfi2_wq, &msg->pfx.work);
}

static inline u32 rhf_egr_index(u64 rhf)
{
	/* NOTE: RHF.EgrIndex is 14 bits but RcvEgrIndexHead has 16 bits */
	return (rhf >> 16) & 0x3fff;
}

/* returns 0 if queue was emptied */
static int lb_do_rcv(struct hfi2_devdata *dd, int thread)
{
	struct vf2pf_lbdata *lbd = dd->vf2pf;
	u16 ctxt = lbd->c.first_rcv_context;
	u32 head, tail;
	u32 rhqoff, etail;
	u32 rhq_max;
	u64 rhf, rhe;
	bool bad;
	void *buf;

	if (!lbd ||
	    ((lbd->flags & VF2PF_LB_FL_SHUTDOWN) && thread != LB_IN_SHUTDOWN))
		return RCV_PKT_DONE;

	rhq_max = lbd->b.rhq_cnt * lbd->b.rhq_ent_size;
	/*
	 * process receive:
	 *
	 * (hdrq)TAIL is set by HFI to the next hdr ent to be used on next pkt recvd.
	 * (hdrq)HEAD is used by driver to pull packet hdrs off queue (DMA memory).
	 * units are DW, incr by RcvHdrEntSize.
	 *
	 * RHEQ entry is same index as RHQ (diff element size).
	 *
	 * RHF contains index/offset for payload in eager buffer.
	 */
	head = (u32)read_uctxt_csr(dd, ctxt, dd->params->rcv_hdr_head_reg);
	tail = (u32)read_uctxt_csr(dd, ctxt, dd->params->rcv_hdr_tail_reg);
	rhqoff = head; /* always multiple of rcvhdrqentsize, units dwords */
	while (rhqoff != tail) {
		rhf = rhf_to_cpu((__le32 *)lbd->b.rhq.va + rhqoff +
				 LB_RHQ_ENT_SIZE - sizeof(u64) / sizeof(u32));
		rhe = *((u64 *)lbd->b.rheq.va + rhqoff / LB_RHQ_ENT_SIZE);
		etail = rhf_egr_index(rhf); /* RHF.EgrIndex */
#ifdef VF2PF_LB_DEBUG
		dd_dev_info(dd,
			    "%s: head=%04x tail=%04x rhf=%016llx rhe=%016llx\n",
			    __func__, rhqoff, tail, rhf, rhe);
#endif
		bad = true;
		if (rhe & RHF_ERROR_SMASK)
			goto drop;
		if (rhf_rcv_type(rhf) != RHF_RCV_TYPE_EAGER)
			goto drop;
		if (rhf_use_egr_bfr(rhf))
			buf = lbd->b.egr.va +
			      rhf_egr_index(rhf) * lbd->b.egr_buf_size +
			      rhf_egr_buf_offset(rhf) * RCV_BUF_BLOCK_SIZE;
		else
			buf = lbd->b.rhq.va +
			      (rhqoff + rhf_hdrq_offset(rhf)) * sizeof(u32) +
			      sizeof(struct vf2pf_lb_hdr);
#ifdef VF2PF_LB_DEBUG
		{
			u32 len = rhf_pkt_len(rhf); /* in bytes */

			dd_dev_info(dd, "LB_PKT @ %p idx=%u off=%u\n", buf,
				    rhf_egr_index(rhf), rhf_egr_buf_offset(rhf));
			print_hex_dump(KERN_INFO, "LB_PKT ", DUMP_PREFIX_OFFSET,
				       16, 1, buf,
				       len - sizeof(struct vf2pf_lb_hdr), false);
		}
#endif

		/* divert responses now (do not use WQ) */
		if (((struct vf2pf_hdr *)buf)->op & VF2PF_OP_RESP) {
			hfi2_vf2pf_rsp_msg(dd, buf);
			goto next;
		}
		lb_queue_rcv_work(dd, buf);
next:
		bad = false;
drop:
		if (bad)
			dd_dev_err(
				dd,
				"Recv error ctxt %u rhq=%04x rhf=%016llx rhe=%016llx\n",
				ctxt, rhqoff, rhf, rhe);
		rhqoff += LB_RHQ_ENT_SIZE;
		if (rhqoff >= rhq_max)
			rhqoff = 0;
#ifdef VF2PF_LB_DEBUG
		dd_dev_info(dd, "%s: hfi2_update_usrhead_ctxt(%u, %04x, 1, %04x)\n",
			    __func__, ctxt, rhqoff, etail);
#endif
		hfi2_update_usrhead_ctxt(dd, ctxt, rhqoff, 1, 1, etail);
	}
	return RCV_PKT_DONE;
}

static inline int lb_rsp_wait(struct vf2pf_prefix *pfx, struct vf2pf_hdr *hdr,
			      long timeout)
{
	int ret;

#ifdef LB_RCV_CANT_SLEEP
	unsigned long expire;

	if (timeout > 0 && timeout != MAX_SCHEDULE_TIMEOUT)
		expire = timeout + jiffies;
	else
		expire = 0;
	ret = 0;
	while (!(hdr->op & VF2PF_OP_RESP)) {
		udelay(100);
		if (expire && time_after_eq(jiffies, expire))
			return -ETIME;
	}
#else
	ret = wait_event_timeout(pfx->wait, (hdr->op & VF2PF_OP_RESP), timeout);
	ret = ret ? 0 : -ETIME; /* convert residual time  to error */
#endif

	return ret;
}

static int lb_rcv_wait(struct hfi2_devdata *dd, void *buf, long timeout)
{
	struct vf2pf_lbdata *lbd = dd->vf2pf;
	struct vf2pf_prefix *pfx = buf;
	struct vf2pf_hdr *hdr;
	int ret = 0;

	if (!lbd)
		return -EINVAL;

	hdr = lb_get_msg(dd, buf);
	if (lbd->rcv_irq) {
		ret = lb_rsp_wait(pfx, hdr, timeout);
	} else {
		u32 head, tail;
		unsigned long expire;
		u16 ctxt = lbd->c.first_rcv_context;

		/* interrupts not setup yet, poll for recv. */
		if (timeout > 0 && timeout != MAX_SCHEDULE_TIMEOUT)
			expire = timeout + jiffies;
		else
			expire = 0;
		while (!(hdr->op & VF2PF_OP_RESP)) {
			spin_lock(&lbd->rcv_lock);
			head = (u32)read_uctxt_csr(
				dd, ctxt, dd->params->rcv_hdr_head_reg);
			tail = (u32)read_uctxt_csr(
				dd, ctxt, dd->params->rcv_hdr_tail_reg);
			if (head != tail) {
				lb_do_rcv(dd, LB_IN_POLL);
				spin_unlock(&lbd->rcv_lock);
			} else {
				spin_unlock(&lbd->rcv_lock);
				udelay(100);
				if (expire && time_after_eq(jiffies, expire))
					return -ETIME;
			}
		}
	}
	return ret;
}

/*
 * This is like __hfi2_rcd_eoi_intr() except not dependent on hfi2_ctxtdata.
 */
static void __hfi2_rctxt_eoi_intr(struct hfi2_devdata *dd, u16 ctxt)
{
	u32 src = dd->params->is_rcvavail_start + ctxt;
	u32 off = sizeof(u64) * (src / 64);
	u64 bit = 1ull << (src % 64);
	u32 head, tail;

	hfi2_write_csr(dd, dd->params->cce_int_clear_reg + off, bit);

	/* these also force the previous write */
	head = (u32)read_uctxt_csr(dd, ctxt, dd->params->rcv_hdr_head_reg);
	tail = (u32)read_uctxt_csr(dd, ctxt, dd->params->rcv_hdr_tail_reg);
	if (head != tail)
		hfi2_write_csr(dd, dd->params->cce_int_force_reg + off, bit);
}

static irqreturn_t lb_rcv_intr(int irq, void *arg)
{
	struct hfi2_devdata *dd = arg;
	struct vf2pf_lbdata *lbd = dd->vf2pf;

	if (!lbd)
		return IRQ_HANDLED;

	this_cpu_inc(*dd->int_counter);

#ifdef VF2PF_LB_DEBUG
	dd_dev_info(dd, "Receive vf2pf interrupt on %u\n",
		    lbd->c.first_rcv_context);
#endif
	if (lb_do_rcv(dd, LB_IN_INTR) == RCV_PKT_LIMIT)
		return IRQ_WAKE_THREAD;

	__hfi2_rctxt_eoi_intr(dd, lbd->c.first_rcv_context);
	return IRQ_HANDLED;
}

static irqreturn_t lb_rcv_thrd(int irq, void *arg)
{
	struct hfi2_devdata *dd = arg;
	struct vf2pf_lbdata *lbd = dd->vf2pf;

	if (!lbd)
		return IRQ_HANDLED;
#ifdef VF2PF_LB_DEBUG
	dd_dev_info(dd, "Receive vf2pf thread on %u\n",
		    lbd->c.first_rcv_context);
#endif
	lb_do_rcv(dd, LB_IN_THREAD);

	__hfi2_rctxt_eoi_intr(dd, lbd->c.first_rcv_context);
	return IRQ_HANDLED;
}

#define lb_tid_limit XA_LIMIT(0, 255) /* must fit in u16 */

static u16 lb_set_tid(struct hfi2_devdata *dd, void *tok)
{
	struct vf2pf_lbdata *lbd = dd->vf2pf;
	u32 idx;
	int ret;

	if (!lbd)
		return ~(u16)0;
	ret = xa_alloc_cyclic(&lbd->tid_xa, &idx, tok, lb_tid_limit,
			      &lbd->tid_next, GFP_KERNEL);
	if (ret < 0)
		return ~(u16)0;
	return (u16)idx;
}

/* destructive lookup... */
static void *lb_get_tid(struct hfi2_devdata *dd, u16 tid)
{
	struct vf2pf_lbdata *lbd = dd->vf2pf;

	if (!lbd)
		return NULL;
	return xa_erase(&lbd->tid_xa, (u32)tid);
}

static int lb_init_irq(struct hfi2_devdata *dd)
{
	struct vf2pf_lbdata *lbd = dd->vf2pf;
	char name[MAX_NAME_SIZE];
	int ret;
	u16 ctxt;

	if (!lbd)
		return 0;
	ctxt = lbd->c.first_rcv_context;

	snprintf(name, sizeof(name), DRIVER_NAME "_%dsi%d", dd->rsrcs.pfunit,
		 dd->rsrcs.si_idx);
	/*
	 * Use IRQ_GENERAL even though this is IRQ_RCVCTXT, to avoid
	 * doing affinity work that is not possible for this ctxt
	 * (no struct hfi2_ctxtdata *rcd exists).
	 */
	ret = hfi2_msix_request_irq_remap(dd, ctxt, IRQ_GENERAL,
				     dd->params->is_rcvavail_start + ctxt,
				     lb_rcv_intr, lb_rcv_thrd, dd, name);
	if (ret < 0)
		return ret;
	if (!ret) {
		/* must not take intr 0 */
		hfi2_msix_free_irq(dd, (u8)ret);
		return -ENOSPC;
	}
	lbd->rcv_irq = ret;
	hfi2_set_intr_bits(dd, dd->params->is_rcvavail_start + ctxt,
		      dd->params->is_rcvavail_start + ctxt, true);
	return 0;
}

static void lb_deinit_irq(struct hfi2_devdata *dd)
{
	struct vf2pf_lbdata *lbd = dd->vf2pf;
	u16 ctxt;
	u8 irq;

	if (!lbd)
		return;
	irq = lbd->rcv_irq;
	if (lbd->rcv_irq <= 0)
		return;
	lbd->rcv_irq = 0; /* trigger polling */

	ctxt = lbd->c.first_rcv_context;
	/* tear-down our interrupt */
	hfi2_set_intr_bits(dd, dd->params->is_rcvavail_start + ctxt,
		      dd->params->is_rcvavail_start + ctxt, false);
	hfi2_msix_free_irq(dd, irq);
#ifdef VF2PF_LB_DEBUG
	dd_dev_info(dd, "Calling lb_do_rcv() in shutdown\n");
#endif
	lb_do_rcv(dd, LB_IN_SHUTDOWN); /* pre-emptive flushing of receives */
}

/*
 * Only called on PF0.
 *
 * Initialize or reset (reinit) contexts.
 */
static int lb_init_ctxts(struct hfi2_devdata *dd, u8 si)
{
	struct vf2pf_lbdata *lbd = dd->vf2pf;
	struct hfi2_ctxtrsrcs lbr;
	int ret;

	lb_set_si_ctxtrsrcs(dd, si, &lbr);
	ret = hfi2_gen_init_sctxt_pio(dd, lbd->pidx, si, lbr.first_send_context,
				 lbr.first_pio_block,
				 lbr.last_pio_block - lbr.first_pio_block);
	if (ret)
		goto out;
	ret = hfi2_gen_init_rctxt_egr(dd, lbd->pidx, si, lbr.first_rcv_context,
				 lbr.first_rcvarray_entry,
				 lbd->b.rhq_cnt, /* all must be the same! */
				 LB_DEFAULT_RCVHDRSIZE);
out:
	return ret;
}

static int lb_start_ctxts(struct hfi2_devdata *dd, u8 si)
{
	struct vf2pf_lbdata *lbd = dd->vf2pf;
	int ret;

	ret = hfi2_gen_start_sctxt(dd, lbd->pidx, lbd->c.first_send_context,
			      &lbd->b);
	if (ret)
		goto out;
	ret = hfi2_gen_start_rctxt_egr(dd, lbd->pidx, lbd->c.first_rcv_context,
				  &lbd->b);
out:
	return ret;
}

/*
 * Only called on PF0.
 *
 * Prepare VF contexts for use. This may require reset/shutdown/disable
 * since contexts might still be "up" from previous run of SRIOV.
 */
static int lb_init_vfs(struct hfi2_devdata *dd)
{
	int si, nsi;
	int ret = 0;

	nsi = dd->rsrcs.num_vfs + 1;
	for (si = 1; si < nsi; ++si) {
		ret = lb_init_ctxts(dd, si);
		if (ret)
			goto err_out;
	}
err_out:
	return ret;
}

/* final deinit (PF0 shutdown) */
static void lb_deinit_ctxts(struct hfi2_devdata *dd, u8 si)
{
	struct vf2pf_lbdata *lbd = dd->vf2pf;
	struct hfi2_ctxtrsrcs lbr;

	lb_set_si_ctxtrsrcs(dd, si, &lbr);
	hfi2_gen_deinit_sctxt(dd, lbd->pidx, si, lbr.first_send_context);
	hfi2_gen_deinit_rctxt(dd, lbd->pidx, si, lbr.first_rcv_context);
}

/*
 * dd->rsrcs has not been setup yet, cannot depend on it.
 */
static int lb_init(struct hfi2_devdata *dd, u8 si)
{
	struct vf2pf_lbdata *lbd;
	u16 ctxt;
	int ret;

	if (si == VF2PF_INIT_ALL)
		return lb_init_vfs(dd);

	lbd = kzalloc_obj(lbd, GFP_KERNEL);
	if (!lbd)
		return -ENOMEM;

	spin_lock_init(&lbd->pio_lock);
	spin_lock_init(&lbd->rcv_lock);
	lbd->dd = dd;
	dd->vf2pf = lbd;
	/*
	 * This is problematic because hfi2_loopback ports are numbered
	 * differently depending on the number of fabric ports, and
	 * that varies with different chips.
	 */
	if (vf2pf_lb_port < dd->num_pports)
		lbd->pidx = loopback_pidx_dd(dd, vf2pf_lb_port);
	else if (vf2pf_lb_port < 2 * dd->num_pports)
		lbd->pidx = vf2pf_lb_port;
	else
		lbd->pidx = loopback_pidx_dd(dd, 0); /* hope it's functional */
	lb_set_si_ctxtrsrcs(dd, si, &lbd->c);

	xa_init_flags(&lbd->tid_xa, XA_FLAGS_ALLOC);
	lbd->pf0_ctxt = lbd->c.first_rcv_context - si;

	lbd->b.cr.va = dma_alloc_coherent(&dd->pcidev->dev,
					  sizeof(*lbd->b.cr.va), &lbd->b.cr.dma,
					  GFP_KERNEL);
	if (!lbd->b.cr.va) {
		ret = -ENOMEM;
		goto err_out;
	}

	/*
	 * rhq_cnt, rhq_ent drives the sizes of all receive allocations.
	 * For PF0, a larger size is justified since there might be
	 * several VFs communicating with PF0 at once. It is also possible
	 * that a VF might have a couple messages to PF0 outstanding at
	 * once, and thus need more than one receive buffer set. For now,
	 * just use the number of VFs (actually, SIs) for all cases.
	 * For hfi2_gen_init_rctxt_egr(), we require that all SIs use identical
	 * eager buffer counts.
	 */
	lbd->b.rhq_cnt = round_up(JKR_C_CCE_NUM_VFS + 1, HDRQ_INCREMENT);
	lbd->b.egr_buf_size = PAGE_SIZE;
	lbd->b.rhq_ent_size = LB_RHQ_ENT_SIZE;
	lbd->b.egr.size = PAGE_ALIGN(lbd->b.rhq_cnt * lbd->b.egr_buf_size);
	lbd->b.rhq.size =
		PAGE_ALIGN(lbd->b.rhq_cnt * lbd->b.rhq_ent_size * sizeof(u32));
	lbd->b.rheq.size = PAGE_ALIGN(lbd->b.rhq_cnt * sizeof(u64));

	lbd->b.egr.va = dma_alloc_coherent(&dd->pcidev->dev, lbd->b.egr.size,
					   &lbd->b.egr.dma, GFP_KERNEL);
	lbd->b.rhq.va = dma_alloc_coherent(&dd->pcidev->dev, lbd->b.rhq.size,
					   &lbd->b.rhq.dma, GFP_KERNEL);
	lbd->b.rheq.va = dma_alloc_coherent(&dd->pcidev->dev, lbd->b.rheq.size,
					    &lbd->b.rheq.dma, GFP_KERNEL);
	if (!lbd->b.egr.va || !lbd->b.rhq.va || !lbd->b.rheq.va) {
		ret = -ENOMEM;
		goto err_out;
	}

	ctxt = lbd->c.first_send_context;
	lbd->pio_mem = dd->bar_maps[ctxt_bar_idx(ctxt)].piobase +
		       ((ctxt_bar_ctxt(ctxt) & PIO_ADDR_CONTEXT_MASK)
			<< PIO_ADDR_CONTEXT_SHIFT);
	lbd->pio_wrap = lbd->pio_mem +
			(lbd->c.last_pio_block - lbd->c.first_pio_block) *
				PIO_BLOCK_SIZE;
	lbd->pio_next = lbd->pio_mem;

	/* now setup contexts as required */
	if (!dd->is_vf) {
		ret = lb_init_ctxts(dd, si);
		if (ret)
			goto err_out;
	}
	ret = lb_start_ctxts(dd, si);
	if (ret)
		goto err_out;
	return 0;

err_out:
	lb_deinit(dd, si);
	return ret;
}

/*
 * Called on PF0 on behalf of 'si', or for final shutdown (si == 0).
 * Called on VFs only to free local resources.
 */
static void lb_deinit(struct hfi2_devdata *dd, u8 si_idx)
{
	struct vf2pf_lbdata *lbd = dd->vf2pf;

	if (!lbd)
		return;

	if (dd->rsrcs.si_idx != si_idx) {
		int ret;

		/* PF0 performing on behalf of VF - VF2PF_STOP */
		ret = lb_init_ctxts(dd, si_idx); /* re-init */
		if (ret)
			dd_dev_err(
				dd,
				"Failed to restart vf2pf contexts for SI %u (%d)\n",
				si_idx, ret);
		return;
	}
	lbd->flags |= VF2PF_LB_FL_SHUTDOWN;
	smp_rmb(); /* ensure flags are updated before continuing */

	lb_deinit_irq(dd); /* in case not already done */
	/*
	 * On VFs, can't stop contexts since we've already sent the STOP
	 * message to PF0 and it has likely taken ownership of them. This
	 * means the lb_deinit_ctxts() routines must also do the stop
	 * function. Only PF0 does the lb_deinit_ctxts() here.
	 */
	if (!dd->is_vf)
		lb_deinit_ctxts(dd, dd->rsrcs.si_idx);

	if (lbd->b.cr.va)
		dma_free_coherent(&dd->pcidev->dev, sizeof(*lbd->b.cr.va),
				  lbd->b.cr.va, lbd->b.cr.dma);
	if (lbd->b.egr.va)
		dma_free_coherent(&dd->pcidev->dev, lbd->b.egr.size,
				  lbd->b.egr.va, lbd->b.egr.dma);
	if (lbd->b.rhq.va)
		dma_free_coherent(&dd->pcidev->dev, lbd->b.rhq.size,
				  lbd->b.rhq.va, lbd->b.rhq.dma);
	if (lbd->b.rheq.va)
		dma_free_coherent(&dd->pcidev->dev, lbd->b.rheq.size,
				  lbd->b.rheq.va, lbd->b.rheq.dma);
	if (!dd->is_vf) {
		/* PF0 must de-init all contexts even if already done */
		int si, nsi;

		nsi = dd->rsrcs.num_vfs + 1;
		for (si = 0; si < nsi; ++si)
			lb_deinit_ctxts(dd, si); /* final deinit */
	}

	xa_destroy(&lbd->tid_xa);
	dd->vf2pf = NULL;
	kfree(lbd);
}

static struct vf2pf_hdr *lb_get_msg(struct hfi2_devdata *dd, void *buf)
{
	struct vf2pf_lb_msg *mem = buf;

	return &mem->hdr;
}

/*
 * Allocate a unified message structure for use with hfi2_loopback implementation.
 *
 * Rounds total length up to qword multiple (for PIO CSR granularity).
 * 'pfx' must be qword multiple to maintain memory alignment.
 */
static void *lb_msg_alloc(struct hfi2_devdata *dd, struct vf2pf_hdr **msg)
{
	struct vf2pf_lb_msg *mem;

	mem = kzalloc_obj(*mem, GFP_KERNEL);
	if (mem && msg)
		*msg = lb_get_msg(dd, mem);
	return mem;
}

#ifdef LB_EGRESS_WAIT
/*
 * Returns number of credits outstanding for ctxt.
 */
static u32 lb_sc_crleft(struct hfi2_devdata *dd, u16 ctxt)
{
	u64 reg;
	u32 curr, last;

	reg = read_sctxt_csr(dd, ctxt, dd->params->send_ctxt_credit_status_reg);
	curr = (reg >> SEND_CTXT_CREDIT_STATUS_CURRENT_FREE_COUNTER_SHIFT) &
	       SEND_CTXT_CREDIT_STATUS_CURRENT_FREE_COUNTER_MASK;
	last = reg & SEND_CTXT_CREDIT_STATUS_LAST_RETURNED_COUNTER_SMASK;
	return (curr - last) &
	       SEND_CTXT_CREDIT_STATUS_LAST_RETURNED_COUNTER_SMASK;
}

static bool is_sc_halted(struct hfi2_devdata *dd, u32 hw_context)
{
	return !!(read_sctxt_csr(dd, hw_context,
				 dd->params->send_ctxt_status_reg) &
		  SEND_CTXT_STATUS_CTXT_HALTED_SMASK);
}
#endif

/*
 * Send 'msg' ('len') to 'si', do not wait for response.
 *
 * The opaque 'msg' has a wait_queue_head_t preceding vf2pf_lb_hdr.
 */
static int lb_send(struct hfi2_devdata *dd, u8 si, void *buf)
{
	struct vf2pf_lbdata *lbd = dd->vf2pf;
	struct vf2pf_lb_msg *msg = buf;
	struct vf2pf_lb_hdr *lbh = &msg->lbh;
	struct vf2pf_hdr *hdr = &msg->hdr;
	u64 *qw = (u64 *)lbh;
	u64 __iomem *dst;
	u32 dw_len, pbc_dw_len;
	u32 qw_len, qw_wrt = 0;
	u64 pbc;
	u8 vl = 15;
	int ret = 0;
	int len;

	if (hdr->len > VF2PF_LB_MAX_MSG)
		return -EINVAL;
	if (!lbd || (lbd->flags & VF2PF_LB_FL_SHUTDOWN))
		return -ENXIO;

	len = hdr->len + sizeof(struct vf2pf_hdr) + sizeof(struct vf2pf_lb_hdr);
	dw_len = DIV_ROUND_UP(len, sizeof(u32));
	pbc_dw_len = dw_len + (sizeof(u64) / sizeof(u32));
	qw_len = DIV_ROUND_UP(dw_len, sizeof(u64) / sizeof(u32)) +
		 1; /* include PBC */

	spin_lock(&lbd->pio_lock);

#ifdef LB_EGRESS_WAIT
	u32 loop = 0;
	/* wait for (any) previous send to complete... (reset credits?) */
	while (lb_sc_crleft(dd, lbd->c.first_send_context)) {
		if (is_sc_halted(dd, lbd->c.first_send_context)) {
			ret = -EIO;
			goto out;
		}
		if (loop > 100) {
			ret = -ETIME;
			goto out;
		}
		++loop;
		mdelay(1);
	}
#endif
	dst = lbd->pio_next + SOP_DISTANCE;
	lbd->pio_next +=
		round_up(len + sizeof(u64), PIO_BLOCK_SIZE); /* incl. PBC */
	if (lbd->pio_next >= lbd->pio_wrap)
		lbd->pio_next -= (lbd->pio_wrap - lbd->pio_mem);
	spin_unlock(&lbd->pio_lock);

	/* force these to legit value */
	hdr->si = dd->rsrcs.si_idx;

	lbh->lrh[0] = cpu_to_be16(HFI2_LRH_BTH | (vl << 12));
	lbh->lrh[1] = cpu_to_be16(IB_LID_PERMISSIVE);
	lbh->lrh[2] = cpu_to_be16(dw_len + SIZE_OF_CRC);
	lbh->lrh[3] = cpu_to_be16(IB_LID_PERMISSIVE);
	lbh->bth[0] = cpu_to_be32(IB_OPCODE_UD_SEND_ONLY << 24); /* used? */
	lbh->bth[1] =
		cpu_to_be32((RVT_KDETH_QP_PREFIX << RCV_BTH_QP_KDETH_QP_SHIFT) |
			    (lbd->pf0_ctxt + si));
	lbh->bth[2] = cpu_to_be32(0 /*loopback_dst_vf_index*/); /* PSN 0 */
	;
	lbh->ver_tid_offset = (1 << KDETH_KVER_SHIFT);
	/* lbh->jkey should be dont-care since checking is OFF */
	/* lbh->hcrc generated by h/w */

	pbc = hfi2_gen_create_pbc_pidx(lbd->pidx, 0, 0, vl, pbc_dw_len, PBC_L2_9B,
				  OPA_LID_PERMISSIVE,
				  lbd->c.first_send_context);
	pbc &= ~PBC_INSERT_HCRC_SMASK;
	pbc |= (u64)PBC_IHCRC_LKDETH << PBC_INSERT_HCRC_SHIFT;

	/*
	 * First block (8 qwords) written with SOP_DISTANCE set,
	 * the rest with SOP_DISTANCE clear. Must write whole blocks.
	 * First block (SOP) never requires wrap.
	 */
	writeq(pbc, dst++);
	++qw_wrt;
	while (qw_wrt < qw_len) {
		writeq(*qw++, dst++);
		++qw_wrt;
		if (qw_wrt >= 8) {
			if (qw_wrt == 8)
				dst = (void *)dst - SOP_DISTANCE;
			if ((void *)dst >= lbd->pio_wrap)
				dst = lbd->pio_mem; /* never needs SOP_DISTANCE */
		}
	}
	while (qw_wrt & 7) {
		writeq(0, dst++);
		++qw_wrt;
	}
	return ret;
}

/*
 * Only called for VFs.
 * Returns 0 on error (invalid VF SI).
 * Must not depend on any setup (hfi2_vf2pf_init() has not been called).
 * BARs have been mapped.
 */
static int lb_probe_si(struct hfi2_devdata *dd)
{
	int nctxt;
	u64 reg;
	u16 pf0_ctxt;
	int si;

	nctxt = vf2pf_lb_dev.num_ctxts;
	if (!nctxt)
		return 0;

	pf0_ctxt = chip_rcv_contexts(dd) - nctxt;
	/*
	 * Except for SI 1, this will cause CSR Access Violations
	 * so we need to clear that after - regardless of the result.
	 */
	for (si = 1; si <= JKR_C_CCE_NUM_VFS; ++si) {
		reg = read_kctxt_csr(dd, pf0_ctxt + si,
				     dd->params->rcv_hdr_ent_size_reg);
		if (reg) {
			dd_dev_info(dd, "Probed SI index %d\n", si);
			goto found;
		}
	}
	si = 0; /* not found */
found:
	/* clear any access violations */
	hfi2_write_csr(dd, JKR_CCE_ERR_INFO_ACCESS_VIOLATION,
		  JKR_CCE_ERR_INFO_ACCESS_VIOLATION_VALID_SMASK);
	return si;
}

static ssize_t vf2pf_lb_debug_show(struct device *device,
				   struct device_attribute *attr, char *buf)
{
	struct hfi2_ibdev *dev =
		rdma_device_to_drv_device(device, struct hfi2_ibdev, rdi.ibdev);
	struct hfi2_devdata *dd = dd_from_dev(dev);
	struct vf2pf_lbdata *lbd = dd->vf2pf;
	ssize_t off = 0;

	if (!lbd)
		return 0;
	off += sysfs_emit_at(buf, off,
			     "rctxt   %u\n"
			     "sctxt   %u\n"
			     "rcvary  %u-%u\n"
			     "pio     %u-%u\n",
			     lbd->c.first_rcv_context,
			     lbd->c.first_send_context,
			     lbd->c.first_rcvarray_entry,
			     lbd->c.last_rcvarray_entry - 1,
			     lbd->c.first_pio_block, lbd->c.last_pio_block - 1);
	off += sysfs_emit_at(buf, off,
			     "pidx %u\n"
			     "pf0_ctxt %u\n"
			     "tid_next %u\n"
			     "CR %p %016llx\n"
			     "PIO mem %p wrap %p next %p\n",
			     lbd->pidx, lbd->pf0_ctxt, lbd->tid_next,
			     lbd->b.cr.va, lbd->b.cr.dma, lbd->pio_mem,
			     lbd->pio_wrap, lbd->pio_next);
	off += sysfs_emit_at(buf, off,
			     "rcv_irq %u\n"
			     "egr size %lu va %p dma %016llx\n"
			     "rhq size %lu va %p dma %016llx\n"
			     "rheq size %lu va %p dma %016llx\n",
			     lbd->rcv_irq, lbd->b.egr.size, lbd->b.egr.va,
			     lbd->b.egr.dma, lbd->b.rhq.size, lbd->b.rhq.va,
			     lbd->b.rhq.dma, lbd->b.rheq.size, lbd->b.rheq.va,
			     lbd->b.rheq.dma);

	if (off >= PAGE_SIZE) {
		dd_dev_warn(dd, "%s exceeds PAGE_SIZE.\n", attr->attr.name);
		return -EFBIG;
	}
	return off;
}

static DEVICE_ATTR_RO(vf2pf_lb_debug);

static ssize_t vf2pf_lb_reset_store(struct device *device,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct hfi2_ibdev *dev =
		rdma_device_to_drv_device(device, struct hfi2_ibdev, rdi.ibdev);
	struct hfi2_devdata *dd = dd_from_dev(dev);
	unsigned long si;
	int ret;

	ret = kstrtoul(buf, 0, &si);
	if (ret || !si || si > dd->rsrcs.num_vfs)
		return -EINVAL;

	lb_deinit(dd, (u8)si);
	return count;
}

static DEVICE_ATTR_WO(vf2pf_lb_reset);

static void lb_init_sysfs(struct hfi2_devdata *dd, struct device *class_dev)
{
	int ret;

	ret = sysfs_create_file(&class_dev->kobj,
				&dev_attr_vf2pf_lb_debug.attr);
	if (ret)
		dd_dev_warn(dd, "failed to create sysfs attr %s (%d)\n",
			    dev_attr_vf2pf_lb_debug.attr.name, ret);
	if (!dd->is_vf) {
		ret = sysfs_create_file(&class_dev->kobj,
					&dev_attr_vf2pf_lb_reset.attr);
		if (ret)
			dd_dev_warn(dd, "failed to create sysfs attr %s (%d)\n",
				    dev_attr_vf2pf_lb_reset.attr.name, ret);
	}
}

/* for additional output to "hw_resources" */
static int lb_sysfs_emit_at(struct hfi2_devdata *dd, char *buf, int at)
{
	struct vf2pf_lbdata *lbd = dd->vf2pf;
	int off = at;

	if (!lbd)
		return 0;

	off += sysfs_emit_at(buf, off,
			     "lb.rctxt  %u\n"
			     "lb.sctxt  %u\n"
			     "lb.rcvary %u-%u\n"
			     "lb.pio    %u-%u\n",
			     lbd->c.first_rcv_context,
			     lbd->c.first_send_context,
			     lbd->c.first_rcvarray_entry,
			     lbd->c.last_rcvarray_entry - 1,
			     lbd->c.first_pio_block, lbd->c.last_pio_block - 1);

	return off - at;
}

static void lb_set_si_enables(struct hfi2_devdata *dd, int si, u64 *csrs,
			      void (*si_enables)(struct hfi2_devdata *dd,
						 u64 *csrs, u32 start, u32 end))
{
	struct hfi2_ctxtrsrcs lbr;

	lb_set_si_ctxtrsrcs(dd, si, &lbr);
	si_enables(dd, csrs,
		   dd->params->is_rcvavail_start + lbr.first_rcv_context,
		   dd->params->is_rcvavail_start + lbr.last_rcv_context);
}

static struct vf2pf_devops vf2pf_lb_dev = {
	.num_ctxts = JKR_C_CCE_NUM_VFS + 1,
	.num_irq = 1,
	.init = lb_init,
	.deinit = lb_deinit,
	.send = lb_send,
	.msg_alloc = lb_msg_alloc,
	.set_tid = lb_set_tid,
	.get_tid = lb_get_tid,
	.get_msg = lb_get_msg,
	.probe_si = lb_probe_si,
	.init_sysfs = lb_init_sysfs,
	.sysfs_emit_at = lb_sysfs_emit_at,
	.init_irq = lb_init_irq,
	.deinit_irq = lb_deinit_irq,
	.rcv_wait = lb_rcv_wait,
	.set_si_enables = lb_set_si_enables,
};

struct vf2pf_devops *hfi2_get_lb_devops(void)
{
	return &vf2pf_lb_dev;
}
