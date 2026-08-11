// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright (c) 2021, Microsoft Corporation. */

#include <net/mana/gdma.h>
#include <net/mana/mana.h>
#include <net/mana/hw_channel.h>
#include <linux/pci.h>
#include <linux/vmalloc.h>

/* Acquire a free message slot from the inflight bitmap.  Returns
 * -ETIMEDOUT if a prior HWC command has timed out (preserving the
 * error code callers expect).
 */
static int mana_hwc_get_msg_index(struct hw_channel_context *hwc, u16 *msg_id)
{
	struct gdma_resource *r = &hwc->inflight_msg_res;
	unsigned long flags;
	u32 index;

	for (;;) {
		spin_lock_irqsave(&r->lock, flags);

		/* Reject new admissions once the channel has latched a genuine
		 * timeout -- but not for a deliberate no-wait teardown, where
		 * mana_serv_reset() sets hwc_timeout = 0 to best-effort post
		 * the teardown commands.  Without this exception an earlier
		 * timeout would block those teardown commands here before the
		 * hwc_timeout == 0 path in mana_hwc_send_request() can run.
		 */
		if (hwc->hwc_timed_out && hwc->hwc_timeout != 0) {
			spin_unlock_irqrestore(&r->lock, flags);
			return -ETIMEDOUT;
		}

		index = find_first_zero_bit(r->map, r->size);
		if (index < r->size) {
			struct hwc_caller_ctx *ctx;

			ctx = &hwc->caller_ctx[index];
			reinit_completion(&ctx->comp_event);
			/* Initialise the slot before publishing its inflight
			 * bit below.  The response-side reference is taken
			 * here, under r->lock, so a stale or duplicate response
			 * that lands before mana_hwc_send_request() posts the
			 * request cannot drop the refcount to zero and free the
			 * slot under the sender.  One reference is the sender's;
			 * the other is released by mana_hwc_handle_resp().
			 */
			refcount_set(&ctx->refcnt, 2);
			ctx->responded = false;
			ctx->msg_id = index;
			ctx->error = -EINPROGRESS;
			/* Publish the slot last.  mana_hwc_handle_resp() honours
			 * a response only after the sender sets ctx->output_buf
			 * (under ctx->lock, after this function returns), so the
			 * initialisation above is always visible before any
			 * response is acted on.
			 */
			bitmap_set(r->map, index, 1);
			spin_unlock_irqrestore(&r->lock, flags);
			break;
		}
		spin_unlock_irqrestore(&r->lock, flags);

		wait_event(hwc->msg_waitq,
			   (READ_ONCE(hwc->hwc_timed_out) &&
			    READ_ONCE(hwc->hwc_timeout) != 0) ||
			   !bitmap_full(r->map, r->size));

		/* Same no-wait teardown exception as the entry gate above:
		 * when hwc_timeout == 0 do not bail on the latch, wait for a
		 * slot to free so the best-effort teardown command can still
		 * be posted instead of spinning here.
		 */
		if (READ_ONCE(hwc->hwc_timed_out) &&
		    READ_ONCE(hwc->hwc_timeout) != 0)
			return -ETIMEDOUT;
	}

	*msg_id = index;
	return 0;
}

static void mana_hwc_put_msg_index(struct hw_channel_context *hwc, u16 msg_id)
{
	struct gdma_resource *r = &hwc->inflight_msg_res;
	unsigned long flags;

	spin_lock_irqsave(&r->lock, flags);
	bitmap_clear(r->map, msg_id, 1);
	spin_unlock_irqrestore(&r->lock, flags);

	wake_up(&hwc->msg_waitq);
}

static void hwc_ctx_put(struct hw_channel_context *hwc,
			struct hwc_caller_ctx *ctx)
{
	if (refcount_dec_and_test(&ctx->refcnt))
		mana_hwc_put_msg_index(hwc, ctx->msg_id);
}

static int mana_hwc_verify_resp_msg(const struct hwc_caller_ctx *caller_ctx,
				    const struct gdma_resp_hdr *resp_msg,
				    u32 resp_len)
{
	if (resp_len < sizeof(*resp_msg))
		return -EPROTO;

	if (resp_len > caller_ctx->output_buflen)
		return -EPROTO;

	return 0;
}

static int mana_hwc_post_rx_wqe(const struct hwc_wq *hwc_rxq,
				struct hwc_work_request *req)
{
	struct device *dev = hwc_rxq->hwc->dev;
	struct gdma_sge *sge;
	int err;

	sge = &req->sge;
	sge->address = (u64)req->buf_sge_addr;
	sge->mem_key = hwc_rxq->msg_buf->gpa_mkey;
	sge->size = req->buf_len;

	memset(&req->wqe_req, 0, sizeof(struct gdma_wqe_request));
	req->wqe_req.sgl = sge;
	req->wqe_req.num_sge = 1;
	req->wqe_req.client_data_unit = 0;

	err = mana_gd_post_and_ring(hwc_rxq->gdma_wq, &req->wqe_req, NULL);
	if (err)
		dev_err(dev, "Failed to post WQE on HWC RQ: %d\n", err);
	return err;
}

static void mana_hwc_handle_resp(struct hw_channel_context *hwc, u32 resp_len,
				 struct hwc_work_request *rx_req, u16 msg_id)
{
	const struct gdma_resp_hdr *resp_msg = rx_req->buf_va;
	struct hwc_caller_ctx *ctx;
	int err;

	/* Defence in depth: the sole caller, mana_hwc_rx_event_handler(),
	 * already rejects msg_id >= hwc->num_inflight_msg with the value it
	 * passes here by value, so this cannot be reached out of range.  Keep
	 * the guard at the indexing site so the bitmap and caller_ctx array
	 * are never indexed without a bound in view.
	 */
	if (msg_id >= hwc->num_inflight_msg) {
		dev_err(hwc->dev, "hwc_rx: msg_id %u >= max %u\n",
			msg_id, hwc->num_inflight_msg);
		mana_hwc_post_rx_wqe(hwc->rxq, rx_req);
		return;
	}

	if (!test_bit(msg_id, hwc->inflight_msg_res.map)) {
		dev_err(hwc->dev, "hwc_rx: invalid msg_id = %u\n", msg_id);
		mana_hwc_post_rx_wqe(hwc->rxq, rx_req);
		return;
	}

