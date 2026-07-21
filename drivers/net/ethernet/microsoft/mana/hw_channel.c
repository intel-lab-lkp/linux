// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright (c) 2021, Microsoft Corporation. */

#include <net/mana/gdma.h>
#include <net/mana/mana.h>
#include <net/mana/hw_channel.h>
#include <linux/pci.h>
#include <linux/vmalloc.h>

/* Acquire a free message slot from the inflight bitmap.  Returns
 * -ENODEV if the channel is torn down, or -ETIMEDOUT if a prior HWC
 * command has timed out (preserving the error code callers expect).
 */
static int mana_hwc_get_msg_index(struct hw_channel_context *hwc, u16 *msg_id)
{
	struct gdma_resource *r = &hwc->inflight_msg_res;
	unsigned long flags;
	u32 index;

	for (;;) {
		spin_lock_irqsave(&r->lock, flags);

		if (!hwc->channel_up || hwc->hwc_timed_out) {
			spin_unlock_irqrestore(&r->lock, flags);
			return hwc->channel_up ? -ETIMEDOUT : -ENODEV;
		}

		index = find_first_zero_bit(r->map, r->size);
		if (index < r->size) {
			struct hwc_caller_ctx *ctx;

			bitmap_set(r->map, index, 1);
			ctx = &hwc->caller_ctx[index];
			reinit_completion(&ctx->comp_event);
			refcount_set(&ctx->refcnt, 1);
			ctx->msg_id = index;
			ctx->error = -EINPROGRESS;
			spin_unlock_irqrestore(&r->lock, flags);
			break;
		}
		spin_unlock_irqrestore(&r->lock, flags);

		wait_event(hwc->msg_waitq,
			   !hwc->channel_up ||
			   hwc->hwc_timed_out ||
			   !bitmap_full(r->map, r->size));

		if (!hwc->channel_up)
			return -ENODEV;

		if (hwc->hwc_timed_out)
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

	/* Validate msg_id is in range before using it to index bitmap
	 * and caller_ctx array.  Malicious firmware could send
	 * out-of-range msg_id causing out-of-bounds access.
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

	err = mana_hwc_verify_resp_msg(ctx, resp_msg, resp_len);

	if (!err && ctx->output_buf) {
		ctx->status_code = resp_msg->status;
		memcpy(ctx->output_buf, resp_msg, resp_len);
		ctx->error = 0;
	} else if (ctx->output_buf) {
		/* Only overwrite error if the sender hasn't timed out
		 * or been force-completed by destroy.  When output_buf
		 * is NULL, a terminal error (-ENODEV or timeout) has
		 * already been set — preserve it so the sender doesn't
		 * see a spurious success.
		 */
		ctx->error = err;
	}

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
			/* HWC_INIT_DATA_QUEUE_DEPTH is a 24-bit field.  Keep
			 * the full device-reported value here; it is clamped
			 * and validated in mana_hwc_create_channel() rather
			 * than silently truncated to u16.
			 */
			hwc->hwc_init_q_depth_max = val;
			break;

		case HWC_INIT_DATA_MAX_REQUEST:
			hwc->hwc_init_max_req_msg_size = val;
			break;

		case HWC_INIT_DATA_MAX_RESPONSE:
			hwc->hwc_init_max_resp_msg_size = val;
			break;

		case HWC_INIT_DATA_MAX_NUM_CQS:
			gd->gdma_context->max_num_cqs = val;
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

static void mana_hwc_rx_event_handler(void *ctx, u32 gdma_rxq_id,
				      const struct hwc_rx_oob *rx_oob)
{
	struct hw_channel_context *hwc = ctx;
	struct hwc_wq *hwc_rxq = hwc->rxq;
	struct hwc_work_request *rx_req;
	struct gdma_resp_hdr *resp;
	struct gdma_wqe *dma_oob;
	struct gdma_queue *rq;
	struct gdma_sge *sge;
	u64 rq_base_addr;
	u64 rx_req_idx;
	u16 msg_id;
	u8 *wqe;

	if (WARN_ON_ONCE(hwc_rxq->gdma_wq->id != gdma_rxq_id))
		return;

	rq = hwc_rxq->gdma_wq;
	wqe = mana_gd_get_wqe_ptr(rq, rx_oob->wqe_offset / GDMA_WQE_BU_SIZE);
	dma_oob = (struct gdma_wqe *)wqe;

	sge = (struct gdma_sge *)(wqe + 8 + dma_oob->inline_oob_size_div4 * 4);

	/* inline_oob_size_div4 is read from the WQE in device-accessible RQ
	 * memory, so a malicious host in a CVM (or buggy firmware) could set
	 * it to push the SGE past the RQ buffer.  Bounds-check the SGE it
	 * locates before dereferencing sge->address below.  A validly posted
	 * WQE keeps the SGE inside the ring (worst case ends exactly at the
	 * buffer boundary); reject anything that would read past it.  The
	 * slot cannot be trusted here, so leak this RX WQE rather than repost
	 * the wrong one -- as in the SGE-address mismatch path below.
	 */
	if ((u8 *)(sge + 1) > (u8 *)rq->queue_mem_ptr + rq->queue_size) {
		dev_err(hwc->dev, "HWC RX: SGE past RQ buffer, oob_div4=%u\n",
			dma_oob->inline_oob_size_div4);
		return;
	}

	/* Recover the originating RX slot from the SGE address.  Of the three
	 * terms here only sge->address lives in device-accessible RQ memory;
	 * rq_base_addr and max_resp_msg_size are driver-private constants.  An
	 * in-range but wrong/unaligned SGE (corrupted WQE, or a malicious host
	 * in a CVM) would otherwise truncate onto a neighbouring slot, letting
	 * us read a stale response that could complete the wrong, reused
	 * in-flight request.  Require the index to be in range AND the address
	 * to exactly match the value the driver posted for that slot.
	 */
	rq_base_addr = hwc_rxq->msg_buf->mem_info.dma_handle;
	rx_req_idx = (sge->address - rq_base_addr) / hwc->max_resp_msg_size;

	if (rx_req_idx >= hwc_rxq->queue_depth) {
		/* Cannot trust which WQE this is, so we cannot safely repost
		 * it; leak one RX WQE and bail.  An out-of-range index means
		 * a corrupted SGE from hardware (or host tampering), an
		 * unrecoverable device error.
		 */
		dev_err(hwc->dev, "HWC RX: SGE idx %llu out of range\n",
			rx_req_idx);
		return;
	}

	rx_req = &hwc_rxq->msg_buf->reqs[rx_req_idx];
	if (sge->address != (u64)rx_req->buf_sge_addr) {
		/* In-range index but the address does not match what the
		 * driver posted for that slot; the same unrecoverable case,
		 * so leak this WQE rather than repost the wrong one.
		 */
		dev_err(hwc->dev, "HWC RX: invalid SGE address %llx (idx=%llu)\n",
			sge->address, rx_req_idx);
		return;
	}

	resp = (struct gdma_resp_hdr *)rx_req->buf_va;

	/* Validate resp_len covers the response header before reading
	 * hwc_msg_id.  A short response leaves stale data from the
	 * previous buffer occupant, which could match a live slot and
	 * complete the wrong request.
	 */
	if (rx_oob->tx_oob_data_size < sizeof(*resp)) {
		dev_err(hwc->dev, "HWC RX: short resp_len=%u\n",
			rx_oob->tx_oob_data_size);
		mana_hwc_post_rx_wqe(hwc_rxq, rx_req);
		return;
	}

	/* Read msg_id once from DMA buffer to prevent TOCTOU:
	 * DMA memory is shared/unencrypted in CVMs - host can
	 * modify it between reads.
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
	if (hwc_cq->gdma_cq)
		mana_gd_destroy_queue(gc, hwc_cq->gdma_cq);

	/* comp_buf is reached only by mana_hwc_comp_event(), which the
	 * EQ handler invokes via cq_table[id].  The CQ destroy above
	 * already cleared that slot and ran synchronize_rcu(), so no
	 * handler can reach comp_buf once it returns.  Destroying the EQ
	 * here additionally tears down the IRQ (defense in depth) before
	 * comp_buf and hwc_cq are freed below.
	 */
	if (hwc_cq->gdma_eq)
		mana_gd_destroy_queue(gc, hwc_cq->gdma_eq);

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
	spin_lock_init(&hwc_wq->lock);

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

static int mana_hwc_post_tx_wqe(struct hwc_wq *hwc_txq,
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

	/* Serialize WQE posting — multiple senders may call concurrently. */
	spin_lock(&hwc_txq->lock);
	err = mana_gd_post_and_ring(hwc_txq->gdma_wq, &req->wqe_req, NULL);
	spin_unlock(&hwc_txq->lock);

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

	/* channel_up must be set before the test EQ request, because
	 * the request goes through mana_hwc_get_msg_index() which
	 * checks channel_up.  caller_ctx is allocated above, so
	 * concurrent access to a NULL caller_ctx is not possible.
	 */
	hwc->channel_up = true;

	err = mana_gd_test_eq(gc, hwc->cq->gdma_eq);
	if (err)
		hwc->channel_up = false;

	return err;
}

static int mana_hwc_establish_channel(struct gdma_context *gc, u32 *q_depth,
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
	int err;

	init_completion(&hwc->hwc_init_eqe_comp);

	err = mana_smc_setup_hwc(&gc->shm_channel, false,
				 eq->mem_info.dma_handle,
				 cq->mem_info.dma_handle,
				 rq->mem_info.dma_handle,
				 sq->mem_info.dma_handle,
				 eq->eq.msix_index);
	if (err)
		return err;

	/* setup_hwc activated MST entries — hardware can now DMA into
	 * our queue buffers.  If anything below fails, we must tear
	 * down before returning so the caller doesn't need to track
	 * whether setup_hwc succeeded.
	 */
	hwc->setup_active = true;

	if (!wait_for_completion_timeout(&hwc->hwc_init_eqe_comp, 60 * HZ)) {
		err = -ETIMEDOUT;
		goto teardown;
	}

	*q_depth = hwc->hwc_init_q_depth_max;
	*max_req_msg_size = hwc->hwc_init_max_req_msg_size;
	*max_resp_msg_size = hwc->hwc_init_max_resp_msg_size;

	/* Both were set in mana_hwc_init_event_handler(). */
	if (WARN_ON(cq->id >= gc->max_num_cqs)) {
		err = -EPROTO;
		goto teardown;
	}

	cq_table = vcalloc(gc->max_num_cqs, sizeof(*cq_table));
	if (!cq_table) {
		err = -ENOMEM;
		goto teardown;
	}

	rcu_assign_pointer(cq_table[cq->id], cq);
	/* Publish the fully-initialised table last; pairs with the
	 * rcu_dereference(gc->cq_table) in mana_gd_process_eqe().
	 */
	rcu_assign_pointer(gc->cq_table, cq_table);

	return 0;

teardown:
	{
		int td_err = mana_smc_teardown_hwc(&gc->shm_channel, false);

		if (!td_err)
			hwc->setup_active = false;

		return td_err ? td_err : err;
	}
}

static int mana_hwc_init_queues(struct hw_channel_context *hwc, u16 q_depth,
				u32 max_req_msg_size, u32 max_resp_msg_size)
{
	int err;

	/* CQ depth is q_depth * 2 (SQ + RQ) passed as u16 to create_cq.
	 * Cap to prevent u16 truncation.
	 */
	if (q_depth > U16_MAX / 2)
		q_depth = U16_MAX / 2;

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

/* Tear down all HWC queues and free associated resources.  Used on
 * the reinit-with-higher-queue-depth path and reinit fallback.
 *
 * PRECONDITION: must be called only during channel bring-up in
 * mana_hwc_create_channel(), before the channel is published to
 * senders.  There the setup thread is effectively single-threaded
 * (serialized against teardown by the PCI/PM device_lock or, on the
 * service path, GC_IN_SERVICE, with the data path not yet probed),
 * channel_up is still false, caller_ctx is not yet allocated, and
 * active_senders is 0 — so no concurrent request/response user can
 * touch these queues.  That is why this skips the hwc_lock-protected
 * driver_data clear + active_senders drain that
 * mana_hwc_destroy_channel() needs for the runtime teardown race;
 * only the CQ-first ordering below (to fence off a pending interrupt)
 * is required.  Calling this on a live, published channel would be a
 * use-after-free.
 */
static void mana_hwc_destroy_queues(struct hw_channel_context *hwc)
{
	struct gdma_context *gc = hwc->gdma_dev->gdma_context;

	/* Destroy CQ first to deregister the EQ from the interrupt
	 * handler list before freeing caller_ctx, TXQ, or RXQ memory.
	 * A pending interrupt handler could still reach handle_resp()
	 * which dereferences caller_ctx.
	 */
	if (hwc->cq) {
		mana_hwc_destroy_cq(gc, hwc->cq);
		hwc->cq = NULL;
	}

	kfree(hwc->caller_ctx);
	hwc->caller_ctx = NULL;

	if (hwc->txq) {
		mana_hwc_destroy_wq(hwc, hwc->txq);
		hwc->txq = NULL;
	}

	if (hwc->rxq) {
		mana_hwc_destroy_wq(hwc, hwc->rxq);
		hwc->rxq = NULL;
	}

	mana_gd_free_res_map(&hwc->inflight_msg_res);
	hwc->num_inflight_msg = 0;
}

int mana_hwc_create_channel(struct gdma_context *gc)
{
	u32 max_req_msg_size, max_resp_msg_size;
	struct gdma_dev *gd = &gc->hwc;
	struct hw_channel_context *hwc;
	struct gdma_queue __rcu **old_cq_table;
	unsigned long flags;
	u32 q_depth_max;
	int err;

	hwc = kzalloc_obj(*hwc);
	if (!hwc)
		return -ENOMEM;

	gd->gdma_context = gc;
	hwc->gdma_dev = gd;
	hwc->dev = gc->dev;
	hwc->hwc_timeout = HW_CHANNEL_WAIT_RESOURCE_TIMEOUT_MS;
	hwc->active_senders = 0;
	init_waitqueue_head(&gc->hwc_drain_waitq);

	/* HWC's instance number is always 0. */
	gd->dev_id.as_uint32 = 0;
	gd->dev_id.type = GDMA_DEVICE_HWC;

	gd->pdid = INVALID_PDID;
	gd->doorbell = INVALID_DOORBELL;

	/* Publish driver_data last, under hwc_lock: the lock orders the hwc
	 * initialisation above before the pointer becomes visible and
	 * serialises the publish against the control-plane readers in
	 * mana_gd_send_request(), mana_need_log() and mana_serv_reset().
	 */
	spin_lock_irqsave(&gc->hwc_lock, flags);
	gc->hwc.driver_data = hwc;
	spin_unlock_irqrestore(&gc->hwc_lock, flags);

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

	/* The channel was bootstrapped at a minimal queue depth.  If the
	 * device reports a higher maximum, tear down and rebuild with
	 * the larger depth so more HWC commands can be in flight.
	 */
	if (q_depth_max > HW_CHANNEL_VF_BOOTSTRAP_QUEUE_DEPTH) {
		/* q_depth_max now carries the full device-reported value
		 * (HWC_INIT_DATA_QUEUE_DEPTH is 24-bit).  Clamp it to the
		 * depth the rest of the driver supports — create_cq() takes
		 * q_depth * 2 as a u16 — before the overflow check below, so
		 * an over-large but otherwise-valid depth is reduced to the
		 * maximum instead of wrapping or being rejected.
		 */
		if (q_depth_max > U16_MAX / 2)
			q_depth_max = U16_MAX / 2;

		/* Sanity-check device-reported values before using them
		 * to size DMA allocations.  Enforce protocol minimums
		 * for message sizes and check that q_depth * max_msg_size
		 * plus alignment headroom fits in u32 (for
		 * mana_hwc_alloc_dma_buf's MANA_PAGE_ALIGN).
		 */
		if (!max_req_msg_size || !max_resp_msg_size ||
		    max_req_msg_size < sizeof(struct gdma_req_hdr) ||
		    max_resp_msg_size < sizeof(struct gdma_resp_hdr) ||
		    (u64)q_depth_max * max_req_msg_size >
			U32_MAX - MANA_PAGE_SIZE ||
		    (u64)q_depth_max * max_resp_msg_size >
			U32_MAX - MANA_PAGE_SIZE) {
			dev_err(hwc->dev,
				"HWC: invalid dims q=%u req=%u resp=%u\n",
				q_depth_max, max_req_msg_size,
				max_resp_msg_size);
			q_depth_max = HW_CHANNEL_VF_BOOTSTRAP_QUEUE_DEPTH;
			goto skip_reinit;
		}

		err = mana_smc_teardown_hwc(&gc->shm_channel, false);
		if (err) {
			/* Keep using the bootstrap-depth channel. */
			dev_err(hwc->dev,
				"Failed to teardown HWC for reinit: %d\n",
				err);
			q_depth_max = HW_CHANNEL_VF_BOOTSTRAP_QUEUE_DEPTH;
			goto skip_reinit;
		}

		hwc->setup_active = false;

		/* Destroy queues first — mana_gd_destroy_cq inside
		 * unpublishes the CQ from cq_table via
		 * rcu_assign_pointer(NULL) + synchronize_rcu.
		 * Must happen while cq_table is still valid.
		 */
		mana_hwc_destroy_queues(hwc);

		old_cq_table = rcu_replace_pointer(gc->cq_table, NULL, true);
		synchronize_rcu();
		vfree(old_cq_table);

		err = mana_hwc_init_queues(hwc, q_depth_max,
					   max_req_msg_size,
					   max_resp_msg_size);
		if (err) {
			dev_err(hwc->dev, "Failed to reinit HWC: %d\n", err);
			goto reinit_fallback;
		}

		err = mana_hwc_establish_channel(gc, &q_depth_max,
						 &max_req_msg_size,
						 &max_resp_msg_size);
		if (err) {
			dev_err(hwc->dev, "Failed to re-establish HWC: %d\n",
				err);
			/* establish_channel does internal teardown on
			 * failure.  If teardown succeeded (setup_active
			 * cleared), MST entries are invalidated and we
			 * can try the bootstrap fallback.  If teardown
			 * also failed (setup_active still set), hardware
			 * mappings may still be active — skip fallback.
			 */
			if (hwc->setup_active)
				goto out;
			goto reinit_fallback;
		}
	}

	goto skip_reinit;

reinit_fallback:
	/* Restore bootstrap-depth channel so the device remains functional.
	 * Free cq_table if it was allocated by a partially successful
	 * establish attempt.
	 */
	dev_warn(hwc->dev, "HWC reinit failed, falling back to bootstrap depth\n");

	mana_hwc_destroy_queues(hwc);

	old_cq_table = rcu_replace_pointer(gc->cq_table, NULL, true);
	synchronize_rcu();
	vfree(old_cq_table);

	err = mana_hwc_init_queues(hwc, HW_CHANNEL_VF_BOOTSTRAP_QUEUE_DEPTH,
				   HW_CHANNEL_MAX_REQUEST_SIZE,
				   HW_CHANNEL_MAX_RESPONSE_SIZE);
	if (err) {
		dev_err(hwc->dev, "Failed to restore bootstrap HWC: %d\n", err);
		goto out;
	}

	err = mana_hwc_establish_channel(gc, &q_depth_max, &max_req_msg_size,
					 &max_resp_msg_size);
	if (err) {
		dev_err(hwc->dev, "Failed to re-establish bootstrap HWC: %d\n",
			err);
		goto out;
	}

skip_reinit:

	/* No RCU needed: still in mana_hwc_create_channel, the
	 * pointer has not been published to concurrent senders yet.
	 */
	err = mana_hwc_test_channel(gc->hwc.driver_data,
				    hwc->num_inflight_msg,
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
	/* This is the only destroy entry point.  driver_data is read
	 * plainly here (teardown is serialised against other teardown);
	 * it is cleared under hwc_lock below before hwc is freed.
	 */
	struct hw_channel_context *hwc = gc->hwc.driver_data;
	struct gdma_queue __rcu **old_cq_table;
	unsigned long flags;

	if (!hwc)
		return;

	/* Prevent new requests from starting and wake any
	 * threads waiting for a free msg slot.  Set channel_up under
	 * the bitmap lock so get_msg_index() cannot acquire a slot
	 * and increment active_senders after this point.
	 *
	 * If channel_up is already false (e.g. init failed before
	 * the channel was established), skip the lock — it may not
	 * have been initialized yet, and no senders can be active.
	 */
	if (hwc->channel_up) {
		spin_lock_irqsave(&hwc->inflight_msg_res.lock, flags);
		hwc->channel_up = false;
		spin_unlock_irqrestore(&hwc->inflight_msg_res.lock, flags);
		wake_up_all(&hwc->msg_waitq);
	}

	/* Clear the pointer under hwc_lock so new callers in
	 * mana_gd_send_request() see NULL and return -ENODEV.  The lock
	 * makes the readers' "load driver_data + active_senders++"
	 * atomic against this store, so once it returns no new sender can
	 * take a reference; the active_senders drain below waits out those
	 * that already did, before their hwc is freed.
	 */
	spin_lock_irqsave(&gc->hwc_lock, flags);
	gc->hwc.driver_data = NULL;
	spin_unlock_irqrestore(&gc->hwc_lock, flags);

	/* Force-complete any in-flight senders so they observe -ENODEV,
	 * return, and drop their references.  This runs before the HWC
	 * hardware teardown below, so a live interrupt may still deliver
	 * a real response via handle_resp() concurrently — that is safe
	 * because the per-slot refcount model tolerates a concurrent
	 * complete() and both paths (handle_resp and this loop) drop
	 * their refs without double-releasing the slot.
	 */
	if (hwc->caller_ctx) {
		struct hwc_caller_ctx *ctx;
		int i;

		for (i = 0; i < hwc->num_inflight_msg; i++) {
			if (!test_bit(i, hwc->inflight_msg_res.map))
				continue;

			ctx = &hwc->caller_ctx[i];

			/* Wake senders blocked on wait_for_completion.
			 * Set error under lock to avoid racing with
			 * handle_resp() which writes error under the
			 * same lock.  The sender NULLs output_buf
			 * after waking — doing it here would race
			 * with a sender that hasn't set output_buf yet.
			 */
			spin_lock_irqsave(&ctx->lock, flags);
			ctx->error = -ENODEV;
			complete(&ctx->comp_event);
			spin_unlock_irqrestore(&ctx->lock, flags);
		}
	}

	/* Wait for all sender threads to finish and drop their refs
	 * before touching the hardware or freeing anything.  Draining
	 * here -- ahead of the teardown/FLR block below -- guarantees no
	 * in-flight sender is still running when this function returns on
	 * any path, including the FLR-failure early return; otherwise a
	 * stranded sender would dereference gc->hwc_lock /
	 * gc->hwc_drain_waitq after the caller frees gc.
	 * After this, only slots held by timed-out senders whose
	 * handle_resp() never ran remain in the bitmap.
	 *
	 * active_senders is only ever modified under hwc_lock, and the
	 * last sender's wake_up() runs under that lock before it is
	 * released.  Evaluating the condition under hwc_lock therefore
	 * guarantees that once we observe 0 the waking sender has
	 * already dropped the lock -- i.e. finished touching gc -- so it
	 * cannot race the caller freeing gc after this returns.
	 */
	spin_lock_irq(&gc->hwc_lock);
	wait_event_lock_irq(gc->hwc_drain_waitq,
			    hwc->active_senders == 0, gc->hwc_lock);
	spin_unlock_irq(&gc->hwc_lock);

	/* Tear down the HWC if setup_hwc previously activated MST entries.
	 * This is the definitive flag — unlike max_num_cqs which depends
	 * on the init EQE arriving.
	 *
	 * The return value is intentionally not checked.  This is the
	 * terminal cleanup path (device removal, suspend, or init
	 * failure) — resources must be freed regardless.  If teardown
	 * fails, hardware may still have active MST entries, but:
	 *
	 *  - Interrupts: mana_hwc_destroy_cq() below calls
	 *    mana_gd_deregister_irq() which removes the HWC EQ from
	 *    the interrupt dispatch list via list_del_rcu() +
	 *    synchronize_rcu().  After that, no interrupt handler can
	 *    invoke handle_resp() or access CQ/RQ buffers — even if
	 *    the IRQ is shared with data path queues.
	 *
	 *  - DMA: mana_hwc_destroy_wq() frees DMA buffers via
	 *    dma_free_coherent() which unmaps the IOVA from the
	 *    IOMMU.  Any stale hardware DMA to the old address
	 *    faults at the IOMMU, not in kernel memory.
	 */
	if (hwc->setup_active) {
		int td_err = mana_smc_teardown_hwc(&gc->shm_channel, false);

		if (td_err) {
			dev_err(gc->dev, "HWC teardown failed: %d, issuing FLR\n",
				td_err);

			/* On systems without IOMMU, freeing DMA memory with
			 * active hardware MST mappings risks memory corruption.
			 * Issue FLR to force-reset the device and invalidate
			 * all hardware state including MST entries.
			 */
			td_err = pcie_flr(to_pci_dev(gc->dev));
			if (td_err) {
				/* Device wedged: teardown and FLR
				 * both failed, so hardware may still
				 * DMA into our queue buffers.  Leak
				 * all HWC DMA buffers -- and the EQ
				 * IRQ registration, whose safe removal
				 * needs those queues -- instead of
				 * freeing memory the device can still
				 * reach; without an IOMMU the freed
				 * pages could be reused under live
				 * DMA.
				 */

				dev_warn(gc->dev,
					 "Leaked HWC DMA buffers (CQ/RQ/TXQ) to prevent memory corruption. Device is no longer usable.\n");

				/* Do not call destroy_cq/wq below;
				 * they would dma_free_coherent().
				 * Leave hwc/cq/rxq/txq allocated.
				 */
				return;
			}

			dev_info(gc->dev, "FLR succeeded, hardware state cleared\n");
		}

		hwc->setup_active = false;
	}

	/* Tear down the HWC CQ object first — mana_hwc_destroy_cq()
	 * both unpublishes the CQ from cq_table (+synchronize_rcu) and
	 * deregisters the HWC EQ from the interrupt handler's RCU list
	 * (via mana_gd_deregister_irq + synchronize_rcu), guaranteeing
	 * no interrupt handler can access RQ/TXQ buffers after this
	 * point.  The active_senders drain above ensures no sender is
	 * accessing the CQ via txq->hwc_cq when it is destroyed.  Then
	 * destroy TXQ and RQ safely.
	 */
	if (hwc->cq)
		mana_hwc_destroy_cq(hwc->gdma_dev->gdma_context, hwc->cq);

	if (hwc->txq)
		mana_hwc_destroy_wq(hwc, hwc->txq);

	if (hwc->rxq)
		mana_hwc_destroy_wq(hwc, hwc->rxq);

	/* Release any slots still held — these belong to timed-out
	 * senders where handle_resp() never ran (refcount = 1 with
	 * handle_resp's ref still outstanding).
	 */
	if (hwc->caller_ctx) {
		struct hwc_caller_ctx *ctx;
		int i;

		for (i = 0; i < hwc->num_inflight_msg; i++) {
			if (!test_bit(i, hwc->inflight_msg_res.map))
				continue;

			ctx = &hwc->caller_ctx[i];
			hwc_ctx_put(hwc, ctx);
		}
	}

	kfree(hwc->caller_ctx);
	hwc->caller_ctx = NULL;

	mana_gd_free_res_map(&hwc->inflight_msg_res);

	hwc->num_inflight_msg = 0;

	hwc->gdma_dev->doorbell = INVALID_DOORBELL;
	hwc->gdma_dev->pdid = INVALID_PDID;

	hwc->hwc_timeout = 0;

	kfree(hwc);
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
	u32 dest_vrcq = 0;
	u32 dest_vrq = 0;
	u32 command;
	u32 status;
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

	/* Take handle_resp's ref before posting — hardware can respond
	 * immediately after the doorbell ring.
	 */
	refcount_inc(&ctx->refcnt);

	err = mana_hwc_post_tx_wqe(txq, tx_wr, dest_vrq, dest_vrcq, false);
	if (err) {
		refcount_dec(&ctx->refcnt);
		dev_err(hwc->dev, "HWC: Failed to post send WQE: %d\n", err);
		goto out;
	}

	if (!wait_for_completion_timeout(&ctx->comp_event,
					 (msecs_to_jiffies(hwc->hwc_timeout)))) {
		if (hwc->hwc_timeout != 0)
			dev_err(hwc->dev, "Command 0x%x timed out: %u ms\n",
				command, hwc->hwc_timeout);

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

		/* Genuine timeout: no response arrived.  Reduce further
		 * waiting, and mark the channel timed out under the bitmap
		 * lock so get_msg_index() cannot acquire new slots after this.
		 */
		if (hwc->hwc_timeout > 1)
			hwc->hwc_timeout = 1;

		spin_lock_irqsave(&hwc->inflight_msg_res.lock, flags);
		hwc->hwc_timed_out = true;
		spin_unlock_irqrestore(&hwc->inflight_msg_res.lock, flags);
		wake_up_all(&hwc->msg_waitq);

		err = -ETIMEDOUT;
		hwc_ctx_put(hwc, ctx);
		goto done;
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
	/* Pre-post error paths: no WQE was submitted so handle_resp()
	 * cannot race here.  refcount is 1 (no second ref taken).
	 */
	ctx = hwc->caller_ctx + msg_id;
	spin_lock_irqsave(&ctx->lock, flags);
	ctx->output_buf = NULL;
	spin_unlock_irqrestore(&ctx->lock, flags);
	hwc_ctx_put(hwc, ctx);
done:
	return err;
}
