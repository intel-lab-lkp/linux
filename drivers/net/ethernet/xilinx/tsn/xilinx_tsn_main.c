// SPDX-License-Identifier: GPL-2.0

/*
 * Time Sensitive Networking (TSN) Ethernet MAC driver
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include "xilinx_tsn.h"

static const char * const tsn_clk_names[TSN_NUM_CLOCKS] = {
	"gtx",
	"gtx90",
	"host_rxfifo",
	"host_txfifo",
	"ref",
	"s_axi",
};

static void tsn_dma_rx_cb(void *data, const struct dmaengine_result *result);

/* Ring accessor helpers */
static inline struct skbuf_dma_descriptor *tsn_get_rx_desc(struct tsn_dma_chan *xchan, int idx)
{
	return xchan->skb_ring[idx];
}

static inline struct skbuf_dma_descriptor *tsn_get_tx_desc(struct tsn_dma_chan *xchan, int idx)
{
	return xchan->skb_ring[idx];
}

static void tsn_rx_submit_desc(struct tsn_dma_chan *xchan)
{
	struct dma_async_tx_descriptor *dma_rx_desc = NULL;
	struct tsn_priv *common = xchan->common;
	struct skbuf_dma_descriptor *skbuf_dma;
	struct sk_buff *skb;
	dma_addr_t addr;

	scoped_guard(spinlock_irq, &common->rx_lock) {
		skbuf_dma = tsn_get_rx_desc(xchan, xchan->ring_head);
		if (!skbuf_dma)
			return;

		xchan->ring_head = (xchan->ring_head + 1) & (RX_BD_NUM_DEFAULT - 1);
	}

	skb = dev_alloc_skb(common->max_frm_size);
	if (!skb)
		goto rx_submit_err_revert_head;

	sg_init_table(skbuf_dma->sgl, 1);
	addr = dma_map_single(common->dev, skb->data, common->max_frm_size,
			      DMA_FROM_DEVICE);
	if (unlikely(dma_mapping_error(common->dev, addr))) {
		if (net_ratelimit())
			dev_warn(common->dev, "DMA mapping error on RX submit\n");
		goto rx_submit_err_free_skb;
	}
	sg_dma_address(skbuf_dma->sgl) = addr;
	sg_dma_len(skbuf_dma->sgl) = common->max_frm_size;
	dma_rx_desc = dmaengine_prep_slave_sg(xchan->chan, skbuf_dma->sgl,
					      1, DMA_DEV_TO_MEM,
					      DMA_PREP_INTERRUPT);
	if (!dma_rx_desc)
		goto rx_submit_err_unmap_skb;
	skbuf_dma->skb = skb;
	skbuf_dma->dma_address = sg_dma_address(skbuf_dma->sgl);
	skbuf_dma->desc = dma_rx_desc;
	dma_rx_desc->callback_param = xchan;
	dma_rx_desc->callback_result = tsn_dma_rx_cb;
	/* Ensure descriptor is fully written before submission */
	wmb();
	dmaengine_submit(dma_rx_desc);

	return;

rx_submit_err_unmap_skb:
	dma_unmap_single(common->dev, addr, common->max_frm_size, DMA_FROM_DEVICE);
rx_submit_err_free_skb:
	dev_kfree_skb(skb);
rx_submit_err_revert_head:
	scoped_guard(spinlock_irq, &common->rx_lock) {
		xchan->ring_head = (xchan->ring_head - 1) & (RX_BD_NUM_DEFAULT - 1);
	}
}

/**
 * tsn_classify_rx_packet - Classify received packet by TUSER metadata
 * @common: TSN common structure
 * @tuser: TUSER metadata word from DMA descriptor
 *
 * Extract Input Port ID from TUSER bits[5:4] and return corresponding netdev.
 * Supports EP (endpoint) and MAC1/MAC2 (EMAC) ports.
 *
 * Return: net_device pointer on success, NULL if port not available
 */