	ctx = hwc->caller_ctx + msg_id;

	/* Reject responses larger than the RX DMA buffer — the SGE
	 * limits what hardware can DMA, so an oversized resp_len
	 * indicates a firmware bug.  Fail rather than silently
	 * truncating.
	 */
	if (resp_len > rx_req->buf_len) {
		dev_err(hwc->dev, "HWC RX: resp_len %u > buf_len %u\n",
			resp_len, rx_req->buf_len);
		resp_len = 0;
	}

	spin_lock(&ctx->lock);

	/* Honour a response only while the sender is actively waiting on
	 * this slot -- that is, it has published ctx->output_buf and not yet
	 * reclaimed it.  A NULL output_buf means the sender has not posted
	 * its request yet (so this is a premature, stale or forged response
	 * that must not complete the slot and let it be freed while the real
	 * request is still in flight) or it already timed out and took
	 * ownership back.  ctx->responded drops a second, duplicate response.
	 * In all these cases drop the response without touching the refcount
	 * or the completion; the genuine response, the sender or the teardown
	 * path still balances the references.
	 */
	if (!ctx->output_buf || ctx->responded) {
		spin_unlock(&ctx->lock);
		mana_hwc_post_rx_wqe(hwc->rxq, rx_req);
		return;
	}
	ctx->responded = true;

	err = mana_hwc_verify_resp_msg(ctx, resp_msg, resp_len);
	if (!err) {
		ctx->status_code = resp_msg->status;
		memcpy(ctx->output_buf, resp_msg, resp_len);
	}
	ctx->error = err;

	/* Post RX WQE before completing — the next response may arrive
	 * immediately and needs a posted buffer.
	 */
	mana_hwc_post_rx_wqe(hwc->rxq, rx_req);
	complete(&ctx->comp_event);
	spin_unlock(&ctx->lock);

	hwc_ctx_put(hwc, ctx);
}

static void mana_hwc_init_event_handler(void *ctx, struct gdma_queue *q_self,
					struct gdma_event *event)
{
	union hwc_init_soc_service_type service_data;
	struct hw_channel_context *hwc = ctx;
	struct gdma_dev *gd = hwc->gdma_dev;
	union hwc_init_type_data type_data;
	union hwc_init_eq_id_db eq_db;
	struct mana_context *ac;
	u32 type, val;
	int ret;

	switch (event->type) {
	case GDMA_EQE_HWC_INIT_EQ_ID_DB:
		eq_db.as_uint32 = event->details[0];
		hwc->cq->gdma_eq->id = eq_db.eq_id;
		gd->doorbell = eq_db.doorbell;
		break;

	case GDMA_EQE_HWC_INIT_DATA:
		type_data.as_uint32 = event->details[0];
		type = type_data.type;
		val = type_data.value;

		switch (type) {
		case HWC_INIT_DATA_CQID:
			hwc->cq->gdma_cq->id = val;
			break;

		case HWC_INIT_DATA_RQID:
			hwc->rxq->gdma_wq->id = val;
			break;

		case HWC_INIT_DATA_SQID:
			hwc->txq->gdma_wq->id = val;
			break;

		case HWC_INIT_DATA_QUEUE_DEPTH:
			hwc->hwc_init_q_depth_max = (u16)val;
			break;

		case HWC_INIT_DATA_MAX_REQUEST:
			hwc->hwc_init_max_req_msg_size = val;
			break;

		case HWC_INIT_DATA_MAX_RESPONSE:
			hwc->hwc_init_max_resp_msg_size = val;
			break;

		case HWC_INIT_DATA_MAX_NUM_CQS:
			/* Store, don't apply: mana_hwc_establish_channel()
			 * commits this to gc->max_num_cqs once, together
			 * with sizing cq_table, so a spoofed post-init event
			 * cannot inflate the bound past the allocation.
			 * WRITE_ONCE() pairs with the READ_ONCE() there:
			 * this store runs in EQ interrupt context,
			 * concurrently with that process-context read.
			 */
			WRITE_ONCE(hwc->hwc_init_max_num_cqs, val);
			break;

		case HWC_INIT_DATA_PDID:
			hwc->gdma_dev->pdid = val;
			break;

		case HWC_INIT_DATA_GPA_MKEY:
			hwc->rxq->msg_buf->gpa_mkey = val;
			hwc->txq->msg_buf->gpa_mkey = val;
			break;

		case HWC_INIT_DATA_PF_DEST_RQ_ID:
			hwc->pf_dest_vrq_id = val;
			break;

		case HWC_INIT_DATA_PF_DEST_CQ_ID:
			hwc->pf_dest_vrcq_id = val;
			break;
		}

		break;

	case GDMA_EQE_HWC_INIT_DONE:
		complete(&hwc->hwc_init_eqe_comp);
		break;

	case GDMA_EQE_HWC_SOC_RECONFIG_DATA:
		type_data.as_uint32 = event->details[0];
		type = type_data.type;
		val = type_data.value;

		switch (type) {
		case HWC_DATA_CFG_HWC_TIMEOUT:
			/* A zero timeout would make every command time out
			 * immediately and latch hwc_timed_out, disabling the
			 * channel.  Ignore it and keep the positive default.
			 */
			if (val)
				hwc->hwc_timeout = val;
			break;

		case HWC_DATA_HW_LINK_CONNECT:
		case HWC_DATA_HW_LINK_DISCONNECT:
			ac = gd->gdma_context->mana.driver_data;
			if (!ac)
				break;

			WRITE_ONCE(ac->link_event, type);
			schedule_work(&ac->link_change_work);

			break;

		default:
			dev_warn(hwc->dev, "Received unknown reconfig type %u\n", type);
			break;
		}

		break;
	case GDMA_EQE_HWC_SOC_SERVICE:
		service_data.as_uint32 = event->details[0];
		type = service_data.type;

		switch (type) {
		case GDMA_SERVICE_TYPE_RDMA_SUSPEND:
		case GDMA_SERVICE_TYPE_RDMA_RESUME:
			ret = mana_rdma_service_event(gd->gdma_context, type);
			if (ret)
				dev_err(hwc->dev, "Failed to schedule adev service event: %d\n",
					ret);
			break;
		default:
			dev_warn(hwc->dev, "Received unknown SOC service type %u\n", type);
			break;
		}

		break;
	default:
		dev_warn(hwc->dev, "Received unknown gdma event %u\n", event->type);
		/* Ignore unknown events, which should never happen. */
		break;
	}
}

