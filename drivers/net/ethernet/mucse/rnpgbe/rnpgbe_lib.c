// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2020 - 2025 Mucse Corporation. */

#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/vmalloc.h>
#include <net/page_pool/helpers.h>

#include "rnpgbe_lib.h"
#include "rnpgbe.h"
#include "rnpgbe_mbx_fw.h"

/**
 * rnpgbe_msix_other - Other irq handler
 * @irq: irq num
 * @data: private data
 *
 * @return: IRQ_HANDLED
 **/
static irqreturn_t rnpgbe_msix_other(int irq, void *data)
{
	struct mucse *mucse = (struct mucse *)data;

	mucse_fw_irq_handler(&mucse->hw);

	return IRQ_HANDLED;
}

static void rnpgbe_irq_disable_queues(struct mucse_q_vector *q_vector)
{
	struct mucse_hw *hw = &q_vector->mucse->hw;
	struct mucse_ring *ring;

	/* tx/rx use one register, different bit */
	mucse_for_each_ring(ring, q_vector->tx) {
		writel(INT_VALID, ring->trig);
		writel((RX_INT_MASK | TX_INT_MASK), ring->irq_mask);
	}
	/* flush posted writes to ensure hardware sees the mask */
	readl(hw->hw_addr);
}

static void rnpgbe_irq_enable_queues(struct mucse_q_vector *q_vector)
{
	struct mucse_ring *ring;

	/* tx/rx use one register, different bit */
	mucse_for_each_ring(ring, q_vector->tx) {
		writel(0, ring->irq_mask);
		writel(INT_VALID | TX_INT_MASK | RX_INT_MASK, ring->trig);
	}
}

/**
 * rnpgbe_clean_tx_irq - Reclaim resources after transmit completes
 * @q_vector: structure containing interrupt and ring information
 * @tx_ring: tx ring to clean
 * @napi_budget: Used to determine if we are in netpoll
 *
 * @return: true is for work done within budget, otherwise false
 **/
static bool rnpgbe_clean_tx_irq(struct mucse_q_vector *q_vector,
				struct mucse_ring *tx_ring,
				int napi_budget)
{
	int budget = q_vector->mucse->tx_work_limit;
	u64 total_bytes = 0, total_packets = 0;
	struct mucse_tx_buffer *tx_buffer;
	struct rnpgbe_tx_desc *tx_desc;
	int i = tx_ring->next_to_clean;

	tx_buffer = &tx_ring->tx_buffer_info[i];
	tx_desc = M_TX_DESC(tx_ring, i);
	i -= tx_ring->count;

	do {
		struct rnpgbe_tx_desc *eop_desc = tx_buffer->next_to_watch;

		/* if next_to_watch is not set then there is no work pending */
		if (!eop_desc)
			break;

		/* prevent any other reads prior to eop_desc */
		rmb();

		/* if eop DD is not set pending work has not been completed */
		if (!(eop_desc->vlan_cmd & cpu_to_le32(M_TXD_STAT_DD)))
			break;
		/* clear next_to_watch to prevent false hangs */
		tx_buffer->next_to_watch = NULL;
		total_bytes += tx_buffer->bytecount;
		total_packets += tx_buffer->gso_segs;
		napi_consume_skb(tx_buffer->skb, napi_budget);
		if (tx_buffer->mapped_as_page) {
			dma_unmap_page(tx_ring->dev,
				       dma_unmap_addr(tx_buffer, dma),
				       dma_unmap_len(tx_buffer, len),
				       DMA_TO_DEVICE);
		} else {
			dma_unmap_single(tx_ring->dev,
					 dma_unmap_addr(tx_buffer, dma),
					 dma_unmap_len(tx_buffer, len),
					 DMA_TO_DEVICE);
		}
		tx_buffer->skb = NULL;
		dma_unmap_len_set(tx_buffer, len, 0);

		/* unmap remaining buffers */
		while (tx_desc != eop_desc) {
			tx_buffer++;
			tx_desc++;
			i++;
			if (unlikely(!i)) {
				i -= tx_ring->count;
				tx_buffer = tx_ring->tx_buffer_info;
				tx_desc = M_TX_DESC(tx_ring, 0);
			}

			/* unmap any remaining paged data */
			if (dma_unmap_len(tx_buffer, len)) {
				dma_unmap_page(tx_ring->dev,
					       dma_unmap_addr(tx_buffer, dma),
					       dma_unmap_len(tx_buffer, len),
					       DMA_TO_DEVICE);
				dma_unmap_len_set(tx_buffer, len, 0);
			}
		}

		/* move us one more past the eop_desc for start of next pkt */
		tx_buffer++;
		tx_desc++;
		i++;
		if (unlikely(!i)) {
			i -= tx_ring->count;
			tx_buffer = tx_ring->tx_buffer_info;
			tx_desc = M_TX_DESC(tx_ring, 0);
		}

		prefetch(tx_desc);
		budget--;
	} while (likely(budget > 0));
	netdev_tx_completed_queue(txring_txq(tx_ring), total_packets,
				  total_bytes);
	i += tx_ring->count;
	tx_ring->next_to_clean = i;
	u64_stats_update_begin(&tx_ring->syncp);
	tx_ring->stats.bytes += total_bytes;
	tx_ring->stats.packets += total_packets;
	u64_stats_update_end(&tx_ring->syncp);

#define TX_WAKE_THRESHOLD (DESC_NEEDED * 2)
	if (likely(netif_carrier_ok(tx_ring->netdev) &&
		   (mucse_desc_unused(tx_ring) >= TX_WAKE_THRESHOLD))) {
		/* Make sure that anybody stopping the queue after this
		 * sees the new next_to_clean.
		 */
		smp_mb();
		if (__netif_subqueue_stopped(tx_ring->netdev,
					     tx_ring->queue_index)) {
			netif_wake_subqueue(tx_ring->netdev,
					    tx_ring->queue_index);
		}
	}

	return !!budget;
}

static bool mucse_alloc_mapped_page(struct mucse_ring *rx_ring,
				    struct mucse_rx_buffer *bi)
{
	struct page *page = bi->page;
	dma_addr_t dma;

	if (page) {
		/* Buffer is being reused without going back through the
		 * page_pool. Do dma_sync for hw use.
		 */
		dma_sync_single_range_for_device(rx_ring->dev, bi->dma,
						 bi->page_offset,
						 PAGE_SIZE - bi->page_offset,
						 DMA_FROM_DEVICE);
		return true;
	}

	page = page_pool_dev_alloc_pages(rx_ring->page_pool);
	if (unlikely(!page))
		return false;
	dma = page_pool_get_dma_addr(page);

	bi->dma = dma;
	bi->page = page;
	bi->page_offset = RNPGBE_SKB_PAD;

	return true;
}

static void mucse_update_rx_tail(struct mucse_ring *rx_ring,
				 u32 val)
{
	rx_ring->next_to_use = val;
	/*
	 * Force memory writes to complete before letting h/w
	 * know there are new descriptors to fetch.  (Only
	 * applicable for weak-ordered memory model archs,
	 * such as IA-64).
	 */
	wmb();
	writel(val, rx_ring->tail);
}

/**
 * rnpgbe_alloc_rx_buffers - Replace used receive buffers
 * @rx_ring: ring to place buffers on
 * @cleaned_count: number of buffers to replace
 * @return: true if alloc failed
 **/
static bool rnpgbe_alloc_rx_buffers(struct mucse_ring *rx_ring,
				    u16 cleaned_count)
{
	u64 fun_id = ((u64)(rx_ring->pfvfnum) << 56);
	union rnpgbe_rx_desc *rx_desc;
	u16 i = rx_ring->next_to_use;
	struct mucse_rx_buffer *bi;
	bool err = false;
	u64 addr;
	/* nothing to do */
	if (!cleaned_count)
		return err;

	rx_desc = M_RX_DESC(rx_ring, i);
	bi = &rx_ring->rx_buffer_info[i];
	i -= rx_ring->count;

	do {
		if (!mucse_alloc_mapped_page(rx_ring, bi)) {
			err = true;
			break;
		}

		addr = (u64)(bi->dma + bi->page_offset);
		rx_desc->pkt_addr = cpu_to_le64(addr | fun_id);
		/* clean dd */
		rx_desc->resv_cmd = 0;
		rx_desc++;
		bi++;
		i++;
		if (unlikely(!i)) {
			rx_desc = M_RX_DESC(rx_ring, 0);
			bi = rx_ring->rx_buffer_info;
			i -= rx_ring->count;
		}
		cleaned_count--;
	} while (cleaned_count);

	i += rx_ring->count;

	if (rx_ring->next_to_use != i) {
		/*
		 * Force memory writes to complete before letting h/w know
		 * there are new rx descriptors to fetch.  (Only applicable
		 * for weak-ordered memory model archs, such as IA-64).
		 */
		dma_wmb();
		/* Notify hw new rx descriptors is ready */
		mucse_update_rx_tail(rx_ring, i);
	}

	return err;
}

/**
 * rnpgbe_get_buffer - Get the rx_buffer to be used
 * @rx_ring: pointer to rx ring
 * @skb: pointer skb for this packet
 * @size: data size in this desc
 * @return: rx_buffer.
 **/
