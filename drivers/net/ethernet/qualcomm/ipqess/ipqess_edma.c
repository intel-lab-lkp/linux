// SPDX-License-Identifier: GPL-2.0 OR ISC
/* Copyright (c) 2014 - 2017, The Linux Foundation. All rights reserved.
 * Copyright (c) 2017 - 2018, John Crispin <john@phrozen.org>
 * Copyright (c) 2018 - 2019, Christian Lamparter <chunkeey@gmail.com>
 * Copyright (c) 2020 - 2021, Gabor Juhos <j4g8y7@gmail.com>
 * Copyright (c) 2021 - 2022, Maxime Chevallier <maxime.chevallier@bootlin.com>
 * Copyright (c) 2023, Romain Gantois <romain.gantois@bootlin.com>
 *
 */

#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/of_net.h>
#include <linux/skbuff.h>
#include <linux/if_vlan.h>
#include <linux/reset.h>
#include <linux/of_platform.h>
#include <net/ip6_checksum.h>
#include <net/dst_metadata.h>
#include <net/page_pool/helpers.h>

#include "ipqess_edma.h"
#include "ipqess_port.h"
#include "ipqess_switch.h"

static void ipqess_edma_w32(struct ipqess_edma *edma, u32 reg, u32 val)
{
	writel(val, edma->hw_addr + reg);
}

static u32 ipqess_edma_r32(struct ipqess_edma *edma, u16 reg)
{
	return readl(edma->hw_addr + reg);
}

static void ipqess_edma_m32(struct ipqess_edma *edma, u32 mask, u32 val,
			    u16 reg)
{
	u32 _val = ipqess_edma_r32(edma, reg);

	_val &= ~mask;
	_val |= val;

	ipqess_edma_w32(edma, reg, _val);
}

static int ipqess_edma_tx_ring_alloc(struct ipqess_edma *edma)
{
	struct device *dev = &edma->pdev->dev;
	int i;

	for (i = 0; i < IPQESS_EDMA_NETDEV_QUEUES; i++) {
		struct ipqess_edma_tx_ring *tx_ring = &edma->tx_ring[i];
		size_t size;
		u32 idx;

		tx_ring->edma = edma;
		tx_ring->ring_id = i;
		tx_ring->idx = i * 4;
		tx_ring->count = IPQESS_EDMA_TX_RING_SIZE;
		tx_ring->nq = netdev_get_tx_queue(edma->netdev, i);

		size = sizeof(struct ipqess_edma_buf) * IPQESS_EDMA_TX_RING_SIZE;
		tx_ring->buf = devm_kzalloc(dev, size, GFP_KERNEL);
		if (!tx_ring->buf)
			return -ENOMEM;

		size = sizeof(struct ipqess_edma_tx_desc) * IPQESS_EDMA_TX_RING_SIZE;
		tx_ring->desc = dmam_alloc_coherent(dev, size, &tx_ring->ring_dma,
						    GFP_KERNEL);
		if (!tx_ring->desc)
			return -ENOMEM;

		ipqess_edma_w32(edma, IPQESS_EDMA_REG_TPD_BASE_ADDR_Q(tx_ring->idx),
				(u32)tx_ring->ring_dma);

		idx = ipqess_edma_r32(edma, IPQESS_EDMA_REG_TPD_IDX_Q(tx_ring->idx));
		idx >>= IPQESS_EDMA_TPD_CONS_IDX_SHIFT; /* need u32 here */
		idx &= 0xffff;
		tx_ring->head = idx;
		tx_ring->tail = idx;

		ipqess_edma_m32(edma,
				IPQESS_EDMA_TPD_PROD_IDX_MASK
					<< IPQESS_EDMA_TPD_PROD_IDX_SHIFT,
				idx, IPQESS_EDMA_REG_TPD_IDX_Q(tx_ring->idx));
		ipqess_edma_w32(edma, IPQESS_EDMA_REG_TX_SW_CONS_IDX_Q(tx_ring->idx),
				idx);
		ipqess_edma_w32(edma, IPQESS_EDMA_REG_TPD_RING_SIZE,
				IPQESS_EDMA_TX_RING_SIZE);
	}

	return 0;
}

static int ipqess_edma_tx_unmap_and_free(struct device *dev,
					 struct ipqess_edma_buf *buf)
{
	int len = 0;

	if (buf->flags & IPQESS_EDMA_DESC_SINGLE)
		dma_unmap_single(dev, buf->dma,	buf->length, DMA_TO_DEVICE);
	else if (buf->flags & IPQESS_EDMA_DESC_PAGE)
		dma_unmap_page(dev, buf->dma, buf->length, DMA_TO_DEVICE);

	if (buf->flags & IPQESS_EDMA_DESC_LAST) {
		len = buf->skb->len;
		dev_kfree_skb_any(buf->skb);
	}

	buf->flags = 0;

	return len;
}

static void ipqess_edma_tx_ring_free(struct ipqess_edma *edma)
{
	int i;

	for (i = 0; i < IPQESS_EDMA_NETDEV_QUEUES; i++) {
		int j;

		if (edma->tx_ring[i].desc)
			continue;

		for (j = 0; j < IPQESS_EDMA_TX_RING_SIZE; j++) {
			struct ipqess_edma_buf *buf = &edma->tx_ring[i].buf[j];

			ipqess_edma_tx_unmap_and_free(&edma->pdev->dev, buf);
		}

		edma->tx_ring[i].buf = NULL;
	}
}

static int ipqess_edma_rx_buf_prepare(struct ipqess_edma_buf *buf,
				      struct ipqess_edma_rx_ring *rx_ring)
{
	memset(buf->skb->data, 0, sizeof(struct ipqess_edma_rx_desc));

	buf->dma = dma_map_single(rx_ring->ppdev, buf->skb->data,
				  IPQESS_EDMA_RX_HEAD_BUFF_SIZE,
				  DMA_FROM_DEVICE);
	if (dma_mapping_error(rx_ring->ppdev, buf->dma)) {
		dev_kfree_skb_any(buf->skb);
		buf->skb = NULL;
		return -EFAULT;
	}