static inline struct net_device *tsn_classify_rx_packet(struct tsn_priv *common, u32 tuser)
{
	u32 port_id;
	int i;

	/* Extract Input Port ID from TUSER bits[5:4] */
	port_id = FIELD_GET(TSN_TUSER_PORT_ID_MASK, tuser);
	switch (port_id) {
	case TSN_TUSER_PORT_EP:
		if (unlikely(!common->ep)) {
			dev_err_once(common->dev, "EP not initialized - dropping packets\n");
			return NULL;
		}
		return common->ep->ndev;

	case TSN_TUSER_PORT_MAC1:
	case TSN_TUSER_PORT_MAC2:
		for (i = 0; i < common->num_emacs; i++) {
			if (common->emacs[i] &&
			    common->emacs[i]->emac_num == port_id)
				return common->emacs[i]->ndev;
		}
		if (net_ratelimit())
			dev_warn(common->dev, "RX from MAC port %u not found\n", port_id);
		return NULL;

	default:
		/* Invalid port ID */
		if (net_ratelimit())
			dev_warn(common->dev, "Invalid TUSER port ID: %u\n", port_id);
		return NULL;
	}
}

/**
 * tsn_dma_rx_cb - DMA engine callback for RX channel completion
 * @data: Pointer to the skbuf_dma_descriptor structure
 * @result: Error reporting through dmaengine_result
 *
 * This function is called by dmaengine driver for RX channel to notify
 * that a packet is received. It processes the received packet, updates
 * statistics, and submits a new RX descriptor.
 */
static void tsn_dma_rx_cb(void *data, const struct dmaengine_result *result)
{
	struct skbuf_dma_descriptor *skbuf_dma;
	size_t meta_len, meta_max_len, rx_len;
	struct tsn_dma_chan *xchan = data;
	struct tsn_priv *common = xchan->common;
	struct net_device *ndev;
	struct sk_buff *skb;
	u32 *metadata;
	u32 tuser;

	scoped_guard(spinlock_irq, &common->rx_lock) {
		skbuf_dma = tsn_get_rx_desc(xchan, xchan->ring_tail);
		xchan->ring_tail = (xchan->ring_tail + 1) & (RX_BD_NUM_DEFAULT - 1);
		skb = skbuf_dma->skb;
	}

	dma_unmap_single(common->dev, skbuf_dma->dma_address,
			 common->max_frm_size, DMA_FROM_DEVICE);
	metadata = dmaengine_desc_get_metadata_ptr(skbuf_dma->desc,
						   &meta_len,
						   &meta_max_len);
	if (IS_ERR_OR_NULL(metadata)) {
		if (net_ratelimit())
			dev_warn(common->dev, "Failed to get RX metadata pointer\n");
		dev_kfree_skb_any(skb);
		goto submit_new;
	}

	rx_len = metadata[0];
	tuser = metadata[1];

	if (rx_len > common->max_frm_size || rx_len < ETH_HLEN) {
		if (net_ratelimit())
			dev_warn(common->dev, "Invalid RX length %zu (max=%u, min=%u)\n",
				 rx_len, common->max_frm_size, ETH_HLEN);
		dev_kfree_skb_any(skb);
		goto submit_new;
	}

	ndev = tsn_classify_rx_packet(common, tuser);
	if (unlikely(!ndev)) {
		if (net_ratelimit())
			dev_warn(common->dev, "RX packet from unknown port");
		ndev->stats.rx_dropped++;
		dev_kfree_skb_any(skb);
		goto submit_new;
	}
	skb_put(skb, rx_len);
	skb->dev = ndev;
	skb->protocol = eth_type_trans(skb, ndev);
	skb->ip_summed = CHECKSUM_NONE;
	__netif_rx(skb);

	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += rx_len;

submit_new:
	tsn_rx_submit_desc(xchan);
	dma_async_issue_pending(xchan->chan);
}

/**
 * tsn_dma_tx_cb - DMA engine callback for TX channel completion
 * @data: Pointer to the tsn_dma_chan structure
 * @result: Error reporting through dmaengine_result
 *
 * This function is called by dmaengine driver for TX channel to notify
 * that transmission is complete. It updates statistics, unmaps DMA,
 * frees SKB, and wakes the transmit queue.
 */