/* An RX WQE whose SGE the handler cannot trust is deliberately not
 * reposted: reposting a slot we may have mis-identified could double-post
 * a buffer the device still owns.  Each such leak permanently lowers the
 * RQ's posted depth, so once the whole depth is gone the channel can no
 * longer receive responses.  Make that terminal state explicit -- log it
 * once and shorten the command timeout so callers fail fast -- rather than
 * letting every later command drain its full timeout against a dead RQ.
 */
static void mana_hwc_rx_leak_wqe(struct hw_channel_context *hwc)
{
	if (++hwc->rx_leaked_wqe == hwc->rxq->queue_depth) {
		dev_err(hwc->dev,
			"HWC RX: RQ exhausted after %u leaked WQEs; channel unusable\n",
			hwc->rx_leaked_wqe);
		hwc->hwc_timeout = 1;
	}
}

static void mana_hwc_rx_event_handler(void *ctx, u32 gdma_rxq_id,
				      const struct hwc_rx_oob *rx_oob)
{
	struct hw_channel_context *hwc = ctx;
	struct hwc_wq *hwc_rxq = hwc->rxq;
	struct hwc_work_request *rx_req;
	struct gdma_wqe oob_snapshot;
	struct gdma_resp_hdr *resp;
	struct gdma_wqe *dma_oob;
	struct gdma_queue *rq;
	struct gdma_sge *sge;
	u64 rq_base_addr;
	u64 rx_req_idx;
	u64 sge_addr;
	u32 oob_div4;
	u16 msg_id;
	u8 *wqe;

	if (WARN_ON_ONCE(hwc_rxq->gdma_wq->id != gdma_rxq_id))
		return;

	rq = hwc_rxq->gdma_wq;
	wqe = mana_gd_get_wqe_ptr(rq, rx_oob->wqe_offset / GDMA_WQE_BU_SIZE);
	dma_oob = (struct gdma_wqe *)wqe;

	/* inline_oob_size_div4 lives in device-accessible RQ memory (shared
	 * and host-writable in a confidential VM), so snapshot it once and
	 * validate and use only the snapshot.  It is a bit-field, which
	 * READ_ONCE() cannot take the size of, so read the u32 flags word it
	 * shares through the union and extract the field from the local copy.
	 * The driver programs INLINE_OOB_SMALL_SIZE for every HWC RQ WQE via
	 * mana_gd_post_work_request(), so the only valid value is
	 * INLINE_OOB_SMALL_SIZE / 4, which puts the SGE at wqe + 16 inside
	 * this WQE's own BU.  Reject anything else -- the slot cannot be
	 * trusted, so leak this RX WQE rather than repost the wrong one.
	 */
	oob_snapshot.flags = READ_ONCE(dma_oob->flags);
	oob_div4 = oob_snapshot.inline_oob_size_div4;
	if (oob_div4 != INLINE_OOB_SMALL_SIZE / 4) {
		dev_err(hwc->dev, "HWC RX: unexpected inline_oob_size_div4=%u\n",
			oob_div4);
		mana_hwc_rx_leak_wqe(hwc);
		return;
	}
	sge = (struct gdma_sge *)(wqe + 8 + oob_div4 * 4);

	/* Recover the originating RX slot from the SGE address.  Snapshot it
	 * once, for the same shared-memory reason: of the three terms only
	 * sge_addr comes from device memory; rq_base_addr and
	 * max_resp_msg_size are driver-private.  An in-range but wrong SGE
	 * would otherwise truncate onto a neighbouring slot, letting us read
	 * a stale response that could complete the wrong, reused in-flight
	 * request.  Require the index in range AND the address to exactly
	 * match the value the driver posted for that slot.
	 */
	sge_addr = READ_ONCE(sge->address);
	rq_base_addr = hwc_rxq->msg_buf->mem_info.dma_handle;
	rx_req_idx = (sge_addr - rq_base_addr) / hwc->max_resp_msg_size;

	if (rx_req_idx >= hwc_rxq->queue_depth) {
		/* Cannot identify the slot, so we cannot safely repost this
		 * WQE; leak it.  An out-of-range index means a corrupted SGE
		 * from hardware or host tampering.
		 */
		dev_err(hwc->dev, "HWC RX: SGE idx %llu out of range\n",
			rx_req_idx);
		mana_hwc_rx_leak_wqe(hwc);
		return;
	}

	rx_req = &hwc_rxq->msg_buf->reqs[rx_req_idx];
	if (sge_addr != (u64)rx_req->buf_sge_addr) {
		/* In-range index but the address does not match what the
		 * driver posted for that slot; the same unrecoverable case,
		 * so leak this WQE rather than repost the wrong one.
		 */
		dev_err(hwc->dev, "HWC RX: invalid SGE address %llx (idx=%llu)\n",
			sge_addr, rx_req_idx);
		mana_hwc_rx_leak_wqe(hwc);
		return;
	}

	resp = (struct gdma_resp_hdr *)rx_req->buf_va;

	/* Read msg_id once from the DMA buffer to prevent TOCTOU: DMA memory
	 * is shared/unencrypted in CVMs, so the host can modify it between
	 * reads.  A short response is not rejected here; it is handed to
	 * mana_hwc_handle_resp() below, whose mana_hwc_verify_resp_msg()
	 * fails it with -EPROTO and completes the waiting sender, so one
	 * malformed response cannot stall the whole channel.
	 */
	msg_id = READ_ONCE(resp->response.hwc_msg_id);
	if (msg_id >= hwc->num_inflight_msg) {
		dev_err(hwc->dev, "HWC RX: wrong msg_id=%u\n", msg_id);
		mana_hwc_post_rx_wqe(hwc_rxq, rx_req);
		return;
	}

	mana_hwc_handle_resp(hwc, rx_oob->tx_oob_data_size, rx_req, msg_id);

	/* Can no longer use 'resp', because the buffer is posted to the HW
	 * in mana_hwc_handle_resp() above.
	 */
	resp = NULL;
}