	buf->length = IPQESS_EDMA_RX_HEAD_BUFF_SIZE;
	rx_ring->desc_dma[rx_ring->head] = buf->dma;
	rx_ring->head = (rx_ring->head + 1) % IPQESS_EDMA_RX_RING_SIZE;

	ipqess_edma_m32(rx_ring->edma, IPQESS_EDMA_RFD_PROD_IDX_BITS,
			(rx_ring->head + IPQESS_EDMA_RX_RING_SIZE - 1)
			% IPQESS_EDMA_RX_RING_SIZE,
			IPQESS_EDMA_REG_RFD_IDX_Q(rx_ring->idx));

	return 0;
}

/* locking is handled by the caller */
static int ipqess_edma_rx_buf_alloc_napi(struct ipqess_edma_rx_ring *rx_ring)
{
	struct ipqess_edma_buf *buf = &rx_ring->buf[rx_ring->head];
	struct page *new_page = page_pool_dev_alloc_pages(rx_ring->page_pool);

	if (!new_page)
		return -ENOMEM;

	buf->skb = napi_build_skb(page_address(new_page),
				  SKB_HEAD_ALIGN(IPQESS_EDMA_RX_HEAD_BUFF_SIZE));
	if (unlikely(!buf->skb)) {
		page_pool_put_page(rx_ring->page_pool, new_page,
				   IPQESS_EDMA_RX_HEAD_BUFF_SIZE,
				   true);
		return -ENOMEM;
	}

	skb_mark_for_recycle(buf->skb);

	return ipqess_edma_rx_buf_prepare(buf, rx_ring);
}

static int ipqess_edma_rx_buf_alloc(struct ipqess_edma_rx_ring *rx_ring)
{
	struct ipqess_edma_buf *buf = &rx_ring->buf[rx_ring->head];
	struct page *new_page = page_pool_dev_alloc_pages(rx_ring->page_pool);

	if (!new_page)
		return -ENOMEM;

	buf->skb = build_skb(page_address(new_page),
			     SKB_HEAD_ALIGN(IPQESS_EDMA_RX_HEAD_BUFF_SIZE));
	if (unlikely(!buf->skb)) {
		page_pool_put_page(rx_ring->page_pool, new_page,
				   IPQESS_EDMA_RX_HEAD_BUFF_SIZE,
				   true);
		return -ENOMEM;
	}

	skb_mark_for_recycle(buf->skb);
	skb_reserve(buf->skb, NET_IP_ALIGN);

	return ipqess_edma_rx_buf_prepare(buf, rx_ring);
}

static void ipqess_edma_refill_work(struct work_struct *work)
{
	struct ipqess_edma_rx_ring_refill *rx_refill =
			container_of(work, struct ipqess_edma_rx_ring_refill,
				     refill_work);
	struct ipqess_edma_rx_ring *rx_ring = rx_refill->rx_ring;
	int refill = 0;

	/* don't let this loop by accident. */
	while (atomic_dec_and_test(&rx_ring->refill_count)) {
		napi_disable(&rx_ring->napi_rx);
		if (ipqess_edma_rx_buf_alloc(rx_ring)) {
			refill++;
			dev_dbg(rx_ring->ppdev,
				"Not all buffers were reallocated");
		}
		napi_enable(&rx_ring->napi_rx);
	}

	if (atomic_add_return(refill, &rx_ring->refill_count))
		schedule_work(&rx_refill->refill_work);
}

static int ipqess_edma_rx_ring_alloc(struct ipqess_edma *edma)
{
	struct page_pool_params pp_params = {
		.order = 0,
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.pool_size = IPQESS_EDMA_RX_RING_SIZE,
		.nid = dev_to_node(&edma->pdev->dev),
		.dev = &edma->pdev->dev,
		.dma_dir = DMA_FROM_DEVICE,
		.offset = 0,
		.max_len = IPQESS_EDMA_RX_HEAD_BUFF_SIZE,
	};
	int i;

	for (i = 0; i < IPQESS_EDMA_NETDEV_QUEUES; i++) {
		struct ipqess_edma_rx_ring *rx_ring = &edma->rx_ring[i];
		int j;

		rx_ring->edma = edma;
		rx_ring->ppdev = &edma->pdev->dev;
		rx_ring->ring_id = i;
		rx_ring->idx = i * 2;

		rx_ring->buf =
			devm_kzalloc(&edma->pdev->dev,
				     sizeof(struct ipqess_edma_buf)
				     * IPQESS_EDMA_RX_RING_SIZE,
				     GFP_KERNEL);

		if (!rx_ring->buf)
			return -ENOMEM;

		rx_ring->desc_dma =
			dmam_alloc_coherent(&edma->pdev->dev,
					    sizeof(dma_addr_t)
					    * IPQESS_EDMA_RX_RING_SIZE,
					    &rx_ring->ring_dma, GFP_KERNEL);

		if (!rx_ring->desc_dma)
			return -ENOMEM;

		rx_ring->page_pool = page_pool_create(&pp_params);

		/* Fill the entire ring once */
		for (j = 0; j < IPQESS_EDMA_RX_RING_SIZE; j++)
			if (ipqess_edma_rx_buf_alloc(rx_ring) < 0)
				return -ENOMEM;

		edma->rx_refill[i].rx_ring = &edma->rx_ring[i];
		INIT_WORK(&edma->rx_refill[i].refill_work,
			  ipqess_edma_refill_work);

		ipqess_edma_w32(edma,
				IPQESS_EDMA_REG_RFD_BASE_ADDR_Q(rx_ring->idx),
				(u32)(rx_ring->ring_dma));
	}

	ipqess_edma_w32(edma, IPQESS_EDMA_REG_RX_DESC0,
			(IPQESS_EDMA_RX_HEAD_BUFF_SIZE << IPQESS_EDMA_RX_BUF_SIZE_SHIFT) |
			(IPQESS_EDMA_RX_RING_SIZE << IPQESS_EDMA_RFD_RING_SIZE_SHIFT));

	return 0;
}

