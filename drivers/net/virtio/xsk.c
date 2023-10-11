// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * virtio-net xsk
 */

#include "virtio_net.h"

static struct virtio_net_hdr_mrg_rxbuf xsk_hdr;

static void sg_fill_dma(struct scatterlist *sg, dma_addr_t addr, u32 len)
{
	sg->dma_address = addr;
	sg->length = len;
}

static unsigned int virtnet_receive_buf_num(struct virtnet_info *vi, char *buf)
{
	struct virtio_net_hdr_mrg_rxbuf *hdr;

	if (vi->mergeable_rx_bufs) {
		hdr = (struct virtio_net_hdr_mrg_rxbuf *)buf;
		return virtio16_to_cpu(vi->vdev, hdr->num_buffers);
	}

	return 1;
}

static void virtnet_xsk_check_queue(struct virtnet_sq *sq)
{
	struct virtnet_info *vi = sq->vq->vdev->priv;
	struct net_device *dev = vi->dev;
	int qnum = sq - vi->sq;

	/* If it is a raw buffer queue, it does not check whether the status
	 * of the queue is stopped when sending. So there is no need to check
	 * the situation of the raw buffer queue.
	 */
	if (virtnet_is_xdp_raw_buffer_queue(vi, qnum))
		return;

	/* If this sq is not the exclusive queue of the current cpu,
	 * then it may be called by start_xmit, so check it running out
	 * of space.
	 *
	 * Stop the queue to avoid getting packets that we are
	 * then unable to transmit. Then wait the tx interrupt.
	 */
	if (sq->vq->num_free < 2 + MAX_SKB_FRAGS)
		netif_stop_subqueue(dev, qnum);
}

static void merge_drop_follow_xdp(struct net_device *dev,
				  struct virtnet_rq *rq,
				  u32 num_buf,
				  struct virtnet_rq_stats *stats)
{
	struct xdp_buff *xdp;
	u32 len;

	while (num_buf-- > 1) {
		xdp = virtqueue_get_buf(rq->vq, &len);
		if (unlikely(!xdp)) {
			pr_debug("%s: rx error: %d buffers missing\n",
				 dev->name, num_buf);
			dev->stats.rx_length_errors++;
			break;
		}
		stats->bytes += len;
		xsk_buff_free(xdp);
	}
}

static struct sk_buff *construct_skb(struct virtnet_rq *rq,
				     struct xdp_buff *xdp)
{
	unsigned int metasize = xdp->data - xdp->data_meta;
	struct sk_buff *skb;
	unsigned int size;

	size = xdp->data_end - xdp->data_hard_start;
	skb = napi_alloc_skb(&rq->napi, size);
	if (unlikely(!skb))
		return NULL;

	skb_reserve(skb, xdp->data_meta - xdp->data_hard_start);

	size = xdp->data_end - xdp->data_meta;
	memcpy(__skb_put(skb, size), xdp->data_meta, size);

	if (metasize) {
		__skb_pull(skb, metasize);
		skb_metadata_set(skb, metasize);
	}

	return skb;
}

struct sk_buff *virtnet_receive_xsk(struct net_device *dev, struct virtnet_info *vi,
				    struct virtnet_rq *rq, void *buf,
				    unsigned int len, unsigned int *xdp_xmit,
				    struct virtnet_rq_stats *stats)
{
	struct virtio_net_hdr_mrg_rxbuf *hdr;
	struct sk_buff *skb = NULL;
	u32 ret, headroom, num_buf;
	struct bpf_prog *prog;
	struct xdp_buff *xdp;

	len -= vi->hdr_len;

	xdp = (struct xdp_buff *)buf;

	xsk_buff_set_size(xdp, len);

	hdr = xdp->data - vi->hdr_len;

	num_buf = virtnet_receive_buf_num(vi, (char *)hdr);
	if (num_buf > 1)
		goto drop;

	headroom = xdp->data - xdp->data_hard_start;

	xdp_prepare_buff(xdp, xdp->data_hard_start, headroom, len, true);
	xsk_buff_dma_sync_for_cpu(xdp, rq->xsk.pool);

	ret = XDP_PASS;
	rcu_read_lock();
	prog = rcu_dereference(rq->xdp_prog);
	if (prog)
		ret = virtnet_xdp_handler(prog, xdp, dev, xdp_xmit, stats);
	rcu_read_unlock();

