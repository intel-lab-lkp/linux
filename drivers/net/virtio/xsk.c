// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * virtio-net xsk
 */

#include "virtio_net.h"
#include "xsk.h"

static struct virtio_net_hdr_mrg_rxbuf xsk_hdr;

static void sg_fill_dma(struct scatterlist *sg, dma_addr_t addr, u32 len)
{
	sg->dma_address = addr;
	sg->length = len;
}

static void xsk_drop_follow_bufs(struct net_device *dev,
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
			DEV_STATS_INC(dev, rx_length_errors);
			break;
		}
		u64_stats_add(&stats->bytes, len);
		xsk_buff_free(xdp);
	}
}

static struct xdp_buff *buf_to_xdp(struct virtnet_info *vi,
				   struct virtnet_rq *rq, void *buf, u32 len)
{
	struct xdp_buff *xdp;
	u32 bufsize;

	xdp = (struct xdp_buff *)buf;

	bufsize = xsk_pool_get_rx_frame_size(rq->xsk.pool) + vi->hdr_len;

	if (unlikely(len > bufsize)) {
		pr_debug("%s: rx error: len %u exceeds truesize %u\n",
			 vi->dev->name, len, bufsize);
		DEV_STATS_INC(vi->dev, rx_length_errors);
		xsk_buff_free(xdp);
		return NULL;
	}

	xsk_buff_set_size(xdp, len);
	xsk_buff_dma_sync_for_cpu(xdp, rq->xsk.pool);

	return xdp;
}

static int xsk_append_merge_buffer(struct virtnet_info *vi,
				   struct virtnet_rq *rq,
				   struct sk_buff *head_skb,
				   u32 num_buf,
				   struct virtio_net_hdr_mrg_rxbuf *hdr,
				   struct virtnet_rq_stats *stats)
{
	struct sk_buff *curr_skb;
	struct xdp_buff *xdp;
	u32 len, truesize;
	struct page *page;
	void *buf;

	curr_skb = head_skb;

	while (--num_buf) {
		buf = virtqueue_get_buf(rq->vq, &len);
		if (unlikely(!buf)) {
			pr_debug("%s: rx error: %d buffers out of %d missing\n",
				 vi->dev->name, num_buf,
				 virtio16_to_cpu(vi->vdev,
						 hdr->num_buffers));
			DEV_STATS_INC(vi->dev, rx_length_errors);
			return -EINVAL;
		}

		u64_stats_add(&stats->bytes, len);

		xdp = buf_to_xdp(vi, rq, buf, len);
		if (!xdp)
			goto err;

		buf = napi_alloc_frag(len);
		if (!buf) {
			xsk_buff_free(xdp);
			goto err;
		}

		memcpy(buf, xdp->data - vi->hdr_len, len);

		xsk_buff_free(xdp);

		page = virt_to_page(buf);

		truesize = len;

		curr_skb  = virtnet_skb_append_frag(head_skb, curr_skb, page,
						    buf, len, truesize);
		if (!curr_skb) {
			put_page(page);
			goto err;
		}
	}

	return 0;

err:
	xsk_drop_follow_bufs(vi->dev, rq, num_buf, stats);
	return -EINVAL;
}

static struct sk_buff *xdp_construct_skb(struct virtnet_rq *rq,
					 struct xdp_buff *xdp)
{
	unsigned int metasize = xdp->data - xdp->data_meta;
	struct sk_buff *skb;
	unsigned int size;

	size = xdp->data_end - xdp->data_hard_start;
	skb = napi_alloc_skb(&rq->napi, size);
	if (unlikely(!skb)) {
		xsk_buff_free(xdp);
		return NULL;
	}

	skb_reserve(skb, xdp->data_meta - xdp->data_hard_start);

	size = xdp->data_end - xdp->data_meta;
	memcpy(__skb_put(skb, size), xdp->data_meta, size);

	if (metasize) {
		__skb_pull(skb, metasize);
		skb_metadata_set(skb, metasize);
	}

	xsk_buff_free(xdp);

	return skb;
}

static struct sk_buff *virtnet_receive_xsk_merge(struct net_device *dev, struct virtnet_info *vi,
						 struct virtnet_rq *rq, struct xdp_buff *xdp,
						 unsigned int *xdp_xmit,
						 struct virtnet_rq_stats *stats)
{
	struct virtio_net_hdr_mrg_rxbuf *hdr;
	struct bpf_prog *prog;
	struct sk_buff *skb;
	u32 ret, num_buf;