static void tsn_dma_tx_cb(void *data, const struct dmaengine_result *result)
{
	struct tsn_dma_chan *xchan = data;
	struct tsn_priv *common = xchan->common;
	struct skbuf_dma_descriptor *skbuf_dma;
	struct netdev_queue *txq;
	int len;
	struct net_device *ndev;

	scoped_guard(spinlock_irq, &common->tx_lock) {
		skbuf_dma = tsn_get_tx_desc(xchan, xchan->ring_tail);
		if (!skbuf_dma || !skbuf_dma->skb)
			return;
		ndev = skbuf_dma->skb->dev;
		if (unlikely(!ndev)) {
			/* Drop silently if SKB lost device association */
			dev_consume_skb_any(skbuf_dma->skb);
			xchan->ring_tail = (xchan->ring_tail + 1) & (TX_BD_NUM_DEFAULT - 1);
			return;
		}
		txq = netdev_get_tx_queue(ndev,
					  skb_get_queue_mapping(skbuf_dma->skb));
		len = skbuf_dma->skb->len;
		xchan->ring_tail = (xchan->ring_tail + 1) & (TX_BD_NUM_DEFAULT - 1);

		ndev->stats.tx_packets++;
		ndev->stats.tx_bytes += len;
	}

	dma_unmap_sg(common->dev, skbuf_dma->sgl, skbuf_dma->sg_len,
		     DMA_TO_DEVICE);
	dev_consume_skb_any(skbuf_dma->skb);
	netif_txq_completed_wake(txq, 1, len,
				 CIRC_SPACE(xchan->ring_head, xchan->ring_tail,
					    TX_BD_NUM_DEFAULT), 2);
}

netdev_tx_t tsn_start_xmit_dmaengine(struct tsn_priv *common,
				     struct sk_buff *skb,
				     struct net_device *ndev)
{
	struct dma_async_tx_descriptor *dma_tx_desc = NULL;
	struct skbuf_dma_descriptor *skbuf_dma;
	int queue = skb_get_queue_mapping(skb);
	struct tsn_dma_chan *xchan;
	struct dma_device *dma_dev;
	struct netdev_queue *txq;
	int sg_len, ret;
	u32 phys_chan;

	if (unlikely(queue >= common->num_tx_queues)) {
		if (net_ratelimit())
			netdev_warn(ndev, "Invalid TX queue %d (max %u)\n",
				    queue, common->num_tx_queues);
		return NETDEV_TX_BUSY;
	}

	/* Map logical software TX queue index to physical DMA channel index */
	phys_chan = common->tx_dma_chan_map[queue];
	if (phys_chan == TSN_DMA_CH_INVALID) {
		if (net_ratelimit())
			netdev_warn(ndev, "Logical TX queue %d has invalid DMA mapping\n", queue);
		return NETDEV_TX_BUSY;
	}

	xchan = common->tx_chans[phys_chan];
	dma_dev = xchan->chan->device;

	sg_len = skb_shinfo(skb)->nr_frags + 1;
	txq = netdev_get_tx_queue(ndev, queue);

	scoped_guard(spinlock_irq, &common->tx_lock) {
		if (CIRC_SPACE(xchan->ring_head, xchan->ring_tail, TX_BD_NUM_DEFAULT) <= 1) {
			netif_tx_stop_queue(txq);
			if (net_ratelimit())
				netdev_warn(ndev, "TSN TX ring full\n");
			return NETDEV_TX_BUSY;
		}

		skbuf_dma = tsn_get_tx_desc(xchan, xchan->ring_head);
		if (!skbuf_dma)
			goto xmit_error_drop_skb;

		xchan->ring_head = (xchan->ring_head + 1) & (TX_BD_NUM_DEFAULT - 1);
	}

	sg_init_table(skbuf_dma->sgl, sg_len);
	ret = skb_to_sgvec(skb, skbuf_dma->sgl, 0, skb->len);
	if (ret < 0)
		goto xmit_error_drop_skb;

	ret = dma_map_sg(common->dev, skbuf_dma->sgl, sg_len, DMA_TO_DEVICE);
	if (!ret)
		goto xmit_error_drop_skb;