static struct mucse_rx_buffer *rnpgbe_get_buffer(struct mucse_ring *rx_ring,
						 struct sk_buff **skb,
						 const unsigned int size)
{
	struct mucse_rx_buffer *rx_buffer;

	rx_buffer = &rx_ring->rx_buffer_info[rx_ring->next_to_clean];
	*skb = rx_buffer->skb;
	prefetchw(page_address(rx_buffer->page) + rx_buffer->page_offset);
	/* we are reusing so sync this buffer for CPU use */
	dma_sync_single_range_for_cpu(rx_ring->dev, rx_buffer->dma,
				      rx_buffer->page_offset, size,
				      DMA_FROM_DEVICE);

	return rx_buffer;
}

/**
 * rnpgbe_add_rx_frag - Add no-linear data to the skb
 * @rx_buffer: pointer to rx_buffer
 * @skb: pointer skb for this packet
 * @size: data size in this desc
 **/
static void rnpgbe_add_rx_frag(struct mucse_rx_buffer *rx_buffer,
			       struct sk_buff *skb,
			       unsigned int size)
{
	/* FIXME: truesize is PAGE_SIZE for 1 page = 1 descriptor.
	 * Optimize with page splitting later when refactoring
	 * the Rx buffer management.
	 */
	unsigned int truesize = PAGE_SIZE;

	skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags, rx_buffer->page,
			rx_buffer->page_offset, size, truesize);
}

/**
 * rnpgbe_build_skb - Try to build a sbk based on rx_buffer
 * @rx_buffer: pointer to rx_buffer
 * @size: data size in this desc
 * @return: skb for this rx_buffer
 **/
static struct sk_buff *rnpgbe_build_skb(struct mucse_rx_buffer *rx_buffer,
					unsigned int size)
{
	void *va = page_address(rx_buffer->page) + rx_buffer->page_offset;
	unsigned int truesize = PAGE_SIZE;
	struct sk_buff *skb;

	net_prefetch(va);
	/* build an skb around the page buffer */
	skb = build_skb(va - RNPGBE_SKB_PAD, truesize);
	if (unlikely(!skb))
		return NULL;
	/* update pointers within the skb to store the data */
	skb_reserve(skb, RNPGBE_SKB_PAD);
	__skb_put(skb, size);
	skb_mark_for_recycle(skb);

	return skb;
}

/**
 * rnpgbe_pull_tail - Pull header to linear portion of buffer
 * @skb: current socket buffer containing buffer in progress
 **/
static void rnpgbe_pull_tail(struct sk_buff *skb)
{
	skb_frag_t *frag = &skb_shinfo(skb)->frags[0];
	unsigned int pull_len;
	unsigned char *va;

	va = skb_frag_address(frag);
	pull_len = eth_get_headlen(skb->dev, va, M_RX_HDR_SIZE);
	/* align pull length to size of long to optimize memcpy performance */
	skb_copy_to_linear_data(skb, va, ALIGN(pull_len, sizeof(long)));
	/* update all of the pointers */
	skb_frag_size_sub(frag, pull_len);
	skb_frag_off_add(frag, pull_len);
	skb->data_len -= pull_len;
	skb->tail += pull_len;
}

/**
 * rnpgbe_is_non_eop - Process handling of non-EOP buffers
 * @rx_ring: rx ring being processed
 * @rx_desc: rx descriptor for current buffer
 * @skb: current socket buffer containing buffer in progress
 *
 * This function updates next to clean.  If the buffer is an EOP buffer
 * this function exits returning false, otherwise it will place the
 * sk_buff in the next buffer to be chained and return true indicating
 * that this is in fact a non-EOP buffer.
 *
 * @return: true for not end of packet
 **/
static bool rnpgbe_is_non_eop(struct mucse_ring *rx_ring,
			      union rnpgbe_rx_desc *rx_desc,
			      struct sk_buff *skb)
{
	u32 ntc = rx_ring->next_to_clean + 1;

	/* fetch, update, and store next to clean */
	ntc = (ntc < rx_ring->count) ? ntc : 0;
	rx_ring->next_to_clean = ntc;
	prefetch(M_RX_DESC(rx_ring, ntc));
	/* if we are the last buffer then there is nothing else to do */
	if (likely(rnpgbe_test_staterr(rx_desc, M_RXD_STAT_EOP)))
		return false;
	if (skb_shinfo(skb)->nr_frags < MAX_SKB_FRAGS) {
		/* place skb in next buffer to be received */
		rx_ring->rx_buffer_info[ntc].skb = skb;
	} else {
		atomic64_inc(&rx_ring->stats.dropped);
		/* too much frags, force free */
		dev_kfree_skb_any(skb);
		rx_ring->drop_status = true;
	}
	/* we should clean it since we used all info in it */
	rx_desc->wb.cmd = 0;

	return true;
}

/**
 * rnpgbe_cleanup_headers - Correct corrupted or empty headers
 * @skb: current socket buffer containing buffer in progress
 * @return: true if an error was encountered and skb was freed.
 **/
static bool rnpgbe_cleanup_headers(struct sk_buff *skb)
{
	/* place header in linear portion of buffer */
	if (!skb_headlen(skb))
		rnpgbe_pull_tail(skb);
	/* if eth_skb_pad returns an error the skb was freed */
	if (eth_skb_pad(skb))
		return true;

	return false;
}

/**
 * rnpgbe_process_skb_fields - Setup skb header fields from desc
 * @rx_ring: structure containing ring specific data
 * @skb: skb currently being received and modified
 *
 * rnpgbe_process_skb_fields checks the ring, descriptor information
 * in order to setup the hash, chksum, vlan, protocol, and other
 * fields within the skb.
 **/
static void rnpgbe_process_skb_fields(struct mucse_ring *rx_ring,
				      struct sk_buff *skb)
{
	struct net_device *dev = rx_ring->netdev;

	skb_record_rx_queue(skb, rx_ring->queue_index);
	skb->protocol = eth_type_trans(skb, dev);
}

/**
 * rnpgbe_clean_rx_irq - Clean completed descriptors from Rx ring
 * @q_vector: structure containing interrupt and ring information
 * @rx_ring: rx descriptor ring to transact packets on
 * @budget: total limit on number of packets to process
 *
 * rnpgbe_clean_rx_irq tries to check dd in desc, handle this desc
 * if dd is set which means data is write-back by hw
 *
 * @return: amount of work completed.
 **/
static int rnpgbe_clean_rx_irq(struct mucse_q_vector *q_vector,
			       struct mucse_ring *rx_ring,
			       int budget)
{
	unsigned int total_rx_bytes = 0, total_rx_packets = 0;
	u16 cleaned_count = mucse_desc_unused_rx(rx_ring);
	bool fail_alloc = false;

	while (likely(total_rx_packets < budget)) {
		struct mucse_rx_buffer *rx_buffer;
		union rnpgbe_rx_desc *rx_desc;
		struct sk_buff *skb;
		unsigned int size;

		if (cleaned_count >= M_RX_BUFFER_WRITE) {
			if (rnpgbe_alloc_rx_buffers(rx_ring, cleaned_count)) {
				fail_alloc = true;
				cleaned_count = mucse_desc_unused_rx(rx_ring);
			} else {
				cleaned_count = 0;
			}
		}
		rx_desc = M_RX_DESC(rx_ring, rx_ring->next_to_clean);

		if (!rnpgbe_test_staterr(rx_desc, M_RXD_STAT_DD))
			break;

		/* This memory barrier is needed to keep us from reading
		 * any other fields out of the rx_desc until we know the
		 * descriptor has been written back
		 */
		dma_rmb();
		/* Hardware enfrces: minimum 33-bytes descriptor(no 1-13 byte
		 * size), multi-descriptors only for jumbo frames > 1536 bytes
		 * (controlled by M_DEFAULT_SG=96, each descriptor no more than
		 * 1536 bytes). Small packets use single descriptor.
		 */
		size = le16_to_cpu(rx_desc->wb.len);

		if (unlikely(rx_ring->drop_status)) {
			cleaned_count++;
			/* drop data until eop */
			if (rnpgbe_test_staterr(rx_desc, M_RXD_STAT_EOP))
				rx_ring->drop_status = false;

			rx_desc->wb.cmd = 0;
			rx_ring->next_to_clean++;
			if (rx_ring->next_to_clean >= rx_ring->count)
				rx_ring->next_to_clean = 0;
			continue;
		}

		/* TODO: hardware error checks (wb.status: crc_err, etc.) in
		 * the feature. For now the basic RX path relies on the stack
		 * to drop malformed packets naturally.
		 */

		rx_buffer = rnpgbe_get_buffer(rx_ring, &skb, size);

		if (skb)
			rnpgbe_add_rx_frag(rx_buffer, skb, size);
		else
			skb = rnpgbe_build_skb(rx_buffer, size);

		if (!skb) {
			/* drop until eop if multiple descriptors */
			if (!(rnpgbe_test_staterr(rx_desc, M_RXD_STAT_EOP)))
				rx_ring->drop_status = true;

			rx_desc->wb.cmd = 0;
			rx_ring->next_to_clean++;
			atomic64_inc(&rx_ring->stats.dropped);
			if (rx_ring->next_to_clean >= rx_ring->count)
				rx_ring->next_to_clean = 0;
			break;
		}

		rx_buffer->page = NULL;
		rx_buffer->skb = NULL;
		cleaned_count++;

		if (rnpgbe_is_non_eop(rx_ring, rx_desc, skb))
			continue;

		/* verify the packet layout is correct */
		if (rnpgbe_cleanup_headers(skb)) {
			/* we should clean it since we used all info in it */
			atomic64_inc(&rx_ring->stats.dropped);
			rx_desc->wb.cmd = 0;
			continue;
		}

		/* probably a little skewed due to removing CRC */
		total_rx_bytes += skb->len;
		rnpgbe_process_skb_fields(rx_ring, skb);
		rx_desc->wb.cmd = 0;
		napi_gro_receive(&q_vector->napi, skb);
		/* update budget accounting */
		total_rx_packets++;
	}

	u64_stats_update_begin(&rx_ring->syncp);
	rx_ring->stats.packets += total_rx_packets;
	rx_ring->stats.bytes += total_rx_bytes;
	u64_stats_update_end(&rx_ring->syncp);
	/* keep polling if alloc mem failed */
	return fail_alloc ? budget : total_rx_packets;
}