	switch (ret) {
	case XDP_PASS:
		skb = construct_skb(rq, xdp);
		xsk_buff_free(xdp);
		break;

	case XDP_TX:
	case XDP_REDIRECT:
		goto consumed;

	default:
		goto drop;
	}

	return skb;

drop:
	stats->drops++;

	xsk_buff_free(xdp);

	if (num_buf > 1)
		merge_drop_follow_xdp(dev, rq, num_buf, stats);
consumed:
	return NULL;
}

static int virtnet_add_recvbuf_batch(struct virtnet_info *vi, struct virtnet_rq *rq,
				     struct xsk_buff_pool *pool, gfp_t gfp)
{
	struct xdp_buff **xsk_buffs;
	dma_addr_t addr;
	u32 len, i;
	int err = 0;

	xsk_buffs = rq->xsk.xsk_buffs;

	if (rq->xsk.nxt_idx >= rq->xsk.num) {
		rq->xsk.num = xsk_buff_alloc_batch(pool, xsk_buffs, rq->xsk.size);
		if (!rq->xsk.num)
			return -ENOMEM;
		rq->xsk.nxt_idx = 0;
	}

	while (rq->xsk.nxt_idx < rq->xsk.num) {
		i = rq->xsk.nxt_idx;

		/* use the part of XDP_PACKET_HEADROOM as the virtnet hdr space */
		addr = xsk_buff_xdp_get_dma(xsk_buffs[i]) - vi->hdr_len;
		len = xsk_pool_get_rx_frame_size(pool) + vi->hdr_len;

		sg_init_table(rq->sg, 1);
		sg_fill_dma(rq->sg, addr, len);

		err = virtqueue_add_inbuf(rq->vq, rq->sg, 1, xsk_buffs[i], gfp);
		if (err)
			return err;

		rq->xsk.nxt_idx++;
	}

	return 0;
}

int virtnet_add_recvbuf_xsk(struct virtnet_info *vi, struct virtnet_rq *rq,
			    struct xsk_buff_pool *pool, gfp_t gfp)
{
	int err;

	do {
		err = virtnet_add_recvbuf_batch(vi, rq, pool, gfp);
		if (err)
			return err;

	} while (rq->vq->num_free);

	return 0;
}

static int virtnet_xsk_xmit_one(struct virtnet_sq *sq,
				struct xsk_buff_pool *pool,
				struct xdp_desc *desc)
{
	struct virtnet_info *vi;
	dma_addr_t addr;

	vi = sq->vq->vdev->priv;

	addr = xsk_buff_raw_get_dma(pool, desc->addr);
	xsk_buff_raw_dma_sync_for_device(pool, addr, desc->len);

	sg_init_table(sq->sg, 2);

	sg_fill_dma(sq->sg, sq->xsk.hdr_dma_address, vi->hdr_len);
	sg_fill_dma(sq->sg + 1, addr, desc->len);

	return virtqueue_add_outbuf(sq->vq, sq->sg, 2,
				    virtnet_xsk_to_ptr(desc->len), GFP_ATOMIC);
}

static int virtnet_xsk_xmit_batch(struct virtnet_sq *sq,
				  struct xsk_buff_pool *pool,
				  unsigned int budget,
				  struct virtnet_sq_stats *stats)
{
	struct xdp_desc *descs = pool->tx_descs;
	u32 nb_pkts, max_pkts, i;
	bool kick = false;
	int err;

	max_pkts = min_t(u32, budget, sq->vq->num_free / 2);

	nb_pkts = xsk_tx_peek_release_desc_batch(pool, max_pkts);
	if (!nb_pkts)
		return 0;

	for (i = 0; i < nb_pkts; i++) {
		err = virtnet_xsk_xmit_one(sq, pool, &descs[i]);
		if (unlikely(err))
			break;

		kick = true;
	}

	if (kick && virtqueue_kick_prepare(sq->vq) && virtqueue_notify(sq->vq))
		++stats->kicks;

	stats->xdp_tx += i;

	return i;
}