static void ipqess_edma_rx_ring_free(struct ipqess_edma *edma)
{
	int i;

	for (i = 0; i < IPQESS_EDMA_NETDEV_QUEUES; i++) {
		struct ipqess_edma_rx_ring *rx_ring = &edma->rx_ring[i];
		int j;

		cancel_work_sync(&edma->rx_refill[i].refill_work);
		atomic_set(&rx_ring->refill_count, 0);

		for (j = 0; j < IPQESS_EDMA_RX_RING_SIZE; j++) {
			dma_unmap_single(&edma->pdev->dev,
					 rx_ring->buf[j].dma,
					 rx_ring->buf[j].length,
					 DMA_FROM_DEVICE);
			dev_kfree_skb_any(rx_ring->buf[j].skb);
		}
		page_pool_destroy(rx_ring->page_pool);
	}
}

static int ipqess_edma_redirect(struct ipqess_edma_rx_ring *rx_ring,
				struct sk_buff *skb, int port_id)
{
	struct ipqess_port *port;

	if (port_id == 0) {
		/* The switch probably redirected an unknown frame to the CPU port
		 * (IGMP,BC,unknown MC, unknown UC)
		 */
		return -EINVAL;
	}

	if (port_id < 0 || port_id > QCA8K_NUM_PORTS) {
		dev_warn_ratelimited(rx_ring->edma->sw->priv->dev,
				     "received packet tagged with out-of-bounds port id %d\n",
				     port_id);
		return -EINVAL;
	}

	port = rx_ring->edma->sw->port_list[port_id - 1];
	if (!port) {
		/* drop packets tagged from unregistered ports */
		return -EINVAL;
	}

	skb->dev = port->netdev;
	skb_push(skb, ETH_HLEN);
	skb->pkt_type = PACKET_HOST;
	skb->protocol = eth_type_trans(skb, skb->dev);

	dev_sw_netstats_rx_add(skb->dev, skb->len);

	napi_gro_receive(&rx_ring->napi_rx, skb);

	return 0;
}

static int ipqess_edma_refill_rx_ring(struct ipqess_edma_rx_ring *rx_ring,
				      u32 num_desc)
{
	struct work_struct *refill_work = &rx_ring->edma->rx_refill[rx_ring->ring_id].refill_work;

	num_desc += atomic_xchg(&rx_ring->refill_count, 0);
	while (num_desc) {
		if (ipqess_edma_rx_buf_alloc_napi(rx_ring)) {
			num_desc = atomic_add_return(num_desc,
						     &rx_ring->refill_count);
			if (num_desc >= DIV_ROUND_UP(IPQESS_EDMA_RX_RING_SIZE * 4, 7))
				schedule_work(refill_work);

			break;
		}
		num_desc--;
	}

	return num_desc;
}

static int ipqess_edma_rx_poll(struct ipqess_edma_rx_ring *rx_ring, int budget)
{
	u32 length = 0, num_desc, tail, rx_ring_tail;
	int done = 0;
	int port_id;

	rx_ring_tail = rx_ring->tail;

	tail = ipqess_edma_r32(rx_ring->edma,
			       IPQESS_EDMA_REG_RFD_IDX_Q(rx_ring->idx));
	tail >>= IPQESS_EDMA_RFD_CONS_IDX_SHIFT;
	tail &= IPQESS_EDMA_RFD_CONS_IDX_MASK;

	while (done < budget) {
		struct ipqess_edma_rx_desc *rd;
		struct sk_buff *skb;

		if (rx_ring_tail == tail)
			break;

		dma_unmap_single(rx_ring->ppdev,
				 rx_ring->buf[rx_ring_tail].dma,
				 rx_ring->buf[rx_ring_tail].length,
				 DMA_FROM_DEVICE);

		skb = xchg(&rx_ring->buf[rx_ring_tail].skb, NULL);
		rd = (struct ipqess_edma_rx_desc *)skb->data;
		rx_ring_tail = IPQESS_EDMA_NEXT_IDX(rx_ring_tail,
						    IPQESS_EDMA_RX_RING_SIZE);

		/* Check if RRD is valid */
		if (!(rd->rrd7 & cpu_to_le16(IPQESS_EDMA_RRD_DESC_VALID))) {
			num_desc = 1;
			dev_kfree_skb_any(skb);
			goto skip;
		}

		num_desc = le16_to_cpu(rd->rrd1) & IPQESS_EDMA_RRD_NUM_RFD_MASK;
		length = le16_to_cpu(rd->rrd6) & IPQESS_EDMA_RRD_PKT_SIZE_MASK;

		/* rrd data should not be included in the packet data */
		skb_reserve(skb, IPQESS_EDMA_RRD_SIZE);
		if (num_desc > 1) {
			struct sk_buff *skb_prev = NULL;
			int size_remaining;
			int i;

			skb->data_len = 0;
			skb->tail += (IPQESS_EDMA_RX_HEAD_BUFF_SIZE
					- IPQESS_EDMA_RRD_SIZE);
			skb->len = length;
			skb->truesize = length;
			size_remaining =
				length - (IPQESS_EDMA_RX_HEAD_BUFF_SIZE
						- IPQESS_EDMA_RRD_SIZE);

			for (i = 1; i < num_desc; i++) {
				struct sk_buff *skb_temp =
					rx_ring->buf[rx_ring_tail].skb;

				dma_unmap_single(rx_ring->ppdev,
						 rx_ring->buf[rx_ring_tail].dma,
						 rx_ring->buf[rx_ring_tail].length,
						 DMA_FROM_DEVICE);

				skb_put(skb_temp,
					min(size_remaining, IPQESS_EDMA_RX_HEAD_BUFF_SIZE));
				if (skb_prev)
					skb_prev->next =
						rx_ring->buf[rx_ring_tail].skb;
				else
					skb_shinfo(skb)->frag_list =
						rx_ring->buf[rx_ring_tail].skb;
				skb_prev = rx_ring->buf[rx_ring_tail].skb;
				rx_ring->buf[rx_ring_tail].skb->next = NULL;

				skb->data_len += rx_ring->buf[rx_ring_tail].skb->len;
				size_remaining -= rx_ring->buf[rx_ring_tail].skb->len;

				rx_ring_tail =
					IPQESS_EDMA_NEXT_IDX(rx_ring_tail,
							     IPQESS_EDMA_RX_RING_SIZE);
			}

		} else {
			skb_put(skb, length);
		}

		skb->dev = rx_ring->edma->netdev;
		skb->protocol = eth_type_trans(skb, rx_ring->edma->netdev);
		skb_record_rx_queue(skb, rx_ring->ring_id);

		if (rd->rrd6 & cpu_to_le16(IPQESS_EDMA_RRD_CSUM_FAIL_MASK))
			skb_checksum_none_assert(skb);
		else
			skb->ip_summed = CHECKSUM_UNNECESSARY;

		if (rd->rrd7 & cpu_to_le16(IPQESS_EDMA_RRD_CVLAN))
			__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q),
					       le16_to_cpu(rd->rrd4));
		else if (rd->rrd1 & cpu_to_le16(IPQESS_EDMA_RRD_SVLAN))
			__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021AD),
					       le16_to_cpu(rd->rrd4));

		port_id = FIELD_GET(IPQESS_EDMA_RRD_PORT_ID_MASK,
				    le16_to_cpu(rd->rrd1));

		if (ipqess_edma_redirect(rx_ring, skb, port_id)) {
			dev_kfree_skb_any(skb);
			goto skip;
		}

		rx_ring->edma->stats.rx_packets++;
		rx_ring->edma->stats.rx_bytes += length;

		done++;