	dma_tx_desc = dma_dev->device_prep_slave_sg(xchan->chan, skbuf_dma->sgl,
					    sg_len, DMA_MEM_TO_DEV,
					    DMA_PREP_INTERRUPT, NULL);
	if (!dma_tx_desc)
		goto xmit_error_unmap_sg;

	skbuf_dma->skb = skb;
	skbuf_dma->sg_len = sg_len;
	dma_tx_desc->callback_param = xchan;
	dma_tx_desc->callback_result = tsn_dma_tx_cb;

	netdev_tx_sent_queue(txq, skb->len);
	if (CIRC_SPACE(xchan->ring_head, xchan->ring_tail, TX_BD_NUM_DEFAULT) < 2)
		netif_tx_stop_queue(txq);
	dmaengine_submit(dma_tx_desc);
	dma_async_issue_pending(xchan->chan);

	return NETDEV_TX_OK;

xmit_error_unmap_sg:
	dma_unmap_sg(common->dev, skbuf_dma->sgl, sg_len, DMA_TO_DEVICE);
xmit_error_drop_skb:
	dev_kfree_skb(skb);
	ndev->stats.tx_dropped++;

	return NETDEV_TX_OK;
}

/*
 * Helper to parse TX queue config subnode referenced by
 * xlnx,tsn-tx-config. This version enumerates child nodes in order and
 * assigns DMA channels sequentially (queue0 == first child, etc.)
 */
static int tsn_parse_tx_queue_config(struct device *dev, struct tsn_priv *common,
				     struct device_node *txcfg_np)
{
	struct device_node *qnode;
	unsigned int queue = 0;
	int ret = 0;

	for_each_available_child_of_node(txcfg_np, qnode) {
		u32 chan;

		if (queue >= common->num_tx_queues) {
			dev_err(dev, "tx-config: extra child nodes beyond %u ignored\n",
				common->num_tx_queues);
			of_node_put(qnode);
			return -EINVAL;
		}

		ret = of_property_read_u32(qnode, "xlnx,dma-channel-num", &chan);
		if (ret) {
			dev_err(dev, "tx-config: Q%u missing xlnx,dma-channel-num\n", queue);
			of_node_put(qnode);
			return ret;
		}

		if (chan > TSN_DMA_MAX_TX_CH) {
			dev_err(dev, "tx-config: Q%u channel %u exceeds max %lu\n",
				queue, chan, TSN_DMA_MAX_TX_CH);
			of_node_put(qnode);
			return -EINVAL;
		}
		common->tx_dma_chan_map[queue++] = chan;
	}

	if (queue != common->num_tx_queues) {
		dev_err(dev, "tx-config: described %u queues but expected %u\n",
			queue, common->num_tx_queues);
		return -EINVAL;
	}

	return 0;
}

/**
 * tsn_parse_device_tree - Parse device tree configuration for TSN device
 * @pdev: Platform device pointer
 *
 * Return: 0 on success, negative error code on failure
 */