/**
 * rnpgbe_poll - NAPI Rx polling callback
 * @napi: structure for representing this polling device
 * @budget: how many packets driver is allowed to clean
 *
 * @return: work done in this call
 * This function is used for legacy and MSI, NAPI mode
 **/
static int rnpgbe_poll(struct napi_struct *napi, int budget)
{
	struct mucse_q_vector *q_vector =
		container_of(napi, struct mucse_q_vector, napi);
	bool clean_complete = true;
	struct mucse_ring *ring;
	int per_ring_budget;
	int work_done = 0;

	mucse_for_each_ring(ring, q_vector->tx) {
		if (!rnpgbe_clean_tx_irq(q_vector, ring, budget))
			clean_complete = false;
	}

	/* Exit if we are called by netpoll */
	if (unlikely(!budget))
		return 0;

	if (q_vector->rx.count > 1)
		per_ring_budget = max(budget / q_vector->rx.count, 1);
	else
		per_ring_budget = budget;

	mucse_for_each_ring(ring, q_vector->rx) {
		int cleaned = 0;

		cleaned = rnpgbe_clean_rx_irq(q_vector, ring, per_ring_budget);
		work_done += cleaned;
		if (cleaned >= per_ring_budget)
			clean_complete = false;
	}

	if (test_bit(__MUCSE_DOWN, &q_vector->mucse->state))
		clean_complete = true;

	if (!clean_complete)
		return budget;

	if (likely(napi_complete_done(napi, work_done))) {
		if (!test_bit(__MUCSE_DOWN, &q_vector->mucse->state))
			rnpgbe_irq_enable_queues(q_vector);
	}

	return work_done;
}

/**
 * register_mbx_irq - Register mbx routine
 * @mucse: pointer to private structure
 *
 * In MSIX mode, register a dedicated handler for vector 0 (mailbox)
 * In MSI/MSI-X_SINGLE mode, mailbox is multiplexed through
 * data tx/rx handler.
 *
 * @return: 0 on success, negative on failure
 **/
int register_mbx_irq(struct mucse *mucse)
{
	struct pci_dev *pdev = mucse->pdev;
	struct mucse_hw *hw = &mucse->hw;
	int err = 0;

	snprintf(mucse->mbx_name, sizeof(mucse->mbx_name),
		 "rnpgbe-mbx:%s", pci_name(pdev));

	if (mucse->flags & M_FLAG_MSIX_EN) {
		err = request_irq(pci_irq_vector(pdev, 0),
				  rnpgbe_msix_other, 0, mucse->mbx_name,
				  mucse);
		if (!err)
			hw->mbx.irq_en = true;
	}

	return err;
}

/**
 * remove_mbx_irq - Remove mbx routine
 * @mucse: pointer to private structure
 **/
void remove_mbx_irq(struct mucse *mucse)
{
	struct pci_dev *pdev = mucse->pdev;
	struct mucse_hw *hw = &mucse->hw;

	if (mucse->flags & M_FLAG_MSIX_EN) {
		free_irq(pci_irq_vector(pdev, 0), mucse);
		hw->mbx.irq_en = false;
	}
}

/**
 * rnpgbe_set_num_queues - Allocate queues for device, feature dependent
 * @mucse: pointer to private structure
 *
 * Determine tx/rx queue nums
 **/
static void rnpgbe_set_num_queues(struct mucse *mucse)
{
	/* start from 1 queue */
	mucse->num_tx_queues = 1;
	mucse->num_rx_queues = 1;
}

/**
 * rnpgbe_set_interrupt_capability - Set MSI-X or MSI if supported
 * @mucse: pointer to private structure
 *
 * Attempt to configure the interrupts using the best available
 * capabilities of the hardware.
 *
 * @return: 0 on success, negative on failure
 **/
static int rnpgbe_set_interrupt_capability(struct mucse *mucse)
{
	int v_budget;

	v_budget = min3(mucse->num_tx_queues, mucse->num_rx_queues,
			MAX_Q_VECTORS);
	v_budget = min_t(int, v_budget, num_online_cpus());
	/* add one vector for mbx */
	v_budget += 1;

	/* Hardware limitation: only 1 MSI vector is supported even
	 * if multiple messages are requested. MSI mode falls back
	 * to single vector automatically.
	 */
	v_budget = pci_alloc_irq_vectors(mucse->pdev, 1, v_budget,
					 PCI_IRQ_MSI | PCI_IRQ_MSIX);
	if (v_budget < 0)
		return v_budget;

	if (mucse->pdev->msix_enabled) {
		/* q_vector not include mbx */
		if (v_budget > 1) {
			mucse->flags |= M_FLAG_MSIX_EN;
			mucse->num_q_vectors = v_budget - 1;
		} else {
			mucse->flags |= M_FLAG_MSIX_SINGLE_EN;
			mucse->num_q_vectors = 1;
		}
	} else {
		/* hw only support 1 msi irq */
		mucse->num_q_vectors = 1;
		mucse->flags |= M_FLAG_MSI_EN;
	}

	return 0;
}

/**
 * mucse_add_ring - Add ring to ring container
 * @ring: ring to be added
 * @head: ring container
 **/
static void mucse_add_ring(struct mucse_ring *ring,
			   struct mucse_ring_container *head)
{
	ring->next = head->ring;
	head->ring = ring;
	head->count++;
}

/**
 * rnpgbe_alloc_q_vector - Allocate memory for a single interrupt vector
 * @mucse: pointer to private structure
 * @eth_queue_idx: queue_index idx for this q_vector
 * @v_idx: index of vector used for this q_vector
 * @r_idx: total number of rings to allocate
 * @r_count: ring count
 * @step: ring step
 *
 * @return: 0 on success. If allocation fails we return -ENOMEM.
 **/
static int rnpgbe_alloc_q_vector(struct mucse *mucse,
				 int eth_queue_idx, int v_idx, int r_idx,
				 int r_count, int step)
{
	int rxr_idx = r_idx, txr_idx = r_idx;
	struct mucse_hw *hw = &mucse->hw;
	struct mucse_q_vector *q_vector;
	int txr_count, rxr_count, idx;
	struct mucse_ring *ring;
	int ring_count;

	txr_count = r_count;
	rxr_count = r_count;
	ring_count = txr_count + rxr_count;

	q_vector = kzalloc_flex(*q_vector, ring, ring_count);
	if (!q_vector)
		return -ENOMEM;

	netif_napi_add(mucse->netdev, &q_vector->napi, rnpgbe_poll);
	/* tie q_vector and mucse together */
	mucse->q_vector[v_idx] = q_vector;
	q_vector->mucse = mucse;
	q_vector->v_idx = v_idx;
	/* if mbx use separate irq, we should add 1 */
	if (mucse->flags & M_FLAG_MSIX_EN)
		q_vector->v_idx++;

	ring = q_vector->ring;

	for (idx = 0; idx < txr_count; idx++) {
		ring->dev = &mucse->pdev->dev;
		mucse_add_ring(ring, &q_vector->tx);
		ring->count = mucse->tx_ring_item_count;
		ring->netdev = mucse->netdev;
		ring->queue_index = eth_queue_idx + idx;
		ring->rnpgbe_queue_idx = txr_idx;
		ring->ring_addr = hw->hw_addr + RING_OFFSET(txr_idx);
		ring->irq_mask = ring->ring_addr + RNPGBE_DMA_INT_MASK;
		ring->trig = ring->ring_addr + RNPGBE_DMA_INT_TRIG;
		ring->pfvfnum = hw->pfvfnum;
		u64_stats_init(&ring->syncp);
		mucse->tx_ring[ring->queue_index] = ring;
		txr_idx += step;
		ring++;
	}

	for (idx = 0; idx < rxr_count; idx++) {
		ring->dev = &mucse->pdev->dev;
		mucse_add_ring(ring, &q_vector->rx);
		ring->count = mucse->rx_ring_item_count;
		ring->netdev = mucse->netdev;
		ring->queue_index = eth_queue_idx + idx;
		ring->rnpgbe_queue_idx = rxr_idx;
		ring->ring_addr = hw->hw_addr + RING_OFFSET(rxr_idx);
		ring->irq_mask = ring->ring_addr + RNPGBE_DMA_INT_MASK;
		ring->trig = ring->ring_addr + RNPGBE_DMA_INT_TRIG;
		ring->pfvfnum = hw->pfvfnum;
		u64_stats_init(&ring->syncp);
		mucse->rx_ring[ring->queue_index] = ring;
		rxr_idx += step;
		ring++;
	}

	return 0;
}