skip:
		num_desc = ipqess_edma_refill_rx_ring(rx_ring, num_desc);
	}

	ipqess_edma_w32(rx_ring->edma,
			IPQESS_EDMA_REG_RX_SW_CONS_IDX_Q(rx_ring->idx),
			rx_ring_tail);
	rx_ring->tail = rx_ring_tail;

	return done;
}

static int ipqess_edma_tx_complete(struct ipqess_edma_tx_ring *tx_ring,
				   int budget)
{
	int total = 0, ret;
	int done = 0;
	u32 tail;

	tail = ipqess_edma_r32(tx_ring->edma,
			       IPQESS_EDMA_REG_TPD_IDX_Q(tx_ring->idx));
	tail >>= IPQESS_EDMA_TPD_CONS_IDX_SHIFT;
	tail &= IPQESS_EDMA_TPD_CONS_IDX_MASK;

	do {
		ret = ipqess_edma_tx_unmap_and_free(&tx_ring->edma->pdev->dev,
						    &tx_ring->buf[tx_ring->tail]);
		tx_ring->tail = IPQESS_EDMA_NEXT_IDX(tx_ring->tail, tx_ring->count);

		total += ret;
	} while ((++done < budget) && (tx_ring->tail != tail));

	ipqess_edma_w32(tx_ring->edma,
			IPQESS_EDMA_REG_TX_SW_CONS_IDX_Q(tx_ring->idx),
			tx_ring->tail);

	if (netif_tx_queue_stopped(tx_ring->nq)) {
		netdev_dbg(tx_ring->edma->netdev, "waking up tx queue %d\n",
			   tx_ring->idx);
		netif_tx_wake_queue(tx_ring->nq);
	}

	netdev_tx_completed_queue(tx_ring->nq, done, total);

	return done;
}

static int ipqess_edma_tx_napi(struct napi_struct *napi, int budget)
{
	struct ipqess_edma_tx_ring *tx_ring =
		container_of(napi, struct ipqess_edma_tx_ring, napi_tx);
	int work_done = 0;
	u32 tx_status;

	tx_status = ipqess_edma_r32(tx_ring->edma, IPQESS_EDMA_REG_TX_ISR);
	tx_status &= BIT(tx_ring->idx);

	work_done = ipqess_edma_tx_complete(tx_ring, budget);

	ipqess_edma_w32(tx_ring->edma, IPQESS_EDMA_REG_TX_ISR, tx_status);

	if (likely(work_done < budget)) {
		if (napi_complete_done(napi, work_done))
			ipqess_edma_w32(tx_ring->edma,
					IPQESS_EDMA_REG_TX_INT_MASK_Q(tx_ring->idx),
					0x1);
	}

	return work_done;
}

static int ipqess_edma_rx_napi(struct napi_struct *napi, int budget)
{
	struct ipqess_edma_rx_ring *rx_ring =
		container_of(napi, struct ipqess_edma_rx_ring, napi_rx);
	struct ipqess_edma *edma = rx_ring->edma;
	u32 rx_mask = BIT(rx_ring->idx);
	int remaining_budget = budget;
	int rx_done;
	u32 status;

	do {
		ipqess_edma_w32(edma, IPQESS_EDMA_REG_RX_ISR, rx_mask);
		rx_done = ipqess_edma_rx_poll(rx_ring, remaining_budget);
		remaining_budget -= rx_done;

		status = ipqess_edma_r32(edma, IPQESS_EDMA_REG_RX_ISR);
	} while (remaining_budget > 0 && (status & rx_mask));

	if (remaining_budget <= 0)
		return budget;

	if (napi_complete_done(napi, budget - remaining_budget))
		ipqess_edma_w32(edma,
				IPQESS_EDMA_REG_RX_INT_MASK_Q(rx_ring->idx),
				0x1);

	return budget - remaining_budget;
}

static irqreturn_t ipqess_edma_interrupt_tx(int irq, void *priv)
{
	struct ipqess_edma_tx_ring *tx_ring =
		(struct ipqess_edma_tx_ring *)priv;

	if (likely(napi_schedule_prep(&tx_ring->napi_tx))) {
		__napi_schedule(&tx_ring->napi_tx);
		ipqess_edma_w32(tx_ring->edma,
				IPQESS_EDMA_REG_TX_INT_MASK_Q(tx_ring->idx),
				0x0);
	}

	return IRQ_HANDLED;
}