static int tsn_parse_device_tree(struct platform_device *pdev)
{
	struct tsn_priv *common = platform_get_drvdata(pdev);
	struct device_node *txcfg_np = NULL;
	struct device *dev = &pdev->dev;
	int i, ret;

	/* Read number of priorities */
	ret = of_property_read_u32(dev->of_node, "xlnx,num-priorities", &common->num_priorities);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get xlnx,num-priorities\n");

	if (common->num_priorities < TSN_MIN_PRIORITIES ||
	    common->num_priorities > TSN_MAX_PRIORITIES)
		return dev_err_probe(dev, -EINVAL, "Invalid xlnx,num-priorities (%u)\n",
				     common->num_priorities);

	/* Count TX and RX queues from dma-names property */
	ret = of_property_count_strings(dev->of_node, "dma-names");
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get dma-names\n");

	common->num_tx_queues = 0;
	common->num_rx_queues = 0;

	for (i = 0; i < ret; i++) {
		const char *dma_name;

		if (of_property_read_string_index(dev->of_node, "dma-names", i, &dma_name))
			continue;

		if (strncmp(dma_name, "tx_chan", 7) == 0)
			common->num_tx_queues++;
		else if (strncmp(dma_name, "rx_chan", 7) == 0)
			common->num_rx_queues++;
	}

	if (!common->num_tx_queues || common->num_tx_queues > TSN_MAX_TX_QUEUE)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid TX queue count (%u, max %u)\n",
				     common->num_tx_queues, TSN_MAX_TX_QUEUE);

	if (!common->num_rx_queues)
		return dev_err_probe(dev, -EINVAL, "No RX DMA channels found\n");

	/* Setup clock IDs */
	for (i = 0; i < TSN_NUM_CLOCKS; i++)
		common->clks[i].id = tsn_clk_names[i];

	/* Get all clocks */
	ret = devm_clk_bulk_get(dev, TSN_NUM_CLOCKS, common->clks);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get clocks\n");

	/* Enable clocks */
	ret = clk_bulk_prepare_enable(TSN_NUM_CLOCKS, common->clks);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable clocks\n");

	for (i = 0; i < TSN_MAX_TX_QUEUE; i++)
		common->tx_dma_chan_map[i] = TSN_DMA_CH_INVALID;

	txcfg_np = of_parse_phandle(dev->of_node, "xlnx,tsn-tx-config", 0);
	if (txcfg_np) {
		ret = tsn_parse_tx_queue_config(dev, common, txcfg_np);
		of_node_put(txcfg_np);
		if (ret)
			goto err_disable_clks;
	}

	return 0;

err_disable_clks:
	clk_bulk_disable_unprepare(TSN_NUM_CLOCKS, common->clks);
	return ret;
}

/**
 * tsn_alloc_dma_chan - Allocate and initialize DMA channel
 * @common: Pointer to TSN common structure
 * @name: DMA channel name
 * @is_tx: True for TX channel, false for RX channel
 * @ring_size: Size of descriptor ring
 *
 * Return: Pointer to allocated TSN DMA channel, ERR_PTR on failure
 *
 * This function allocates DMA channel, creates descriptor ring, and
 * initializes channel structure for packet transmission/reception.
 */
static struct tsn_dma_chan *tsn_alloc_dma_chan(struct tsn_priv *common,
					       const char *name, bool is_tx,
					       int ring_size)
{
	struct tsn_dma_chan *chan;
	struct dma_chan *err_chan;
	int i;

	chan = kzalloc(sizeof(*chan), GFP_KERNEL);
	if (!chan)
		return ERR_PTR(-ENOMEM);

	chan->chan = dma_request_chan(common->dev, name);
	if (IS_ERR(chan->chan)) {
		err_chan = chan->chan;
		kfree(chan);
		return ERR_CAST(err_chan);
	}

	chan->skb_ring = kcalloc(ring_size, sizeof(*chan->skb_ring), GFP_KERNEL);
	if (!chan->skb_ring) {
		err_chan = ERR_PTR(-ENOMEM);
		dma_release_channel(chan->chan);
		kfree(chan);
		return ERR_PTR(-ENOMEM);
	}

	for (i = 0; i < ring_size; i++) {
		chan->skb_ring[i] = kzalloc(sizeof(*chan->skb_ring[i]),
					    GFP_KERNEL);
		if (!chan->skb_ring[i]) {
			/* Free already allocated descriptors */
			while (--i >= 0)
				kfree(chan->skb_ring[i]);
			kfree(chan->skb_ring);
			dma_release_channel(chan->chan);
			kfree(chan);
			return ERR_PTR(-ENOMEM);
		}
	}

	chan->ring_head = 0;
	chan->ring_tail = 0;
	chan->is_tx = is_tx;
	chan->common = common;

	return chan;
}

/**
 * tsn_free_dma_chan - Free DMA channel and associated resources
 * @chan: Pointer to TSN DMA channel structure
 *
 * This function releases DMA channel, frees descriptor ring memory,
 * and cleans up all associated resources.
 */
static void tsn_free_dma_chan(struct tsn_dma_chan *chan)
{
	int i;

	if (!chan)
		return;

	if (chan->skb_ring) {
		for (i = 0; i < (chan->is_tx ? TX_BD_NUM_DEFAULT : RX_BD_NUM_DEFAULT); i++)
			kfree(chan->skb_ring[i]);
		kfree(chan->skb_ring);
	}

	if (chan->chan)
		dma_release_channel(chan->chan);

	kfree(chan);
}