/**
 * rnpgbe_free_q_vector - Free memory allocated for specific interrupt vector
 * @mucse: pointer to private structure
 * @v_idx: index of vector to be freed
 *
 * This function frees the memory allocated to the q_vector.  In addition if
 * NAPI is enabled it will delete any references to the NAPI struct prior
 * to freeing the q_vector.
 **/
static void rnpgbe_free_q_vector(struct mucse *mucse, int v_idx)
{
	struct mucse_q_vector *q_vector = mucse->q_vector[v_idx];
	struct mucse_ring *ring;

	mucse_for_each_ring(ring, q_vector->tx)
		mucse->tx_ring[ring->queue_index] = NULL;
	mucse_for_each_ring(ring, q_vector->rx)
		mucse->rx_ring[ring->queue_index] = NULL;
	mucse->q_vector[v_idx] = NULL;
	netif_napi_del(&q_vector->napi);
	kfree(q_vector);
}

/**
 * rnpgbe_alloc_q_vectors - Allocate memory for interrupt vectors
 * @mucse: pointer to private structure
 *
 * @return: 0 if success. if allocation fails we return -ENOMEM.
 **/
static int rnpgbe_alloc_q_vectors(struct mucse *mucse)
{
	int err, ring_cnt, v_remaing = mucse->num_q_vectors;
	int r_remaing = min_t(int, mucse->num_tx_queues,
			      mucse->num_rx_queues);
	int q_vector_nums = 0;
	int eth_queue_idx = 0;
	int ring_step = 1;
	int ring_idx = 0;
	int v_idx = 0;

	for (; r_remaing > 0 && v_remaing > 0; v_remaing--) {
		ring_cnt = DIV_ROUND_UP(r_remaing, v_remaing);
		err = rnpgbe_alloc_q_vector(mucse, eth_queue_idx,
					    v_idx, ring_idx, ring_cnt,
					    ring_step);
		if (err)
			goto err_free_q_vector;
		ring_idx += ring_step * ring_cnt;
		eth_queue_idx += ring_cnt;
		r_remaing -= ring_cnt;
		q_vector_nums++;
		v_idx++;
	}
	/* Fix the real used q_vectors_nums */
	mucse->num_q_vectors = q_vector_nums;
	mucse->num_tx_queues = eth_queue_idx;
	mucse->num_rx_queues = eth_queue_idx;

	return 0;

err_free_q_vector:
	mucse->num_tx_queues = 0;
	mucse->num_rx_queues = 0;
	mucse->num_q_vectors = 0;

	while (v_idx--)
		rnpgbe_free_q_vector(mucse, v_idx);

	return err;
}

/**
 * rnpgbe_reset_interrupt_capability - Reset irq capability setup
 * @mucse: pointer to private structure
 **/
static void rnpgbe_reset_interrupt_capability(struct mucse *mucse)
{
	pci_free_irq_vectors(mucse->pdev);
	mucse->flags &= ~(M_FLAG_MSIX_EN |
			M_FLAG_MSIX_SINGLE_EN |
			M_FLAG_MSI_EN);
}

/**
 * rnpgbe_init_interrupt_scheme - Determine proper interrupt scheme
 * @mucse: pointer to private structure
 *
 * We determine which interrupt scheme to use based on...
 * - Hardware queue count
 * - cpu numbers
 * - irq mode (msi/legacy force 1)
 *
 * @return: 0 on success, negative on failure
 **/
int rnpgbe_init_interrupt_scheme(struct mucse *mucse)
{
	int err;

	rnpgbe_set_num_queues(mucse);

	err = rnpgbe_set_interrupt_capability(mucse);
	if (err)
		return err;

	err = rnpgbe_alloc_q_vectors(mucse);
	if (err) {
		rnpgbe_reset_interrupt_capability(mucse);
		return err;
	}

	return 0;
}

/**
 * rnpgbe_free_q_vectors - Free memory allocated for interrupt vectors
 * @mucse: pointer to private structure
 *
 * This function frees the memory allocated to the q_vectors.  In addition if
 * NAPI is enabled it will delete any references to the NAPI struct prior
 * to freeing the q_vector.
 **/
static void rnpgbe_free_q_vectors(struct mucse *mucse)
{
	int v_idx = mucse->num_q_vectors;

	mucse->num_rx_queues = 0;
	mucse->num_tx_queues = 0;
	mucse->num_q_vectors = 0;

	while (v_idx--)
		rnpgbe_free_q_vector(mucse, v_idx);
}

/**
 * rnpgbe_clear_interrupt_scheme - Clear the current interrupt scheme settings
 * @mucse: pointer to private structure
 *
 * Clear interrupt specific resources and reset the structure
 **/
void rnpgbe_clear_interrupt_scheme(struct mucse *mucse)
{
	mucse->num_tx_queues = 0;
	mucse->num_rx_queues = 0;
	rnpgbe_free_q_vectors(mucse);
	rnpgbe_reset_interrupt_capability(mucse);
}

/**
 * rnpgbe_msix_clean_rings - Msix irq handler for ring irq
 * @irq: irq num
 * @data: private data
 *
 * rnpgbe_msix_clean_rings handle irq from ring, start napi
 * @return: IRQ_HANDLED
 **/
static irqreturn_t rnpgbe_msix_clean_rings(int irq, void *data)
{
	struct mucse_q_vector *q_vector = (struct mucse_q_vector *)data;

	rnpgbe_irq_disable_queues(q_vector);
	if (q_vector->rx.ring || q_vector->tx.ring)
		napi_schedule_irqoff(&q_vector->napi);

	return IRQ_HANDLED;
}

/**
 * rnpgbe_int_single - Msix-signle/msi irq handler
 * @irq: irq num
 * @data: private data
 * @return: IRQ_HANDLED
 **/
static irqreturn_t rnpgbe_int_single(int irq, void *data)
{
	struct mucse *mucse = (struct mucse *)data;
	struct mucse_q_vector *q_vector;

	mucse_fw_irq_handler(&mucse->hw);

	q_vector = mucse->q_vector[0];
	rnpgbe_irq_disable_queues(q_vector);
	if (q_vector->rx.ring || q_vector->tx.ring)
		napi_schedule_irqoff(&q_vector->napi);

	return IRQ_HANDLED;
}

/**
 * rnpgbe_request_irq - Initialize interrupts
 * @mucse: pointer to private structure
 *
 * Attempts to configure interrupts using the best available
 * capabilities of the hardware and kernel.
 *
 * @return: 0 on success, negative value on failure
 **/
int rnpgbe_request_irq(struct mucse *mucse)
{
	struct net_device *netdev = mucse->netdev;
	struct pci_dev *pdev = mucse->pdev;
	struct mucse_hw *hw = &mucse->hw;
	struct mucse_q_vector *q_vector;
	int err, i;

	if (mucse->flags & M_FLAG_MSIX_EN) {
		for (i = 0; i < mucse->num_q_vectors; i++) {
			q_vector = mucse->q_vector[i];

			snprintf(q_vector->name, sizeof(q_vector->name),
				 "%s-%s-%d", netdev->name, "TxRx", i);

			err = request_irq(pci_irq_vector(pdev, i + 1),
					  rnpgbe_msix_clean_rings, 0,
					  q_vector->name,
					  q_vector);
			if (err) {
				dev_err(&pdev->dev, "MSI-X req err %d: %d\n",
					i + 1, err);
				goto err_free_irqs;
			}
		}
	} else {
		/* msi/msix_single */
		err = request_irq(pci_irq_vector(pdev, 0),
				  rnpgbe_int_single, 0, netdev->name,
				  mucse);
		if (err)
			return err;
		hw->mbx.irq_en = true;
	}

	return 0;
err_free_irqs:
	while (i--) {
		q_vector = mucse->q_vector[i];
		synchronize_irq(pci_irq_vector(pdev, i + 1));
		free_irq(pci_irq_vector(pdev, i + 1), q_vector);
	}

	return err;
}

/**
 * rnpgbe_free_irq - Free interrupts
 * @mucse: pointer to private structure
 *
 * Attempts to free interrupts according initialized type.
 **/
void rnpgbe_free_irq(struct mucse *mucse)
{
	struct pci_dev *pdev = mucse->pdev;
	struct mucse_hw *hw = &mucse->hw;
	struct mucse_q_vector *q_vector;

	if (mucse->flags & M_FLAG_MSIX_EN) {
		for (int i = 0; i < mucse->num_q_vectors; i++) {
			q_vector = mucse->q_vector[i];
			if (!q_vector)
				continue;

			free_irq(pci_irq_vector(pdev, i + 1), q_vector);
		}
	} else {
		free_irq(pci_irq_vector(pdev, 0), mucse);
		hw->mbx.irq_en = false;
	}
}

/**
 * rnpgbe_set_ring_vector - Set the ring_vector registers,
 * mapping interrupt causes to vectors
 * @mucse: pointer to private structure
 * @queue: queue to map the corresponding interrupt to
 * @vector: the vector num to map to the corresponding queue
 *
 */