bool virtnet_xsk_xmit(struct virtnet_sq *sq, struct xsk_buff_pool *pool,
		      int budget)
{
	struct virtnet_sq_stats stats = {};
	int sent;

	virtnet_free_old_xmit(sq, true, &stats);

	sent = virtnet_xsk_xmit_batch(sq, pool, budget, &stats);

	virtnet_xsk_check_queue(sq);

	if (stats.packets) {
		struct netdev_queue *txq;
		struct virtnet_info *vi;

		vi = sq->vq->vdev->priv;

		txq = netdev_get_tx_queue(vi->dev, sq - vi->sq);
		txq_trans_cond_update(txq);
	}

	u64_stats_update_begin(&sq->stats.syncp);
	sq->stats.packets += stats.packets;
	sq->stats.bytes += stats.bytes;
	sq->stats.kicks += stats.kicks;
	sq->stats.xdp_tx += stats.xdp_tx;
	u64_stats_update_end(&sq->stats.syncp);

	if (xsk_uses_need_wakeup(pool))
		xsk_set_tx_need_wakeup(pool);

	return sent == budget;
}

static void virtnet_remote_napi_schedule(void *info)
{
	struct virtnet_sq *sq = info;

	virtnet_vq_napi_schedule(&sq->napi, sq->vq);
}

static void virtnet_remote_raise_napi(struct virtnet_sq *sq)
{
	u32 last_cpu, cur_cpu;

	last_cpu = sq->xsk.last_cpu;
	cur_cpu = get_cpu();

	/* On remote cpu, softirq will run automatically when ipi irq exit. On
	 * local cpu, smp_call_xxx will not trigger ipi interrupt, then softirq
	 * cannot be triggered automatically. So Call local_bh_enable after to
	 * trigger softIRQ processing.
	 */
	if (last_cpu == cur_cpu) {
		local_bh_disable();
		virtnet_vq_napi_schedule(&sq->napi, sq->vq);
		local_bh_enable();
	} else {
		if (spin_trylock(&sq->xsk.ipi_lock)) {
			smp_call_function_single_async(last_cpu, &sq->xsk.csd);
			spin_unlock(&sq->xsk.ipi_lock);
		}
	}

	put_cpu();
}

int virtnet_xsk_wakeup(struct net_device *dev, u32 qid, u32 flag)
{
	struct virtnet_info *vi = netdev_priv(dev);
	struct virtnet_sq *sq;

	if (!netif_running(dev))
		return -ENETDOWN;

	if (qid >= vi->curr_queue_pairs)
		return -EINVAL;

	sq = &vi->sq[qid];

	if (napi_if_scheduled_mark_missed(&sq->napi))
		return 0;

	virtnet_remote_raise_napi(sq);

	return 0;
}

static int virtnet_rq_bind_xsk_pool(struct virtnet_info *vi, struct virtnet_rq *rq,
				    struct xsk_buff_pool *pool)
{
	int err, qindex;

	qindex = rq - vi->rq;

	if (pool) {
		err = xdp_rxq_info_reg(&rq->xsk.xdp_rxq, vi->dev, qindex, rq->napi.napi_id);
		if (err < 0)
			return err;

		err = xdp_rxq_info_reg_mem_model(&rq->xsk.xdp_rxq,
						 MEM_TYPE_XSK_BUFF_POOL, NULL);
		if (err < 0) {
			xdp_rxq_info_unreg(&rq->xsk.xdp_rxq);
			return err;
		}

		xsk_pool_set_rxq_info(pool, &rq->xsk.xdp_rxq);
	} else {
		xdp_rxq_info_unreg(&rq->xsk.xdp_rxq);
	}

	virtnet_rx_pause(vi, rq);

	err = virtqueue_reset(rq->vq, virtnet_rq_free_unused_buf);
	if (err)
		netdev_err(vi->dev, "reset rx fail: rx queue index: %d err: %d\n", qindex, err);

	if (pool && err)
		xdp_rxq_info_unreg(&rq->xsk.xdp_rxq);
	else
		rcu_assign_pointer(rq->xsk.pool, pool);

	virtnet_rx_resume(vi, rq);

	return err;
}

static int virtnet_sq_bind_xsk_pool(struct virtnet_info *vi,
				    struct virtnet_sq *sq,
				    struct xsk_buff_pool *pool)
{
	int err, qindex;

	qindex = sq - vi->sq;

	virtnet_tx_pause(vi, sq);

	err = virtqueue_reset(sq->vq, virtnet_sq_free_unused_buf);
	if (err)
		netdev_err(vi->dev, "reset tx fail: tx queue index: %d err: %d\n", qindex, err);

	if (pool) {
		if (!err)
			err = virtnet_sq_set_premapped(sq);

		if (!err)
			rcu_assign_pointer(sq->xsk.pool, pool);
	} else {
		rcu_assign_pointer(sq->xsk.pool, NULL);
		virtnet_sq_unset_premapped(sq);
	}