static irqreturn_t ipqess_edma_interrupt_rx(int irq, void *priv)
{
	struct ipqess_edma_rx_ring *rx_ring = (struct ipqess_edma_rx_ring *)priv;

	if (likely(napi_schedule_prep(&rx_ring->napi_rx))) {
		__napi_schedule(&rx_ring->napi_rx);
		ipqess_edma_w32(rx_ring->edma,
				IPQESS_EDMA_REG_RX_INT_MASK_Q(rx_ring->idx),
				0x0);
	}

	return IRQ_HANDLED;
}

static void ipqess_edma_irq_enable(struct ipqess_edma *edma)
{
	int i;

	ipqess_edma_w32(edma, IPQESS_EDMA_REG_RX_ISR, 0xff);
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_TX_ISR, 0xffff);
	for (i = 0; i < IPQESS_EDMA_NETDEV_QUEUES; i++) {
		ipqess_edma_w32(edma,
				IPQESS_EDMA_REG_RX_INT_MASK_Q(edma->rx_ring[i].idx),
				1);
		ipqess_edma_w32(edma,
				IPQESS_EDMA_REG_TX_INT_MASK_Q(edma->tx_ring[i].idx),
				1);
	}
}

static void ipqess_edma_irq_disable(struct ipqess_edma *edma)
{
	int i;

	for (i = 0; i < IPQESS_EDMA_NETDEV_QUEUES; i++) {
		ipqess_edma_w32(edma,
				IPQESS_EDMA_REG_RX_INT_MASK_Q(edma->rx_ring[i].idx),
				0);
		ipqess_edma_w32(edma,
				IPQESS_EDMA_REG_TX_INT_MASK_Q(edma->tx_ring[i].idx),
				0);
	}
}

static u16 ipqess_edma_tx_desc_available(struct ipqess_edma_tx_ring *tx_ring)
{
	u16 count = 0;

	if (tx_ring->tail <= tx_ring->head)
		count = IPQESS_EDMA_TX_RING_SIZE;

	count += tx_ring->tail - tx_ring->head - 1;

	return count;
}

static int ipqess_edma_cal_txd_req(struct sk_buff *skb)
{
	int tpds;

	/* one TPD for the header, and one for each fragments */
	tpds = 1 + skb_shinfo(skb)->nr_frags;
	if (skb_is_gso(skb) && skb_is_gso_v6(skb)) {
		/* for LSOv2 one extra TPD is needed */
		tpds++;
	}

	return tpds;
}

static struct ipqess_edma_buf *ipqess_edma_get_tx_buffer(struct ipqess_edma_tx_ring *tx_ring,
							 struct ipqess_edma_tx_desc *desc)
{
	return &tx_ring->buf[desc - tx_ring->desc];
}

static struct ipqess_edma_tx_desc *ipqess_edma_tx_desc_next(struct ipqess_edma_tx_ring *tx_ring)
{
	struct ipqess_edma_tx_desc *desc;

	desc = &tx_ring->desc[tx_ring->head];
	tx_ring->head = IPQESS_EDMA_NEXT_IDX(tx_ring->head, tx_ring->count);

	return desc;
}

static void ipqess_edma_rollback_tx(struct ipqess_edma *eth,
				    struct ipqess_edma_tx_desc *first_desc,
				    int ring_id)
{
	struct ipqess_edma_tx_ring *tx_ring = &eth->tx_ring[ring_id];
	struct ipqess_edma_tx_desc *desc = NULL;
	struct ipqess_edma_buf *buf;
	u16 start_index, index;

	start_index = first_desc - tx_ring->desc;

	index = start_index;
	while (index != tx_ring->head) {
		desc = &tx_ring->desc[index];
		buf = &tx_ring->buf[index];
		ipqess_edma_tx_unmap_and_free(&eth->pdev->dev, buf);
		memset(desc, 0, sizeof(*desc));
		if (++index == tx_ring->count)
			index = 0;
	}
	tx_ring->head = start_index;
}

static int ipqess_edma_tx_map_and_fill(struct ipqess_edma_tx_ring *tx_ring,
				       struct sk_buff *skb, int port_id)
{
	struct ipqess_edma_tx_desc *desc = NULL, *first_desc = NULL;
	u32 word1 = 0, word3 = 0, lso_word1 = 0, svlan_tag = 0;
	struct platform_device *pdev = tx_ring->edma->pdev;
	struct ipqess_edma_buf *buf = NULL;
	u16 len;
	int i;

	word3 |= port_id << IPQESS_EDMA_TPD_PORT_BITMAP_SHIFT;
	word3 |= BIT(IPQESS_EDMA_TPD_FROM_CPU_SHIFT);
	word3 |= 0x3e << IPQESS_EDMA_TPD_PORT_BITMAP_SHIFT;

	if (skb_is_gso(skb)) {
		if (skb_shinfo(skb)->gso_type & SKB_GSO_TCPV4) {
			lso_word1 |= IPQESS_EDMA_TPD_IPV4_EN;
			ip_hdr(skb)->check = 0;
			tcp_hdr(skb)->check = ~csum_tcpudp_magic(ip_hdr(skb)->saddr,
								 ip_hdr(skb)->daddr,
								 0, IPPROTO_TCP, 0);
		} else if (skb_shinfo(skb)->gso_type & SKB_GSO_TCPV6) {
			lso_word1 |= IPQESS_EDMA_TPD_LSO_V2_EN;
			ipv6_hdr(skb)->payload_len = 0;
			tcp_hdr(skb)->check = ~csum_ipv6_magic(&ipv6_hdr(skb)->saddr,
							       &ipv6_hdr(skb)->daddr,
							       0, IPPROTO_TCP, 0);
		}

		lso_word1 |= IPQESS_EDMA_TPD_LSO_EN |
					((skb_shinfo(skb)->gso_size & IPQESS_EDMA_TPD_MSS_MASK) <<
						IPQESS_EDMA_TPD_MSS_SHIFT) |
					(skb_transport_offset(skb) << IPQESS_EDMA_TPD_HDR_SHIFT);
	} else if (likely(skb->ip_summed == CHECKSUM_PARTIAL)) {
		u8 css, cso;

		cso = skb_checksum_start_offset(skb);
		css = cso + skb->csum_offset;

		word1 |= (IPQESS_EDMA_TPD_CUSTOM_CSUM_EN);
		word1 |= (cso >> 1) << IPQESS_EDMA_TPD_HDR_SHIFT;
		word1 |= ((css >> 1) << IPQESS_EDMA_TPD_CUSTOM_CSUM_SHIFT);
	}