static void rnpgbe_set_ring_vector(struct mucse *mucse,
				   u8 queue, u8 vector)
{
	struct mucse_hw *hw = &mucse->hw;
	u32 data;

	data = hw->pfvfnum << 24;
	data |= (vector << 8);
	data |= vector;
	writel(data, hw->ring_msix_base + RING_VECTOR(queue));
}

/**
 * rnpgbe_configure_msi - Configure MSI hardware
 * @mucse: pointer to private structure
 *
 * rnpgbe_configure_msi sets up the hardware to properly generate MSI
 * interrupts.
 **/
static void rnpgbe_configure_msi(struct mucse *mucse)
{
	struct mucse_q_vector *q_vector = mucse->q_vector[0];
	struct mucse_ring *ring;

	/* tx/rx use one register, different bit */
	mucse_for_each_ring(ring, q_vector->tx)
		rnpgbe_set_ring_vector(mucse, ring->rnpgbe_queue_idx, 0);
}

/**
 * rnpgbe_configure_msix - Configure MSI-X hardware
 * @mucse: pointer to private structure
 *
 * rnpgbe_configure_msix sets up the hardware to properly generate MSI-X
 * interrupts.
 **/
static void rnpgbe_configure_msix(struct mucse *mucse)
{
	struct mucse_q_vector *q_vector;

	for (int i = 0; i < mucse->num_q_vectors; i++) {
		struct mucse_ring *ring;

		q_vector = mucse->q_vector[i];
		/* tx/rx use one register, different bit */
		mucse_for_each_ring(ring, q_vector->tx) {
			rnpgbe_set_ring_vector(mucse, ring->rnpgbe_queue_idx,
					       q_vector->v_idx);
		}
	}
}

static void rnpgbe_irq_enable(struct mucse *mucse)
{
	for (int i = 0; i < mucse->num_q_vectors; i++)
		rnpgbe_irq_enable_queues(mucse->q_vector[i]);
}

/**
 * rnpgbe_irq_disable - Mask off interrupt generation on the NIC
 * @mucse: board private structure
 **/
void rnpgbe_irq_disable(struct mucse *mucse)
{
	struct pci_dev *pdev = mucse->pdev;

	if (mucse->flags & M_FLAG_MSIX_EN) {
		for (int i = 0; i < mucse->num_q_vectors; i++) {
			rnpgbe_irq_disable_queues(mucse->q_vector[i]);
			synchronize_irq(pci_irq_vector(pdev, i + 1));
		}
	} else {
		rnpgbe_irq_disable_queues(mucse->q_vector[0]);
		synchronize_irq(pci_irq_vector(pdev, 0));
	}
}

static void rnpgbe_napi_enable_all(struct mucse *mucse)
{
	for (int i = 0; i < mucse->num_q_vectors; i++)
		napi_enable(&mucse->q_vector[i]->napi);
}

static void rnpgbe_napi_disable_all(struct mucse *mucse)
{
	for (int i = 0; i < mucse->num_q_vectors; i++)
		napi_disable(&mucse->q_vector[i]->napi);
}

/**
 * rnpgbe_clean_tx_ring - Free Tx Buffers
 * @tx_ring: ring to be cleaned
 **/
static void rnpgbe_clean_tx_ring(struct mucse_ring *tx_ring)
{
	u16 i = tx_ring->next_to_clean;
	struct mucse_tx_buffer *tx_buffer = &tx_ring->tx_buffer_info[i];
	unsigned long size;

	/* Stop hw. hardware design guarantees:
	 * - No new descriptors will be fetched after TX_START=0
	 * - No DMA will be initiated for already-fetched descriptors
	 */
	mucse_ring_wr32(tx_ring, RNPGBE_TX_START, 0);

	/* Flush posted write to ensure hardware sees the disable command.
	 * After this read completes, all TX DMA for this ring is
	 * guaranteed quiesced.
	 */
	(void)mucse_ring_rd32(tx_ring, RNPGBE_TX_START);

	/* ring already cleared, nothing to do */
	if (!tx_ring->tx_buffer_info)
		return;

	while (i != tx_ring->next_to_use) {
		struct rnpgbe_tx_desc *eop_desc, *tx_desc;

		dev_kfree_skb_any(tx_buffer->skb);
		/* unmap skb header data */
		if (dma_unmap_len(tx_buffer, len)) {
			if (tx_buffer->mapped_as_page) {
				dma_unmap_page(tx_ring->dev,
					       dma_unmap_addr(tx_buffer, dma),
					       dma_unmap_len(tx_buffer, len),
					       DMA_TO_DEVICE);
			} else {
				dma_unmap_single(tx_ring->dev,
						 dma_unmap_addr(tx_buffer, dma),
						 dma_unmap_len(tx_buffer, len),
						 DMA_TO_DEVICE);
			}
		}
		eop_desc = tx_buffer->next_to_watch;
		tx_desc = M_TX_DESC(tx_ring, i);
		/* unmap remaining buffers */
		while (tx_desc != eop_desc) {
			tx_buffer++;
			tx_desc++;
			i++;
			if (unlikely(i == tx_ring->count)) {
				i = 0;
				tx_buffer = tx_ring->tx_buffer_info;
				tx_desc = M_TX_DESC(tx_ring, 0);
			}

			/* unmap any remaining paged data */
			if (dma_unmap_len(tx_buffer, len))
				dma_unmap_page(tx_ring->dev,
					       dma_unmap_addr(tx_buffer, dma),
					       dma_unmap_len(tx_buffer, len),
					       DMA_TO_DEVICE);
		}
		/* move us one more past the eop_desc for start of next pkt */
		tx_buffer++;
		i++;
		if (unlikely(i == tx_ring->count)) {
			i = 0;
			tx_buffer = tx_ring->tx_buffer_info;
		}
	}

	netdev_tx_reset_queue(txring_txq(tx_ring));
	size = sizeof(struct mucse_tx_buffer) * tx_ring->count;
	memset(tx_ring->tx_buffer_info, 0, size);
	/* Zero out the descriptor ring */
	memset(tx_ring->desc, 0, tx_ring->size);
	tx_ring->next_to_use = 0;
	tx_ring->next_to_clean = 0;
}

/**
 * rnpgbe_clean_all_tx_rings - Free Tx Buffers for all queues
 * @mucse: board private structure
 **/
static void rnpgbe_clean_all_tx_rings(struct mucse *mucse)
{
	for (int i = 0; i < mucse->num_tx_queues; i++)
		rnpgbe_clean_tx_ring(mucse->tx_ring[i]);
}

/**
 * rnpgbe_clean_all_rx_rings - Free Rx Buffers for all queues
 * @mucse: board private structure
 **/
static void rnpgbe_clean_all_rx_rings(struct mucse *mucse)
{
	for (int i = 0; i < mucse->num_rx_queues; i++)
		rnpgbe_clean_rx_ring(mucse->rx_ring[i]);
}

void rnpgbe_down(struct mucse *mucse)
{
	struct net_device *netdev = mucse->netdev;
	struct mucse_hw *hw = &mucse->hw;
	unsigned long flags;
	int err;

	set_bit(__MUCSE_DOWN, &mucse->state);
	cancel_delayed_work_sync(&mucse->serv_task);

	spin_lock_irqsave(&mucse->link_lock, flags);
	hw->link = false;
	hw->speed = 0;
	hw->duplex = 0;
	mucse->flags &= ~M_FLAG_NEED_LINK_UPDATE;
	spin_unlock_irqrestore(&mucse->link_lock, flags);
	rnpgbe_set_rx(hw, false);

	err = rnpgbe_send_notify(hw, false, mucse_fw_link_report_en);
	if (err) {
		dev_warn(&hw->pdev->dev, "Send link report to hw failed %d\n",
			 err);
		dev_warn(&hw->pdev->dev, "Fw will still report link event\n");
	}

	err = rnpgbe_send_notify(hw, false, mucse_fw_portup);
	if (err) {
		dev_warn(&hw->pdev->dev, "Send port down to hw failed %d\n",
			 err);
		dev_warn(&hw->pdev->dev, "Port is not truly down\n");
	}
	netif_carrier_off(netdev);
	netif_tx_stop_all_queues(netdev);
	netif_tx_disable(netdev);
	rnpgbe_napi_disable_all(mucse);
	rnpgbe_irq_disable(mucse);
	rnpgbe_clean_all_tx_rings(mucse);
	rnpgbe_clean_all_rx_rings(mucse);
}

/**
 * rnpgbe_up_complete - Final step for port up
 * @mucse: pointer to private structure
 **/