static void mana_hwc_tx_event_handler(void *ctx, u32 gdma_txq_id,
				      const struct hwc_rx_oob *rx_oob)
{
	struct hw_channel_context *hwc = ctx;
	struct hwc_wq *hwc_txq = hwc->txq;

	WARN_ON_ONCE(!hwc_txq || hwc_txq->gdma_wq->id != gdma_txq_id);
}

static int mana_hwc_create_gdma_wq(struct hw_channel_context *hwc,
				   enum gdma_queue_type type, u64 queue_size,
				   struct gdma_queue **queue)
{
	struct gdma_queue_spec spec = {};

	if (type != GDMA_SQ && type != GDMA_RQ)
		return -EINVAL;

	spec.type = type;
	spec.monitor_avl_buf = false;
	spec.queue_size = queue_size;

	return mana_gd_create_hwc_queue(hwc->gdma_dev, &spec, queue);
}

static int mana_hwc_create_gdma_cq(struct hw_channel_context *hwc,
				   u64 queue_size,
				   void *ctx, gdma_cq_callback *cb,
				   struct gdma_queue *parent_eq,
				   struct gdma_queue **queue)
{
	struct gdma_queue_spec spec = {};

	spec.type = GDMA_CQ;
	spec.monitor_avl_buf = false;
	spec.queue_size = queue_size;
	spec.cq.context = ctx;
	spec.cq.callback = cb;
	spec.cq.parent_eq = parent_eq;

	return mana_gd_create_hwc_queue(hwc->gdma_dev, &spec, queue);
}

static int mana_hwc_create_gdma_eq(struct hw_channel_context *hwc,
				   u64 queue_size,
				   void *ctx, gdma_eq_callback *cb,
				   struct gdma_queue **queue)
{
	struct gdma_queue_spec spec = {};

	spec.type = GDMA_EQ;
	spec.monitor_avl_buf = false;
	spec.queue_size = queue_size;
	spec.eq.context = ctx;
	spec.eq.callback = cb;
	spec.eq.log2_throttle_limit = DEFAULT_LOG2_THROTTLING_FOR_ERROR_EQ;
	spec.eq.msix_index = 0;

	return mana_gd_create_hwc_queue(hwc->gdma_dev, &spec, queue);
}

static void mana_hwc_comp_event(void *ctx, struct gdma_queue *q_self)
{
	struct hwc_rx_oob comp_data = {};
	struct gdma_comp *completions;
	struct hwc_cq *hwc_cq = ctx;
	int comp_read, i;

	WARN_ON_ONCE(hwc_cq->gdma_cq != q_self);

	completions = hwc_cq->comp_buf;
	comp_read = mana_gd_poll_cq(q_self, completions, hwc_cq->queue_depth);
	WARN_ON_ONCE(comp_read <= 0 || comp_read > hwc_cq->queue_depth);

	for (i = 0; i < comp_read; ++i) {
		comp_data = *(struct hwc_rx_oob *)completions[i].cqe_data;

		if (completions[i].is_sq)
			hwc_cq->tx_event_handler(hwc_cq->tx_event_ctx,
						completions[i].wq_num,
						&comp_data);
		else
			hwc_cq->rx_event_handler(hwc_cq->rx_event_ctx,
						completions[i].wq_num,
						&comp_data);
	}

	mana_gd_ring_cq(q_self, SET_ARM_BIT);
}

static void mana_hwc_destroy_cq(struct gdma_context *gc, struct hwc_cq *hwc_cq)
{
	/* Destroy the EQ before the CQ.  mana_gd_destroy_queue() on the EQ
	 * deregisters its IRQ and waits out in-flight handlers, fencing all
	 * EQE dispatch — both the completion path and HWC init/reconfig
	 * events.  Freeing the CQ first would leave the EQ live and able to
	 * dispatch an event that dereferences hwc->cq->gdma_cq (e.g.
	 * mana_hwc_init_event_handler()) after it has been freed.
	 */
	if (hwc_cq->gdma_eq)
		mana_gd_destroy_queue(gc, hwc_cq->gdma_eq);

	/* comp_buf is reached only by mana_hwc_comp_event(), invoked from
	 * the now-fenced EQ handler, so it is safe to free once the EQ and
	 * CQ are gone.
	 */
	if (hwc_cq->gdma_cq)
		mana_gd_destroy_queue(gc, hwc_cq->gdma_cq);

	kfree(hwc_cq->comp_buf);
	kfree(hwc_cq);
}

static int mana_hwc_create_cq(struct hw_channel_context *hwc, u16 q_depth,
			      gdma_eq_callback *callback, void *ctx,
			      hwc_rx_event_handler_t *rx_ev_hdlr,
			      void *rx_ev_ctx,
			      hwc_tx_event_handler_t *tx_ev_hdlr,
			      void *tx_ev_ctx, struct hwc_cq **hwc_cq_ptr)
{
	struct gdma_queue *eq, *cq;
	struct gdma_comp *comp_buf;
	struct hwc_cq *hwc_cq;
	u32 eq_size, cq_size;
	int err;

	eq_size = roundup_pow_of_two(GDMA_EQE_SIZE * q_depth);
	if (eq_size < MANA_MIN_QSIZE)
		eq_size = MANA_MIN_QSIZE;

	cq_size = roundup_pow_of_two(GDMA_CQE_SIZE * q_depth);
	if (cq_size < MANA_MIN_QSIZE)
		cq_size = MANA_MIN_QSIZE;

	hwc_cq = kzalloc_obj(*hwc_cq);
	if (!hwc_cq)
		return -ENOMEM;

	err = mana_hwc_create_gdma_eq(hwc, eq_size, ctx, callback, &eq);
	if (err) {
		dev_err(hwc->dev, "Failed to create HWC EQ for RQ: %d\n", err);
		goto out;
	}
	hwc_cq->gdma_eq = eq;

	err = mana_hwc_create_gdma_cq(hwc, cq_size, hwc_cq, mana_hwc_comp_event,
				      eq, &cq);
	if (err) {
		dev_err(hwc->dev, "Failed to create HWC CQ for RQ: %d\n", err);
		goto out;
	}
	hwc_cq->gdma_cq = cq;

	comp_buf = kzalloc_objs(*comp_buf, q_depth);
	if (!comp_buf) {
		err = -ENOMEM;
		goto out;
	}

	hwc_cq->hwc = hwc;
	hwc_cq->comp_buf = comp_buf;
	hwc_cq->queue_depth = q_depth;
	hwc_cq->rx_event_handler = rx_ev_hdlr;
	hwc_cq->rx_event_ctx = rx_ev_ctx;
	hwc_cq->tx_event_handler = tx_ev_hdlr;
	hwc_cq->tx_event_ctx = tx_ev_ctx;

	*hwc_cq_ptr = hwc_cq;
	return 0;
out:
	mana_hwc_destroy_cq(hwc->gdma_dev->gdma_context, hwc_cq);
	return err;
}