/**
 * tsn_exit_dmaengine - Clean up DMA engine resources
 * @pdev: Platform device pointer
 *
 * This function releases all TX and RX DMA channels and frees
 * associated memory during driver shutdown.
 */
static void tsn_exit_dmaengine(struct platform_device *pdev)
{
	struct tsn_priv *common = platform_get_drvdata(pdev);
	int i;

	/* Free TX channels */
	if (common->tx_chans) {
		for (i = 0; i < common->num_tx_queues; i++) {
			if (common->tx_chans[i])
				tsn_free_dma_chan(common->tx_chans[i]);
		}
		kfree(common->tx_chans);
		common->tx_chans = NULL;
	}

	/* Free RX channels */
	if (common->rx_chans) {
		for (i = 0; i < common->num_rx_queues; i++) {
			if (common->rx_chans[i])
				tsn_free_dma_chan(common->rx_chans[i]);
		}
		kfree(common->rx_chans);
		common->rx_chans = NULL;
	}
}

/**
 * tsn_init_dmaengine - Initialize DMA engine for TSN endpoint
 * @pdev: Platform device pointer
 *
 * Return: 0 on success, negative error code on failure
 *
 * This function allocates TX/RX DMA channels, creates descriptor rings,
 * and submits initial RX descriptors for packet reception.
 */
static int tsn_init_dmaengine(struct platform_device *pdev)
{
	struct tsn_priv *common = platform_get_drvdata(pdev);
	int tx_ring_allocated = 0, rx_ring_allocated = 0;
	int i, j, ret = 0;

	common->tx_chans = kcalloc(common->num_tx_queues,
				   sizeof(*common->tx_chans),
				   GFP_KERNEL);
	if (!common->tx_chans)
		return -ENOMEM;

	common->rx_chans = kcalloc(common->num_rx_queues,
				   sizeof(*common->rx_chans),
				   GFP_KERNEL);
	if (!common->rx_chans) {
		ret = -ENOMEM;
		goto err_free_tx;
	}

	// Allocate TX channels
	for (i = 0; i < common->num_tx_queues; i++) {
		char name[16];

		snprintf(name, sizeof(name), "tx_chan%d", i);
		common->tx_chans[i] = tsn_alloc_dma_chan(common, name, true, TX_BD_NUM_DEFAULT);
		if (IS_ERR(common->tx_chans[i])) {
			ret = PTR_ERR(common->tx_chans[i]);
			goto err_free_tx_chans;
		}
		tx_ring_allocated++;
	}

	// Allocate RX channels
	for (i = 0; i < common->num_rx_queues; i++) {
		char name[16];

		snprintf(name, sizeof(name), "rx_chan%d", i);
		common->rx_chans[i] = tsn_alloc_dma_chan(common, name, false, RX_BD_NUM_DEFAULT);
		if (IS_ERR(common->rx_chans[i])) {
			ret = PTR_ERR(common->rx_chans[i]);
			goto err_free_rx_chans;
		}
		rx_ring_allocated++;
	}

	// Submit initial RX descriptors
	for (i = 0; i < common->num_rx_queues; i++) {
		for (j = 0; j < RX_BD_NUM_DEFAULT; j++)
			tsn_rx_submit_desc(common->rx_chans[i]);
		dma_async_issue_pending(common->rx_chans[i]->chan);
	}

	return 0;

err_free_rx_chans:
	while (--rx_ring_allocated >= 0)
		tsn_free_dma_chan(common->rx_chans[rx_ring_allocated]);
err_free_tx_chans:
	while (--tx_ring_allocated >= 0)
		tsn_free_dma_chan(common->tx_chans[tx_ring_allocated]);
	kfree(common->rx_chans);
err_free_tx:
	kfree(common->tx_chans);
	return ret;
}