	if (skb_vlan_tag_present(skb)) {
		switch (skb->vlan_proto) {
		case htons(ETH_P_8021Q):
			word3 |= BIT(IPQESS_EDMA_TX_INS_CVLAN);
			word3 |= skb_vlan_tag_get(skb) << IPQESS_EDMA_TX_CVLAN_TAG_SHIFT;
			break;
		case htons(ETH_P_8021AD):
			word1 |= BIT(IPQESS_EDMA_TX_INS_SVLAN);
			svlan_tag = skb_vlan_tag_get(skb);
			break;
		default:
			dev_err(&pdev->dev, "no ctag or stag present\n");
			goto vlan_tag_error;
		}
	}

	if (eth_type_vlan(skb->protocol))
		word1 |= IPQESS_EDMA_TPD_VLAN_TAGGED;

	if (skb->protocol == htons(ETH_P_PPP_SES))
		word1 |= IPQESS_EDMA_TPD_PPPOE_EN;

	len = skb_headlen(skb);

	first_desc = ipqess_edma_tx_desc_next(tx_ring);
	desc = first_desc;
	if (lso_word1 & IPQESS_EDMA_TPD_LSO_V2_EN) {
		desc->addr = cpu_to_le32(skb->len);
		desc->word1 = cpu_to_le32(word1 | lso_word1);
		desc->svlan_tag = cpu_to_le16(svlan_tag);
		desc->word3 = cpu_to_le32(word3);
		desc = ipqess_edma_tx_desc_next(tx_ring);
	}

	buf = ipqess_edma_get_tx_buffer(tx_ring, desc);
	buf->length = len;
	buf->dma = dma_map_single(&pdev->dev, skb->data, len, DMA_TO_DEVICE);

	if (dma_mapping_error(&pdev->dev, buf->dma))
		goto dma_error;

	desc->addr = cpu_to_le32(buf->dma);
	desc->len = cpu_to_le16(len);

	buf->flags |= IPQESS_EDMA_DESC_SINGLE;
	desc->word1 = cpu_to_le32(word1 | lso_word1);
	desc->svlan_tag = cpu_to_le16(svlan_tag);
	desc->word3 = cpu_to_le32(word3);

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

		len = skb_frag_size(frag);
		desc = ipqess_edma_tx_desc_next(tx_ring);
		buf = ipqess_edma_get_tx_buffer(tx_ring, desc);
		buf->length = len;
		buf->flags |= IPQESS_EDMA_DESC_PAGE;
		buf->dma = skb_frag_dma_map(&pdev->dev, frag, 0, len,
					    DMA_TO_DEVICE);

		if (dma_mapping_error(&pdev->dev, buf->dma))
			goto dma_error;

		desc->addr = cpu_to_le32(buf->dma);
		desc->len = cpu_to_le16(len);
		desc->svlan_tag = cpu_to_le16(svlan_tag);
		desc->word1 = cpu_to_le32(word1 | lso_word1);
		desc->word3 = cpu_to_le32(word3);
	}
	desc->word1 |= cpu_to_le32(1 << IPQESS_EDMA_TPD_EOP_SHIFT);
	buf->skb = skb;
	buf->flags |= IPQESS_EDMA_DESC_LAST;

	return 0;

dma_error:
	ipqess_edma_rollback_tx(tx_ring->edma, first_desc, tx_ring->ring_id);
	dev_err(&pdev->dev, "TX DMA map failed\n");

vlan_tag_error:
	return -ENOMEM;
}

static void ipqess_edma_kick_tx(struct ipqess_edma_tx_ring *tx_ring)
{
	/* Ensure that all TPDs has been written completely */
	dma_wmb();

	/* update software producer index */
	ipqess_edma_w32(tx_ring->edma, IPQESS_EDMA_REG_TPD_IDX_Q(tx_ring->idx),
			tx_ring->head);
}

netdev_tx_t ipqess_edma_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct ipqess_port *port = netdev_priv(netdev);
	struct ipqess_edma *edma = port->edma;
	struct ipqess_edma_tx_ring *tx_ring;
	int port_id = port->index;
	int tx_num;
	int avail;
	int ret;

	tx_ring = &edma->tx_ring[skb_get_queue_mapping(skb)];
	tx_num = ipqess_edma_cal_txd_req(skb);
	avail = ipqess_edma_tx_desc_available(tx_ring);
	if (avail < tx_num) {
		netdev_dbg(netdev,
			   "stopping tx queue %d, avail=%d req=%d im=%x\n",
			   tx_ring->idx, avail, tx_num,
			   ipqess_edma_r32(edma, IPQESS_EDMA_REG_TX_INT_MASK_Q(tx_ring->idx)));
		netif_tx_stop_queue(tx_ring->nq);
		ipqess_edma_w32(tx_ring->edma,
				IPQESS_EDMA_REG_TX_INT_MASK_Q(tx_ring->idx),
				0x1);
		ipqess_edma_kick_tx(tx_ring);
		return NETDEV_TX_BUSY;
	}

	ret = ipqess_edma_tx_map_and_fill(tx_ring, skb, port_id);
	if (ret) {
		dev_kfree_skb_any(skb);
		edma->stats.tx_errors++;
		return ret;
	}

	edma->stats.tx_packets++;
	edma->stats.tx_bytes += skb->len;
	netdev_tx_sent_queue(tx_ring->nq, skb->len);

	if (!netdev_xmit_more() || netif_xmit_stopped(tx_ring->nq))
		ipqess_edma_kick_tx(tx_ring);

	return NETDEV_TX_OK;
}
EXPORT_SYMBOL(ipqess_edma_xmit);