void rnpgbe_up_complete(struct mucse *mucse)
{
	struct net_device *netdev = mucse->netdev;
	struct mucse_hw *hw = &mucse->hw;
	int err;

	if (mucse->flags & (M_FLAG_MSIX_EN | M_FLAG_MSIX_SINGLE_EN))
		rnpgbe_configure_msix(mucse);
	else
		rnpgbe_configure_msi(mucse);

	clear_bit(__MUCSE_DOWN, &mucse->state);
	rnpgbe_napi_enable_all(mucse);
	rnpgbe_irq_enable(mucse);
	netif_tx_start_all_queues(netdev);
	for (int i = 0; i < mucse->num_rx_queues; i++)
		mucse_ring_wr32(mucse->rx_ring[i], RNPGBE_RX_START, 1);

	err = rnpgbe_send_notify(hw, true, mucse_fw_portup);
	if (err) {
		dev_warn(&hw->pdev->dev, "Send portup to hw failed %d\n", err);
		dev_warn(&hw->pdev->dev, "Port is not truly up\n");
	}

	err = rnpgbe_send_notify(hw, true, mucse_fw_link_report_en);
	if (err) {
		dev_warn(&hw->pdev->dev, "Send link report to hw failed %d\n",
			 err);
		dev_warn(&hw->pdev->dev, "Fw will not report link event\n");
	}

	/* echo driver down to hw */
	mucse_hw_wr32(hw, RNPGBE_LINK_ST, M_DEFAULT_ST);
	queue_delayed_work(system_wq, &mucse->serv_task,
			   msecs_to_jiffies(500));
}

/**
 * rnpgbe_free_tx_resources - Free Tx Resources per Queue
 * @tx_ring: tx descriptor ring for a specific queue
 *
 * Free all transmit software resources
 **/
static void rnpgbe_free_tx_resources(struct mucse_ring *tx_ring)
{
	rnpgbe_clean_tx_ring(tx_ring);
	vfree(tx_ring->tx_buffer_info);
	tx_ring->tx_buffer_info = NULL;
	/* if not set, then don't free */
	if (!tx_ring->desc)
		return;

	dma_free_coherent(tx_ring->dev, tx_ring->size, tx_ring->desc,
			  tx_ring->dma);
	tx_ring->desc = NULL;
}

/**
 * rnpgbe_setup_tx_resources - allocate Tx resources (Descriptors)
 * @tx_ring: tx descriptor ring (for a specific queue) to setup
 * @mucse: pointer to private structure
 *
 * @return: 0 on success, negative on failure
 **/
static int rnpgbe_setup_tx_resources(struct mucse_ring *tx_ring,
				     struct mucse *mucse)
{
	struct device *dev = tx_ring->dev;
	int size;

	size = sizeof(struct mucse_tx_buffer) * tx_ring->count;

	tx_ring->tx_buffer_info = vzalloc(size);
	if (!tx_ring->tx_buffer_info)
		goto err_return;
	/* round up to nearest 4K */
	tx_ring->size = tx_ring->count * sizeof(struct rnpgbe_tx_desc);
	tx_ring->size = ALIGN(tx_ring->size, 4096);
	tx_ring->desc = dma_alloc_coherent(dev, tx_ring->size, &tx_ring->dma,
					   GFP_KERNEL);
	if (!tx_ring->desc)
		goto err_free_buffer;

	tx_ring->next_to_use = 0;
	tx_ring->next_to_clean = 0;

	return 0;

err_free_buffer:
	vfree(tx_ring->tx_buffer_info);
err_return:
	tx_ring->tx_buffer_info = NULL;
	return -ENOMEM;
}

/**
 * rnpgbe_configure_tx_ring - Configure Tx ring after Reset
 * @mucse: pointer to private structure
 * @ring: structure containing ring specific data
 *
 * Configure the Tx descriptor ring after a reset.
 **/
static void rnpgbe_configure_tx_ring(struct mucse *mucse,
				     struct mucse_ring *ring)
{
	struct mucse_hw *hw = &mucse->hw;

	/* Stop hw. hardware design guarantees:
	 * - No new descriptors will be fetched after TX_START=0
	 * - No DMA will be initiated for already-fetched descriptors
	 */
	mucse_ring_wr32(ring, RNPGBE_TX_START, 0);
	/* Flush posted write to ensure hardware sees the disable command.
	 * After this read completes, all TX DMA for this ring is
	 * guaranteed quiesced.
	 */
	(void)mucse_ring_rd32(ring, RNPGBE_TX_START);

	mucse_ring_wr32(ring, RNPGBE_TX_BASE_ADDR_LO, (u32)ring->dma);
	mucse_ring_wr32(ring, RNPGBE_TX_BASE_ADDR_HI,
			(u32)(((u64)ring->dma) >> 32) | (hw->pfvfnum << 24));
	mucse_ring_wr32(ring, RNPGBE_TX_LEN, ring->count);
	ring->next_to_clean = mucse_ring_rd32(ring, RNPGBE_TX_HEAD);
	ring->next_to_use = ring->next_to_clean;
	ring->tail = ring->ring_addr + RNPGBE_TX_TAIL;
	writel(ring->next_to_use, ring->tail);
	mucse_ring_wr32(ring, RNPGBE_TX_FETCH_CTRL, M_DEFAULT_TX_FETCH);
	mucse_ring_wr32(ring, RNPGBE_TX_INT_TIMER,
			M_DEFAULT_INT_TIMER * hw->cycles_per_us);
	mucse_ring_wr32(ring, RNPGBE_TX_INT_PKTCNT, M_DEFAULT_INT_PKTCNT);
	/* Ensure all config is written before enabling queue */
	wmb();
	mucse_ring_wr32(ring, RNPGBE_TX_START, 1);
}

/**
 * rnpgbe_configure_tx - Configure Transmit Unit after Reset
 * @mucse: pointer to private structure
 *
 * Configure the Tx DMA after a reset.
 **/
void rnpgbe_configure_tx(struct mucse *mucse)
{
	struct mucse_hw *hw = &mucse->hw;
	u32 i, dma_axi_ctl;

	dma_axi_ctl = mucse_hw_rd32(hw, RNPGBE_DMA_AXI_EN);
	dma_axi_ctl |= TX_AXI_RW_EN;
	mucse_hw_wr32(hw, RNPGBE_DMA_AXI_EN, dma_axi_ctl);
	/* Setup the HW Tx Head and Tail descriptor pointers */
	for (i = 0; i < mucse->num_tx_queues; i++)
		rnpgbe_configure_tx_ring(mucse, mucse->tx_ring[i]);
}

/**
 * rnpgbe_setup_all_tx_resources - allocate all queues Tx resources
 * @mucse: pointer to private structure
 *
 * Allocate memory for tx_ring.
 *
 * @return: 0 on success, negative on failure
 **/
int rnpgbe_setup_all_tx_resources(struct mucse *mucse)
{
	int i, err = 0;

	for (i = 0; i < mucse->num_tx_queues; i++) {
		err = rnpgbe_setup_tx_resources(mucse->tx_ring[i], mucse);
		if (!err)
			continue;

		goto err_free_res;
	}

	return 0;
err_free_res:
	while (i--)
		rnpgbe_free_tx_resources(mucse->tx_ring[i]);
	return err;
}

/**
 * rnpgbe_free_all_tx_resources - Free Tx Resources for All Queues
 * @mucse: pointer to private structure
 *
 * Free all transmit software resources
 **/
void rnpgbe_free_all_tx_resources(struct mucse *mucse)
{
	for (int i = 0; i < (mucse->num_tx_queues); i++)
		rnpgbe_free_tx_resources(mucse->tx_ring[i]);
}

