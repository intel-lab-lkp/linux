// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
// Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.

#include "en_accel/nvmeotcp_rxtx.h"
#include <linux/mlx5/mlx5_ifc.h>
#include "en/txrx.h"

#define MLX5E_TC_FLOW_ID_MASK  0x00ffffff

static struct mlx5e_frag_page *mlx5e_get_frag(struct mlx5e_rq *rq,
					      struct mlx5_cqe64 *cqe)
{
	struct mlx5e_frag_page *fp;

	if (rq->wq_type == MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ) {
		u16 wqe_id         = be16_to_cpu(cqe->wqe_id);
		u16 stride_ix      = mpwrq_get_cqe_stride_index(cqe);
		u32 wqe_offset     = stride_ix << rq->mpwqe.log_stride_sz;
		u32 page_idx       = wqe_offset >> rq->mpwqe.page_shift;
		struct mlx5e_mpw_info *wi = mlx5e_get_mpw_info(rq, wqe_id);
		union mlx5e_alloc_units *au = &wi->alloc_units;

		fp = &au->frag_pages[page_idx];
	} else {
		/* Legacy */
		struct mlx5_wq_cyc *wq = &rq->wqe.wq;
		u16 ci = mlx5_wq_cyc_ctr2ix(wq, be16_to_cpu(cqe->wqe_counter));
		struct mlx5e_wqe_frag_info *wi = get_frag(rq, ci);

		fp = wi->frag_page;
	}

	return fp;
}

static void nvmeotcp_update_resync(struct mlx5e_nvmeotcp_queue *queue,
				   struct mlx5e_cqe128 *cqe128)
{
	const struct ulp_ddp_ulp_ops *ulp_ops;
	u32 seq;

	seq = be32_to_cpu(cqe128->resync_tcp_sn);
	ulp_ops = inet_csk(queue->sk)->icsk_ulp_ddp_ops;
	if (ulp_ops && ulp_ops->resync_request)
		ulp_ops->resync_request(queue->sk, seq, ULP_DDP_RESYNC_PENDING);
}

static void mlx5e_nvmeotcp_advance_sgl_iter(struct mlx5e_nvmeotcp_queue *queue)
{
	struct mlx5e_nvmeotcp_queue_entry *nqe = &queue->ccid_table[queue->ccid];

	queue->ccoff += nqe->sgl[queue->ccsglidx].length;
	queue->ccoff_inner = 0;
	queue->ccsglidx++;
}

static inline void
mlx5e_nvmeotcp_add_skb_frag(struct net_device *netdev, struct sk_buff *skb,
			    struct mlx5e_nvmeotcp_queue *queue,
			    struct mlx5e_nvmeotcp_queue_entry *nqe, u32 fragsz)
{
	dma_sync_single_for_cpu(&netdev->dev,
				nqe->sgl[queue->ccsglidx].offset + queue->ccoff_inner,
				fragsz, DMA_FROM_DEVICE);

	page_ref_inc(compound_head(sg_page(&nqe->sgl[queue->ccsglidx])));

	skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags,
			sg_page(&nqe->sgl[queue->ccsglidx]),
			nqe->sgl[queue->ccsglidx].offset + queue->ccoff_inner,
			fragsz,
			fragsz);
}

static inline void
mlx5_nvmeotcp_add_tail_nonlinear(struct sk_buff *skb, skb_frag_t *org_frags,
				 int org_nr_frags, int frag_index)
{
	while (org_nr_frags != frag_index) {
		skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags,
				skb_frag_page(&org_frags[frag_index]),
				skb_frag_off(&org_frags[frag_index]),
				skb_frag_size(&org_frags[frag_index]),
				skb_frag_size(&org_frags[frag_index]));
		frag_index++;
	}
}

static void
mlx5_nvmeotcp_add_tail(struct mlx5e_rq *rq, struct mlx5_cqe64 *cqe,
		       struct mlx5e_nvmeotcp_queue *queue, struct sk_buff *skb,
		       int offset, int len)
{
	struct mlx5e_frag_page *frag_page = mlx5e_get_frag(rq, cqe);

	frag_page->frags++;
	skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags,
			virt_to_page(skb->data), offset, len, len);
}

static void mlx5_nvmeotcp_trim_nonlinear(struct sk_buff *skb, skb_frag_t *org_frags,
					 int *frag_index, int remaining)
{
	unsigned int frag_size;
	int nr_frags;

	/* skip @remaining bytes in frags */
	*frag_index = 0;
	while (remaining) {
		frag_size = skb_frag_size(&skb_shinfo(skb)->frags[*frag_index]);
		if (frag_size > remaining) {
			skb_frag_off_add(&skb_shinfo(skb)->frags[*frag_index],
					 remaining);
			skb_frag_size_sub(&skb_shinfo(skb)->frags[*frag_index],
					  remaining);
			remaining = 0;
		} else {
			remaining -= frag_size;
			skb_frag_unref(skb, *frag_index);
			*frag_index += 1;
		}
	}

	/* save original frags for the tail and unref */
	nr_frags = skb_shinfo(skb)->nr_frags;
	memcpy(&org_frags[*frag_index], &skb_shinfo(skb)->frags[*frag_index],
	       (nr_frags - *frag_index) * sizeof(skb_frag_t));

	/* remove frags from skb */
	skb_shinfo(skb)->nr_frags = 0;
	skb->len -= skb->data_len;
	skb->truesize -= skb->data_len;
	skb->data_len = 0;
}