static int mana_hwc_alloc_dma_buf(struct hw_channel_context *hwc, u16 q_depth,
				  u32 max_msg_size,
				  struct hwc_dma_buf **dma_buf_ptr)
{
	struct gdma_context *gc = hwc->gdma_dev->gdma_context;
	struct hwc_work_request *hwc_wr;
	struct hwc_dma_buf *dma_buf;
	struct gdma_mem_info *gmi;
	void *virt_addr;
	u32 buf_size;
	u8 *base_pa;
	int err;
	u16 i;

	dma_buf = kzalloc_flex(*dma_buf, reqs, q_depth);
	if (!dma_buf)
		return -ENOMEM;

	dma_buf->num_reqs = q_depth;

	buf_size = MANA_PAGE_ALIGN(q_depth * max_msg_size);

	gmi = &dma_buf->mem_info;
	err = mana_gd_alloc_memory(gc, buf_size, gmi);
	if (err) {
		dev_err(hwc->dev, "Failed to allocate DMA buffer size: %u, err %d\n",
			buf_size, err);
		goto out;
	}

	virt_addr = dma_buf->mem_info.virt_addr;
	base_pa = (u8 *)dma_buf->mem_info.dma_handle;

	for (i = 0; i < q_depth; i++) {
		hwc_wr = &dma_buf->reqs[i];

		hwc_wr->buf_va = virt_addr + i * max_msg_size;
		hwc_wr->buf_sge_addr = base_pa + i * max_msg_size;

		hwc_wr->buf_len = max_msg_size;
	}

	*dma_buf_ptr = dma_buf;
	return 0;
out:
	kfree(dma_buf);
	return err;
}

static void mana_hwc_dealloc_dma_buf(struct hw_channel_context *hwc,
				     struct hwc_dma_buf *dma_buf)
{
	if (!dma_buf)
		return;

	mana_gd_free_memory(&dma_buf->mem_info);

	kfree(dma_buf);
}

static void mana_hwc_destroy_wq(struct hw_channel_context *hwc,
				struct hwc_wq *hwc_wq)
{
	mana_hwc_dealloc_dma_buf(hwc, hwc_wq->msg_buf);

	if (hwc_wq->gdma_wq)
		mana_gd_destroy_queue(hwc->gdma_dev->gdma_context,
				      hwc_wq->gdma_wq);

	kfree(hwc_wq);
}

static int mana_hwc_create_wq(struct hw_channel_context *hwc,
			      enum gdma_queue_type q_type, u16 q_depth,
			      u32 max_msg_size, struct hwc_cq *hwc_cq,
			      struct hwc_wq **hwc_wq_ptr)
{
	struct gdma_queue *queue;
	struct hwc_wq *hwc_wq;
	u32 queue_size;
	int err;

	WARN_ON(q_type != GDMA_SQ && q_type != GDMA_RQ);

	if (q_type == GDMA_RQ)
		queue_size = roundup_pow_of_two(GDMA_MAX_RQE_SIZE * q_depth);
	else
		queue_size = roundup_pow_of_two(GDMA_MAX_SQE_SIZE * q_depth);

	if (queue_size < MANA_MIN_QSIZE)
		queue_size = MANA_MIN_QSIZE;

	hwc_wq = kzalloc_obj(*hwc_wq);
	if (!hwc_wq)
		return -ENOMEM;

	err = mana_hwc_create_gdma_wq(hwc, q_type, queue_size, &queue);
	if (err)
		goto out;

	hwc_wq->hwc = hwc;
	hwc_wq->gdma_wq = queue;
	hwc_wq->queue_depth = q_depth;
	hwc_wq->hwc_cq = hwc_cq;

	err = mana_hwc_alloc_dma_buf(hwc, q_depth, max_msg_size,
				     &hwc_wq->msg_buf);
	if (err)
		goto out;

	*hwc_wq_ptr = hwc_wq;
	return 0;
out:
	if (err)
		mana_hwc_destroy_wq(hwc, hwc_wq);

	dev_err(hwc->dev, "Failed to create HWC queue size= %u type= %d err= %d\n",
		queue_size, q_type, err);
	return err;
}

static int mana_hwc_post_tx_wqe(const struct hwc_wq *hwc_txq,
				struct hwc_work_request *req,
				u32 dest_virt_rq_id, u32 dest_virt_rcq_id,
				bool dest_pf)
{
	struct device *dev = hwc_txq->hwc->dev;
	struct hwc_tx_oob *tx_oob;
	struct gdma_sge *sge;
	int err;

	if (req->msg_size == 0 || req->msg_size > req->buf_len) {
		dev_err(dev, "wrong msg_size: %u, buf_len: %u\n",
			req->msg_size, req->buf_len);
		return -EINVAL;
	}

	tx_oob = &req->tx_oob;

	tx_oob->vrq_id = dest_virt_rq_id;
	tx_oob->dest_vfid = 0;
	tx_oob->vrcq_id = dest_virt_rcq_id;
	tx_oob->vscq_id = hwc_txq->hwc_cq->gdma_cq->id;
	tx_oob->loopback = false;
	tx_oob->lso_override = false;
	tx_oob->dest_pf = dest_pf;
	tx_oob->vsq_id = hwc_txq->gdma_wq->id;

	sge = &req->sge;
	sge->address = (u64)req->buf_sge_addr;
	sge->mem_key = hwc_txq->msg_buf->gpa_mkey;
	sge->size = req->msg_size;

	memset(&req->wqe_req, 0, sizeof(struct gdma_wqe_request));
	req->wqe_req.sgl = sge;
	req->wqe_req.num_sge = 1;
	req->wqe_req.inline_oob_size = sizeof(struct hwc_tx_oob);
	req->wqe_req.inline_oob_data = tx_oob;
	req->wqe_req.client_data_unit = 0;

	err = mana_gd_post_and_ring(hwc_txq->gdma_wq, &req->wqe_req, NULL);
	if (err)
		dev_err(dev, "Failed to post WQE on HWC SQ: %d\n", err);
	return err;
}