	hdr = xdp->data - vi->hdr_len;
	num_buf = virtio16_to_cpu(vi->vdev, hdr->num_buffers);

	ret = XDP_PASS;
	rcu_read_lock();
	prog = rcu_dereference(rq->xdp_prog);
	/* TODO: support multi buffer. */
	if (prog && num_buf == 1)
		ret = virtnet_xdp_handler(prog, xdp, dev, xdp_xmit, stats);
	rcu_read_unlock();

	switch (ret) {
	case XDP_PASS:
		skb = xdp_construct_skb(rq, xdp);
		if (!skb)
			goto drop_bufs;

		if (xsk_append_merge_buffer(vi, rq, skb, num_buf, hdr, stats)) {
			dev_kfree_skb(skb);
			goto drop;
		}

		return skb;

	case XDP_TX:
	case XDP_REDIRECT:
		return NULL;

	default:
		/* drop packet */
		xsk_buff_free(xdp);
	}

drop_bufs:
	xsk_drop_follow_bufs(dev, rq, num_buf, stats);

drop:
	u64_stats_inc(&stats->drops);
	return NULL;
}

struct sk_buff *virtnet_receive_xsk_buf(struct virtnet_info *vi, struct virtnet_rq *rq,
					void *buf, u32 len,
					unsigned int *xdp_xmit,
					struct virtnet_rq_stats *stats)
{
	struct net_device *dev = vi->dev;
	struct sk_buff *skb = NULL;
	struct xdp_buff *xdp;

	if (unlikely(len < vi->hdr_len + ETH_HLEN)) {
		pr_debug("%s: short packet %i\n", dev->name, len);
		DEV_STATS_INC(dev, rx_length_errors);

		xsk_buff_free(xdp);
		return NULL;
	}

	len -= vi->hdr_len;

	u64_stats_add(&stats->bytes, len);

	xdp = buf_to_xdp(vi, rq, buf, len);
	if (!xdp)
		return NULL;

	if (vi->mergeable_rx_bufs)
		skb = virtnet_receive_xsk_merge(dev, vi, rq, xdp, xdp_xmit, stats);

	return skb;
}

int virtnet_add_recvbuf_xsk(struct virtnet_info *vi, struct virtnet_rq *rq,
			    struct xsk_buff_pool *pool, gfp_t gfp)
{
	struct xdp_buff **xsk_buffs;
	dma_addr_t addr;
	u32 len, i;
	int err = 0;
	int num;

	xsk_buffs = rq->xsk.xsk_buffs;

	num = xsk_buff_alloc_batch(pool, xsk_buffs, rq->vq->num_free);
	if (!num)
		return -ENOMEM;

	len = xsk_pool_get_rx_frame_size(pool) + vi->hdr_len;

	for (i = 0; i < num; ++i) {
		/* use the part of XDP_PACKET_HEADROOM as the virtnet hdr space */
		addr = xsk_buff_xdp_get_dma(xsk_buffs[i]) - vi->hdr_len;

		sg_init_table(rq->sg, 1);
		sg_fill_dma(rq->sg, addr, len);

		err = virtqueue_add_inbuf(rq->vq, rq->sg, 1, xsk_buffs[i], gfp);
		if (err)
			goto err;
	}

	return num;

err:
	if (i)
		err = i;

	for (; i < num; ++i)
		xsk_buff_free(xsk_buffs[i]);

	return err;
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
				  u64 *kicks)
{
	struct xdp_desc *descs = pool->tx_descs;
	u32 nb_pkts, max_pkts, i;
	bool kick = false;
	int err;

	/* Every xsk tx packet needs two desc(virtnet header and packet). So we
	 * use sq->vq->num_free / 2 as the limitation.
	 */
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
		(*kicks)++;

	return i;
}