static bool
mlx5e_nvmeotcp_rebuild_rx_skb_nonlinear(struct mlx5e_rq *rq, struct sk_buff *skb,
					struct mlx5_cqe64 *cqe, u32 cqe_bcnt)
{
	int ccoff, cclen, hlen, ccid, remaining, fragsz, to_copy = 0;
	struct net_device *netdev = rq->netdev;
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5e_rq_stats *stats = rq->stats;
	struct mlx5e_nvmeotcp_queue_entry *nqe;
	skb_frag_t org_frags[MAX_SKB_FRAGS];
	struct mlx5e_nvmeotcp_queue *queue;
	int org_nr_frags, frag_index;
	struct mlx5e_cqe128 *cqe128;
	u32 queue_id;

	queue_id = (be32_to_cpu(cqe->sop_drop_qpn) & MLX5E_TC_FLOW_ID_MASK);
	queue = mlx5e_nvmeotcp_get_queue(priv->nvmeotcp, queue_id);
	if (unlikely(!queue)) {
		dev_kfree_skb_any(skb);
		stats->nvmeotcp_drop++;
		return false;
	}

	cqe128 = container_of(cqe, struct mlx5e_cqe128, cqe64);
	if (cqe_is_nvmeotcp_resync(cqe)) {
		nvmeotcp_update_resync(queue, cqe128);
		stats->nvmeotcp_resync++;
		mlx5e_nvmeotcp_put_queue(queue);
		return true;
	}

	/* If a resync occurred in the previous cqe,
	 * the current cqe.crcvalid bit may not be valid,
	 * so we will treat it as 0
	 */
	if (unlikely(queue->after_resync_cqe) && cqe_is_nvmeotcp_crcvalid(cqe)) {
		skb->ulp_crc = 0;
		queue->after_resync_cqe = 0;
	} else {
		if (queue->crc_rx)
			skb->ulp_crc = cqe_is_nvmeotcp_crcvalid(cqe);
	}

	skb->no_condense = cqe_is_nvmeotcp_zc(cqe);
	if (!cqe_is_nvmeotcp_zc(cqe)) {
		mlx5e_nvmeotcp_put_queue(queue);
		return true;
	}

	/* cc ddp from cqe */
	ccid	= be16_to_cpu(cqe128->ccid);
	ccoff	= be32_to_cpu(cqe128->ccoff);
	cclen	= be16_to_cpu(cqe128->cclen);
	hlen	= be16_to_cpu(cqe128->hlen);

	/* carve a hole in the skb for DDP data */
	org_nr_frags = skb_shinfo(skb)->nr_frags;
	mlx5_nvmeotcp_trim_nonlinear(skb, org_frags, &frag_index, cclen);
	nqe = &queue->ccid_table[ccid];

	/* packet starts new ccid? */
	if (queue->ccid != ccid || queue->ccid_gen != nqe->ccid_gen) {
		queue->ccid = ccid;
		queue->ccoff = 0;
		queue->ccoff_inner = 0;
		queue->ccsglidx = 0;
		queue->ccid_gen = nqe->ccid_gen;
	}

	/* skip inside cc until the ccoff in the cqe */
	while (queue->ccoff + queue->ccoff_inner < ccoff) {
		remaining = nqe->sgl[queue->ccsglidx].length - queue->ccoff_inner;
		fragsz = min_t(off_t, remaining,
			       ccoff - (queue->ccoff + queue->ccoff_inner));

		if (fragsz == remaining)
			mlx5e_nvmeotcp_advance_sgl_iter(queue);
		else
			queue->ccoff_inner += fragsz;
	}

	/* adjust the skb according to the cqe cc */
	while (to_copy < cclen) {
		remaining = nqe->sgl[queue->ccsglidx].length - queue->ccoff_inner;
		fragsz = min_t(int, remaining, cclen - to_copy);

		mlx5e_nvmeotcp_add_skb_frag(netdev, skb, queue, nqe, fragsz);
		to_copy += fragsz;
		if (fragsz == remaining)
			mlx5e_nvmeotcp_advance_sgl_iter(queue);
		else
			queue->ccoff_inner += fragsz;
	}

	if (cqe_bcnt > hlen + cclen) {
		remaining = cqe_bcnt - hlen - cclen;
		mlx5_nvmeotcp_add_tail_nonlinear(skb, org_frags,
						 org_nr_frags,
						 frag_index);
	}
	stats->nvmeotcp_packets++;
	stats->nvmeotcp_bytes += cclen;
	mlx5e_nvmeotcp_put_queue(queue);
	return true;
}