static int mana_hwc_init_inflight_msg(struct hw_channel_context *hwc,
				      u16 num_msg)
{
	int err;

	init_waitqueue_head(&hwc->msg_waitq);

	err = mana_gd_alloc_res_map(num_msg, &hwc->inflight_msg_res);
	if (err)
		dev_err(hwc->dev, "Failed to init inflight_msg_res: %d\n", err);
	return err;
}

static int mana_hwc_test_channel(struct hw_channel_context *hwc, u16 q_depth,
				 u32 max_req_msg_size, u32 max_resp_msg_size)
{
	struct gdma_context *gc = hwc->gdma_dev->gdma_context;
	struct hwc_wq *hwc_rxq = hwc->rxq;
	struct hwc_work_request *req;
	struct hwc_caller_ctx *ctx;
	int err;
	int i;

	/* Post all WQEs on the RQ */
	for (i = 0; i < q_depth; i++) {
		req = &hwc_rxq->msg_buf->reqs[i];
		err = mana_hwc_post_rx_wqe(hwc_rxq, req);
		if (err)
			return err;
	}

	ctx = kzalloc_objs(*ctx, q_depth);
	if (!ctx)
		return -ENOMEM;

	for (i = 0; i < q_depth; ++i) {
		spin_lock_init(&ctx[i].lock);
		init_completion(&ctx[i].comp_event);
	}

	hwc->caller_ctx = ctx;

	return mana_gd_test_eq(gc, hwc->cq->gdma_eq);
}

static int mana_hwc_establish_channel(struct gdma_context *gc, u16 *q_depth,
				      u32 *max_req_msg_size,
				      u32 *max_resp_msg_size)
{
	/* No RCU needed: called only from mana_hwc_create_channel
	 * during init, before the channel is published to senders.
	 */
	struct hw_channel_context *hwc = gc->hwc.driver_data;
	struct gdma_queue *rq = hwc->rxq->gdma_wq;
	struct gdma_queue *sq = hwc->txq->gdma_wq;
	struct gdma_queue *eq = hwc->cq->gdma_eq;
	struct gdma_queue *cq = hwc->cq->gdma_cq;
	struct gdma_queue __rcu **cq_table;
	u32 num_cqs;
	u32 cq_id;
	int err;

	init_completion(&hwc->hwc_init_eqe_comp);

	/* Arm setup_active before issuing the setup command.
	 * mana_smc_setup_hwc() hands the queue PFNs to the PF, activating
	 * MST entries so the device can DMA into our queue buffers, before
	 * it can report a later failure such as a possession-poll timeout.
	 * Recording it up front guarantees the error path
	 * (mana_hwc_create_channel() -> mana_hwc_destroy_channel()) still
	 * tears the HWC down instead of freeing buffers the device may still
	 * write to.  Setting it for a rare pre-submission failure too is
	 * harmless -- the teardown is then a no-op the device ignores.  Do
	 * not also tear down here: a second teardown would double the
	 * hardware timeout on failure and mask the original error code.
	 */
	hwc->setup_active = true;

	err = mana_smc_setup_hwc(&gc->shm_channel, false,
				 eq->mem_info.dma_handle,
				 cq->mem_info.dma_handle,
				 rq->mem_info.dma_handle,
				 sq->mem_info.dma_handle,
				 eq->eq.msix_index);
	if (err)
		return err;

	if (!wait_for_completion_timeout(&hwc->hwc_init_eqe_comp, 60 * HZ))
		return -ETIMEDOUT;

	*q_depth = hwc->hwc_init_q_depth_max;
	*max_req_msg_size = hwc->hwc_init_max_req_msg_size;
	*max_resp_msg_size = hwc->hwc_init_max_resp_msg_size;

	/* Snapshot the device-reported CQ count and CQ id into locals and
	 * use only the locals below, so the same value that sizes cq_table
	 * also bounds and indexes it -- even across the sleeping vcalloc().
	 * Both fields are written by mana_hwc_init_event_handler() from EQ
	 * interrupt context: hwc_init_max_num_cqs under WRITE_ONCE() (paired
	 * here), and cq->id as an ordinary store.  READ_ONCE() keeps each
	 * read tear-free and, crucially, non-reloadable, so a spoofed
	 * post-init event cannot make the WARN_ON() pass against one value
	 * while the allocation or the index uses another.
	 */
	num_cqs = READ_ONCE(hwc->hwc_init_max_num_cqs);
	cq_id = READ_ONCE(cq->id);

	if (WARN_ON(cq_id >= num_cqs))
		return -EPROTO;

	cq_table = vcalloc(num_cqs, sizeof(*cq_table));
	if (!cq_table)
		return -ENOMEM;

	gc->max_num_cqs = num_cqs;

	rcu_assign_pointer(cq_table[cq_id], cq);
	/* Publish the fully-initialised table last; pairs with the
	 * rcu_dereference(gc->cq_table) in mana_gd_process_eqe().
	 */
	rcu_assign_pointer(gc->cq_table, cq_table);

	return 0;
}

static int mana_hwc_init_queues(struct hw_channel_context *hwc, u16 q_depth,
				u32 max_req_msg_size, u32 max_resp_msg_size)
{
	int err;

	err = mana_hwc_init_inflight_msg(hwc, q_depth);
	if (err)
		return err;

	/* CQ is shared by SQ and RQ, so CQ's queue depth is the sum of SQ
	 * queue depth and RQ queue depth.
	 */
	err = mana_hwc_create_cq(hwc, q_depth * 2,
				 mana_hwc_init_event_handler, hwc,
				 mana_hwc_rx_event_handler, hwc,
				 mana_hwc_tx_event_handler, hwc, &hwc->cq);
	if (err) {
		dev_err(hwc->dev, "Failed to create HWC CQ: %d\n", err);
		goto out;
	}

	err = mana_hwc_create_wq(hwc, GDMA_RQ, q_depth, max_resp_msg_size,
				 hwc->cq, &hwc->rxq);
	if (err) {
		dev_err(hwc->dev, "Failed to create HWC RQ: %d\n", err);
		goto out;
	}

	err = mana_hwc_create_wq(hwc, GDMA_SQ, q_depth, max_req_msg_size,
				 hwc->cq, &hwc->txq);
	if (err) {
		dev_err(hwc->dev, "Failed to create HWC SQ: %d\n", err);
		goto out;
	}

	hwc->num_inflight_msg = q_depth;
	hwc->max_req_msg_size = max_req_msg_size;
	hwc->max_resp_msg_size = max_resp_msg_size;

	return 0;
out:
	/* mana_hwc_create_channel() will do the cleanup.*/
	return err;
}

