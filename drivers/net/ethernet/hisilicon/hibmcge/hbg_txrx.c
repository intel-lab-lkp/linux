// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2024 Hisilicon Limited.

#include "hbg_common.h"
#include "hbg_irq.h"
#include "hbg_reg.h"
#include "hbg_txrx.h"

#define netdev_get_tx_ring(netdev)  (&(((struct hbg_priv *)netdev_priv(netdev))->tx_ring))

#define buffer_to_dma_dir(buffer) (((buffer)->dir == HBG_DIR_RX) ? \
				   DMA_FROM_DEVICE : DMA_TO_DEVICE)

#define hbg_queue_is_full(head, tail, ring) ((head) == ((tail) + 1) % (ring)->len)
#define hbg_queue_is_empty(head, tail) ((head) == (tail))
#define hbg_queue_next_prt(p, ring) (((p) + 1) % (ring)->len)
#define hbg_queue_move_next(p, ring) ({ \
	typeof(ring) _ring = (ring); \
	_ring->p = hbg_queue_next_prt(_ring->p, _ring); })

static int hbg_dma_map(struct hbg_buffer *buffer)
{
	struct hbg_priv *priv = buffer->priv;

	buffer->skb_dma = dma_map_single(&priv->pdev->dev,
					 buffer->skb->data, buffer->skb_len,
					 buffer_to_dma_dir(buffer));
	if (unlikely(dma_mapping_error(&priv->pdev->dev, buffer->skb_dma)))
		return -ENOMEM;

	return 0;
}

static void hbg_dma_unmap(struct hbg_buffer *buffer)
{
	struct hbg_priv *priv = buffer->priv;

	if (unlikely(!buffer->skb_dma))
		return;

	dma_unmap_single(&priv->pdev->dev, buffer->skb_dma, buffer->skb_len,
			 buffer_to_dma_dir(buffer));
	buffer->skb_dma = 0;
}

static void hbg_init_tx_desc(struct hbg_buffer *buffer,
			     struct hbg_tx_desc *tx_desc)
{
	u32 ip_offset = buffer->skb->network_header - buffer->skb->mac_header;
	u32 word0 = 0;

	word0 |= FIELD_PREP(HBG_TX_DESC_W0_WB_B, HBG_STATUS_ENABLE);
	word0 |= FIELD_PREP(HBG_TX_DESC_W0_IP_OFF_M, ip_offset);
	if (likely(buffer->skb->ip_summed == CHECKSUM_PARTIAL)) {
		word0 |= FIELD_PREP(HBG_TX_DESC_W0_l3_CS_B, HBG_STATUS_ENABLE);
		word0 |= FIELD_PREP(HBG_TX_DESC_W0_l4_CS_B, HBG_STATUS_ENABLE);
	}

	tx_desc->word0 = word0;
	tx_desc->word1 = FIELD_PREP(HBG_TX_DESC_W1_SEND_LEN_M, buffer->skb->len);
	tx_desc->word2 = buffer->skb_dma;
	tx_desc->word3 = buffer->state_dma;
}