bool virtnet_xsk_xmit(struct virtnet_sq *sq, struct xsk_buff_pool *pool,
		      int budget)
{
	struct virtnet_info *vi = sq->vq->vdev->priv;
	u64 bytes = 0, packets = 0, kicks = 0;
	u64 xsknum = 0;
	int sent;

	/* Avoid to wakeup napi meanless, so call __virtnet_free_old_xmit. */
	__virtnet_free_old_xmit(sq, true, &bytes, &packets, &xsknum);
	if (xsknum)
		xsk_tx_completed(sq->xsk.pool, xsknum);

	sent = virtnet_xsk_xmit_batch(sq, pool, budget, &kicks);

	if (!virtnet_is_xdp_raw_buffer_queue(vi, sq - vi->sq))
		virtnet_check_sq_full_and_disable(vi, vi->dev, sq);

	u64_stats_update_begin(&sq->stats.syncp);
	u64_stats_add(&sq->stats.packets, packets);
	u64_stats_add(&sq->stats.bytes,   bytes);
	u64_stats_add(&sq->stats.kicks,   kicks);
	u64_stats_add(&sq->stats.xdp_tx,  sent);
	u64_stats_update_end(&sq->stats.syncp);

	if (xsk_uses_need_wakeup(pool))
		xsk_set_tx_need_wakeup(pool);

	return sent == budget;
}

static void xsk_wakeup(struct virtnet_sq *sq)
{
	if (napi_if_scheduled_mark_missed(&sq->napi))
		return;

	local_bh_disable();
	virtnet_vq_napi_schedule(&sq->napi, sq->vq);
	local_bh_enable();
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

	xsk_wakeup(sq);
	return 0;
}

void virtnet_xsk_completed(struct virtnet_sq *sq, int num)
{
	xsk_tx_completed(sq->xsk.pool, num);

	/* If this is called by rx poll, start_xmit and xdp xmit we should
	 * wakeup the tx napi to consume the xsk tx queue, because the tx
	 * interrupt may not be triggered.
	 */
	xsk_wakeup(sq);
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
	}

	virtnet_rx_pause(vi, rq);

	err = virtqueue_reset(rq->vq, virtnet_rq_free_unused_bufs);
	if (err) {
		netdev_err(vi->dev, "reset rx fail: rx queue index: %d err: %d\n", qindex, err);

		pool = NULL;
	}

	if (!pool)
		xdp_rxq_info_unreg(&rq->xsk.xdp_rxq);

	rq->xsk.pool = pool;

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

	err = virtqueue_reset(sq->vq, virtnet_sq_free_unused_bufs);
	if (err) {
		pool = NULL;
		netdev_err(vi->dev, "reset tx fail: tx queue index: %d err: %d\n", qindex, err);
	}

	sq->xsk.pool = pool;

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

	if (!virtqueue_get_dma_premapped(rq->vq) || !virtqueue_get_dma_premapped(sq->vq))
		return -EPERM;

	/* For the xsk, the tx and rx should have the same device. But
	 * vq->dma_dev allows every vq has the respective dma dev. So I check
	 * the dma dev of vq and sq is the same dev.
	 */
	if (virtqueue_dma_dev(rq->vq) != virtqueue_dma_dev(sq->vq))
		return -EPERM;

	dma_dev = virtqueue_dma_dev(rq->vq);
	if (!dma_dev)
		return -EPERM;

	size = virtqueue_get_vring_size(rq->vq);

	rq->xsk.xsk_buffs = kcalloc(size, sizeof(*rq->xsk.xsk_buffs), GFP_KERNEL);
	if (!rq->xsk.xsk_buffs)
		return -ENOMEM;

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

	/* Now, we do not support tx offset, so all the tx virtnet hdr is zero.
	 * So all the tx packets can share a single hdr.
	 */
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
	struct xsk_buff_pool *pool;
	struct device *dma_dev;
	struct virtnet_rq *rq;
	struct virtnet_sq *sq;
	int err1, err2;

	if (qid >= vi->curr_queue_pairs)
		return -EINVAL;

	sq = &vi->sq[qid];
	rq = &vi->rq[qid];

	pool = sq->xsk.pool;

	err1 = virtnet_sq_bind_xsk_pool(vi, sq, NULL);
	err2 = virtnet_rq_bind_xsk_pool(vi, rq, NULL);

	xsk_pool_dma_unmap(pool, 0);

	dma_dev = virtqueue_dma_dev(rq->vq);

	dma_unmap_single(dma_dev, sq->xsk.hdr_dma_address, vi->hdr_len, DMA_TO_DEVICE);

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