	virtnet_tx_resume(vi, sq);

	return err;
}

static int virtnet_xsk_pool_enable(struct net_device *dev,
				   struct xsk_buff_pool *pool,
				   u16 qid)
{
	struct virtnet_info *vi = netdev_priv(dev);
	struct virtnet_rq *rq;
	struct virtnet_sq *sq;
	struct device *dma_dev;
	dma_addr_t hdr_dma;
	int err, size;

	/* In big_packets mode, xdp cannot work, so there is no need to
	 * initialize xsk of rq.
	 */
	if (vi->big_packets && !vi->mergeable_rx_bufs)
		return -ENOENT;

	if (qid >= vi->curr_queue_pairs)
		return -EINVAL;

	sq = &vi->sq[qid];
	rq = &vi->rq[qid];

	/* xsk tx zerocopy depend on the tx napi.
	 *
	 * All xsk packets are actually consumed and sent out from the xsk tx
	 * queue under the tx napi mechanism.
	 */
	if (!sq->napi.weight)
		return -EPERM;

	if (!rq->do_dma)
		return -EPERM;

	if (virtqueue_dma_dev(rq->vq) != virtqueue_dma_dev(sq->vq))
		return -EPERM;

	dma_dev = virtqueue_dma_dev(rq->vq);
	if (!dma_dev)
		return -EPERM;

	size = virtqueue_get_vring_size(rq->vq);

	rq->xsk.xsk_buffs = kcalloc(size, sizeof(*rq->xsk.xsk_buffs), GFP_KERNEL);
	if (!rq->xsk.xsk_buffs)
		return -ENOMEM;

	rq->xsk.size = size;
	rq->xsk.nxt_idx = 0;
	rq->xsk.num = 0;

	hdr_dma = dma_map_single(dma_dev, &xsk_hdr, vi->hdr_len, DMA_TO_DEVICE);
	if (dma_mapping_error(dma_dev, hdr_dma))
		return -ENOMEM;

	err = xsk_pool_dma_map(pool, dma_dev, 0);
	if (err)
		goto err_xsk_map;

	err = virtnet_rq_bind_xsk_pool(vi, rq, pool);
	if (err)
		goto err_rq;

	err = virtnet_sq_bind_xsk_pool(vi, sq, pool);
	if (err)
		goto err_sq;

	sq->xsk.hdr_dma_address = hdr_dma;

	INIT_CSD(&sq->xsk.csd, virtnet_remote_napi_schedule, sq);
	spin_lock_init(&sq->xsk.ipi_lock);

	return 0;

err_sq:
	virtnet_rq_bind_xsk_pool(vi, rq, NULL);
err_rq:
	xsk_pool_dma_unmap(pool, 0);
err_xsk_map:
	dma_unmap_single(dma_dev, hdr_dma, vi->hdr_len, DMA_TO_DEVICE);
	return err;
}

static int virtnet_xsk_pool_disable(struct net_device *dev, u16 qid)
{
	struct virtnet_info *vi = netdev_priv(dev);
	struct device *dma_dev;
	struct virtnet_rq *rq;
	struct virtnet_sq *sq;
	int err1, err2;

	if (qid >= vi->curr_queue_pairs)
		return -EINVAL;

	sq = &vi->sq[qid];
	rq = &vi->rq[qid];

	dma_dev = virtqueue_dma_dev(rq->vq);

	dma_unmap_single(dma_dev, sq->xsk.hdr_dma_address, vi->hdr_len, DMA_TO_DEVICE);

	xsk_pool_dma_unmap(sq->xsk.pool, 0);

	/* Sync with the XSK wakeup and with NAPI. */
	synchronize_net();

	err1 = virtnet_sq_bind_xsk_pool(vi, sq, NULL);
	err2 = virtnet_rq_bind_xsk_pool(vi, rq, NULL);

	kfree(rq->xsk.xsk_buffs);

	return err1 | err2;
}

int virtnet_xsk_pool_setup(struct net_device *dev, struct netdev_bpf *xdp)
{
	if (xdp->xsk.pool)
		return virtnet_xsk_pool_enable(dev, xdp->xsk.pool,
					       xdp->xsk.queue_id);
	else
		return virtnet_xsk_pool_disable(dev, xdp->xsk.queue_id);
}