static bool
mlx5e_nvmeotcp_rebuild_rx_skb_linear(struct mlx5e_rq *rq, struct sk_buff *skb,
				     struct mlx5_cqe64 *cqe, u32 cqe_bcnt)
{
	int ccoff, cclen, hlen, ccid, remaining, fragsz, to_copy = 0;
	struct net_device *netdev = rq->netdev;
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5e_rq_stats *stats = rq->stats;
	struct mlx5e_nvmeotcp_queue_entry *nqe;
	struct mlx5e_nvmeotcp_queue *queue;
	struct mlx5e_cqe128 *cqe128;
	u32 queue_id;

	queue_id = (be32_to_cpu(cqe->sop_drop_qpn) & MLX5E_TC_FLOW_ID_MASK);
	queue = mlx5e_nvmeotcp_get_queue(priv->nvmeotcp, queue_id);
	if (unlikely(!queue)) {
		dev_kfree_skb_any(skb);
		stats->nvmeotcp_drop++;
		return false;
	}

	cqe128 = container_of(cqe, struct mlx5e_cqe128, cqe64);
	if (cqe_is_nvmeotcp_resync(cqe)) {
		nvmeotcp_update_resync(queue, cqe128);
		stats->nvmeotcp_resync++;
		mlx5e_nvmeotcp_put_queue(queue);
		return true;
	}

	/* If a resync occurred in the previous cqe,
	 * the current cqe.crcvalid bit may not be valid,
	 * so we will treat it as 0
	 */
	if (unlikely(queue->after_resync_cqe) && cqe_is_nvmeotcp_crcvalid(cqe)) {
		skb->ulp_crc = 0;
		queue->after_resync_cqe = 0;
	} else {
		if (queue->crc_rx)
			skb->ulp_crc = cqe_is_nvmeotcp_crcvalid(cqe);
	}

	skb->no_condense = cqe_is_nvmeotcp_zc(cqe);
	if (!cqe_is_nvmeotcp_zc(cqe)) {
		mlx5e_nvmeotcp_put_queue(queue);
		return true;
	}

	/* cc ddp from cqe */
	ccid	= be16_to_cpu(cqe128->ccid);
	ccoff	= be32_to_cpu(cqe128->ccoff);
	cclen	= be16_to_cpu(cqe128->cclen);
	hlen	= be16_to_cpu(cqe128->hlen);

	/* carve a hole in the skb for DDP data */
	skb_trim(skb, hlen);
	nqe = &queue->ccid_table[ccid];

	/* packet starts new ccid? */
	if (queue->ccid != ccid || queue->ccid_gen != nqe->ccid_gen) {
		queue->ccid = ccid;
		queue->ccoff = 0;
		queue->ccoff_inner = 0;
		queue->ccsglidx = 0;
		queue->ccid_gen = nqe->ccid_gen;
	}

	/* skip inside cc until the ccoff in the cqe */
	while (queue->ccoff + queue->ccoff_inner < ccoff) {
		remaining = nqe->sgl[queue->ccsglidx].length - queue->ccoff_inner;
		fragsz = min_t(off_t, remaining,
			       ccoff - (queue->ccoff + queue->ccoff_inner));

		if (fragsz == remaining)
			mlx5e_nvmeotcp_advance_sgl_iter(queue);
		else
			queue->ccoff_inner += fragsz;
	}

	/* adjust the skb according to the cqe cc */
	while (to_copy < cclen) {
		remaining = nqe->sgl[queue->ccsglidx].length - queue->ccoff_inner;
		fragsz = min_t(int, remaining, cclen - to_copy);

		mlx5e_nvmeotcp_add_skb_frag(netdev, skb, queue, nqe, fragsz);
		to_copy += fragsz;
		if (fragsz == remaining)
			mlx5e_nvmeotcp_advance_sgl_iter(queue);
		else
			queue->ccoff_inner += fragsz;
	}

	if (cqe_bcnt > hlen + cclen) {
		remaining = cqe_bcnt - hlen - cclen;
		mlx5_nvmeotcp_add_tail(rq, cqe, queue, skb,
				       offset_in_page(skb->data) +
				       hlen + cclen, remaining);
	}

	stats->nvmeotcp_packets++;
	stats->nvmeotcp_bytes += cclen;
	mlx5e_nvmeotcp_put_queue(queue);
	return true;
}

bool
mlx5e_nvmeotcp_rebuild_rx_skb(struct mlx5e_rq *rq, struct sk_buff *skb,
			      struct mlx5_cqe64 *cqe, u32 cqe_bcnt)
{
	if (skb->data_len)
		return mlx5e_nvmeotcp_rebuild_rx_skb_nonlinear(rq, skb, cqe, cqe_bcnt);
	else
		return mlx5e_nvmeotcp_rebuild_rx_skb_linear(rq, skb, cqe, cqe_bcnt);
}