static int rnpgbe_tx_map(struct mucse_ring *tx_ring,
			 struct mucse_tx_buffer *first, u32 mac_ip_len,
			 u32 tx_flags)
{
	/* hw need this in high 8 bytes desc */
	u64 fun_id = ((u64)(tx_ring->pfvfnum) << (56));
	struct mucse_tx_buffer *tx_buffer;
	struct sk_buff *skb = first->skb;
	struct rnpgbe_tx_desc *tx_desc;
	u16 i = tx_ring->next_to_use;
	unsigned int data_len, size;
	skb_frag_t *frag;
	dma_addr_t dma;

	tx_desc = M_TX_DESC(tx_ring, i);
	size = skb_headlen(skb);
	data_len = skb->data_len;
	frag = &skb_shinfo(skb)->frags[0];

	if (size) {
		dma = dma_map_single(tx_ring->dev, skb->data, size,
				     DMA_TO_DEVICE);
		first->mapped_as_page = false;
	} else if (data_len) {
		/* skip zero-size fragments. data_len > 0 guarantees at
		 * least one fragment with non-zero size exists
		 */
		while (!skb_frag_size(frag))
			frag++;
		size = skb_frag_size(frag);

		dma = skb_frag_dma_map(tx_ring->dev, frag, 0,
				       size, DMA_TO_DEVICE);
		first->mapped_as_page = true;
		data_len -= size;
		frag++;
	} else {
		goto err_unmap;
	}

	tx_buffer = first;

	dma_unmap_len_set(tx_buffer, len, 0);
	dma_unmap_addr_set(tx_buffer, dma, 0);

	for (;; frag++) {
		if (dma_mapping_error(tx_ring->dev, dma))
			goto err_unmap;

		/* record length, and DMA address */
		dma_unmap_len_set(tx_buffer, len, size);
		dma_unmap_addr_set(tx_buffer, dma, dma);

		tx_desc->pkt_addr = cpu_to_le64(dma | fun_id);

		while (unlikely(size > M_MAX_DATA_PER_TXD)) {
			tx_desc->vlan_cmd_bsz = build_ctob(tx_flags,
							   mac_ip_len,
							   M_MAX_DATA_PER_TXD);
			i++;
			tx_desc++;
			if (i == tx_ring->count) {
				tx_desc = M_TX_DESC(tx_ring, 0);
				i = 0;
			}
			dma += M_MAX_DATA_PER_TXD;
			size -= M_MAX_DATA_PER_TXD;
			tx_desc->pkt_addr = cpu_to_le64(dma | fun_id);
		}

		if (likely(!data_len))
			break;
		tx_desc->vlan_cmd_bsz = build_ctob(tx_flags, mac_ip_len, size);
		i++;
		tx_desc++;
		if (i == tx_ring->count) {
			tx_desc = M_TX_DESC(tx_ring, 0);
			i = 0;
		}

		/* skip zero-size fragments. data_len > 0 guarantees at
		 * least one fragment with non-zero size exists
		 */
		while (!skb_frag_size(frag))
			frag++;
		size = skb_frag_size(frag);
		data_len -= size;
		dma = skb_frag_dma_map(tx_ring->dev, frag, 0, size,
				       DMA_TO_DEVICE);
		tx_buffer = &tx_ring->tx_buffer_info[i];
		tx_buffer->mapped_as_page = true;
	}

	/* write last descriptor with RS and EOP bits */
	tx_desc->vlan_cmd_bsz = build_ctob(tx_flags | M_TXD_CMD_EOP |
					   M_TXD_CMD_RS,
					   mac_ip_len, size);

	/*
	 * Force memory writes to complete before letting h/w know there
	 * are new descriptors to fetch.  (Only applicable for weak-ordered
	 * memory model archs, such as IA-64).
	 *
	 * We also need this memory barrier to make certain all of the
	 * status bits have been updated before next_to_watch is written.
	 */
	wmb();
	/* set next_to_watch value indicating a packet is present */
	first->next_to_watch = tx_desc;
	i++;
	if (i == tx_ring->count)
		i = 0;
	tx_ring->next_to_use = i;
	skb_tx_timestamp(skb);
	netdev_tx_sent_queue(txring_txq(tx_ring), first->bytecount);
	/* notify HW of packet */
	writel(i, tx_ring->tail);

	return 0;
err_unmap:
	for (;;) {
		tx_buffer = &tx_ring->tx_buffer_info[i];
		if (dma_unmap_len(tx_buffer, len)) {
			if (tx_buffer->mapped_as_page) {
				dma_unmap_page(tx_ring->dev,
					       dma_unmap_addr(tx_buffer, dma),
					       dma_unmap_len(tx_buffer, len),
					       DMA_TO_DEVICE);
			} else {
				dma_unmap_single(tx_ring->dev,
						 dma_unmap_addr(tx_buffer, dma),
						 dma_unmap_len(tx_buffer, len),
						 DMA_TO_DEVICE);
			}
		}
		dma_unmap_len_set(tx_buffer, len, 0);
		dma_unmap_addr_set(tx_buffer, dma, 0);
		if (tx_buffer == first)
			break;
		if (i == 0)
			i += tx_ring->count;
		i--;
	}
	dev_kfree_skb_any(first->skb);
	first->skb = NULL;
	tx_ring->next_to_use = i;

	return -ENOMEM;
}

static int rnpgbe_maybe_stop_tx(struct mucse_ring *tx_ring, u16 size)
{
	if (likely(mucse_desc_unused(tx_ring) >= size))
		return 0;

	netif_stop_subqueue(tx_ring->netdev, tx_ring->queue_index);
	/* Herbert's original patch had:
	 *  smp_mb__after_netif_stop_queue();
	 * but since that doesn't exist yet, just open code it.
	 */
	smp_mb();

	/* We need to check again in a case another CPU has just
	 * made room available.
	 */
	if (likely(mucse_desc_unused(tx_ring) < size))
		return -EBUSY;

	/* A reprieve! - use start_queue because it doesn't call schedule */
	netif_start_subqueue(tx_ring->netdev, tx_ring->queue_index);

	return 0;
}

netdev_tx_t rnpgbe_xmit_frame_ring(struct sk_buff *skb,
				   struct mucse_ring *tx_ring)
{
	u16 count = TXD_USE_COUNT(skb_headlen(skb));
	/* hw requires it not zero */
	u32 mac_ip_len = M_DEFAULT_MAC_IP_LEN;
	struct mucse_tx_buffer *first;
	u32 tx_flags = 0;
	unsigned short f;

	for (f = 0; f < skb_shinfo(skb)->nr_frags; f++) {
		skb_frag_t *frag_temp = &skb_shinfo(skb)->frags[f];

		count += TXD_USE_COUNT(skb_frag_size(frag_temp));
	}

	if (rnpgbe_maybe_stop_tx(tx_ring, count + 3))
		return NETDEV_TX_BUSY;

	/* record the location of the first descriptor for this packet */
	first = &tx_ring->tx_buffer_info[tx_ring->next_to_use];
	first->skb = skb;
	first->bytecount = skb->len;
	first->gso_segs = 1;

	if (rnpgbe_tx_map(tx_ring, first, mac_ip_len, tx_flags)) {
		atomic64_inc(&tx_ring->stats.dropped);

		goto out;
	}

	rnpgbe_maybe_stop_tx(tx_ring, DESC_NEEDED);
out:
	return NETDEV_TX_OK;
}

/**
 * rnpgbe_get_stats64 - Get stats for this netdev
 * @netdev: network interface device structure
 * @stats: stats data
 **/
void rnpgbe_get_stats64(struct net_device *netdev,
			struct rtnl_link_stats64 *stats)
{
	struct mucse *mucse = netdev_priv(netdev);
	int i;

	rcu_read_lock();
	for (i = 0; i < mucse->num_tx_queues; i++) {
		struct mucse_ring *ring = READ_ONCE(mucse->tx_ring[i]);
		u64 bytes, packets, dropped;
		unsigned int start;

		if (ring) {
			do {
				start = u64_stats_fetch_begin(&ring->syncp);
				packets = ring->stats.packets;
				bytes = ring->stats.bytes;
				dropped = atomic64_read(&ring->stats.dropped);
			} while (u64_stats_fetch_retry(&ring->syncp, start));
			stats->tx_packets += packets;
			stats->tx_dropped += dropped;
			stats->tx_bytes += bytes;
		}
	}

	for (i = 0; i < mucse->num_rx_queues; i++) {
		struct mucse_ring *ring = READ_ONCE(mucse->rx_ring[i]);
		u64 bytes, packets, dropped;
		unsigned int start;

		if (ring) {
			do {
				start = u64_stats_fetch_begin(&ring->syncp);
				packets = ring->stats.packets;
				bytes = ring->stats.bytes;
				dropped = atomic64_read(&ring->stats.dropped);
			} while (u64_stats_fetch_retry(&ring->syncp, start));
			stats->rx_packets += packets;
			stats->rx_dropped += dropped;
			stats->rx_bytes += bytes;
		}
	}
	rcu_read_unlock();
}

static int mucse_alloc_page_pool(struct mucse_ring *rx_ring)
{
	int ret = 0;

	struct page_pool_params pp_params = {
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.order = 0,
		.pool_size = rx_ring->count,
		.nid = dev_to_node(rx_ring->dev),
		.dev = rx_ring->dev,
		.dma_dir = DMA_FROM_DEVICE,
		.offset = 0,
		.max_len = PAGE_SIZE,
	};

	rx_ring->page_pool = page_pool_create(&pp_params);
	if (IS_ERR(rx_ring->page_pool)) {
		ret = PTR_ERR(rx_ring->page_pool);
		rx_ring->page_pool = NULL;
	}

	return ret;
}

/**
 * rnpgbe_setup_rx_resources - allocate Rx resources (Descriptors)
 * @rx_ring:    rx descriptor ring (for a specific queue) to setup
 * @mucse: pointer to private structure
 *
 * @return: 0 on success, negative on failure
 **/
static int rnpgbe_setup_rx_resources(struct mucse_ring *rx_ring,
				     struct mucse *mucse)
{
	struct device *dev = rx_ring->dev;
	int size;

	size = sizeof(struct mucse_rx_buffer) * rx_ring->count;

	rx_ring->rx_buffer_info = vzalloc(size);

	if (!rx_ring->rx_buffer_info)
		goto err_return;
	/* Round up to nearest 4K */
	rx_ring->size = rx_ring->count * sizeof(union rnpgbe_rx_desc);
	rx_ring->size = ALIGN(rx_ring->size, 4096);
	rx_ring->desc = dma_alloc_coherent(dev, rx_ring->size, &rx_ring->dma,
					   GFP_KERNEL);
	if (!rx_ring->desc)
		goto err_free_buffer;

	rx_ring->next_to_clean = 0;
	rx_ring->next_to_use = 0;

	if (mucse_alloc_page_pool(rx_ring))
		goto err_free_desc;

	return 0;
err_free_desc:
	dma_free_coherent(dev, rx_ring->size, rx_ring->desc,
			  rx_ring->dma);
	rx_ring->desc = NULL;
err_free_buffer:
	vfree(rx_ring->rx_buffer_info);
err_return:
	rx_ring->rx_buffer_info = NULL;
	return -ENOMEM;
}

/**
 * rnpgbe_clean_rx_ring - Free Rx Buffers per Queue
 * @rx_ring: ring to free buffers from
 **/