int mana_hwc_create_channel(struct gdma_context *gc)
{
	u32 max_req_msg_size, max_resp_msg_size;
	struct gdma_dev *gd = &gc->hwc;
	struct hw_channel_context *hwc;
	u16 q_depth_max;
	int err;

	/* A previous teardown may have failed and deliberately left the old
	 * HWC context reachable (see mana_hwc_destroy_channel()).  Retry the
	 * teardown now -- the device has since been reset -- before building
	 * a new channel, so we neither orphan the old context nor stack a
	 * second channel on one whose DESTROY_HWC never completed.  If it is
	 * still failing, return an error that steers mana_serv_reset() to a
	 * full PCI rescan instead of silently leaking another generation.
	 */
	if (gd->driver_data) {
		mana_hwc_destroy_channel(gc);
		if (gd->driver_data)
			return -ETIMEDOUT;
	}

	hwc = kzalloc_obj(*hwc);
	if (!hwc)
		return -ENOMEM;

	gd->gdma_context = gc;
	gd->driver_data = hwc;
	hwc->gdma_dev = gd;
	hwc->dev = gc->dev;
	hwc->hwc_timeout = HW_CHANNEL_WAIT_RESOURCE_TIMEOUT_MS;

	/* HWC's instance number is always 0. */
	gd->dev_id.as_uint32 = 0;
	gd->dev_id.type = GDMA_DEVICE_HWC;

	gd->pdid = INVALID_PDID;
	gd->doorbell = INVALID_DOORBELL;

	/* mana_hwc_init_queues() only creates the required data structures,
	 * and doesn't touch the HWC device.
	 */
	err = mana_hwc_init_queues(hwc, HW_CHANNEL_VF_BOOTSTRAP_QUEUE_DEPTH,
				   HW_CHANNEL_MAX_REQUEST_SIZE,
				   HW_CHANNEL_MAX_RESPONSE_SIZE);
	if (err) {
		dev_err(hwc->dev, "Failed to initialize HWC: %d\n", err);
		goto out;
	}

	err = mana_hwc_establish_channel(gc, &q_depth_max, &max_req_msg_size,
					 &max_resp_msg_size);
	if (err) {
		dev_err(hwc->dev, "Failed to establish HWC: %d\n", err);
		goto out;
	}

	err = mana_hwc_test_channel(gc->hwc.driver_data,
				    HW_CHANNEL_VF_BOOTSTRAP_QUEUE_DEPTH,
				    max_req_msg_size, max_resp_msg_size);
	if (err) {
		dev_err(hwc->dev, "Failed to test HWC: %d\n", err);
		goto out;
	}

	return 0;
out:
	mana_hwc_destroy_channel(gc);
	return err;
}

void mana_hwc_destroy_channel(struct gdma_context *gc)
{
	struct hw_channel_context *hwc = gc->hwc.driver_data;
	struct gdma_queue __rcu **old_cq_table;

	if (!hwc)
		return;

	/* Tear down the HWC if setup_hwc previously activated MST entries.
	 * This is the definitive flag — unlike max_num_cqs which depends
	 * on the init EQE arriving.
	 *
	 * If teardown fails the device may still have active MST entries
	 * and can DMA into the HWC queue buffers.  Freeing them would risk
	 * memory corruption on systems without an IOMMU to fault the stale
	 * DMA, so leak the HWC resources instead of handing the pages back
	 * to the allocator.  Keep setup_active set so the failure is not
	 * mistaken for a clean teardown.
	 */
	if (hwc->setup_active) {
		int td_err = mana_smc_teardown_hwc(&gc->shm_channel, false);

		if (td_err) {
			dev_err(gc->dev,
				"HWC teardown failed: %d, leaking resources\n",
				td_err);
			return;
		}

		hwc->setup_active = false;
	}

	/* Tear down the HWC CQ object first — mana_hwc_destroy_cq()
	 * both unpublishes the CQ from cq_table (+synchronize_rcu) and
	 * deregisters the HWC EQ from the interrupt handler list (via
	 * mana_gd_deregister_irq + synchronize_rcu), guaranteeing no
	 * interrupt handler can access RQ/TXQ buffers after this point.
	 */
	if (hwc->cq)
		mana_hwc_destroy_cq(hwc->gdma_dev->gdma_context, hwc->cq);

	if (hwc->txq)
		mana_hwc_destroy_wq(hwc, hwc->txq);

	if (hwc->rxq)
		mana_hwc_destroy_wq(hwc, hwc->rxq);

	kfree(hwc->caller_ctx);
	hwc->caller_ctx = NULL;

	mana_gd_free_res_map(&hwc->inflight_msg_res);

	hwc->num_inflight_msg = 0;

	hwc->gdma_dev->doorbell = INVALID_DOORBELL;
	hwc->gdma_dev->pdid = INVALID_PDID;

	hwc->hwc_timeout = 0;

	kfree(hwc);
	gc->hwc.driver_data = NULL;
	gc->hwc.gdma_context = NULL;

	old_cq_table = rcu_replace_pointer(gc->cq_table, NULL, true);
	synchronize_rcu();
	vfree(old_cq_table);
}

