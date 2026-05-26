// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2020 - 2025 Mucse Corporation. */

#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/vmalloc.h>

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
	int work_done = 0;

	mucse_for_each_ring(ring, q_vector->tx) {
		if (!rnpgbe_clean_tx_irq(q_vector, ring, budget))
			clean_complete = false;
	}

	/* Exit if we are called by netpoll */
	if (unlikely(!budget))
		return 0;

	if (!clean_complete)
		return budget;

	if (likely(napi_complete_done(napi, work_done)))
		rnpgbe_irq_enable_queues(q_vector);

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
	int err = 0;

	snprintf(mucse->mbx_name, sizeof(mucse->mbx_name),
		 "rnpgbe-mbx:%s", pci_name(pdev));

	if (mucse->flags & M_FLAG_MSIX_EN) {
		err = request_irq(pci_irq_vector(pdev, 0),
				  rnpgbe_msix_other, 0, mucse->mbx_name,
				  mucse);
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

	if (mucse->flags & M_FLAG_MSIX_EN)
		free_irq(pci_irq_vector(pdev, 0), mucse);
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
		mucse_add_ring(ring, &q_vector->rx);
		ring->queue_index = eth_queue_idx + idx;
		ring->rnpgbe_queue_idx = rxr_idx;
		ring->ring_addr = hw->hw_addr + RING_OFFSET(rxr_idx);
		ring->irq_mask = ring->ring_addr + RNPGBE_DMA_INT_MASK;
		ring->trig = ring->ring_addr + RNPGBE_DMA_INT_TRIG;
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

void rnpgbe_down(struct mucse *mucse)
{
	struct net_device *netdev = mucse->netdev;

	netif_tx_stop_all_queues(netdev);
	netif_tx_disable(netdev);
	rnpgbe_napi_disable_all(mucse);
	rnpgbe_irq_disable(mucse);
	rnpgbe_clean_all_tx_rings(mucse);
}

/**
 * rnpgbe_up_complete - Final step for port up
 * @mucse: pointer to private structure
 **/
void rnpgbe_up_complete(struct mucse *mucse)
{
	struct net_device *netdev = mucse->netdev;

	if (mucse->flags & (M_FLAG_MSIX_EN | M_FLAG_MSIX_SINGLE_EN))
		rnpgbe_configure_msix(mucse);
	else
		rnpgbe_configure_msi(mucse);

	rnpgbe_napi_enable_all(mucse);
	rnpgbe_irq_enable(mucse);
	netif_tx_start_all_queues(netdev);
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
	rcu_read_unlock();
}