void rnpgbe_clean_rx_ring(struct mucse_ring *rx_ring)
{
	struct mucse_rx_buffer *rx_buffer;
	u16 i;

	/* Stop hw. hardware design guarantees:
	 * - No new descriptors will be fetched after RX_START=0
	 * - No DMA will be initiated for already-fetched descriptors
	 */
	mucse_ring_wr32(rx_ring, RNPGBE_RX_START, 0);
	/* Flush posted write to ensure hardware sees the disable command.
	 * After this read completes, all RX DMA for this ring is
	 * guaranteed quiesced.
	 */
	(void)mucse_ring_rd32(rx_ring, RNPGBE_RX_START);
	/* Free all the Rx ring sk_buffs */
	for (i = 0; i < rx_ring->count; i++) {
		rx_buffer = &rx_ring->rx_buffer_info[i];

		if (rx_buffer->skb) {
			struct sk_buff *skb = rx_buffer->skb;

			dev_kfree_skb(skb);
			rx_buffer->skb = NULL;
		}

		if (rx_buffer->page) {
			page_pool_put_full_page(rx_ring->page_pool,
						rx_buffer->page, false);
			rx_buffer->page = NULL;
		}
	}

	rx_ring->next_to_clean = 0;
	rx_ring->next_to_use = 0;
}

/**
 * rnpgbe_free_rx_resources - Free Rx Resources
 * @rx_ring: ring to clean the resources from
 *
 * Free all receive software resources
 **/
static void rnpgbe_free_rx_resources(struct mucse_ring *rx_ring)
{
	rnpgbe_clean_rx_ring(rx_ring);
	vfree(rx_ring->rx_buffer_info);
	rx_ring->rx_buffer_info = NULL;
	/* if not set, then don't free */
	if (!rx_ring->desc)
		return;

	dma_free_coherent(rx_ring->dev, rx_ring->size, rx_ring->desc,
			  rx_ring->dma);
	rx_ring->desc = NULL;
	if (rx_ring->page_pool) {
		page_pool_destroy(rx_ring->page_pool);
		rx_ring->page_pool = NULL;
	}
}

/**
 * rnpgbe_setup_all_rx_resources - allocate all queues Rx resources
 * @mucse: pointer to private structure
 *
 * @return: 0 on success, negative on failure
 **/
int rnpgbe_setup_all_rx_resources(struct mucse *mucse)
{
	int i, err = 0;

	for (i = 0; i < mucse->num_rx_queues; i++) {
		err = rnpgbe_setup_rx_resources(mucse->rx_ring[i], mucse);
		if (!err)
			continue;

		goto err_setup_rx;
	}

	return 0;
err_setup_rx:
	while (i--)
		rnpgbe_free_rx_resources(mucse->rx_ring[i]);
	return err;
}

/**
 * rnpgbe_free_all_rx_resources - Free Rx Resources for All Queues
 * @mucse: pointer to private structure
 *
 * Free all receive software resources
 **/
void rnpgbe_free_all_rx_resources(struct mucse *mucse)
{
	for (int i = 0; i < (mucse->num_rx_queues); i++) {
		if (mucse->rx_ring[i]->desc)
			rnpgbe_free_rx_resources(mucse->rx_ring[i]);
	}
}

/**
 * rnpgbe_configure_rx_ring - Configure Rx ring info to hw
 * @mucse: pointer to private structure
 * @ring: structure containing ring specific data
 *
 * Configure the Rx descriptor ring after a reset.
 **/
static void rnpgbe_configure_rx_ring(struct mucse *mucse,
				     struct mucse_ring *ring)
{
	struct mucse_hw *hw = &mucse->hw;

	/* Stop hw. hardware design guarantees:
	 * - No new descriptors will be fetched after RX_START=0
	 * - No DMA will be initiated for already-fetched descriptors
	 */
	mucse_ring_wr32(ring, RNPGBE_RX_START, 0);
	/* Flush posted write to ensure hardware sees the disable command.
	 * After this read completes, all RX DMA for this ring is
	 * guaranteed quiesced.
	 */
	(void)mucse_ring_rd32(ring, RNPGBE_RX_START);
	/* set descripts registers*/
	mucse_ring_wr32(ring, RNPGBE_RX_BASE_ADDR_LO, (u32)ring->dma);
	mucse_ring_wr32(ring, RNPGBE_RX_BASE_ADDR_HI,
			(u32)((u64)ring->dma >> 32) | (hw->pfvfnum << 24));
	mucse_ring_wr32(ring, RNPGBE_RX_LEN, ring->count);
	ring->tail = ring->ring_addr + RNPGBE_RX_TAIL;
	ring->next_to_clean = mucse_ring_rd32(ring, RNPGBE_RX_HEAD);
	ring->next_to_use = ring->next_to_clean;
	ring->drop_status = false;
	mucse_ring_wr32(ring, RNPGBE_RX_SG_LEN, M_DEFAULT_SG);
	mucse_ring_wr32(ring, RNPGBE_RX_FETCH, M_DEFAULT_RX_FETCH);
	mucse_ring_wr32(ring, RNPGBE_RX_TIMEOUT_TH, 0);
	mucse_ring_wr32(ring, RNPGBE_RX_INT_TIMER,
			M_DEFAULT_INT_TIMER_R * hw->cycles_per_us);
	mucse_ring_wr32(ring, RNPGBE_RX_INT_PKTCNT, M_DEFAULT_RX_INT_PKTCNT);
	rnpgbe_alloc_rx_buffers(ring, mucse_desc_unused_rx(ring));
}

/**
 * rnpgbe_configure_rx - Configure Receive Unit after Reset
 * @mucse: pointer to private structure
 *
 * Configure the Rx unit after a reset.
 **/
void rnpgbe_configure_rx(struct mucse *mucse)
{
	struct mucse_hw *hw = &mucse->hw;
	u32 dma_axi_ctl;

	for (int i = 0; i < mucse->num_rx_queues; i++)
		rnpgbe_configure_rx_ring(mucse, mucse->rx_ring[i]);

	dma_axi_ctl = mucse_hw_rd32(hw, RNPGBE_DMA_AXI_EN);
	dma_axi_ctl |= RX_AXI_RW_EN;
	mucse_hw_wr32(hw, RNPGBE_DMA_AXI_EN, dma_axi_ctl);
}

/**
 * rnpgbe_process_link_event - Update the link status
 * @mucse: pointer to the device private structure
 *
 * @return: link status
 **/
static int rnpgbe_process_link_event(struct mucse *mucse)
{
	struct net_device *netdev = mucse->netdev;
	struct mucse_hw *hw = &mucse->hw;
	unsigned long flags;
	bool link;
	int speed;
	u8 duplex;

	if (!(mucse->flags & M_FLAG_NEED_LINK_UPDATE))
		return hw->link;

	spin_lock_irqsave(&mucse->link_lock, flags);

	link = hw->link;
	speed = hw->speed;
	duplex = hw->duplex;

	mucse->flags &= ~M_FLAG_NEED_LINK_UPDATE;
	spin_unlock_irqrestore(&mucse->link_lock, flags);

	if (link) {
		netdev_info(netdev, "NIC Link is Up %d Mbps, %s Duplex\n",
			    speed,
			    duplex ? "Full" : "Half");
	}

	return link;
}

/**
 * rnpgbe_link_is_up - Update netif_carrier status and
 * print link up message
 * @mucse: pointer to the device private structure
 **/
static void rnpgbe_link_is_up(struct mucse *mucse)
{
	struct net_device *netdev = mucse->netdev;
	struct mucse_hw *hw = &mucse->hw;

	/* Only continue if link was previously down */
	if (netif_carrier_ok(netdev))
		return;
	rnpgbe_set_rx(hw, true);
	netif_carrier_on(netdev);
}

/**
 * rnpgbe_link_is_down - Update netif_carrier status and
 * print link down message
 * @mucse: pointer to the private structure
 **/
static void rnpgbe_link_is_down(struct mucse *mucse)
{
	struct net_device *netdev = mucse->netdev;
	struct mucse_hw *hw = &mucse->hw;

	/* Only continue if link was up previously */
	if (!netif_carrier_ok(netdev))
		return;
	netdev_info(netdev, "NIC Link is Down\n");
	rnpgbe_set_rx(hw, false);
	netif_carrier_off(netdev);
}

/**
 * rnpgbe_process_link_subtask - Check and bring link up
 * @mucse: pointer to the device private structure
 **/
static void rnpgbe_process_link_subtask(struct mucse *mucse)
{
	/* if interface is down do nothing */
	if (test_bit(__MUCSE_DOWN, &mucse->state))
		return;

	if (rnpgbe_process_link_event(mucse))
		rnpgbe_link_is_up(mucse);
	else
		rnpgbe_link_is_down(mucse);
}

/**
 * rnpgbe_service_task - Manages and runs subtasks
 * @work: pointer to work_struct containing our data
 **/
void rnpgbe_service_task(struct work_struct *work)
{
	struct mucse *mucse = container_of(work, struct mucse, serv_task.work);

	if (test_bit(__MUCSE_DOWN, &mucse->state))
		return;

	rnpgbe_process_link_subtask(mucse);

	/* Periodic requeue is intentional: future patches will add
	 * statistics polling and other housekeeping tasks beyond
	 * link state handling.
	 */
	if (!test_bit(__MUCSE_DOWN, &mucse->state))
		queue_delayed_work(system_wq, &mucse->serv_task,
				   msecs_to_jiffies(500));
}