static void ipqess_edma_hw_stop(struct ipqess_edma *edma)
{
	int i;

	/* disable all RX queue IRQs */
	for (i = 0; i < IPQESS_EDMA_MAX_RX_QUEUE; i++)
		ipqess_edma_w32(edma, IPQESS_EDMA_REG_RX_INT_MASK_Q(i), 0);

	/* disable all TX queue IRQs */
	for (i = 0; i < IPQESS_EDMA_MAX_TX_QUEUE; i++)
		ipqess_edma_w32(edma, IPQESS_EDMA_REG_TX_INT_MASK_Q(i), 0);

	/* disable all other IRQs */
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_MISC_IMR, 0);
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_WOL_IMR, 0);

	/* clear the IRQ status registers */
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_RX_ISR, 0xff);
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_TX_ISR, 0xffff);
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_MISC_ISR, 0x1fff);
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_WOL_ISR, 0x1);
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_WOL_CTRL, 0);

	/* disable RX and TX queues */
	ipqess_edma_m32(edma, IPQESS_EDMA_RXQ_CTRL_EN_MASK, 0,
			IPQESS_EDMA_REG_RXQ_CTRL);
	ipqess_edma_m32(edma, IPQESS_EDMA_TXQ_CTRL_TXQ_EN, 0,
			IPQESS_EDMA_REG_TXQ_CTRL);
}

static int ipqess_edma_hw_init(struct ipqess_edma *edma)
{
	int i, err;
	u32 tmp;

	ipqess_edma_hw_stop(edma);

	ipqess_edma_m32(edma, BIT(IPQESS_EDMA_INTR_SW_IDX_W_TYP_SHIFT),
			IPQESS_EDMA_INTR_SW_IDX_W_TYPE
				<< IPQESS_EDMA_INTR_SW_IDX_W_TYP_SHIFT,
			IPQESS_EDMA_REG_INTR_CTRL);

	/* enable IRQ delay slot */
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_IRQ_MODRT_TIMER_INIT,
			(IPQESS_EDMA_TX_IMT
				<< IPQESS_EDMA_IRQ_MODRT_TX_TIMER_SHIFT) |
			(IPQESS_EDMA_RX_IMT
				<< IPQESS_EDMA_IRQ_MODRT_RX_TIMER_SHIFT));

	/* Set Customer and Service VLAN TPIDs */
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_VLAN_CFG,
			(ETH_P_8021Q << IPQESS_EDMA_VLAN_CFG_CVLAN_TPID_SHIFT)
			| (ETH_P_8021AD << IPQESS_EDMA_VLAN_CFG_SVLAN_TPID_SHIFT));

	/* Configure the TX Queue bursting */
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_TXQ_CTRL,
			(IPQESS_EDMA_TPD_BURST << IPQESS_EDMA_TXQ_NUM_TPD_BURST_SHIFT)
			| (IPQESS_EDMA_TXF_BURST << IPQESS_EDMA_TXQ_TXF_BURST_NUM_SHIFT)
			| IPQESS_EDMA_TXQ_CTRL_TPD_BURST_EN);

	/* Set RSS type */
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_RSS_TYPE,
			IPQESS_EDMA_RSS_TYPE_IPV4TCP
			| IPQESS_EDMA_RSS_TYPE_IPV6_TCP
			| IPQESS_EDMA_RSS_TYPE_IPV4_UDP
			| IPQESS_EDMA_RSS_TYPE_IPV6UDP
			| IPQESS_EDMA_RSS_TYPE_IPV4
			| IPQESS_EDMA_RSS_TYPE_IPV6);

	/* Set RFD ring burst and threshold */
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_RX_DESC1,
			(IPQESS_EDMA_RFD_BURST << IPQESS_EDMA_RXQ_RFD_BURST_NUM_SHIFT)
			| (IPQESS_EDMA_RFD_THR << IPQESS_EDMA_RXQ_RFD_PF_THRESH_SHIFT)
			| (IPQESS_EDMA_RFD_LTHR << IPQESS_EDMA_RXQ_RFD_LOW_THRESH_SHIFT));

	/* Set Rx FIFO
	 * - threshold to start to DMA data to host
	 */
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_RXQ_CTRL,
			IPQESS_EDMA_FIFO_THRESH_128_BYTE
			| IPQESS_EDMA_RXQ_CTRL_RMV_VLAN);

	err = ipqess_edma_rx_ring_alloc(edma);
	if (err)
		return err;

	err = ipqess_edma_tx_ring_alloc(edma);
	if (err)
		goto err_rx_ring_free;

	/* Load all of ring base address above into the dma engine */
	ipqess_edma_m32(edma, 0, BIT(IPQESS_EDMA_LOAD_PTR_SHIFT),
			IPQESS_EDMA_REG_TX_SRAM_PART);

	/* Disable TX FIFO low watermark and high watermark */
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_TXF_WATER_MARK, 0);

	/* Configure RSS indirection table.
	 * 128 hash will be configured in the following
	 * pattern: hash{0,1,2,3} = {Q0,Q2,Q4,Q6} respectively
	 * and so on
	 */
	for (i = 0; i < IPQESS_EDMA_NUM_IDT; i++)
		ipqess_edma_w32(edma, IPQESS_EDMA_REG_RSS_IDT(i),
				IPQESS_EDMA_RSS_IDT_VALUE);

	/* Configure load balance mapping table.
	 * 4 table entry will be configured according to the
	 * following pattern: load_balance{0,1,2,3} = {Q0,Q1,Q3,Q4}
	 * respectively.
	 */
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_LB_RING, IPQESS_EDMA_LB_REG_VALUE);

	/* Configure Virtual queue for Tx rings */
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_VQ_CTRL0, IPQESS_EDMA_VQ_REG_VALUE);
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_VQ_CTRL1, IPQESS_EDMA_VQ_REG_VALUE);

	/* Configure Max AXI Burst write size to 128 bytes*/
	ipqess_edma_w32(edma, IPQESS_EDMA_REG_AXIW_CTRL_MAXWRSIZE,
			IPQESS_EDMA_AXIW_MAXWRSIZE_VALUE);

	/* Enable TX queues */
	ipqess_edma_m32(edma, 0, IPQESS_EDMA_TXQ_CTRL_TXQ_EN,
			IPQESS_EDMA_REG_TXQ_CTRL);

	/* Enable RX queues */
	tmp = 0;
	for (i = 0; i < IPQESS_EDMA_NETDEV_QUEUES; i++)
		tmp |= IPQESS_EDMA_RXQ_CTRL_EN(edma->rx_ring[i].idx);

	ipqess_edma_m32(edma, IPQESS_EDMA_RXQ_CTRL_EN_MASK, tmp,
			IPQESS_EDMA_REG_RXQ_CTRL);

	return 0;