static int tsn_reset_dma_controller(struct tsn_priv *common)
{
	struct xilinx_vdma_config cfg = { .reset = 1 };
	struct dma_chan *tx_chan0;
	int ret;

	tx_chan0 = dma_request_chan(common->dev, "tx_chan0");
	if (IS_ERR(tx_chan0))
		return dev_err_probe(common->dev, PTR_ERR(tx_chan0),
				     "Failed to request tx_chan0 for reset\n");

	ret = xilinx_vdma_channel_set_config(tx_chan0, &cfg);
	dma_release_channel(tx_chan0);

	if (ret < 0)
		return dev_err_probe(common->dev, ret,
				"Failed to reset DMA controller\n");

	dev_info(common->dev, "DMA controller reset successful\n");
	return 0;
}

/**
 * tsn_ip_probe - Probe TSN IP core device
 * @pdev: Platform device pointer
 *
 * Return: 0 on success, negative error code on failure
 */
static int tsn_ip_probe(struct platform_device *pdev)
{
	struct tsn_priv *common;
	int ret;

	common = devm_kzalloc(&pdev->dev, sizeof(*common), GFP_KERNEL);
	if (!common)
		return -ENOMEM;

	platform_set_drvdata(pdev, common);
	common->pdev = pdev;
	common->dev = &pdev->dev;

	/* Initialize synchronization primitives */
	spin_lock_init(&common->tx_lock);
	spin_lock_init(&common->rx_lock);
	mutex_init(&common->mdio_lock);

	common->res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!common->res)
		return -ENODEV;
	common->regs_start = common->res->start;
	common->regs = devm_ioremap_resource(&pdev->dev, common->res);
	if (IS_ERR(common->regs))
		return PTR_ERR(common->regs);

	ret = tsn_parse_device_tree(pdev);
	if (ret)
		return ret;

	common->max_frm_size = TSN_MAX_VLAN_FRAME_SIZE;

	/* Reset DMA controller BEFORE any channel allocation */
	ret = tsn_reset_dma_controller(common);
	if (ret)
		goto free_clk;

	/* Initialize DMA engine - allocate channels */
	ret = tsn_init_dmaengine(pdev);
	if (ret) {
		dev_err(common->dev, "Failed to initialize DMA engine: %d\n", ret);
		goto free_clk;
	}

	ret = tsn_ep_init(pdev);
	if (ret)
		goto exit_dma;

	ret = tsn_emac_init(pdev);
	if (ret)
		goto exit_ep;

	ret = tsn_switch_init(pdev);
	if (ret)
		goto exit_emac;

	return 0;

exit_emac:
	tsn_emac_exit(pdev);
exit_ep:
	tsn_ep_exit(pdev);
exit_dma:
	tsn_exit_dmaengine(pdev);
free_clk:
	clk_bulk_disable_unprepare(TSN_NUM_CLOCKS, common->clks);
	return ret;
}

/**
 * tsn_ip_remove - Remove TSN IP core device
 * @pdev: Platform device pointer
 */
static void tsn_ip_remove(struct platform_device *pdev)
{
	struct tsn_priv *common = platform_get_drvdata(pdev);

	tsn_switch_exit(pdev);
	tsn_emac_exit(pdev);
	/* Tear down DMA channels and endpoint */
	if (common->ep)
		tsn_ep_exit(pdev);
	tsn_exit_dmaengine(pdev);
	clk_bulk_disable_unprepare(TSN_NUM_CLOCKS, common->clks);
}

static const struct of_device_id tsn_of_match[] = {
	{ .compatible = "xlnx,tsn-endpoint-ethernet-mac-3.0", },
	{ }
};
MODULE_DEVICE_TABLE(of, tsn_of_match);

static struct platform_driver tsn_driver = {
	.probe = tsn_ip_probe,
	.remove = tsn_ip_remove,
	.driver = {
		.name = "xilinx-tsn",
		.of_match_table = tsn_of_match,
	},
};
module_platform_driver(tsn_driver);

MODULE_AUTHOR("Neeli Srinivas <srinivas.neeli@amd.com>");
MODULE_DESCRIPTION("Time Sensitive Networking (TSN) Ethernet MAC driver");
MODULE_LICENSE("GPL");
