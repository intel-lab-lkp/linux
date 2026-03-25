// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2020 - 2025 Mucse Corporation. */

#include <linux/pci.h>
#include <linux/netdevice.h>

#include "rnpgbe_lib.h"
#include "rnpgbe.h"

/**
 * rnpgbe_msix_other - Other irq handler
 * @irq: irq num
 * @data: private data
 *
 * @return: IRQ_HANDLED
 **/
static irqreturn_t rnpgbe_msix_other(int irq, void *data)
{
	return IRQ_HANDLED;
}

static void rnpgbe_irq_disable_queues(struct mucse_q_vector *q_vector)
{
	struct mucse_ring *ring;

	/* tx/rx use one register, different bit */
	mucse_for_each_ring(ring, q_vector->tx) {
		writel(INT_VALID, ring->trig);
		writel((RX_INT_MASK | TX_INT_MASK), ring->irq_mask);
	}
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

	rnpgbe_irq_enable_queues(q_vector);

	return 0;
}

/**
 * register_mbx_irq - Register mbx routine
 * @mucse: pointer to private structure
 *
 * @return: 0 on success, negative on failure
 **/
int register_mbx_irq(struct mucse *mucse)
{
	struct net_device *netdev = mucse->netdev;
	struct pci_dev *pdev = mucse->pdev;
	int err = 0;

	if (mucse->flags & M_FLAG_MSIX_EN) {
		err = request_irq(pci_irq_vector(pdev, 0),
				  rnpgbe_msix_other, 0, netdev->name,
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

	v_budget = min_t(int, mucse->num_tx_queues, mucse->num_rx_queues);
	v_budget = min_t(int, v_budget, num_online_cpus());
	/* add one vector for mbx */
	v_budget += 1;
	v_budget = pci_alloc_irq_vectors(mucse->pdev, 1, v_budget,
					 PCI_IRQ_ALL_TYPES);
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
		/* msi/legacy use only 1 irq */
		mucse->num_q_vectors = 1;

		if (mucse->pdev->msi_enabled)
			mucse->flags |= M_FLAG_MSI_EN;
		else
			mucse->flags |= M_FLAG_LEGACY_EN;
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
	int ring_count, size;

	txr_count = r_count;
	rxr_count = r_count;
	ring_count = txr_count + rxr_count;
	size = sizeof(struct mucse_q_vector) +
	       (sizeof(struct mucse_ring) * ring_count);

	q_vector = kzalloc(size, GFP_KERNEL);
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
		mucse_add_ring(ring, &q_vector->tx);
		ring->queue_index = eth_queue_idx + idx;
		ring->rnpgbe_queue_idx = txr_idx;
		ring->ring_addr = hw->hw_addr + RING_OFFSET(txr_idx);
		ring->irq_mask = ring->ring_addr + RNPGBE_DMA_INT_MASK;
		ring->trig = ring->ring_addr + RNPGBE_DMA_INT_TRIG;
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
			M_FLAG_MSI_EN |
			M_FLAG_LEGACY_EN);
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
 * rnpgbe_intr - Msi/Legacy irq handler
 * @irq: irq num
 * @data: private data
 * @return: IRQ_HANDLED
 **/
static irqreturn_t rnpgbe_intr(int irq, void *data)
{
	struct mucse *mucse = (struct mucse *)data;
	struct mucse_q_vector *q_vector;

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

			snprintf(q_vector->name, sizeof(q_vector->name) - 1,
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
		/* Generic irq handler for all single-vector modes */
		err = request_irq(pci_irq_vector(pdev, 0),
				  rnpgbe_intr, 0, netdev->name,
				  mucse);
		if (err)
			return err;
	}

	if (mucse->flags & M_FLAG_LEGACY_EN) {
		mucse_hw_wr32(hw, RNPGBE_LEGACY_ENABLE, 1);
		mucse_hw_wr32(hw, RNPGBE_LEGACY_TIME, 0x200);
	} else {
		mucse_hw_wr32(hw, RNPGBE_LEGACY_ENABLE, 0);
	}

	return 0;
err_free_irqs:
	while (i >= 0) {
		i--;
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
 * @msix_vector: the vector num to map to the corresponding queue
 *
 */
static void rnpgbe_set_ring_vector(struct mucse *mucse,
				   u8 queue, u8 msix_vector)
{
	struct mucse_hw *hw = &mucse->hw;
	u32 data;

	data = hw->pfvfnum << 24;
	data |= (msix_vector << 8);
	data |= msix_vector;
	writel(data, hw->ring_msix_base + RING_VECTOR(queue));
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

	if (!(mucse->flags & (M_FLAG_MSIX_EN | M_FLAG_MSIX_SINGLE_EN)))
		return;

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

void rnpgbe_down(struct mucse *mucse)
{
	rnpgbe_irq_disable(mucse);
	rnpgbe_napi_disable_all(mucse);
}

/**
 * rnpgbe_up_complete - Final step for port up
 * @mucse: pointer to private structure
 **/
void rnpgbe_up_complete(struct mucse *mucse)
{
	rnpgbe_configure_msix(mucse);
	rnpgbe_napi_enable_all(mucse);
	rnpgbe_irq_enable(mucse);
}