netdev_tx_t hbg_net_start_xmit(struct sk_buff *skb, struct net_device *net_dev)
{
	struct hbg_ring *ring = netdev_get_tx_ring(net_dev);
	struct hbg_priv *priv = netdev_priv(net_dev);
	/* This smp_load_acquire() pairs with smp_store_release() in
	 * hbg_tx_buffer_recycle() called in tx interrupt handle process.
	 */
	u32 ntc = smp_load_acquire(&ring->ntc);
	struct hbg_buffer *buffer;
	struct hbg_tx_desc tx_desc;
	u32 ntu = ring->ntu;

	if (unlikely(!hbg_nic_is_open(priv))) {
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	if (unlikely(!skb->len ||
		     skb->len > hbg_spec_max_frame_len(priv, HBG_DIR_TX))) {
		dev_kfree_skb_any(skb);
		net_dev->stats.tx_errors++;
		return NETDEV_TX_OK;
	}

	if (unlikely(hbg_queue_is_full(ntc, ntu, ring) ||
		     hbg_fifo_is_full(ring->priv, ring->dir))) {
		netif_stop_queue(net_dev);
		return NETDEV_TX_BUSY;
	}

	buffer = &ring->queue[ntu];
	buffer->skb = skb;
	buffer->skb_len = skb->len;
	if (unlikely(hbg_dma_map(buffer))) {
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	buffer->state = HBG_TX_STATE_START;
	hbg_init_tx_desc(buffer, &tx_desc);
	hbg_hw_set_tx_desc(priv, &tx_desc);

	/* This smp_store_release() pairs with smp_load_acquire() in
	 * hbg_tx_buffer_recycle() called in tx interrupt handle process.
	 */
	smp_store_release(&ring->ntu, hbg_queue_next_prt(ntu, ring));
	net_dev->stats.tx_bytes += skb->len;
	net_dev->stats.tx_packets++;
	return NETDEV_TX_OK;
}

static void hbg_buffer_free_skb(struct hbg_buffer *buffer)
{
	if (unlikely(!buffer->skb))
		return;

	dev_kfree_skb_any(buffer->skb);
	buffer->skb = NULL;
}

static int hbg_buffer_alloc_skb(struct hbg_buffer *buffer)
{
	u32 len = hbg_spec_max_frame_len(buffer->priv, buffer->dir);
	struct hbg_priv *priv = buffer->priv;

	buffer->skb = netdev_alloc_skb(priv->netdev, len);
	if (unlikely(!buffer->skb))
		return -ENOMEM;

	buffer->skb_len = len;
	memset(buffer->skb->data, 0, HBG_PACKET_HEAD_SIZE);
	return 0;
}

static void hbg_buffer_free(struct hbg_buffer *buffer)
{
	hbg_dma_unmap(buffer);
	hbg_buffer_free_skb(buffer);
}

static int hbg_napi_tx_recycle(struct napi_struct *napi, int budget)
{
	struct hbg_ring *ring = container_of(napi, struct hbg_ring, napi);
	/* This smp_load_acquire() pairs with smp_store_release() in
	 * hbg_start_xmit() called in xmit process.
	 */
	u32 ntu = smp_load_acquire(&ring->ntu);
	struct hbg_priv *priv = ring->priv;
	struct hbg_buffer *buffer;
	u32 ntc = ring->ntc;
	int packet_done = 0;

	while (packet_done < budget) {
		if (unlikely(hbg_queue_is_empty(ntc, ntu)))
			break;

		/* make sure HW write desc complete */
		dma_rmb();

		buffer = &ring->queue[ntc];
		if (buffer->state != HBG_TX_STATE_COMPLETE)
			break;

		hbg_buffer_free(buffer);
		ntc = hbg_queue_next_prt(ntc, ring);
		packet_done++;
	}

	/* This smp_store_release() pairs with smp_load_acquire() in
	 * hbg_start_xmit() called in xmit process.
	 */
	smp_store_release(&ring->ntc, ntc);
	netif_wake_queue(priv->netdev);

	if (likely(napi_complete_done(napi, packet_done)))
		hbg_hw_irq_enable(priv, HBG_INT_MSK_TX_B, true);

	return packet_done;
}

static int hbg_rx_fill_one_buffer(struct hbg_priv *priv)
{
	struct hbg_ring *ring = &priv->rx_ring;
	struct hbg_buffer *buffer;
	int ret;

	if (hbg_queue_is_full(ring->ntc, ring->ntu, ring))
		return 0;

	buffer = &ring->queue[ring->ntu];
	ret = hbg_buffer_alloc_skb(buffer);
	if (unlikely(ret))
		return ret;

	ret = hbg_dma_map(buffer);
	if (unlikely(ret)) {
		hbg_buffer_free_skb(buffer);
		return ret;
	}

	hbg_hw_fill_buffer(priv, buffer->skb_dma);
	hbg_queue_move_next(ntu, ring);
	return 0;
}

static int hbg_rx_fill_buffers(struct hbg_priv *priv)
{
	struct hbg_ring *ring = &priv->rx_ring;
	u32 fifo_left;
	int ret;

	if (hbg_fifo_is_full(priv, ring->dir))
		return 0;

	fifo_left = hbg_get_spec_fifo_max_num(priv, ring->dir) -
		    hbg_hw_get_fifo_used_num(priv, ring->dir);
	while (fifo_left) {
		ret = hbg_rx_fill_one_buffer(priv);
		if (ret)
			return ret;

		fifo_left--;
	}

	return 0;
}

static bool hbg_sync_data_from_hw(struct hbg_priv *priv,
				  struct hbg_buffer *buffer)
{
	struct hbg_rx_desc *rx_desc;

	/* make sure HW write desc complete */
	dma_rmb();

	dma_sync_single_for_cpu(&priv->pdev->dev, buffer->skb_dma,
				buffer->skb_len, DMA_FROM_DEVICE);

	rx_desc = (struct hbg_rx_desc *)buffer->skb->data;
	return FIELD_GET(HBG_RX_DESC_W2_PKT_LEN_M, rx_desc->word2) != 0;
}

static int hbg_napi_rx_poll(struct napi_struct *napi, int budget)
{
	struct hbg_ring *ring = container_of(napi, struct hbg_ring, napi);
	struct hbg_priv *priv = ring->priv;
	struct hbg_rx_desc *rx_desc;
	struct hbg_buffer *buffer;
	u32 packet_done = 0;
	u32 pkt_len;

	if (unlikely(!hbg_nic_is_open(priv))) {
		napi_complete(napi);
		return 0;
	}

	while (packet_done < budget) {
		if (unlikely(hbg_queue_is_empty(ring->ntc, ring->ntu)))
			break;

		buffer = &ring->queue[ring->ntc];
		if (unlikely(!buffer->skb))
			goto next_buffer;

		if (unlikely(!hbg_sync_data_from_hw(priv, buffer)))
			break;

		hbg_dma_unmap(buffer);

		skb_reserve(buffer->skb, HBG_PACKET_HEAD_SIZE + NET_IP_ALIGN);

		rx_desc = (struct hbg_rx_desc *)buffer->skb->data;
		pkt_len = FIELD_GET(HBG_RX_DESC_W2_PKT_LEN_M, rx_desc->word2);
		skb_put(buffer->skb, pkt_len);
		buffer->skb->protocol = eth_type_trans(buffer->skb, priv->netdev);

		priv->netdev->stats.rx_bytes += pkt_len;
		priv->netdev->stats.rx_packets++;
		netif_receive_skb(buffer->skb);
		buffer->skb = NULL;
		hbg_rx_fill_one_buffer(priv);

next_buffer:
		hbg_queue_move_next(ntc, ring);
		packet_done++;
	}

	hbg_rx_fill_buffers(priv);
	if (likely(napi_complete_done(napi, packet_done)))
		hbg_hw_irq_enable(priv, HBG_INT_MSK_RX_B, true);

	return packet_done;
}

static void hbg_ring_uninit(struct hbg_ring *ring)
{
	struct hbg_buffer *buffer;
	u32 i;

	if (!ring->queue)
		return;

	netif_napi_del(&ring->napi);

	for (i = 0; i < ring->len; i++) {
		buffer = &ring->queue[i];
		hbg_buffer_free(buffer);
		buffer->ring = NULL;
		buffer->priv = NULL;
	}

	dma_free_coherent(&ring->priv->pdev->dev,
			  ring->len * sizeof(*ring->queue),
			  ring->queue, ring->queue_dma);
	ring->queue = NULL;
	ring->queue_dma = 0;
	ring->len = 0;
	ring->priv = NULL;
}

static int hbg_ring_init(struct hbg_priv *priv, struct hbg_ring *ring,
			 enum hbg_dir dir)
{
	struct hbg_buffer *buffer;
	u32 i, len;

	len = hbg_get_spec_fifo_max_num(priv, dir) + 1;
	ring->queue = dma_alloc_coherent(&priv->pdev->dev,
					 len * sizeof(*ring->queue),
					 &ring->queue_dma, GFP_KERNEL);
	if (!ring->queue)
		return -ENOMEM;

	for (i = 0; i < len; i++) {
		buffer = &ring->queue[i];
		buffer->skb_len = 0;
		buffer->dir = dir;
		buffer->ring = ring;
		buffer->priv = priv;
		buffer->state_dma = ring->queue_dma + (i * sizeof(*buffer));
	}

	ring->dir = dir;
	ring->priv = priv;
	ring->ntc = 0;
	ring->ntu = 0;
	ring->len = len;
	return 0;
}

static int hbg_tx_ring_init(struct hbg_priv *priv)
{
	struct hbg_ring *tx_ring = &priv->tx_ring;
	int ret;

	if (!tx_ring->tout_log_buf)
		tx_ring->tout_log_buf = devm_kzalloc(&priv->pdev->dev,
						     HBG_TX_TIMEOUT_BUF_LEN,
						     GFP_KERNEL);

	if (!tx_ring->tout_log_buf)
		return -ENOMEM;

	ret = hbg_ring_init(priv, tx_ring, HBG_DIR_TX);
	if (ret)
		return ret;

	netif_napi_add_tx(priv->netdev, &tx_ring->napi, hbg_napi_tx_recycle);
	return 0;
}

static int hbg_rx_ring_init(struct hbg_priv *priv)
{
	struct hbg_ring *rx_ring = &priv->rx_ring;
	int ret;

	ret = hbg_ring_init(priv, rx_ring, HBG_DIR_RX);
	if (ret)
		return ret;

	netif_napi_add(priv->netdev, &priv->rx_ring.napi, hbg_napi_rx_poll);
	return 0;
}

int hbg_txrx_init(struct hbg_priv *priv)
{
	int ret;

	ret = hbg_tx_ring_init(priv);
	if (ret) {
		dev_err(&priv->pdev->dev,
			"failed to init tx ring, ret = %d\n", ret);
		return ret;
	}

	ret = hbg_rx_ring_init(priv);
	if (ret) {
		dev_err(&priv->pdev->dev,
			"failed to init rx ring, ret = %d\n", ret);
		goto err_uninit_tx;
	}

	ret = hbg_rx_fill_buffers(priv);
	if (ret) {
		dev_err(&priv->pdev->dev,
			"failed to fill rx buffers, ret = %d\n", ret);
		goto err_uninit_rx;
	}

	return 0;

err_uninit_rx:
	hbg_ring_uninit(&priv->rx_ring);
err_uninit_tx:
	hbg_ring_uninit(&priv->tx_ring);
	return ret;
}

void hbg_txrx_uninit(void *data)
{
	struct hbg_priv *priv = data;

	hbg_ring_uninit(&priv->tx_ring);
	hbg_ring_uninit(&priv->rx_ring);
}
