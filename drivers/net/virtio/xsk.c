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
	int err;

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