err_rx_ring_free:

	ipqess_edma_rx_ring_free(edma);
	return err;
}

static void ipqess_edma_reset(struct ipqess_edma *edma)
{
	reset_control_assert(edma->edma_rst);

	mdelay(10);

	reset_control_deassert(edma->edma_rst);

	/* Waiting for all inner tables to be flushed and reinitialized.
	 * This takes between 5 and 10 ms
	 */

	mdelay(10);
}

int ipqess_edma_init(struct platform_device *pdev, struct device_node *np)
{
	struct ipqess_switch *sw = platform_get_drvdata(pdev);
	struct net_device *netdev;
	struct ipqess_edma *edma;
	struct ipqess_port *port;
	int i, err = 0;
	int qid;

	edma = devm_kzalloc(&pdev->dev, sizeof(*edma), GFP_KERNEL);
	if (!edma)
		return -ENOMEM;

	edma->pdev = pdev;

	spin_lock_init(&edma->stats_lock);

	edma->hw_addr = devm_platform_ioremap_resource_byname(pdev, "edma");
	if (IS_ERR(edma->hw_addr)) {
		err = PTR_ERR(edma->hw_addr);
		goto err_edma;
	}

	edma->edma_clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(edma->edma_clk)) {
		err = PTR_ERR(edma->edma_clk);
		goto err_edma;
	}

	err = clk_prepare_enable(edma->edma_clk);
	if (err)
		goto err_edma;

	edma->edma_rst = devm_reset_control_get(&pdev->dev, "ess");
	if (IS_ERR(edma->edma_rst)) {
		err = PTR_ERR(edma->edma_rst);
		goto err_clk;
	}

	ipqess_edma_reset(edma);

	for (i = 0; i < IPQESS_EDMA_MAX_TX_QUEUE; i++) {
		edma->tx_irq[i] = platform_get_irq(pdev, i);
		scnprintf(edma->tx_irq_names[i], sizeof(edma->tx_irq_names[i]),
			  "%s:txq%d", pdev->name, i);
	}

	for (i = 0; i < IPQESS_EDMA_MAX_RX_QUEUE; i++) {
		edma->rx_irq[i] = platform_get_irq(pdev,
						   i + IPQESS_EDMA_MAX_TX_QUEUE);
		scnprintf(edma->rx_irq_names[i], sizeof(edma->rx_irq_names[i]),
			  "%s:rxq%d", pdev->name, i);
	}

	netdev = sw->napi_leader;
	sw->edma = edma;
	edma->sw = sw;
	edma->netdev = netdev;

	err = ipqess_edma_hw_init(edma);
	if (err)
		goto err_clk;

	for (i = 0; i < IPQESS_EDMA_NETDEV_QUEUES; i++) {
		netif_napi_add_tx(netdev, &edma->tx_ring[i].napi_tx,
				  ipqess_edma_tx_napi);
		netif_napi_add(netdev, &edma->rx_ring[i].napi_rx,
			       ipqess_edma_rx_napi);
	}

	for (i = 0; i < IPQESS_EDMA_NETDEV_QUEUES; i++) {
		qid = edma->tx_ring[i].idx;
		err = devm_request_irq(&netdev->dev, edma->tx_irq[qid],
				       ipqess_edma_interrupt_tx, 0,
				       edma->tx_irq_names[qid],
				       &edma->tx_ring[i]);
		if (err)
			goto err_clk;

		qid = edma->rx_ring[i].idx;
		err = devm_request_irq(&netdev->dev, edma->rx_irq[qid],
				       ipqess_edma_interrupt_rx, 0,
				       edma->rx_irq_names[qid],
				       &edma->rx_ring[i]);
		if (err)
			goto err_clk;

		napi_enable(&edma->tx_ring[i].napi_tx);
		napi_enable(&edma->rx_ring[i].napi_rx);
	}

	ipqess_edma_irq_enable(edma);
	netif_tx_start_all_queues(netdev);

	if (err)
		goto err_hw_stop;

	for (i = 0; i < IPQESS_SWITCH_MAX_PORTS; i++) {
		port = sw->port_list[i];
		if (port)
			port->edma = edma;
	}

	return 0;

err_hw_stop:
	ipqess_edma_hw_stop(edma);

	ipqess_edma_tx_ring_free(edma);
	ipqess_edma_rx_ring_free(edma);
err_clk:
	clk_disable_unprepare(edma->edma_clk);
err_edma:
	devm_kfree(&pdev->dev, edma);

	return err;
}

void ipqess_edma_uninit(struct ipqess_edma *edma)
{
	struct qca8k_priv *priv = edma->sw->priv;
	u32 val;

	ipqess_edma_irq_disable(edma);
	ipqess_edma_hw_stop(edma);

	ipqess_edma_tx_ring_free(edma);
	ipqess_edma_rx_ring_free(edma);

	/* This register read fixes a bug where
	 * the switch ID is incorrect at the next probe.
	 * The source of this issue is unknown.
	 */
	qca8k_read(priv, QCA8K_REG_MASK_CTRL, &val);

	clk_disable_unprepare(edma->edma_clk);
}