int mana_hwc_send_request(struct hw_channel_context *hwc, u32 req_len,
			  const void *req, u32 resp_len, void *resp)
{
	struct gdma_context *gc = hwc->gdma_dev->gdma_context;
	struct hwc_work_request *tx_wr;
	struct hwc_wq *txq = hwc->txq;
	struct gdma_req_hdr *req_msg;
	struct hwc_caller_ctx *ctx;
	unsigned long flags;
	bool drop_resp_ref;
	u32 dest_vrcq = 0;
	u32 dest_vrq = 0;
	u32 command;
	u32 status;
	u32 wait_ms;
	u16 msg_id;
	int err;

	err = mana_hwc_get_msg_index(hwc, &msg_id);
	if (err)
		return err;

	tx_wr = &txq->msg_buf->reqs[msg_id];

	if (req_len > tx_wr->buf_len) {
		dev_err(hwc->dev, "HWC: req msg size: %d > %d\n", req_len,
			tx_wr->buf_len);
		err = -EINVAL;
		goto out;
	}

	ctx = hwc->caller_ctx + msg_id;

	spin_lock_irqsave(&ctx->lock, flags);
	ctx->output_buf = resp;
	ctx->output_buflen = resp_len;
	spin_unlock_irqrestore(&ctx->lock, flags);

	req_msg = (struct gdma_req_hdr *)tx_wr->buf_va;
	if (req)
		memcpy(req_msg, req, req_len);

	req_msg->req.hwc_msg_id = msg_id;

	tx_wr->msg_size = req_len;
	command = req_msg->req.msg_type;

	if (gc->is_pf) {
		dest_vrq = hwc->pf_dest_vrq_id;
		dest_vrcq = hwc->pf_dest_vrcq_id;
	}

	/* handle_resp()'s reference was taken in mana_hwc_get_msg_index(),
	 * so hardware responding immediately after the doorbell ring cannot
	 * release the slot before this sender is done with it.
	 */
	err = mana_hwc_post_tx_wqe(txq, tx_wr, dest_vrq, dest_vrcq, false);
	if (err) {
		dev_err(hwc->dev, "HWC: Failed to post send WQE: %d\n", err);
		goto out;
	}

	wait_ms = hwc->hwc_timeout;
	if (!wait_for_completion_timeout(&ctx->comp_event,
					 msecs_to_jiffies(wait_ms))) {
		if (wait_ms != 0)
			dev_err(hwc->dev, "Command 0x%x timed out: %u ms\n",
				command, wait_ms);

		/* NULL out output_buf so a late handle_resp() won't write
		 * into the caller's buffer after the sender returns, then
		 * check whether handle_resp() already delivered a valid
		 * response between the timeout firing and this lock
		 * acquisition — ctx->error != -EINPROGRESS means it ran.
		 */
		spin_lock_irqsave(&ctx->lock, flags);
		ctx->output_buf = NULL;
		err = ctx->error;
		status = ctx->status_code;
		spin_unlock_irqrestore(&ctx->lock, flags);

		if (err != -EINPROGRESS) {
			/* handle_resp() delivered a valid response just after
			 * the timeout fired.  The hardware is alive, so use
			 * the response and leave the channel usable; do not
			 * latch hwc_timed_out or degrade hwc_timeout for what
			 * turned out to be a transient race.
			 */
			hwc_ctx_put(hwc, ctx);
			goto check_status;
		}

		err = -ETIMEDOUT;

		/* A deliberate no-wait send -- mana_serv_reset() sets
		 * hwc_timeout = 0 when the HWC is already unresponsive and it
		 * only needs to best-effort post the teardown commands -- is
		 * expected to expire here.  Do not latch hwc_timed_out for it:
		 * that would make mana_hwc_get_msg_index() reject the remaining
		 * teardown commands before they are even posted.  Release the
		 * slot through the out: path so the next command can reuse it,
		 * matching the pre-refcount behaviour where every command was
		 * posted and only the wait was skipped.
		 */
		if (wait_ms == 0)
			goto out;

		/* Genuine timeout: no response arrived.  Reduce further
		 * waiting, and mark the channel timed out under the bitmap
		 * lock so get_msg_index() cannot acquire new slots after this.
		 */
		if (hwc->hwc_timeout > 1)
			hwc->hwc_timeout = 1;

		spin_lock_irqsave(&hwc->inflight_msg_res.lock, flags);
		WRITE_ONCE(hwc->hwc_timed_out, true);
		spin_unlock_irqrestore(&hwc->inflight_msg_res.lock, flags);
		wake_up_all(&hwc->msg_waitq);

		/* Release the slot through out:, which also drops the
		 * response-side reference taken in mana_hwc_get_msg_index().
		 * A late response for this slot cannot drop it -- once the
		 * sender NULLs output_buf, mana_hwc_handle_resp() early-returns
		 * without touching the refcount -- so the sender must free it
		 * here, otherwise the slot bit would leak until channel
		 * teardown.
		 */
		goto out;
	}

	/* NULL output_buf so a late handle_resp() won't memcpy into
	 * the caller's buffer after the sender exits.  Read error and
	 * status_code under the same lock — after hwc_ctx_put the slot
	 * may be reused and these fields overwritten.
	 */
	spin_lock_irqsave(&ctx->lock, flags);
	ctx->output_buf = NULL;
	err = ctx->error;
	status = ctx->status_code;
	spin_unlock_irqrestore(&ctx->lock, flags);
	hwc_ctx_put(hwc, ctx);

check_status:
	if (err)
		goto done;

	if (status && status != GDMA_STATUS_MORE_ENTRIES) {
		if (status == GDMA_STATUS_CMD_UNSUPPORTED) {
			err = -EOPNOTSUPP;
			goto done;
		}

		if (command != MANA_QUERY_PHY_STAT)
			dev_err(hwc->dev, "Command 0x%x failed with status: 0x%x\n",
				command, status);
		err = -EPROTO;
		goto done;
	}

	err = 0;
	goto done;
out:
	/* Reached by the pre-post error paths (request never submitted), by
	 * the deliberate no-wait teardown, and by a genuine timeout (request
	 * posted, but no valid response arrived).  In every case the sender
	 * must drop the response-side reference taken in
	 * mana_hwc_get_msg_index() and its own.  Guard against a stale or
	 * forged response that raced in first: latch ->responded under the
	 * lock so any later handle_resp() is a no-op, and drop the response-
	 * side reference here only if handle_resp() has not already done so.
	 */
	ctx = hwc->caller_ctx + msg_id;
	spin_lock_irqsave(&ctx->lock, flags);
	ctx->output_buf = NULL;
	drop_resp_ref = !ctx->responded;
	ctx->responded = true;
	spin_unlock_irqrestore(&ctx->lock, flags);
	if (drop_resp_ref)
		refcount_dec(&ctx->refcnt);
	hwc_ctx_put(hwc, ctx);
done:
	return err;
}
