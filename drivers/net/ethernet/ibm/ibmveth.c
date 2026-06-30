// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * IBM Power Virtual Ethernet Device Driver
 *
 * Copyright (C) IBM Corporation, 2003, 2010
 *
 * Authors: Dave Larson <larson1@us.ibm.com>
 *	    Santiago Leon <santil@linux.vnet.ibm.com>
 *	    Brian King <brking@linux.vnet.ibm.com>
 *	    Robert Jennings <rcj@linux.vnet.ibm.com>
 *	    Anton Blanchard <anton@au.ibm.com>
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/mm.h>
#include <linux/pm.h>
#include <linux/ethtool.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <asm/hvcall.h>
#include <linux/atomic.h>
#include <asm/vio.h>
#include <asm/iommu.h>
#include <asm/firmware.h>
#include <net/tcp.h>
#include <net/ip6_checksum.h>

#include "ibmveth.h"

static irqreturn_t ibmveth_interrupt(int irq, void *dev_instance);
static unsigned long ibmveth_get_desired_dma(struct vio_dev *vdev);

static struct kobj_type ktype_veth_pool;


static const char ibmveth_driver_name[] = "ibmveth";
static const char ibmveth_driver_string[] = "IBM Power Virtual Ethernet Driver";
#define ibmveth_driver_version "1.06"

MODULE_AUTHOR("Santiago Leon <santil@linux.vnet.ibm.com>");
MODULE_DESCRIPTION("IBM Power Virtual Ethernet Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(ibmveth_driver_version);

static unsigned int tx_copybreak __read_mostly = 128;
module_param(tx_copybreak, uint, 0644);
MODULE_PARM_DESC(tx_copybreak,
	"Maximum size of packet that is copied to a new buffer on transmit");

static unsigned int rx_copybreak __read_mostly = 128;
module_param(rx_copybreak, uint, 0644);
MODULE_PARM_DESC(rx_copybreak,
	"Maximum size of packet that is copied to a new buffer on receive");

static unsigned int rx_flush __read_mostly = 0;
module_param(rx_flush, uint, 0644);
MODULE_PARM_DESC(rx_flush, "Flush receive buffers before use");

static bool old_large_send __read_mostly;
module_param(old_large_send, bool, 0444);
MODULE_PARM_DESC(old_large_send,
	"Use old large send method on firmware that supports the new method");

struct ibmveth_stat {
	char name[ETH_GSTRING_LEN];
	int offset;
};

#define IBMVETH_STAT_OFF(stat) offsetof(struct ibmveth_adapter, stat)
#define IBMVETH_GET_STAT(a, off) *((u64 *)(((unsigned long)(a)) + off))

static struct ibmveth_stat ibmveth_stats[] = {
	{ "replenish_task_cycles", IBMVETH_STAT_OFF(replenish_task_cycles) },
	{ "replenish_no_mem", IBMVETH_STAT_OFF(replenish_no_mem) },
	{ "replenish_add_buff_failure",
			IBMVETH_STAT_OFF(replenish_add_buff_failure) },
	{ "replenish_add_buff_success",
			IBMVETH_STAT_OFF(replenish_add_buff_success) },
	{ "rx_invalid_buffer", IBMVETH_STAT_OFF(rx_invalid_buffer) },
	{ "rx_no_buffer", IBMVETH_STAT_OFF(rx_no_buffer) },
	{ "tx_map_failed", IBMVETH_STAT_OFF(tx_map_failed) },
	{ "tx_send_failed", IBMVETH_STAT_OFF(tx_send_failed) },
	{ "fw_enabled_ipv4_csum", IBMVETH_STAT_OFF(fw_ipv4_csum_support) },
	{ "fw_enabled_ipv6_csum", IBMVETH_STAT_OFF(fw_ipv6_csum_support) },
	{ "tx_large_packets", IBMVETH_STAT_OFF(tx_large_packets) },
	{ "rx_large_packets", IBMVETH_STAT_OFF(rx_large_packets) },
	{ "fw_enabled_large_send", IBMVETH_STAT_OFF(fw_large_send_support) },
	{ "hcall_reg_lan_queue", IBMVETH_STAT_OFF(hcall_stats.reg_lan_queue) },
	{ "hcall_reg_lan", IBMVETH_STAT_OFF(hcall_stats.reg_lan) },
	{ "hcall_add_bufs_queue", IBMVETH_STAT_OFF(hcall_stats.add_bufs_queue) },
	{ "hcall_add_bufs", IBMVETH_STAT_OFF(hcall_stats.add_bufs) },
	{ "hcall_add_buf", IBMVETH_STAT_OFF(hcall_stats.add_buf) },
	{ "hcall_free_lan_queue", IBMVETH_STAT_OFF(hcall_stats.free_lan_queue) },
	{ "hcall_free_lan", IBMVETH_STAT_OFF(hcall_stats.free_lan) },
	{ "hcall_send_lan", IBMVETH_STAT_OFF(hcall_stats.send_lan) },
};

/* simple methods of getting data from the current rxq entry */
static inline u32 ibmveth_rxq_flags(struct ibmveth_adapter *adapter,
				    int queue_index)
{
	struct ibmveth_rx_q *rxq = &adapter->rx_queue[queue_index];

	return be32_to_cpu(rxq->queue_addr[rxq->index].flags_off);
}

static inline int ibmveth_rxq_toggle(struct ibmveth_adapter *adapter,
				     int queue_index)
{
	return (ibmveth_rxq_flags(adapter, queue_index) & IBMVETH_RXQ_TOGGLE) >>
		IBMVETH_RXQ_TOGGLE_SHIFT;
}

static inline int ibmveth_rxq_pending_buffer(struct ibmveth_adapter *adapter,
					     int queue_index)
{
	return ibmveth_rxq_toggle(adapter, queue_index) ==
		adapter->rx_queue[queue_index].toggle;
}

static inline int ibmveth_rxq_buffer_valid(struct ibmveth_adapter *adapter,
					   int queue_index)
{
	return ibmveth_rxq_flags(adapter, queue_index) & IBMVETH_RXQ_VALID;
}

static inline int ibmveth_rxq_frame_offset(struct ibmveth_adapter *adapter,
					   int queue_index)
{
	return ibmveth_rxq_flags(adapter, queue_index) & IBMVETH_RXQ_OFF_MASK;
}

static inline int ibmveth_rxq_large_packet(struct ibmveth_adapter *adapter,
					   int queue_index)
{
	return ibmveth_rxq_flags(adapter, queue_index) & IBMVETH_RXQ_LRG_PKT;
}

static inline int ibmveth_rxq_frame_length(struct ibmveth_adapter *adapter,
					   int queue_index)
{
	struct ibmveth_rx_q *rxq = &adapter->rx_queue[queue_index];

	return be32_to_cpu(rxq->queue_addr[rxq->index].length);
}

static inline int ibmveth_rxq_csum_good(struct ibmveth_adapter *adapter,
					int queue_index)
{
	return ibmveth_rxq_flags(adapter, queue_index) & IBMVETH_RXQ_CSUM_GOOD;
}

static unsigned int ibmveth_real_max_tx_queues(void)
{
	unsigned int n_cpu = num_online_cpus();

	return min(n_cpu, IBMVETH_MAX_QUEUES);
}

/**
 * ibmveth_alloc_filter_list - Allocate and map filter list
 * @adapter: ibmveth adapter structure
 *
 * Return: 0 on success, negative error code on failure
 */
static int ibmveth_alloc_filter_list(struct ibmveth_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	struct net_device *netdev = adapter->netdev;

	adapter->filter_list_addr = (void *)get_zeroed_page(GFP_KERNEL);
	if (!adapter->filter_list_addr) {
		netdev_err(netdev, "unable to allocate filter pages\n");
		return -ENOMEM;
	}

	adapter->filter_list_dma = dma_map_single(dev,
						  adapter->filter_list_addr,
						  4096, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, adapter->filter_list_dma)) {
		netdev_err(netdev, "unable to map filter list pages\n");
		free_page((unsigned long)adapter->filter_list_addr);
		adapter->filter_list_addr = NULL;
		return -ENOMEM;
	}

	netdev_dbg(netdev, "filter list @ 0x%p (DMA: 0x%llx)\n",
		   adapter->filter_list_addr,
		   (unsigned long long)adapter->filter_list_dma);

	return 0;
}

/**
 * ibmveth_free_filter_list - Free filter list resources
 * @adapter: ibmveth adapter structure
 */
static void ibmveth_free_filter_list(struct ibmveth_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;

	if (adapter->filter_list_dma) {
		dma_unmap_single(dev, adapter->filter_list_dma, 4096,
				 DMA_BIDIRECTIONAL);
		adapter->filter_list_dma = 0;
	}

	if (adapter->filter_list_addr) {
		free_page((unsigned long)adapter->filter_list_addr);
		adapter->filter_list_addr = NULL;
	}
}

/**
 * ibmveth_alloc_rx_qstats - Allocate per-queue RX statistics
 * @adapter: ibmveth adapter structure
 *
 * Return: 0 on success, -ENOMEM on failure
 */
static int ibmveth_alloc_rx_qstats(struct ibmveth_adapter *adapter)
{
	adapter->rx_qstats = kcalloc(IBMVETH_MAX_RX_QUEUES,
				     sizeof(struct ibmveth_rx_queue_stats),
				     GFP_KERNEL);
	if (!adapter->rx_qstats)
		return -ENOMEM;

	return 0;
}

/**
 * ibmveth_free_rx_qstats - Free per-queue RX statistics
 * @adapter: ibmveth adapter structure
 */
static void ibmveth_free_rx_qstats(struct ibmveth_adapter *adapter)
{
	kfree(adapter->rx_qstats);
	adapter->rx_qstats = NULL;
}

/**
 * ibmveth_alloc_tx_qstats - Allocate per-queue TX statistics
 * @adapter: ibmveth adapter structure
 *
 * Return: 0 on success, -ENOMEM on failure
 */
static int ibmveth_alloc_tx_qstats(struct ibmveth_adapter *adapter)
{
	adapter->tx_qstats = kcalloc(IBMVETH_MAX_QUEUES,
				     sizeof(struct ibmveth_tx_queue_stats),
				     GFP_KERNEL);
	if (!adapter->tx_qstats)
		return -ENOMEM;

	return 0;
}

/**
 * ibmveth_free_tx_qstats - Free per-queue TX statistics
 * @adapter: ibmveth adapter structure
 */
static void ibmveth_free_tx_qstats(struct ibmveth_adapter *adapter)
{
	kfree(adapter->tx_qstats);
	adapter->tx_qstats = NULL;
}

/**
 * ibmveth_alloc_rx_queues - Allocate per-queue RX resources
 * @adapter: ibmveth adapter structure
 * @rxq_entries: Number of entries per RX queue
 *
 * Return: 0 on success, negative error code on failure
 */
static int
ibmveth_alloc_rx_queues(struct ibmveth_adapter *adapter, int rxq_entries)
{
	struct device *dev = &adapter->vdev->dev;
	struct net_device *netdev = adapter->netdev;
	int i;

	for (i = 0; i < adapter->num_rx_queues; i++) {
		adapter->buffer_list_addr[i] = (void *)get_zeroed_page(GFP_KERNEL);
		if (!adapter->buffer_list_addr[i]) {
			netdev_err(netdev, "unable to allocate buffer list for queue %d\n", i);
			goto err_cleanup;
		}

		adapter->rx_queue[i].queue_len =
			sizeof(struct ibmveth_rx_q_entry) * rxq_entries;
		adapter->rx_queue[i].queue_addr =
			dma_alloc_coherent(dev, adapter->rx_queue[i].queue_len,
					   &adapter->rx_queue[i].queue_dma,
					   GFP_KERNEL);
		if (!adapter->rx_queue[i].queue_addr) {
			netdev_err(netdev, "unable to allocate RX queue for queue %d\n", i);
			goto err_cleanup;
		}

		adapter->buffer_list_dma[i] = dma_map_single(dev,
							     adapter->buffer_list_addr[i],
							     4096, DMA_BIDIRECTIONAL);
		if (dma_mapping_error(dev, adapter->buffer_list_dma[i])) {
			netdev_err(netdev, "unable to map buffer list for queue %d\n", i);
			adapter->buffer_list_dma[i] = 0;
			goto err_cleanup;
		}

		adapter->rx_queue[i].index = 0;
		adapter->rx_queue[i].num_slots = rxq_entries;
		adapter->rx_queue[i].toggle = 1;
		spin_lock_init(&adapter->rx_queue[i].replenish_lock);

		netdev_dbg(netdev, "queue %d: buffer_list @ 0x%p (DMA: 0x%llx), rx_queue @ 0x%p (DMA: 0x%llx), %llu entries\n",
			   i, adapter->buffer_list_addr[i],
			   (unsigned long long)adapter->buffer_list_dma[i],
			   adapter->rx_queue[i].queue_addr,
			   (unsigned long long)adapter->rx_queue[i].queue_dma,
			   (unsigned long long)rxq_entries);
	}

	netdev_dbg(netdev, "allocated %d RX queue(s) with %d entries each\n",
		   adapter->num_rx_queues, rxq_entries);

	return 0;

err_cleanup:
	/* Clean up previously allocated queues */
	for (; i >= 0; i--) {
		if (adapter->buffer_list_dma[i]) {
			dma_unmap_single(dev, adapter->buffer_list_dma[i],
					 4096, DMA_BIDIRECTIONAL);
			adapter->buffer_list_dma[i] = 0;
		}
		if (adapter->rx_queue[i].queue_addr) {
			dma_free_coherent(dev, adapter->rx_queue[i].queue_len,
					  adapter->rx_queue[i].queue_addr,
					  adapter->rx_queue[i].queue_dma);
			adapter->rx_queue[i].queue_addr = NULL;
		}
		if (adapter->buffer_list_addr[i]) {
			free_page((unsigned long)adapter->buffer_list_addr[i]);
			adapter->buffer_list_addr[i] = NULL;
		}
	}

	return -ENOMEM;
}

/**
 * ibmveth_cleanup_rx_resources - Free all RX queue resources
 * @adapter: ibmveth adapter structure
 */
static void ibmveth_cleanup_rx_resources(struct ibmveth_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	int i;

	netdev_dbg(adapter->netdev, "cleaning up %d RX queue(s)\n",
		   adapter->num_rx_queues);

	for (i = 0; i < adapter->num_rx_queues; i++) {
		if (adapter->buffer_list_dma[i]) {
			dma_unmap_single(dev, adapter->buffer_list_dma[i],
					 4096, DMA_BIDIRECTIONAL);
			adapter->buffer_list_dma[i] = 0;
		}

		if (adapter->rx_queue[i].queue_addr) {
			dma_free_coherent(dev, adapter->rx_queue[i].queue_len,
					  adapter->rx_queue[i].queue_addr,
					  adapter->rx_queue[i].queue_dma);
			adapter->rx_queue[i].queue_addr = NULL;
		}

		if (adapter->buffer_list_addr[i]) {
			free_page((unsigned long)adapter->buffer_list_addr[i]);
			adapter->buffer_list_addr[i] = NULL;
		}
	}
}

/**
 * ibmveth_toggle_irq - Common helper to enable/disable queue interrupts
 * @adapter: ibmveth adapter structure
 * @queue_index: Index of the queue (0 for primary, 1+ for subordinate)
 * @enable: true to enable, false to disable
 *
 * For queue 0 (primary), uses h_vio_signal() as it's registered via
 * h_register_logical_lan(). For subordinate queues (1+), uses H_VIOCTL
 * with H_ENABLE/DISABLE_VIO_INTERRUPT for per-queue interrupt control.
 *
 * Return: 0 on success, error code otherwise
 */
static int
ibmveth_toggle_irq(struct ibmveth_adapter *adapter, int queue_index, bool enable)
{
	unsigned long rc;
	unsigned long irq = adapter->queue_irq[queue_index];
	const char *action = enable ? "enable" : "disable";

	if (queue_index == 0) {
		/* Primary queue: use h_vio_signal() */
		rc = h_vio_signal(adapter->vdev->unit_address,
				  enable ? VIO_IRQ_ENABLE : VIO_IRQ_DISABLE);
	} else {
		/* Subordinate queues: use H_VIOCTL with hardware IRQ */
		struct irq_data *irq_data = irq_get_irq_data(irq);
		irq_hw_number_t hwirq;
		u64 vioctl_cmd = enable ? H_ENABLE_VIO_INTERRUPT : H_DISABLE_VIO_INTERRUPT;

		if (!irq_data) {
			netdev_err(adapter->netdev,
				   "Failed to get IRQ data for queue %d (virq=%lu)\n",
				   queue_index, irq);
			return -EINVAL;
		}

		hwirq = irqd_to_hwirq(irq_data);
		rc = plpar_hcall_norets(H_VIOCTL,
					adapter->vdev->unit_address,
					vioctl_cmd,
					hwirq, 0, 0);

		if (rc == H_PARAMETER) {
			/* H_PARAMETER is non-fatal when IRQ is already in the requested state. */
			netdev_warn_once(adapter->netdev,
					 "H_VIOCTL %s IRQ returned H_PARAMETER for queue %d (hwirq=%lu)\n",
					 action, queue_index, hwirq);
			return 0;
		}
	}

	if (rc)
		netdev_err(adapter->netdev,
			   "Failed to %s IRQ for queue %d, rc=%ld\n",
			   action, queue_index, rc);
	return rc;
}

/**
 * ibmveth_disable_irq - Disable interrupt for a specific queue
 * @adapter: ibmveth adapter structure
 * @queue_index: Index of the queue (0 for primary, 1+ for subordinate)
 *
 * Return: 0 on success, error code otherwise
 */
static int
ibmveth_disable_irq(struct ibmveth_adapter *adapter, int queue_index)
{
	return ibmveth_toggle_irq(adapter, queue_index, false);
}

/**
 * ibmveth_enable_irq - Enable interrupt for a specific queue
 * @adapter: ibmveth adapter structure
 * @queue_index: Index of the queue (0 for primary, 1+ for subordinate)
 *
 * Return: 0 on success, error code otherwise
 */
static int
ibmveth_enable_irq(struct ibmveth_adapter *adapter, int queue_index)
{
	return ibmveth_toggle_irq(adapter, queue_index, true);
}

/**
 * ibmveth_dispose_subordinate_irq_mappings - Drop virq mappings for queues 1..N
 * @adapter: ibmveth adapter structure
 *
 * Subordinate queues get mappings from irq_create_mapping() during PHYP
 * registration.  Queue 0 uses netdev->irq from device tree and is left alone.
 * Call after free_irq() when handlers were installed, or alone when open
 * fails during register_rx_queues() before request_irq().
 */
static void
ibmveth_dispose_subordinate_irq_mappings(struct ibmveth_adapter *adapter)
{
	int i;

	for (i = 1; i < adapter->num_rx_queues; i++) {
		if (adapter->queue_irq[i]) {
			irq_dispose_mapping(adapter->queue_irq[i]);
			adapter->queue_irq[i] = 0;
		}
	}
}

/**
 * ibmveth_setup_rx_interrupts - Register IRQ handlers and enable NAPI
 * @adapter: ibmveth adapter structure
 *
 * Registers interrupt handlers for all RX queues and enables NAPI polling.
 * For multi-queue mode, enables hypervisor interrupt delivery only after
 * every queue has a Linux handler installed.
 *
 * Return: 0 on success, negative error code on failure
 */
static int
ibmveth_setup_rx_interrupts(struct ibmveth_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int i, rc, num = adapter->num_rx_queues;

	for (i = 0; i < num; i++) {
		if (!adapter->queue_irq[i]) {
			netdev_err(netdev, "queue %d has invalid IRQ (0)\n", i);
			rc = -EINVAL;
			goto err_free_irqs;
		}

		rc = request_irq(adapter->queue_irq[i], ibmveth_interrupt,
				 0, netdev->name, &adapter->napi[i]);
		if (rc) {
			netdev_err(netdev,
				   "request_irq() failed for irq 0x%x queue %d: %d\n",
				   adapter->queue_irq[i], i, rc);
			goto err_free_irqs;
		}
	}

	for (i = 0; i < num; i++)
		napi_enable(&adapter->napi[i]);

	if (adapter->multi_queue && num > 1) {
		for (i = 0; i < num; i++) {
			rc = ibmveth_enable_irq(adapter, i);
			if (rc) {
				netdev_err(netdev,
					   "Failed to enable IRQ for queue %d, rc=%d\n",
					   i, rc);
				while (--i >= 0)
					ibmveth_disable_irq(adapter, i);
				rc = -EIO;
				goto err_disable_napi;
			}
		}
	}

	return 0;

err_disable_napi:
	for (i = 0; i < num; i++)
		napi_disable(&adapter->napi[i]);
	i = num;
err_free_irqs:
	while (--i >= 0)
		free_irq(adapter->queue_irq[i], &adapter->napi[i]);
	ibmveth_dispose_subordinate_irq_mappings(adapter);
	return rc;
}

/**
 * ibmveth_cleanup_rx_interrupts - Disable NAPI and free IRQs
 * @adapter: ibmveth adapter structure
 *
 * Disables NAPI polling and frees interrupt handlers for all RX queues.
 */
static void
ibmveth_cleanup_rx_interrupts(struct ibmveth_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_rx_queues; i++)
		napi_disable(&adapter->napi[i]);

	for (i = 0; i < adapter->num_rx_queues; i++) {
		if (adapter->queue_irq[i])
			free_irq(adapter->queue_irq[i], &adapter->napi[i]);
	}

	ibmveth_dispose_subordinate_irq_mappings(adapter);

	/* Clear queue 0 IRQ number */
	adapter->queue_irq[0] = 0;
}

/**
 * ibmveth_setup_single_rx_interrupt - Setup interrupt for a single RX queue
 * @adapter: ibmveth adapter structure
 * @queue_idx: Queue index to setup
 *
 * Registers the IRQ handler for one queue. Used during incremental
 * scale-up when adding new RX queues; the caller enables NAPI via
 * napi_enable() after ibmveth_enable_irq().
 *
 * Return: 0 on success, negative error code on failure
 */
static int
ibmveth_setup_single_rx_interrupt(struct ibmveth_adapter *adapter,
				  int queue_idx)
{
	struct net_device *netdev = adapter->netdev;
	int rc;

	rc = request_irq(adapter->queue_irq[queue_idx], ibmveth_interrupt,
			 0, netdev->name, &adapter->napi[queue_idx]);
	if (rc) {
		netdev_err(netdev, "request_irq() failed for queue %d: %d\n",
			   queue_idx, rc);
		return rc;
	}

	netdev_dbg(netdev, "Setup IRQ %d for queue %d\n",
		   adapter->queue_irq[queue_idx], queue_idx);
	return 0;
}

/**
 * ibmveth_cleanup_single_rx_interrupt - Cleanup interrupt for a single RX queue
 * @adapter: ibmveth adapter structure
 * @queue_idx: Queue index to cleanup
 *
 * Frees the IRQ handler for one queue. Used during incremental scale-down.
 */
static void
ibmveth_cleanup_single_rx_interrupt(struct ibmveth_adapter *adapter,
				    int queue_idx)
{
	if (adapter->queue_irq[queue_idx]) {
		free_irq(adapter->queue_irq[queue_idx], &adapter->napi[queue_idx]);
		netdev_dbg(adapter->netdev, "Freed IRQ for queue %d\n", queue_idx);
	}
}

/* setup the initial settings for a buffer pool */
static void ibmveth_init_buffer_pool(struct ibmveth_buff_pool *pool,
				     u32 pool_index, u32 pool_size,
				     u32 buff_size, u32 pool_active)
{
	pool->size = pool_size;
	pool->index = pool_index;
	pool->buff_size = buff_size;
	pool->threshold = pool_size * 7 / 8;
	pool->active = pool_active;
}

/* allocate and setup an buffer pool - called during open */
static int ibmveth_alloc_buffer_pool(struct ibmveth_buff_pool *pool)
{
	int i;

	pool->free_map = kmalloc_array(pool->size, sizeof(u16), GFP_KERNEL);

	if (!pool->free_map)
		return -1;

	pool->dma_addr = kzalloc_objs(dma_addr_t, pool->size);
	if (!pool->dma_addr) {
		kfree(pool->free_map);
		pool->free_map = NULL;
		return -1;
	}

	pool->skbuff = kcalloc(pool->size, sizeof(void *), GFP_KERNEL);

	if (!pool->skbuff) {
		kfree(pool->dma_addr);
		pool->dma_addr = NULL;

		kfree(pool->free_map);
		pool->free_map = NULL;
		return -1;
	}

	for (i = 0; i < pool->size; ++i)
		pool->free_map[i] = i;

	atomic_set(&pool->available, 0);
	pool->producer_index = 0;
	pool->consumer_index = 0;

	return 0;
}

static inline void ibmveth_flush_buffer(void *addr, unsigned long length)
{
	unsigned long offset;

	for (offset = 0; offset < length; offset += SMP_CACHE_BYTES)
		asm("dcbf %0,%1,1" :: "b" (addr), "r" (offset));
}

/**
 * ibmveth_add_logical_lan_buffers - Add receive buffers to hypervisor
 * @adapter: ibmveth adapter structure
 * @descs: array of buffer descriptors to add
 * @filled: number of valid descriptors in the array
 * @buff_size: size of each buffer (multi-queue mode only)
 * @queue_index: RX queue index
 *
 * Return: hypervisor return code
 */
static long ibmveth_add_logical_lan_buffers(struct ibmveth_adapter *adapter,
					    union ibmveth_buf_desc *descs,
					    int filled,
					    unsigned long buff_size,
					    int queue_index)
{
	struct vio_dev *vdev = adapter->vdev;
	unsigned long rc;

	if (adapter->multi_queue) {
		unsigned long buffersznum = (buff_size << 32) | filled;
		unsigned long ioba[IBMVETH_MAX_RX_PER_HCALL / 2] = {0};
		int i;

		/* Pack descriptor addresses into ioba pairs.
		 * Each ioba holds two 32-bit addresses packed into 64 bits:
		 * - Even descriptors (0,2,4...) go in high 32 bits
		 * - Odd descriptors (1,3,5...) go in low 32 bits
		 */
		for (i = 0; i < filled && i < IBMVETH_MAX_RX_PER_HCALL; i++) {
			int pair_idx = i / 2;           /* Which pair: 0-5 */
			int is_high = (i % 2 == 0);     /* High or low 32 bits */

			if (is_high)
				ioba[pair_idx] = (unsigned long)descs[i].fields.address << 32;
			else
				ioba[pair_idx] |= descs[i].fields.address;
		}

		rc = h_add_logical_lan_buffers_queue(vdev->unit_address,
						     adapter->queue_handle[queue_index],
						     buffersznum,
						     ioba[0], ioba[1], ioba[2],
						     ioba[3], ioba[4], ioba[5]);
		adapter->hcall_stats.add_bufs_queue++;
	} else if (filled == 1) {
		rc = h_add_logical_lan_buffer(vdev->unit_address,
					      descs[0].desc);
		adapter->hcall_stats.add_buf++;
	} else {
		rc = h_add_logical_lan_buffers(vdev->unit_address,
					       descs[0].desc, descs[1].desc,
					       descs[2].desc, descs[3].desc,
					       descs[4].desc, descs[5].desc,
					       descs[6].desc, descs[7].desc);
		adapter->hcall_stats.add_bufs++;
	}

	return rc;
}

/* replenish the buffers for a pool.  note that we don't need to
 * skb_reserve these since they are used for incoming...
 */
static void ibmveth_replenish_buffer_pool(struct ibmveth_adapter *adapter,
					  struct ibmveth_buff_pool *pool,
					  int queue_index)
{
	union ibmveth_buf_desc descs[IBMVETH_MAX_RX_PER_HCALL] = {0};
	u32 remaining = pool->size - atomic_read(&pool->available);
	u64 correlators[IBMVETH_MAX_RX_PER_HCALL] = {0};
	unsigned long lpar_rc;
	u32 buffers_added = 0;
	u32 i, filled, batch;
	struct vio_dev *vdev;
	dma_addr_t dma_addr;
	struct device *dev;
	u32 index;

	vdev = adapter->vdev;
	dev = &vdev->dev;

	mb();

	batch = adapter->rx_buffers_per_hcall;

	while (remaining > 0) {
		unsigned int free_index = pool->consumer_index;

		/* Fill a batch of descriptors */
		for (filled = 0; filled < min(remaining, batch); filled++) {
			index = pool->free_map[free_index];
			if (WARN_ON(index == IBM_VETH_INVALID_MAP)) {
				adapter->replenish_add_buff_failure++;
				netdev_info(adapter->netdev,
					    "Invalid map index %u, reset\n",
					    index);
				schedule_work(&adapter->work);
				break;
			}

			if (!pool->skbuff[index]) {
				struct sk_buff *skb = NULL;

				skb = netdev_alloc_skb(adapter->netdev,
						       pool->buff_size);
				if (!skb) {
					adapter->replenish_no_mem++;
					adapter->replenish_add_buff_failure++;
					break;
				}

				dma_addr = dma_map_single(dev, skb->data,
							  pool->buff_size,
							  DMA_FROM_DEVICE);
				if (dma_mapping_error(dev, dma_addr)) {
					dev_kfree_skb_any(skb);
					adapter->replenish_add_buff_failure++;
					break;
				}

				pool->dma_addr[index] = dma_addr;
				pool->skbuff[index] = skb;
			} else {
				/* re-use case */
				dma_addr = pool->dma_addr[index];
			}

			if (rx_flush) {
				unsigned int len;

				len = adapter->netdev->mtu + IBMVETH_BUFF_OH;
				len = min(pool->buff_size, len);
				ibmveth_flush_buffer(pool->skbuff[index]->data,
						     len);
			}

			descs[filled].fields.flags_len = IBMVETH_BUF_VALID |
							  pool->buff_size;
			descs[filled].fields.address = dma_addr;

			correlators[filled] = ((u64)pool->index << 32) | index;
			*(u64 *)pool->skbuff[index]->data = correlators[filled];

			free_index++;
			if (free_index >= pool->size)
				free_index = 0;
		}

		if (!filled)
			break;

		lpar_rc = ibmveth_add_logical_lan_buffers(adapter, descs,
							  filled,
							  pool->buff_size,
							  queue_index);

		if (lpar_rc != H_SUCCESS) {
			dev_warn_ratelimited(dev,
					     "RX h_add_logical_lan %s failed: filled=%u, rc=%lu, batch=%u\n",
					     adapter->multi_queue ? "_queue" : "",
					     filled, lpar_rc, batch);
			goto hcall_failure;
		}

		/* Only update pool state after hcall succeeds */
		for (i = 0; i < filled; i++) {
			free_index = pool->consumer_index;
			pool->free_map[free_index] = IBM_VETH_INVALID_MAP;

			pool->consumer_index++;
			if (pool->consumer_index >= pool->size)
				pool->consumer_index = 0;
		}

		buffers_added += filled;
		adapter->replenish_add_buff_success += filled;
		remaining -= filled;

		memset(&descs, 0, sizeof(descs));
		memset(&correlators, 0, sizeof(correlators));
		continue;

hcall_failure:
		for (i = 0; i < filled; i++) {
			index = correlators[i] & 0xffffffffUL;
			dma_addr =  pool->dma_addr[index];

			if (pool->skbuff[index]) {
				if (dma_addr &&
				    !dma_mapping_error(dev, dma_addr))
					dma_unmap_single(dev, dma_addr,
							 pool->buff_size,
							 DMA_FROM_DEVICE);

				dev_kfree_skb_any(pool->skbuff[index]);
				pool->skbuff[index] = NULL;
			}
		}
		adapter->replenish_add_buff_failure += filled;

		if (lpar_rc == H_FUNCTION) {
			if (adapter->multi_queue) {
				netdev_err(adapter->netdev,
					   "Unexpected H_FUNCTION from multi-queue buffer add (queue=%d, batch=%d)\n",
					   queue_index, batch);
				break;
			} else if (batch > 1) {
				netdev_warn(adapter->netdev,
					    "H_FUNCTION from legacy batch buffer add (batch=%d), falling back to single buffer mode\n",
					    batch);
				adapter->rx_buffers_per_hcall = 1;
				continue;
			}
		}
		break;
	}

	mb();
	atomic_add(buffers_added, &(pool->available));
}

/*
 * The final 8 bytes of the buffer list is a counter of frames dropped
 * because there was not a buffer in the buffer list capable of holding
 * the frame.
 */
static void ibmveth_update_rx_no_buffer(struct ibmveth_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_rx_queues; i++) {
		__be64 *p = adapter->buffer_list_addr[i] + 4096 - 8;
		u64 drops = be64_to_cpup(p);

		if (adapter->rx_qstats)
			adapter->rx_qstats[i].no_buffer_drops = drops;
		if (i == 0)
			adapter->rx_no_buffer = drops;
	}
}

/* replenish routine */
static void ibmveth_replenish_task(struct ibmveth_adapter *adapter,
				   int queue_index)
{
	struct ibmveth_rx_q *rxq = &adapter->rx_queue[queue_index];
	unsigned long flags;
	int i;

	adapter->replenish_task_cycles++;

	if (queue_index >= adapter->num_rx_queues) {
		netdev_dbg(adapter->netdev,
			   "Skipping replenish for freed queue %d (num_queues=%d)\n",
			   queue_index, adapter->num_rx_queues);
		return;
	}

	spin_lock_irqsave(&rxq->replenish_lock, flags);

	for (i = (IBMVETH_NUM_BUFF_POOLS - 1); i >= 0; i--) {
		struct ibmveth_buff_pool *pool =
			&adapter->rx_buff_pool[queue_index][i];

		if (pool->active && pool->free_map &&
		    (atomic_read(&pool->available) < pool->threshold))
			ibmveth_replenish_buffer_pool(adapter, pool,
						      queue_index);
	}

	ibmveth_update_rx_no_buffer(adapter);

	spin_unlock_irqrestore(&rxq->replenish_lock, flags);
}

/* empty and free ana buffer pool - also used to do cleanup in error paths */
static void ibmveth_free_buffer_pool(struct ibmveth_adapter *adapter,
				     struct ibmveth_buff_pool *pool)
{
	int i;

	kfree(pool->free_map);
	pool->free_map = NULL;

	if (pool->skbuff && pool->dma_addr) {
		for (i = 0; i < pool->size; ++i) {
			struct sk_buff *skb = pool->skbuff[i];
			if (skb) {
				dma_unmap_single(&adapter->vdev->dev,
						 pool->dma_addr[i],
						 pool->buff_size,
						 DMA_FROM_DEVICE);
				dev_kfree_skb_any(skb);
				pool->skbuff[i] = NULL;
			}
		}
	}

	if (pool->dma_addr) {
		kfree(pool->dma_addr);
		pool->dma_addr = NULL;
	}

	if (pool->skbuff) {
		kfree(pool->skbuff);
		pool->skbuff = NULL;
	}
}

/**
 * ibmveth_alloc_queue_buffer_pools - Allocate buffer pools for a single queue
 * @adapter: ibmveth adapter structure
 * @queue: queue index
 *
 * Allocates all active buffer pools for the specified queue.
 * Pool metadata must be initialized before calling this function.
 *
 * Return: 0 on success, negative error code on failure
 */
static int ibmveth_alloc_queue_buffer_pools(struct ibmveth_adapter *adapter,
					    int queue)
{
	struct net_device *netdev = adapter->netdev;
	int i;

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
		if (!adapter->rx_buff_pool[queue][i].active)
			continue;

		if (ibmveth_alloc_buffer_pool(&adapter->rx_buff_pool[queue][i])) {
			netdev_err(netdev,
				   "unable to allocate buffer pool %d for queue %d (size=%u, count=%u)\n",
				   i, queue,
				   adapter->rx_buff_pool[queue][i].buff_size,
				   adapter->rx_buff_pool[queue][i].size);
			adapter->rx_buff_pool[queue][i].active = 0;

			/* Free pools allocated so far for this queue */
			while (--i >= 0) {
				if (adapter->rx_buff_pool[queue][i].active)
					ibmveth_free_buffer_pool(adapter,
								 &adapter->rx_buff_pool[queue][i]);
			}
			return -ENOMEM;
		}
	}

	return 0;
}

/**
 * ibmveth_free_queue_buffer_pools - Free buffer pools for a single queue
 * @adapter: ibmveth adapter structure
 * @queue: queue index
 *
 * Frees all active buffer pools for the specified queue.
 */
static void ibmveth_free_queue_buffer_pools(struct ibmveth_adapter *adapter,
					    int queue)
{
	int i;

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
		struct ibmveth_buff_pool *pool = &adapter->rx_buff_pool[queue][i];

		/* Free pool if it has allocated memory, regardless of active flag.
		 * Pools may have memory allocated but not marked active during
		 * queue scale-up, so we must check for actual allocations.
		 */
		if (pool->free_map || pool->dma_addr || pool->skbuff)
			ibmveth_free_buffer_pool(adapter, pool);
	}
}

/**
 * ibmveth_alloc_buffer_pools - Allocate buffer pools for all queues
 * @adapter: ibmveth adapter structure
 *
 * Initializes pool metadata for queues 1-N from queue 0 settings,
 * then allocates buffer pools for all queues using the helper function.
 *
 * Return: 0 on success, negative error code on failure
 */
static int ibmveth_alloc_buffer_pools(struct ibmveth_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int i, q, rc;

	/* Initialize pool metadata for queues 1-15 from queue 0 settings */
	for (q = 1; q < adapter->num_rx_queues; q++) {
		for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
			struct ibmveth_buff_pool *src = &adapter->rx_buff_pool[0][i];
			struct ibmveth_buff_pool *dst = &adapter->rx_buff_pool[q][i];

			dst->size = src->size;
			dst->index = src->index;
			dst->buff_size = src->buff_size;
			dst->threshold = src->threshold;
			dst->active = src->active;
		}
	}

	/* Allocate actual buffers for all queues */
	for (q = 0; q < adapter->num_rx_queues; q++) {
		rc = ibmveth_alloc_queue_buffer_pools(adapter, q);
		if (rc) {
			/* Free pools for all previous queues */
			while (--q >= 0)
				ibmveth_free_queue_buffer_pools(adapter, q);
			return rc;
		}
	}

	netdev_dbg(netdev, "allocated buffer pools for %d queue(s)\n",
		   adapter->num_rx_queues);
	return 0;
}

/**
 * ibmveth_free_buffer_pools - Free buffer pools for all queues
 * @adapter: ibmveth adapter structure
 *
 * Frees buffer pools for all queues using the helper function.
 */
static void ibmveth_free_buffer_pools(struct ibmveth_adapter *adapter)
{
	int q;

	/* Free buffer pools for all queues */
	for (q = 0; q < adapter->num_rx_queues; q++)
		ibmveth_free_queue_buffer_pools(adapter, q);

	netdev_dbg(adapter->netdev, "freed buffer pools for %d queue(s)\n",
		   adapter->num_rx_queues);
}

/**
 * ibmveth_alloc_single_rx_queue - Allocate resources for a single RX queue
 * @adapter: ibmveth adapter structure
 * @queue_idx: Queue index to allocate
 * @rxq_entries: Number of RX queue entries
 *
 * Allocates buffer list, RX queue, and per-queue buffer pools for one queue.
 * Used during incremental scale-up without affecting existing queues.
 *
 * Return: 0 on success, negative error code on failure
 */
static int
ibmveth_alloc_single_rx_queue(struct ibmveth_adapter *adapter, int queue_idx,
			      int rxq_entries)
{
	struct device *dev = &adapter->vdev->dev;
	struct net_device *netdev = adapter->netdev;
	int i, rc = -ENOMEM;

	adapter->buffer_list_addr[queue_idx] = (void *)get_zeroed_page(GFP_KERNEL);
	if (!adapter->buffer_list_addr[queue_idx]) {
		netdev_err(netdev, "unable to allocate buffer list for queue %d\n",
			   queue_idx);
		return -ENOMEM;
	}

	adapter->rx_queue[queue_idx].queue_len =
		sizeof(struct ibmveth_rx_q_entry) * rxq_entries;
	adapter->rx_queue[queue_idx].queue_addr =
		dma_alloc_coherent(dev, adapter->rx_queue[queue_idx].queue_len,
				   &adapter->rx_queue[queue_idx].queue_dma,
				   GFP_KERNEL);
	if (!adapter->rx_queue[queue_idx].queue_addr) {
		netdev_err(netdev, "unable to allocate RX queue for queue %d\n",
			   queue_idx);
		goto out_free_buflist;
	}

	adapter->buffer_list_dma[queue_idx] =
		dma_map_single(dev, adapter->buffer_list_addr[queue_idx],
			       4096, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, adapter->buffer_list_dma[queue_idx])) {
		netdev_err(netdev, "unable to map buffer list for queue %d\n",
			   queue_idx);
		goto out_free_rxq;
	}

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
		adapter->rx_buff_pool[queue_idx][i].size =
			adapter->rx_buff_pool[0][i].size;
		adapter->rx_buff_pool[queue_idx][i].buff_size =
			adapter->rx_buff_pool[0][i].buff_size;
		adapter->rx_buff_pool[queue_idx][i].threshold =
			adapter->rx_buff_pool[0][i].threshold;
		adapter->rx_buff_pool[queue_idx][i].active =
			adapter->rx_buff_pool[0][i].active;
	}

	rc = ibmveth_alloc_queue_buffer_pools(adapter, queue_idx);
	if (rc) {
		netdev_err(netdev,
			   "Failed to allocate buffer pools for queue %d\n",
			   queue_idx);
		goto out_unmap_buflist;
	}

	adapter->rx_queue[queue_idx].index = 0;
	adapter->rx_queue[queue_idx].num_slots = rxq_entries;
	adapter->rx_queue[queue_idx].toggle = 1;
	spin_lock_init(&adapter->rx_queue[queue_idx].replenish_lock);

	netdev_dbg(netdev,
		   "Allocated queue %d: buffer_list @ %p (DMA: 0x%llx), rx_queue @ %p (DMA: 0x%llx), %d entries\n",
		   queue_idx, adapter->buffer_list_addr[queue_idx],
		   (unsigned long long)adapter->buffer_list_dma[queue_idx],
		   adapter->rx_queue[queue_idx].queue_addr,
		   (unsigned long long)adapter->rx_queue[queue_idx].queue_dma,
		   rxq_entries);

	return 0;

out_unmap_buflist:
	dma_unmap_single(dev, adapter->buffer_list_dma[queue_idx],
			 4096, DMA_BIDIRECTIONAL);
	adapter->buffer_list_dma[queue_idx] = 0;
out_free_rxq:
	dma_free_coherent(dev, adapter->rx_queue[queue_idx].queue_len,
			  adapter->rx_queue[queue_idx].queue_addr,
			  adapter->rx_queue[queue_idx].queue_dma);
	adapter->rx_queue[queue_idx].queue_addr = NULL;
out_free_buflist:
	free_page((unsigned long)adapter->buffer_list_addr[queue_idx]);
	adapter->buffer_list_addr[queue_idx] = NULL;
	return rc;
}

/**
 * ibmveth_free_single_rx_queue - Free resources for a single RX queue
 * @adapter: ibmveth adapter structure
 * @queue_idx: Queue index to free
 *
 * Frees buffer list, RX queue, and per-queue buffer pools for one queue.
 * Used during incremental scale-down without affecting remaining queues.
 */
static void
ibmveth_free_single_rx_queue(struct ibmveth_adapter *adapter, int queue_idx)
{
	struct device *dev = &adapter->vdev->dev;

	ibmveth_free_queue_buffer_pools(adapter, queue_idx);

	if (adapter->buffer_list_dma[queue_idx]) {
		dma_unmap_single(dev, adapter->buffer_list_dma[queue_idx],
				 4096, DMA_BIDIRECTIONAL);
		adapter->buffer_list_dma[queue_idx] = 0;
	}

	if (adapter->rx_queue[queue_idx].queue_addr) {
		dma_free_coherent(dev, adapter->rx_queue[queue_idx].queue_len,
				  adapter->rx_queue[queue_idx].queue_addr,
				  adapter->rx_queue[queue_idx].queue_dma);
		adapter->rx_queue[queue_idx].queue_addr = NULL;
	}

	if (adapter->buffer_list_addr[queue_idx]) {
		free_page((unsigned long)adapter->buffer_list_addr[queue_idx]);
		adapter->buffer_list_addr[queue_idx] = NULL;
	}

	netdev_dbg(adapter->netdev, "Freed queue %d resources\n", queue_idx);
}

/**
 * ibmveth_remove_buffer_from_pool - remove a buffer from a pool
 * @adapter: adapter instance
 * @correlator: identifies pool and index
 * @reuse: whether to reuse buffer
 *
 * Return:
 * * %0       - success
 * * %-EINVAL - correlator maps to pool or index out of range
 * * %-EFAULT - pool and index map to null skb
 */
static int ibmveth_remove_buffer_from_pool(struct ibmveth_adapter *adapter,
					   u64 correlator, int queue_index,
					   bool reuse)
{
	unsigned int pool  = correlator >> 32;
	unsigned int index = correlator & 0xffffffffUL;
	unsigned int free_index;
	struct sk_buff *skb;

	if (WARN_ON(pool >= IBMVETH_NUM_BUFF_POOLS) ||
	    WARN_ON(index >= adapter->rx_buff_pool[queue_index][pool].size)) {
		schedule_work(&adapter->work);
		return -EINVAL;
	}

	skb = adapter->rx_buff_pool[queue_index][pool].skbuff[index];
	if (WARN_ON(!skb)) {
		schedule_work(&adapter->work);
		return -EFAULT;
	}

	/* if we are going to reuse the buffer then keep the pointers around
	 * but mark index as available. replenish will see the skb pointer and
	 * assume it is to be recycled.
	 */
	if (!reuse) {
		/* remove the skb pointer to mark free. actual freeing is done
		 * by upper level networking after gro_receive
		 */
		adapter->rx_buff_pool[queue_index][pool].skbuff[index] = NULL;

		dma_unmap_single(&adapter->vdev->dev,
				 adapter->rx_buff_pool[queue_index][pool].dma_addr[index],
				 adapter->rx_buff_pool[queue_index][pool].buff_size,
				 DMA_FROM_DEVICE);
	}

	free_index = adapter->rx_buff_pool[queue_index][pool].producer_index;
	adapter->rx_buff_pool[queue_index][pool].producer_index++;
	if (adapter->rx_buff_pool[queue_index][pool].producer_index >=
	    adapter->rx_buff_pool[queue_index][pool].size)
		adapter->rx_buff_pool[queue_index][pool].producer_index = 0;
	adapter->rx_buff_pool[queue_index][pool].free_map[free_index] = index;

	mb();

	atomic_dec(&adapter->rx_buff_pool[queue_index][pool].available);

	return 0;
}

/* get the current buffer on the rx queue */
static inline struct sk_buff *ibmveth_rxq_get_buffer(struct ibmveth_adapter *adapter,
						     int queue_index)
{
	struct ibmveth_rx_q *rxq = &adapter->rx_queue[queue_index];
	u64 correlator = rxq->queue_addr[rxq->index].correlator;
	unsigned int pool = correlator >> 32;
	unsigned int index = correlator & 0xffffffffUL;

	if (WARN_ON(pool >= IBMVETH_NUM_BUFF_POOLS) ||
	    WARN_ON(index >= adapter->rx_buff_pool[queue_index][pool].size)) {
		schedule_work(&adapter->work);
		return NULL;
	}

	return adapter->rx_buff_pool[queue_index][pool].skbuff[index];
}

/**
 * ibmveth_rxq_harvest_buffer - Harvest buffer from pool
 *
 * @adapter: pointer to adapter
 * @reuse:   whether to reuse buffer
 *
 * Context: called from ibmveth_poll
 *
 * Return:
 * * %0    - success
 * * other - non-zero return from ibmveth_remove_buffer_from_pool
 */
static int ibmveth_rxq_harvest_buffer(struct ibmveth_adapter *adapter,
				      int queue_index, bool reuse)
{
	struct ibmveth_rx_q *rxq = &adapter->rx_queue[queue_index];
	u64 cor;
	int rc;

	cor = rxq->queue_addr[rxq->index].correlator;
	rc = ibmveth_remove_buffer_from_pool(adapter, cor, queue_index, reuse);
	if (unlikely(rc))
		return rc;

	if (++rxq->index == rxq->num_slots) {
		rxq->index = 0;
		rxq->toggle = !rxq->toggle;
	}

	return 0;
}

/**
 * ibmveth_drain_rx_queue - Drain pending buffers from an RX queue
 * @adapter: ibmveth adapter structure
 * @queue_index: Queue index to drain
 *
 * Recycles all pending buffers back to the per-queue buffer pools.
 * Must be called with NAPI disabled for this queue.
 *
 * Return: Number of buffers drained
 */
static int
ibmveth_drain_rx_queue(struct ibmveth_adapter *adapter, int queue_index)
{
	struct net_device *netdev = adapter->netdev;
	int drained = 0;
	int limit = adapter->rx_queue[queue_index].num_slots;
	int rc;

	netdev_dbg(netdev, "Draining RX queue %d (limit: %d slots)\n",
		   queue_index, limit);

	while (drained < limit &&
	       ibmveth_rxq_pending_buffer(adapter, queue_index)) {
		rc = ibmveth_rxq_harvest_buffer(adapter, queue_index, true);
		if (rc) {
			netdev_err(netdev,
				   "Failed to harvest buffer from queue %d during drain: %d\n",
				   queue_index, rc);
			break;
		}
		drained++;
	}

	if (drained > 0)
		netdev_dbg(netdev, "Drained %d buffer(s) from RX queue %d\n",
			   drained, queue_index);
	else
		netdev_dbg(netdev, "No buffers to drain from RX queue %d\n",
			   queue_index);

	return drained;
}

static void ibmveth_free_tx_ltb(struct ibmveth_adapter *adapter, int idx)
{
	dma_unmap_single(&adapter->vdev->dev, adapter->tx_ltb_dma[idx],
			 adapter->tx_ltb_size, DMA_TO_DEVICE);
	kfree(adapter->tx_ltb_ptr[idx]);
	adapter->tx_ltb_ptr[idx] = NULL;
}

static int ibmveth_allocate_tx_ltb(struct ibmveth_adapter *adapter, int idx)
{
	adapter->tx_ltb_ptr[idx] = kzalloc(adapter->tx_ltb_size,
					   GFP_KERNEL);
	if (!adapter->tx_ltb_ptr[idx]) {
		netdev_err(adapter->netdev,
			   "unable to allocate tx long term buffer\n");
		return -ENOMEM;
	}
	adapter->tx_ltb_dma[idx] = dma_map_single(&adapter->vdev->dev,
						  adapter->tx_ltb_ptr[idx],
						  adapter->tx_ltb_size,
						  DMA_TO_DEVICE);
	if (dma_mapping_error(&adapter->vdev->dev, adapter->tx_ltb_dma[idx])) {
		netdev_err(adapter->netdev,
			   "unable to DMA map tx long term buffer\n");
		kfree(adapter->tx_ltb_ptr[idx]);
		adapter->tx_ltb_ptr[idx] = NULL;
		return -ENOMEM;
	}

	return 0;
}

/**
 * ibmveth_alloc_tx_resources - Allocate TX resources for all queues
 * @adapter: ibmveth adapter structure
 *
 * Allocates TX Long Term Buffers (LTBs) for all TX queues.
 *
 * Return: 0 on success, -ENOMEM on failure
 */
static int
ibmveth_alloc_tx_resources(struct ibmveth_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int i;

	for (i = 0; i < netdev->real_num_tx_queues; i++) {
		if (ibmveth_allocate_tx_ltb(adapter, i))
			goto err_free_ltbs;
	}

	return 0;

err_free_ltbs:
	while (--i >= 0)
		ibmveth_free_tx_ltb(adapter, i);
	return -ENOMEM;
}

/**
 * ibmveth_free_tx_resources - Free TX resources for all queues
 * @adapter: ibmveth adapter structure
 *
 * Frees TX Long Term Buffers (LTBs) for all TX queues.
 */
static void
ibmveth_free_tx_resources(struct ibmveth_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int i;

	for (i = 0; i < netdev->real_num_tx_queues; i++)
		ibmveth_free_tx_ltb(adapter, i);
}

static int ibmveth_register_logical_lan(struct ibmveth_adapter *adapter,
					union ibmveth_buf_desc rxq_desc,
					u64 mac_address)
{
	int rc, try_again = 1;

	/*
	 * After a kexec the adapter will still be open, so our attempt to
	 * open it will fail. So if we get a failure we free the adapter and
	 * try again, but only once.
	 */
retry:
	/* In multi-queue mode, obtain a queue handle for queue 0 so all RX
	 * queues can use the same per-queue buffer hypercalls.
	 */
	if (adapter->multi_queue) {
		rc = h_register_logical_lan_with_handle(adapter->vdev->unit_address,
							adapter->buffer_list_dma[0],
							rxq_desc.desc,
							adapter->filter_list_dma,
							mac_address,
							&adapter->queue_handle[0]);
	} else {
		rc = h_register_logical_lan(adapter->vdev->unit_address,
					    adapter->buffer_list_dma[0],
					    rxq_desc.desc,
					    adapter->filter_list_dma,
					    mac_address);
	}
	adapter->hcall_stats.reg_lan++;

	if (rc != H_SUCCESS && try_again) {
		do {
			rc = h_free_logical_lan(adapter->vdev->unit_address);
			adapter->hcall_stats.free_lan++;
		} while (H_IS_LONG_BUSY(rc) || (rc == H_BUSY));

		try_again = 0;
		goto retry;
	}

	return rc;
}

/**
 * ibmveth_register_logical_lan_queue - Register subordinate queue with hypervisor
 * @adapter: ibmveth adapter structure
 * @rxq_desc: Receive queue descriptor
 * @queue_index: RX queue index (1..N for subordinate queues)
 *
 * Registers a subordinate receive queue using H_REG_LOGICAL_LAN_QUEUE.
 * On success, stores the queue handle and virtual IRQ in the adapter.
 * Retries once if registration fails (handles kexec case).  If IRQ mapping
 * fails after a successful hypervisor registration, the queue is freed
 * before returning.
 *
 * Return: H_SUCCESS on success, negative errno on IRQ mapping failure,
 *         hypervisor error code otherwise
 */
static int
ibmveth_register_logical_lan_queue(struct ibmveth_adapter *adapter,
				   union ibmveth_buf_desc rxq_desc,
				   int queue_index)
{
	unsigned long handle, hwirq;
	unsigned int virq;
	long lpar_rc;
	int try_again = 1;

retry:
	netdev_dbg(adapter->netdev,
		   "Attempting to register queue %d: unit_addr=0x%x buffer_list_dma=0x%llx rxq_desc=0x%llx\n",
		   queue_index, adapter->vdev->unit_address,
		   (unsigned long long)adapter->buffer_list_dma[queue_index],
		   (unsigned long long)rxq_desc.desc);

	lpar_rc = h_reg_logical_lan_queue(adapter->vdev->unit_address,
					  adapter->buffer_list_dma[queue_index],
					  rxq_desc.desc, &handle, &hwirq);
	adapter->hcall_stats.reg_lan_queue++;

	if (lpar_rc == H_SUCCESS) {
		virq = irq_create_mapping(NULL, hwirq);
		if (!virq) {
			unsigned long free_rc;

			netdev_err(adapter->netdev,
				   "Failed to map IRQ for queue %d (hwirq=%lu)\n",
				   queue_index, hwirq);
			do {
				free_rc = h_free_logical_lan_queue(adapter->vdev->unit_address,
								   handle);
			} while (H_IS_LONG_BUSY(free_rc) || (free_rc == H_BUSY));
			adapter->hcall_stats.free_lan_queue++;
			if (free_rc != H_SUCCESS)
				netdev_err(adapter->netdev,
					   "h_free_logical_lan_queue failed for queue %d after IRQ map failure: rc=0x%lx\n",
					   queue_index, free_rc);
			return -EINVAL;
		}

		adapter->queue_handle[queue_index] = handle;
		adapter->queue_irq[queue_index] = virq;

		netdev_dbg(adapter->netdev,
			   "queue %d registered: handle=0x%llx irq=%u\n",
			   queue_index, adapter->queue_handle[queue_index],
			   adapter->queue_irq[queue_index]);
		return H_SUCCESS;
	}

	if (lpar_rc == H_FUNCTION) {
		if (adapter->multi_queue) {
			netdev_info(adapter->netdev,
				    "Multi queue mode not supported by firmware, falling back to single queue\n");
			adapter->multi_queue = 0;
		} else {
			netdev_err(adapter->netdev,
				   "Unexpected H_FUNCTION for queue %d registration (MQ mode already disabled)\n",
				   queue_index);
		}
		return lpar_rc;
	}

	if (try_again) {
		try_again = 0;
		goto retry;
	}

	netdev_err(adapter->netdev,
		   "h_reg_logical_lan_queue failed with %ld after retry\n",
		   lpar_rc);
	netdev_err(adapter->netdev,
		   "queue %d params: unit_addr=0x%x buffer_list_dma=0x%llx rxq_desc=0x%llx\n",
		   queue_index, adapter->vdev->unit_address,
		   (unsigned long long)adapter->buffer_list_dma[queue_index],
		   (unsigned long long)rxq_desc.desc);

	return lpar_rc;
}

/**
 * ibmveth_register_single_rx_queue - Register one subordinate RX queue
 * @adapter: ibmveth adapter structure
 * @queue_idx: Queue index to register (1..N)
 * @mac_address: MAC address (unused; reserved for API symmetry)
 *
 * Builds the queue descriptor and registers with the hypervisor via
 * ibmveth_register_logical_lan_queue().
 *
 * Return: 0 on success, -EINVAL if @queue_idx is invalid, -EIO on failure
 */
static int
ibmveth_register_single_rx_queue(struct ibmveth_adapter *adapter,
				 int queue_idx, u64 mac_address)
{
	struct net_device *netdev = adapter->netdev;
	union ibmveth_buf_desc rxq_desc;
	long lpar_rc;

	(void)mac_address;

	if (WARN_ON(queue_idx < 1 || queue_idx >= IBMVETH_MAX_RX_QUEUES))
		return -EINVAL;

	rxq_desc.fields.flags_len = IBMVETH_BUF_VALID |
				    adapter->rx_queue[queue_idx].queue_len;
	rxq_desc.fields.address = adapter->rx_queue[queue_idx].queue_dma;

	lpar_rc = ibmveth_register_logical_lan_queue(adapter, rxq_desc,
						     queue_idx);
	if (lpar_rc != H_SUCCESS) {
		netdev_err(netdev, "Failed to register queue %d: rc=0x%lx\n",
			   queue_idx, lpar_rc);
		return -EIO;
	}

	netdev_dbg(netdev, "Registered queue %d with handle 0x%llx\n",
		   queue_idx, adapter->queue_handle[queue_idx]);

	return 0;
}

/**
 * ibmveth_deregister_single_rx_queue - Deregister one subordinate RX queue
 * @adapter: ibmveth adapter structure
 * @queue_idx: Queue index to deregister (1..N)
 *
 * Deregisters a single queue via H_FREE_LOGICAL_LAN_QUEUE and disposes
 * the IRQ mapping for subordinate queues. Queue 0 is freed only through
 * ibmveth_free_all_queues() (H_FREE_LOGICAL_LAN).
 */
static void
ibmveth_deregister_single_rx_queue(struct ibmveth_adapter *adapter,
				   int queue_idx)
{
	unsigned long lpar_rc;

	if (!adapter->queue_handle[queue_idx])
		return;

	do {
		lpar_rc = h_free_logical_lan_queue(adapter->vdev->unit_address,
						   adapter->queue_handle[queue_idx]);
	} while (H_IS_LONG_BUSY(lpar_rc) || (lpar_rc == H_BUSY));

	adapter->hcall_stats.free_lan_queue++;

	if (lpar_rc != H_SUCCESS) {
		netdev_err(adapter->netdev,
			   "h_free_logical_lan_queue failed for queue %d: rc=0x%lx\n",
			   queue_idx, lpar_rc);
	}

	adapter->queue_handle[queue_idx] = 0;

	if (queue_idx > 0 && adapter->queue_irq[queue_idx]) {
		irq_dispose_mapping(adapter->queue_irq[queue_idx]);
		adapter->queue_irq[queue_idx] = 0;
	}

	netdev_dbg(adapter->netdev, "Deregistered queue %d\n", queue_idx);
}

/**
 * ibmveth_resize_rx_queues_incremental - Resize RX queue count incrementally
 * @adapter: ibmveth adapter structure
 * @new_count: Target number of RX queues
 * @rxq_entries: Number of entries per RX queue
 *
 * Adds or removes RX queues without tearing down the entire adapter.
 * Active queues continue receiving during scale-up; scale-down drains
 * excess queues before deregistering them with the hypervisor.
 *
 * Return: 0 on success, negative error code on failure
 */
static int
ibmveth_resize_rx_queues_incremental(struct ibmveth_adapter *adapter,
				     int new_count, int rxq_entries)
{
	struct net_device *netdev = adapter->netdev;
	u64 mac_address = ether_addr_to_u64(netdev->dev_addr);
	int old_count = adapter->num_rx_queues;
	int failed_queue;
	int rc, i;

	if (old_count == new_count) {
		netdev_dbg(netdev, "RX queue count unchanged (%d), nothing to do\n",
			   old_count);
		return 0;
	}

	if (new_count < 1 || new_count > IBMVETH_MAX_RX_QUEUES) {
		netdev_err(netdev, "Invalid RX queue count %d (must be 1-%d)\n",
			   new_count, IBMVETH_MAX_RX_QUEUES);
		return -EINVAL;
	}

	netdev_info(netdev, "Incrementally resizing RX queues: %d to %d\n",
		    old_count, new_count);

	if (new_count > old_count) {
		netdev_dbg(netdev, "Scale-up: adding queues %d-%d\n",
			   old_count, new_count - 1);

		for (i = old_count; i < new_count; i++) {
			rc = ibmveth_alloc_single_rx_queue(adapter, i, rxq_entries);
			if (rc) {
				netdev_err(netdev, "Failed to allocate queue %d: %d\n",
					   i, rc);
				goto cleanup_new_queues;
			}

			rc = ibmveth_register_single_rx_queue(adapter, i,
							      mac_address);
			if (rc) {
				netdev_err(netdev, "Failed to register queue %d: %d\n",
					   i, rc);
				ibmveth_free_single_rx_queue(adapter, i);
				goto cleanup_new_queues;
			}

			rc = ibmveth_setup_single_rx_interrupt(adapter, i);
			if (rc) {
				netdev_err(netdev,
					   "Failed to setup IRQ for queue %d: %d\n",
					   i, rc);
				ibmveth_deregister_single_rx_queue(adapter, i);
				ibmveth_free_single_rx_queue(adapter, i);
				goto cleanup_new_queues;
			}

			rc = ibmveth_enable_irq(adapter, i);
			if (rc) {
				netdev_err(netdev,
					   "Failed to enable IRQ for queue %d: %d\n",
					   i, rc);
				ibmveth_cleanup_single_rx_interrupt(adapter, i);
				ibmveth_deregister_single_rx_queue(adapter, i);
				ibmveth_free_single_rx_queue(adapter, i);
				goto cleanup_new_queues;
			}

			napi_enable(&adapter->napi[i]);
		}

		adapter->num_rx_queues = new_count;

		for (i = old_count; i < new_count; i++)
			ibmveth_replenish_task(adapter, i);

		rc = netif_set_real_num_rx_queues(netdev, new_count);
		if (rc) {
			netdev_err(netdev, "Failed to set real RX queues to %d: %d\n",
				   new_count, rc);
			goto cleanup_new_queues;
		}
	} else {
		netdev_dbg(netdev, "Scale-down: removing queues %d-%d\n",
			   new_count, old_count - 1);

		for (i = new_count; i < old_count; i++)
			napi_disable(&adapter->napi[i]);

		for (i = new_count; i < old_count; i++)
			ibmveth_drain_rx_queue(adapter, i);

		synchronize_net();

		rc = netif_set_real_num_rx_queues(netdev, new_count);
		if (rc) {
			netdev_err(netdev, "Failed to set real RX queues to %d: %d\n",
				   new_count, rc);
			for (i = new_count; i < old_count; i++)
				napi_enable(&adapter->napi[i]);
			return rc;
		}

		/* Disable hypervisor interrupts and wait for handlers to complete
		 * before updating num_rx_queues.
		 */
		for (i = new_count; i < old_count; i++) {
			ibmveth_disable_irq(adapter, i);
			synchronize_irq(adapter->queue_irq[i]);
		}

		adapter->num_rx_queues = new_count;

		for (i = new_count; i < old_count; i++) {
			ibmveth_cleanup_single_rx_interrupt(adapter, i);
			ibmveth_deregister_single_rx_queue(adapter, i);
			ibmveth_free_single_rx_queue(adapter, i);
		}
	}

	netdev_info(netdev, "Successfully resized to %d RX queues (incremental)\n",
		    adapter->num_rx_queues);

	if (firmware_has_feature(FW_FEATURE_CMO))
		vio_cmo_set_dev_desired(adapter->vdev,
					ibmveth_get_desired_dma(adapter->vdev));

	return 0;

cleanup_new_queues:
	failed_queue = i;
	netdev_err(netdev,
		   "Scale-up failed at queue %d, cleaning up queues %d-%d\n",
		   failed_queue, old_count, failed_queue - 1);
	for (i = old_count; i < failed_queue; i++)
		napi_disable(&adapter->napi[i]);

	for (i = old_count; i < failed_queue; i++)
		ibmveth_drain_rx_queue(adapter, i);

	synchronize_net();

	for (i = old_count; i < failed_queue; i++) {
		ibmveth_disable_irq(adapter, i);
		synchronize_irq(adapter->queue_irq[i]);
	}

	for (i = old_count; i < failed_queue; i++) {
		ibmveth_cleanup_single_rx_interrupt(adapter, i);
		ibmveth_deregister_single_rx_queue(adapter, i);
		ibmveth_free_single_rx_queue(adapter, i);
	}
	adapter->num_rx_queues = old_count;
	netdev_warn(netdev, "Keeping %d queues after scale-up failure\n",
		    old_count);
	return rc;
}

/**
 * ibmveth_free_all_queues - Free all RX queues at once
 * @adapter: ibmveth adapter structure
 *
 * Uses H_FREE_LOGICAL_LAN to free all queues in one hypercall.
 * Used during interface close and registration error cleanup.
 *
 * Clears queue handles only; queue_irq[] is released by
 * ibmveth_cleanup_rx_interrupts() on close, or by
 * ibmveth_dispose_subordinate_irq_mappings() on partial register failure.
 */
static void ibmveth_free_all_queues(struct ibmveth_adapter *adapter)
{
	unsigned long lpar_rc;
	int i;

	netdev_dbg(adapter->netdev, "freeing all RX queues at once\n");

	do {
		lpar_rc = h_free_logical_lan(adapter->vdev->unit_address);
		adapter->hcall_stats.free_lan++;
	} while (H_IS_LONG_BUSY(lpar_rc) || (lpar_rc == H_BUSY));

	if (lpar_rc != H_SUCCESS) {
		netdev_err(adapter->netdev,
			   "h_free_logical_lan failed: %ld\n", lpar_rc);
	}

	for (i = 0; i < adapter->num_rx_queues; i++)
		adapter->queue_handle[i] = 0;
}

/**
 * ibmveth_register_rx_queues - Register RX queues with hypervisor
 * @adapter: ibmveth adapter structure
 * @mac_address: MAC address for device registration
 *
 * Registers queue 0 via ibmveth_register_logical_lan(), then subordinate
 * queues 1..N when multi-queue mode is enabled.
 *
 * Return: 0 on success, -ENONET if queue 0 registration fails, -EIO on
 *         subordinate queue registration failure
 */
static int
ibmveth_register_rx_queues(struct ibmveth_adapter *adapter, u64 mac_address)
{
	struct net_device *netdev = adapter->netdev;
	union ibmveth_buf_desc rxq_desc;
	unsigned long lpar_rc;
	int i, rc;

	rxq_desc.fields.flags_len = IBMVETH_BUF_VALID |
				    adapter->rx_queue[0].queue_len;
	rxq_desc.fields.address = adapter->rx_queue[0].queue_dma;
	adapter->queue_irq[0] = netdev->irq;

	rc = ibmveth_disable_irq(adapter, 0);
	if (rc != H_SUCCESS)
		netdev_dbg(netdev,
			   "Failed to disable IRQ for queue 0 before registration, rc=%d\n",
			   rc);

	lpar_rc = ibmveth_register_logical_lan(adapter, rxq_desc, mac_address);
	if (lpar_rc != H_SUCCESS) {
		netdev_err(netdev, "h_register_logical_lan failed: %ld\n", lpar_rc);
		netdev_err(netdev,
			   "buffer TCE:0x%llx filter TCE:0x%llx rxq desc:0x%llx MAC:0x%llx\n",
			   adapter->buffer_list_dma[0],
			   adapter->filter_list_dma,
			   rxq_desc.desc, mac_address);
		return -ENONET;
	}

	if (adapter->num_rx_queues == 1 || !adapter->multi_queue) {
		netdev_dbg(netdev,
			   "registered 1 RX queue with hypervisor (single-queue mode)\n");
		return 0;
	}

	netdev_dbg(netdev, "Registering %d subordinate queues (1-%d)\n",
		   adapter->num_rx_queues - 1, adapter->num_rx_queues - 1);

	for (i = 1; i < adapter->num_rx_queues; i++) {
		rc = ibmveth_register_single_rx_queue(adapter, i, mac_address);
		if (rc) {
			if (!adapter->queue_handle[i] || !adapter->queue_irq[i]) {
				netdev_err(netdev,
					   "Invalid hypervisor return for queue %d: handle=0x%llx irq=%u\n",
					   i, adapter->queue_handle[i],
					   adapter->queue_irq[i]);
			}
			goto err_unregister;
		}
	}

	netdev_dbg(netdev,
		   "registered %d RX queues with hypervisor (multi-queue mode)\n",
		   adapter->num_rx_queues);

	return 0;

err_unregister:
	ibmveth_dispose_subordinate_irq_mappings(adapter);
	ibmveth_free_all_queues(adapter);
	return rc;
}

static int ibmveth_open(struct net_device *netdev)
{
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	u64 mac_address = ether_addr_to_u64(netdev->dev_addr);
	int rxq_entries = 1;
	int rc;
	int i;

	netdev_dbg(netdev, "open starting\n");

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++)
		rxq_entries += adapter->rx_buff_pool[0][i].size;

	rc = ibmveth_alloc_rx_qstats(adapter);
	if (rc)
		goto out;

	rc = ibmveth_alloc_filter_list(adapter);
	if (rc)
		goto out_free_rx_qstats;

	rc = ibmveth_alloc_rx_queues(adapter, rxq_entries);
	if (rc)
		goto out_free_filter_list;

	rc = ibmveth_alloc_buffer_pools(adapter);
	if (rc)
		goto out_free_queue_mem;

	rc = ibmveth_register_rx_queues(adapter, mac_address);
	if (rc)
		goto out_free_buffer_pools;

	rc = netif_set_real_num_rx_queues(netdev, adapter->num_rx_queues);
	if (rc) {
		netdev_err(netdev, "failed to set number of rx queues\n");
		goto out_unregister_queues;
	}

	rc = ibmveth_setup_rx_interrupts(adapter);
	if (rc)
		goto out_unregister_queues;

	if (adapter->num_rx_queues > 1) {
		for (i = 0; i < adapter->num_rx_queues; i++) {
			netdev_dbg(netdev, "initial replenish cycle for queue %d\n", i);
			ibmveth_replenish_task(adapter, i);
		}
	} else {
		netdev_dbg(netdev, "initial replenish cycle\n");
		ibmveth_interrupt(adapter->queue_irq[0], &adapter->napi[0]);
	}

	rc = ibmveth_alloc_tx_resources(adapter);
	if (rc)
		goto out_cleanup_rx_interrupts;

	rc = ibmveth_alloc_tx_qstats(adapter);
	if (rc)
		goto out_free_tx_resources;

	netif_tx_start_all_queues(netdev);

	netdev_dbg(netdev, "open complete\n");
	return 0;

out_cleanup_rx_interrupts:
	ibmveth_cleanup_rx_interrupts(adapter);
out_free_tx_resources:
	ibmveth_free_tx_resources(adapter);
out_free_buffer_pools:
	ibmveth_free_buffer_pools(adapter);
out_unregister_queues:
	ibmveth_dispose_subordinate_irq_mappings(adapter);
	ibmveth_free_all_queues(adapter);
out_free_queue_mem:
	ibmveth_cleanup_rx_resources(adapter);
out_free_filter_list:
	ibmveth_free_filter_list(adapter);
out_free_rx_qstats:
	ibmveth_free_rx_qstats(adapter);
out:
	return rc;
}

static int ibmveth_close(struct net_device *netdev)
{
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	int i;

	netdev_dbg(netdev, "close starting\n");

	netif_tx_stop_all_queues(netdev);

	for (i = 0; i < adapter->num_rx_queues; i++) {
		if (adapter->queue_irq[i]) {
			ibmveth_disable_irq(adapter, i);
			synchronize_irq(adapter->queue_irq[i]);
		}
	}

	ibmveth_free_tx_qstats(adapter);
	ibmveth_free_tx_resources(adapter);
	ibmveth_cleanup_rx_interrupts(adapter);
	ibmveth_update_rx_no_buffer(adapter);
	ibmveth_free_all_queues(adapter);
	ibmveth_free_buffer_pools(adapter);
	ibmveth_cleanup_rx_resources(adapter);
	ibmveth_free_filter_list(adapter);
	ibmveth_free_rx_qstats(adapter);

	netdev_dbg(netdev, "close complete\n");

	return 0;
}

/**
 * ibmveth_reset - Handle scheduled reset work
 *
 * @w: pointer to work_struct embedded in adapter structure
 *
 * Context: This routine acquires rtnl_mutex and disables its NAPI through
 *          ibmveth_close. It can't be called directly in a context that has
 *          already acquired rtnl_mutex or disabled its NAPI, or directly from
 *          a poll routine.
 *
 * Return: void
 */
static void ibmveth_reset(struct work_struct *w)
{
	struct ibmveth_adapter *adapter = container_of(w, struct ibmveth_adapter, work);
	struct net_device *netdev = adapter->netdev;

	netdev_dbg(netdev, "reset starting\n");

	rtnl_lock();

	dev_close(adapter->netdev);
	dev_open(adapter->netdev, NULL);

	rtnl_unlock();

	netdev_dbg(netdev, "reset complete\n");
}

static int ibmveth_set_link_ksettings(struct net_device *dev,
				      const struct ethtool_link_ksettings *cmd)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);

	return ethtool_virtdev_set_link_ksettings(dev, cmd,
						  &adapter->speed,
						  &adapter->duplex);
}

static int ibmveth_get_link_ksettings(struct net_device *dev,
				      struct ethtool_link_ksettings *cmd)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);

	cmd->base.speed = adapter->speed;
	cmd->base.duplex = adapter->duplex;
	cmd->base.port = PORT_OTHER;

	return 0;
}

static void ibmveth_init_link_settings(struct net_device *dev)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);

	adapter->speed = SPEED_1000;
	adapter->duplex = DUPLEX_FULL;
}

static void netdev_get_drvinfo(struct net_device *dev,
			       struct ethtool_drvinfo *info)
{
	strscpy(info->driver, ibmveth_driver_name, sizeof(info->driver));
	strscpy(info->version, ibmveth_driver_version, sizeof(info->version));
}

static netdev_features_t ibmveth_fix_features(struct net_device *dev,
	netdev_features_t features)
{
	/*
	 * Since the ibmveth firmware interface does not have the
	 * concept of separate tx/rx checksum offload enable, if rx
	 * checksum is disabled we also have to disable tx checksum
	 * offload. Once we disable rx checksum offload, we are no
	 * longer allowed to send tx buffers that are not properly
	 * checksummed.
	 */

	if (!(features & NETIF_F_RXCSUM))
		features &= ~NETIF_F_CSUM_MASK;

	return features;
}

static int ibmveth_set_csum_offload(struct net_device *dev, u32 data)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	unsigned long set_attr, clr_attr, ret_attr;
	unsigned long set_attr6, clr_attr6;
	long ret, ret4, ret6;
	int rc1 = 0, rc2 = 0;
	int restart = 0;

	if (netif_running(dev)) {
		restart = 1;
		ibmveth_close(dev);
	}

	set_attr = 0;
	clr_attr = 0;
	set_attr6 = 0;
	clr_attr6 = 0;

	if (data) {
		set_attr = IBMVETH_ILLAN_IPV4_TCP_CSUM;
		set_attr6 = IBMVETH_ILLAN_IPV6_TCP_CSUM;
	} else {
		clr_attr = IBMVETH_ILLAN_IPV4_TCP_CSUM;
		clr_attr6 = IBMVETH_ILLAN_IPV6_TCP_CSUM;
	}

	ret = h_illan_attributes(adapter->vdev->unit_address, 0, 0, &ret_attr);

	if (ret == H_SUCCESS &&
	    (ret_attr & IBMVETH_ILLAN_PADDED_PKT_CSUM)) {
		ret4 = h_illan_attributes(adapter->vdev->unit_address, clr_attr,
					 set_attr, &ret_attr);

		if (ret4 != H_SUCCESS) {
			netdev_err(dev, "unable to change IPv4 checksum "
					"offload settings. %d rc=%ld\n",
					data, ret4);

			h_illan_attributes(adapter->vdev->unit_address,
					   set_attr, clr_attr, &ret_attr);

			if (data == 1)
				dev->features &= ~NETIF_F_IP_CSUM;

		} else {
			adapter->fw_ipv4_csum_support = data;
		}

		ret6 = h_illan_attributes(adapter->vdev->unit_address,
					 clr_attr6, set_attr6, &ret_attr);

		if (ret6 != H_SUCCESS) {
			netdev_err(dev, "unable to change IPv6 checksum "
					"offload settings. %d rc=%ld\n",
					data, ret6);

			h_illan_attributes(adapter->vdev->unit_address,
					   set_attr6, clr_attr6, &ret_attr);

			if (data == 1)
				dev->features &= ~NETIF_F_IPV6_CSUM;

		} else
			adapter->fw_ipv6_csum_support = data;

		if (ret4 == H_SUCCESS || ret6 == H_SUCCESS)
			adapter->rx_csum = data;
		else
			rc1 = -EIO;
	} else {
		rc1 = -EIO;
		netdev_err(dev, "unable to change checksum offload settings."
				     " %d rc=%ld ret_attr=%lx\n", data, ret,
				     ret_attr);
	}

	if (restart)
		rc2 = ibmveth_open(dev);

	return rc1 ? rc1 : rc2;
}

static int ibmveth_set_tso(struct net_device *dev, u32 data)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	unsigned long set_attr, clr_attr, ret_attr;
	long ret1, ret2;
	int rc1 = 0, rc2 = 0;
	int restart = 0;

	if (netif_running(dev)) {
		restart = 1;
		ibmveth_close(dev);
	}

	set_attr = 0;
	clr_attr = 0;

	if (data)
		set_attr = IBMVETH_ILLAN_LRG_SR_ENABLED;
	else
		clr_attr = IBMVETH_ILLAN_LRG_SR_ENABLED;

	ret1 = h_illan_attributes(adapter->vdev->unit_address, 0, 0, &ret_attr);

	if (ret1 == H_SUCCESS && (ret_attr & IBMVETH_ILLAN_LRG_SND_SUPPORT) &&
	    !old_large_send) {
		ret2 = h_illan_attributes(adapter->vdev->unit_address, clr_attr,
					  set_attr, &ret_attr);

		if (ret2 != H_SUCCESS) {
			netdev_err(dev, "unable to change tso settings. %d rc=%ld\n",
				   data, ret2);

			h_illan_attributes(adapter->vdev->unit_address,
					   set_attr, clr_attr, &ret_attr);

			if (data == 1)
				dev->features &= ~(NETIF_F_TSO | NETIF_F_TSO6);
			rc1 = -EIO;

		} else {
			adapter->fw_large_send_support = data;
			adapter->large_send = data;
		}
	} else {
		/* Older firmware version of large send offload does not
		 * support tcp6/ipv6
		 */
		if (data == 1) {
			dev->features &= ~NETIF_F_TSO6;
			netdev_info(dev, "TSO feature requires all partitions to have updated driver");
		}
		adapter->large_send = data;
	}

	if (restart)
		rc2 = ibmveth_open(dev);

	return rc1 ? rc1 : rc2;
}

static int ibmveth_set_features(struct net_device *dev,
	netdev_features_t features)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	int rx_csum = !!(features & NETIF_F_RXCSUM);
	int large_send = !!(features & (NETIF_F_TSO | NETIF_F_TSO6));
	int rc1 = 0, rc2 = 0;

	if (rx_csum != adapter->rx_csum) {
		rc1 = ibmveth_set_csum_offload(dev, rx_csum);
		if (rc1 && !adapter->rx_csum)
			dev->features =
				features & ~(NETIF_F_CSUM_MASK |
					     NETIF_F_RXCSUM);
	}

	if (large_send != adapter->large_send) {
		rc2 = ibmveth_set_tso(dev, large_send);
		if (rc2 && !adapter->large_send)
			dev->features =
				features & ~(NETIF_F_TSO | NETIF_F_TSO6);
	}

	return rc1 ? rc1 : rc2;
}

/**
 * ibmveth_aggregate_rx_qstats - Sum per-queue RX stats into globals
 * @adapter: ibmveth adapter
 *
 * Cold path only (ethtool). Keeps legacy global counters meaningful for
 * tools that read the adapter-level fields in ibmveth_stats[].
 */
static void ibmveth_aggregate_rx_qstats(struct ibmveth_adapter *adapter)
{
	u64 total_invalid = 0;
	u64 total_large = 0;
	int i;

	if (!adapter->rx_qstats)
		return;

	for (i = 0; i < adapter->num_rx_queues; i++) {
		total_invalid += adapter->rx_qstats[i].invalid_buffers;
		total_large += adapter->rx_qstats[i].large_packets;
	}

	adapter->rx_invalid_buffer = total_invalid;
	adapter->rx_large_packets = total_large;
}

/**
 * ibmveth_aggregate_tx_qstats - Sum per-queue TX stats into globals
 * @adapter: ibmveth adapter
 *
 * Cold path only (ethtool). Keeps legacy global counters meaningful for
 * tools that read the adapter-level fields in ibmveth_stats[].
 */
static void ibmveth_aggregate_tx_qstats(struct ibmveth_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	u64 total_large = 0;
	u64 total_send_failed = 0;
	int i;

	if (!adapter->tx_qstats)
		return;

	for (i = 0; i < netdev->real_num_tx_queues; i++) {
		total_large += adapter->tx_qstats[i].large_packets;
		total_send_failed += adapter->tx_qstats[i].send_failures;
	}

	adapter->tx_large_packets = total_large;
	adapter->tx_send_failed = total_send_failed;
}

static void ibmveth_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	u8 *p = data;
	int i;

	if (stringset != ETH_SS_STATS)
		return;

	for (i = 0; i < ARRAY_SIZE(ibmveth_stats); i++) {
		memcpy(p, ibmveth_stats[i].name, ETH_GSTRING_LEN);
		p += ETH_GSTRING_LEN;
	}

	for (i = 0; i < adapter->num_rx_queues; i++) {
		ethtool_sprintf(&p, "rx%d_packets", i);
		ethtool_sprintf(&p, "rx%d_bytes", i);
		ethtool_sprintf(&p, "rx%d_interrupts", i);
		ethtool_sprintf(&p, "rx%d_polls", i);
		ethtool_sprintf(&p, "rx%d_large_packets", i);
		ethtool_sprintf(&p, "rx%d_invalid_buffers", i);
		ethtool_sprintf(&p, "rx%d_no_buffer_drops", i);
	}

	for (i = 0; i < dev->real_num_tx_queues; i++) {
		ethtool_sprintf(&p, "tx%d_packets", i);
		ethtool_sprintf(&p, "tx%d_bytes", i);
		ethtool_sprintf(&p, "tx%d_large_packets", i);
		ethtool_sprintf(&p, "tx%d_dropped_packets", i);
		ethtool_sprintf(&p, "tx%d_send_failures", i);
		ethtool_sprintf(&p, "tx%d_checksum_offload", i);
	}

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
		ethtool_sprintf(&p, "pool%d_size", i);
		ethtool_sprintf(&p, "pool%d_active", i);
		ethtool_sprintf(&p, "pool%d_available", i);
	}
}

static int ibmveth_get_sset_count(struct net_device *dev, int sset)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);

	switch (sset) {
	case ETH_SS_STATS:
		return ARRAY_SIZE(ibmveth_stats) +
		       adapter->num_rx_queues * IBMVETH_NUM_RX_QSTATS +
		       dev->real_num_tx_queues * IBMVETH_NUM_TX_QSTATS +
		       IBMVETH_NUM_BUFF_POOLS * 3;
	default:
		return -EOPNOTSUPP;
	}
}

static void ibmveth_get_ethtool_stats(struct net_device *dev,
				      struct ethtool_stats *stats, u64 *data)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	int i, j;

	ibmveth_aggregate_rx_qstats(adapter);
	ibmveth_aggregate_tx_qstats(adapter);

	for (i = 0; i < ARRAY_SIZE(ibmveth_stats); i++)
		data[i] = IBMVETH_GET_STAT(adapter, ibmveth_stats[i].offset);

	for (j = 0; j < adapter->num_rx_queues; j++) {
		if (adapter->rx_qstats) {
			data[i++] = adapter->rx_qstats[j].packets;
			data[i++] = adapter->rx_qstats[j].bytes;
			data[i++] = adapter->rx_qstats[j].interrupts;
			data[i++] = adapter->rx_qstats[j].polls;
			data[i++] = adapter->rx_qstats[j].large_packets;
			data[i++] = adapter->rx_qstats[j].invalid_buffers;
			data[i++] = adapter->rx_qstats[j].no_buffer_drops;
		} else {
			i += IBMVETH_NUM_RX_QSTATS;
		}
	}

	for (j = 0; j < dev->real_num_tx_queues; j++) {
		if (adapter->tx_qstats) {
			data[i++] = adapter->tx_qstats[j].packets;
			data[i++] = adapter->tx_qstats[j].bytes;
			data[i++] = adapter->tx_qstats[j].large_packets;
			data[i++] = adapter->tx_qstats[j].dropped_packets;
			data[i++] = adapter->tx_qstats[j].send_failures;
			data[i++] = adapter->tx_qstats[j].checksum_offload;
		} else {
			i += IBMVETH_NUM_TX_QSTATS;
		}
	}

	for (j = 0; j < IBMVETH_NUM_BUFF_POOLS; j++) {
		data[i++] = adapter->rx_buff_pool[0][j].size;
		data[i++] = adapter->rx_buff_pool[0][j].active;
		data[i++] = atomic_read(&adapter->rx_buff_pool[0][j].available);
	}
}

static void ibmveth_get_channels(struct net_device *netdev,
				 struct ethtool_channels *channels)
{
	struct ibmveth_adapter *adapter = netdev_priv(netdev);

	channels->max_tx = ibmveth_real_max_tx_queues();
	channels->tx_count = netdev->real_num_tx_queues;

	if (adapter->multi_queue)
		channels->max_rx = IBMVETH_MAX_RX_QUEUES;
	else
		channels->max_rx = 1;
	channels->rx_count = adapter->num_rx_queues;
}

static int ibmveth_set_channels(struct net_device *netdev,
				struct ethtool_channels *channels)
{
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	unsigned int old = netdev->real_num_tx_queues,
		     goal = channels->tx_count;
	int rc, i;

	/* If ndo_open has not been called yet then don't allocate, just set
	 * desired netdev_queue's and return
	 */
	if (!(netdev->flags & IFF_UP))
		return netif_set_real_num_tx_queues(netdev, goal);

	/* We have IBMVETH_MAX_QUEUES netdev_queue's allocated
	 * but we may need to alloc/free the ltb's.
	 */
	netif_tx_stop_all_queues(netdev);

	/* Allocate any queue that we need */
	for (i = old; i < goal; i++) {
		if (adapter->tx_ltb_ptr[i])
			continue;

		rc = ibmveth_allocate_tx_ltb(adapter, i);
		if (!rc)
			continue;

		/* if something goes wrong, free everything we just allocated */
		netdev_err(netdev, "Failed to allocate more tx queues, returning to %d queues\n",
			   old);
		goal = old;
		old = i;
		break;
	}
	rc = netif_set_real_num_tx_queues(netdev, goal);
	if (rc) {
		netdev_err(netdev, "Failed to set real tx queues, returning to %d queues\n",
			   old);
		goal = old;
		old = i;
	}
	/* Free any that are no longer needed */
	for (i = old; i > goal; i--) {
		if (adapter->tx_ltb_ptr[i - 1])
			ibmveth_free_tx_ltb(adapter, i - 1);
	}

	netif_tx_wake_all_queues(netdev);

	return rc;
}

static const struct ethtool_ops netdev_ethtool_ops = {
	.get_drvinfo		         = netdev_get_drvinfo,
	.get_link		         = ethtool_op_get_link,
	.get_strings		         = ibmveth_get_strings,
	.get_sset_count		         = ibmveth_get_sset_count,
	.get_ethtool_stats	         = ibmveth_get_ethtool_stats,
	.get_link_ksettings	         = ibmveth_get_link_ksettings,
	.set_link_ksettings              = ibmveth_set_link_ksettings,
	.get_channels			 = ibmveth_get_channels,
	.set_channels			 = ibmveth_set_channels
};

static int ibmveth_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	return -EOPNOTSUPP;
}

static int ibmveth_send(struct ibmveth_adapter *adapter,
			unsigned long desc, unsigned long mss)
{
	unsigned long correlator;
	unsigned int retry_count;
	unsigned long ret;

	/*
	 * The retry count sets a maximum for the number of broadcast and
	 * multicast destinations within the system.
	 */
	retry_count = 1024;
	correlator = 0;
	do {
		ret = h_send_logical_lan(adapter->vdev->unit_address, desc,
					 correlator, &correlator, mss,
					 adapter->fw_large_send_support);
	} while ((ret == H_BUSY) && (retry_count--));

	if (ret != H_SUCCESS && ret != H_DROPPED) {
		netdev_err(adapter->netdev, "tx: h_send_logical_lan failed "
			   "with rc=%ld\n", ret);
		return 1;
	}

	adapter->hcall_stats.send_lan++;
	return 0;
}

static int ibmveth_is_packet_unsupported(struct sk_buff *skb,
					 struct ibmveth_adapter *adapter,
					 int queue_num)
{
	struct net_device *netdev = adapter->netdev;
	struct ethhdr *ether_header;
	int ret = 0;

	ether_header = eth_hdr(skb);

	if (ether_addr_equal(ether_header->h_dest, netdev->dev_addr)) {
		netdev_dbg(netdev, "veth doesn't support loopback packets, dropping packet.\n");
		if (adapter->tx_qstats)
			adapter->tx_qstats[queue_num].dropped_packets++;
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static netdev_tx_t ibmveth_start_xmit(struct sk_buff *skb,
				      struct net_device *netdev)
{
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	unsigned int desc_flags, total_bytes;
	union ibmveth_buf_desc desc;
	int i, queue_num = skb_get_queue_mapping(skb);
	unsigned long mss = 0;

	if (ibmveth_is_packet_unsupported(skb, adapter, queue_num))
		goto out;
	/* veth can't checksum offload UDP */
	if (skb->ip_summed == CHECKSUM_PARTIAL &&
	    ((skb->protocol == htons(ETH_P_IP) &&
	      ip_hdr(skb)->protocol != IPPROTO_TCP) ||
	     (skb->protocol == htons(ETH_P_IPV6) &&
	      ipv6_hdr(skb)->nexthdr != IPPROTO_TCP)) &&
	    skb_checksum_help(skb)) {

		netdev_err(netdev, "tx: failed to checksum packet\n");
		adapter->tx_qstats[queue_num].dropped_packets++;
		goto out;
	}

	desc_flags = IBMVETH_BUF_VALID;

	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		unsigned char *buf = skb_transport_header(skb) +
						skb->csum_offset;

		desc_flags |= (IBMVETH_BUF_NO_CSUM | IBMVETH_BUF_CSUM_GOOD);

		adapter->tx_qstats[queue_num].checksum_offload++;

		/* Need to zero out the checksum */
		buf[0] = 0;
		buf[1] = 0;

		if (skb_is_gso(skb) && adapter->fw_large_send_support)
			desc_flags |= IBMVETH_BUF_LRG_SND;
	}

	if (skb->ip_summed == CHECKSUM_PARTIAL && skb_is_gso(skb)) {
		if (adapter->fw_large_send_support) {
			mss = (unsigned long)skb_shinfo(skb)->gso_size;
			adapter->tx_qstats[queue_num].large_packets++;
		} else if (!skb_is_gso_v6(skb)) {
			/* Put -1 in the IP checksum to tell phyp it
			 * is a largesend packet. Put the mss in
			 * the TCP checksum.
			 */
			ip_hdr(skb)->check = 0xffff;
			tcp_hdr(skb)->check =
				cpu_to_be16(skb_shinfo(skb)->gso_size);
			adapter->tx_qstats[queue_num].large_packets++;
		}
	}

	/* Copy header into mapped buffer */
	if (unlikely(skb->len > adapter->tx_ltb_size)) {
		netdev_err(adapter->netdev, "tx: packet size (%u) exceeds ltb (%u)\n",
			   skb->len, adapter->tx_ltb_size);
		adapter->tx_qstats[queue_num].dropped_packets++;
		goto out;
	}
	memcpy(adapter->tx_ltb_ptr[queue_num], skb->data, skb_headlen(skb));
	total_bytes = skb_headlen(skb);
	/* Copy frags into mapped buffers */
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

		memcpy(adapter->tx_ltb_ptr[queue_num] + total_bytes,
		       skb_frag_address_safe(frag), skb_frag_size(frag));
		total_bytes += skb_frag_size(frag);
	}

	if (unlikely(total_bytes != skb->len)) {
		netdev_err(adapter->netdev, "tx: incorrect packet len copied into ltb (%u != %u)\n",
			   skb->len, total_bytes);
		adapter->tx_qstats[queue_num].dropped_packets++;
		goto out;
	}
	desc.fields.flags_len = desc_flags | skb->len;
	desc.fields.address = adapter->tx_ltb_dma[queue_num];
	/* finish writing to long_term_buff before VIOS accessing it */
	dma_wmb();

	if (ibmveth_send(adapter, desc.desc, mss)) {
		adapter->tx_qstats[queue_num].send_failures++;
		adapter->tx_qstats[queue_num].dropped_packets++;
	} else {
		adapter->tx_qstats[queue_num].packets++;
		adapter->tx_qstats[queue_num].bytes += skb->len;
	}

out:
	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;


}

static void ibmveth_rx_mss_helper(struct sk_buff *skb, u16 mss, int lrg_pkt)
{
	struct tcphdr *tcph;
	int offset = 0;
	int hdr_len;

	/* only TCP packets will be aggregated */
	if (skb->protocol == htons(ETH_P_IP)) {
		struct iphdr *iph = (struct iphdr *)skb->data;

		if (iph->protocol == IPPROTO_TCP) {
			offset = iph->ihl * 4;
			skb_shinfo(skb)->gso_type = SKB_GSO_TCPV4;
		} else {
			return;
		}
	} else if (skb->protocol == htons(ETH_P_IPV6)) {
		struct ipv6hdr *iph6 = (struct ipv6hdr *)skb->data;

		if (iph6->nexthdr == IPPROTO_TCP) {
			offset = sizeof(struct ipv6hdr);
			skb_shinfo(skb)->gso_type = SKB_GSO_TCPV6;
		} else {
			return;
		}
	} else {
		return;
	}
	/* if mss is not set through Large Packet bit/mss in rx buffer,
	 * expect that the mss will be written to the tcp header checksum.
	 */
	tcph = (struct tcphdr *)(skb->data + offset);
	if (lrg_pkt) {
		skb_shinfo(skb)->gso_size = mss;
	} else if (offset) {
		skb_shinfo(skb)->gso_size = ntohs(tcph->check);
		tcph->check = 0;
	}

	if (skb_shinfo(skb)->gso_size) {
		hdr_len = offset + tcph->doff * 4;
		skb_shinfo(skb)->gso_segs =
				DIV_ROUND_UP(skb->len - hdr_len,
					     skb_shinfo(skb)->gso_size);
	}
}

static void ibmveth_rx_csum_helper(struct sk_buff *skb,
				   struct ibmveth_adapter *adapter)
{
	struct iphdr *iph = NULL;
	struct ipv6hdr *iph6 = NULL;
	__be16 skb_proto = 0;
	u16 iphlen = 0;
	u16 iph_proto = 0;
	u16 tcphdrlen = 0;

	skb_proto = be16_to_cpu(skb->protocol);

	if (skb_proto == ETH_P_IP) {
		iph = (struct iphdr *)skb->data;

		/* If the IP checksum is not offloaded and if the packet
		 *  is large send, the checksum must be rebuilt.
		 */
		if (iph->check == 0xffff) {
			iph->check = 0;
			iph->check = ip_fast_csum((unsigned char *)iph,
						  iph->ihl);
		}

		iphlen = iph->ihl * 4;
		iph_proto = iph->protocol;
	} else if (skb_proto == ETH_P_IPV6) {
		iph6 = (struct ipv6hdr *)skb->data;
		iphlen = sizeof(struct ipv6hdr);
		iph_proto = iph6->nexthdr;
	}

	/* When CSO is enabled the TCP checksum may have be set to NULL by
	 * the sender given that we zeroed out TCP checksum field in
	 * transmit path (refer ibmveth_start_xmit routine). In this case set
	 * up CHECKSUM_PARTIAL. If the packet is forwarded, the checksum will
	 * then be recalculated by the destination NIC (CSO must be enabled
	 * on the destination NIC).
	 *
	 * In an OVS environment, when a flow is not cached, specifically for a
	 * new TCP connection, the first packet information is passed up to
	 * the user space for finding a flow. During this process, OVS computes
	 * checksum on the first packet when CHECKSUM_PARTIAL flag is set.
	 *
	 * So, re-compute TCP pseudo header checksum.
	 */

	if (iph_proto == IPPROTO_TCP) {
		struct tcphdr *tcph = (struct tcphdr *)(skb->data + iphlen);

		if (tcph->check == 0x0000) {
			/* Recompute TCP pseudo header checksum  */
			tcphdrlen = skb->len - iphlen;
			if (skb_proto == ETH_P_IP)
				tcph->check =
				 ~csum_tcpudp_magic(iph->saddr,
				iph->daddr, tcphdrlen, iph_proto, 0);
			else if (skb_proto == ETH_P_IPV6)
				tcph->check =
				 ~csum_ipv6_magic(&iph6->saddr,
				&iph6->daddr, tcphdrlen, iph_proto, 0);
			/* Setup SKB fields for checksum offload */
			skb_partial_csum_set(skb, iphlen,
					     offsetof(struct tcphdr, check));
			skb_reset_network_header(skb);
		}
	}
}

static int ibmveth_poll(struct napi_struct *napi, int budget)
{
	struct net_device *netdev = napi->dev;
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	int frames_processed = 0;
	unsigned long lpar_rc;
	int queue_index, rc;
	u16 mss = 0;

	queue_index = napi - adapter->napi;

	if (WARN_ON(queue_index < 0 || queue_index >= adapter->num_rx_queues))
		return 0;

	if (adapter->rx_qstats)
		adapter->rx_qstats[queue_index].polls++;

restart_poll:
	while (frames_processed < budget) {
		if (!ibmveth_rxq_pending_buffer(adapter, queue_index))
			break;

		smp_rmb();
		if (!ibmveth_rxq_buffer_valid(adapter, queue_index)) {
			wmb(); /* suggested by larson1 */
			if (adapter->rx_qstats)
				adapter->rx_qstats[queue_index].invalid_buffers++;
			else
				adapter->rx_invalid_buffer++;
			netdev_dbg(netdev, "recycling invalid buffer\n");
			rc = ibmveth_rxq_harvest_buffer(adapter, queue_index, true);
			if (unlikely(rc))
				break;
		} else {
			struct sk_buff *skb, *new_skb;
			int length = ibmveth_rxq_frame_length(adapter, queue_index);
			int offset = ibmveth_rxq_frame_offset(adapter, queue_index);
			int csum_good = ibmveth_rxq_csum_good(adapter, queue_index);
			int lrg_pkt = ibmveth_rxq_large_packet(adapter, queue_index);
			__sum16 iph_check = 0;

			skb = ibmveth_rxq_get_buffer(adapter, queue_index);
			if (unlikely(!skb))
				break;

			/* if the large packet bit is set in the rx queue
			 * descriptor, the mss will be written by PHYP eight
			 * bytes from the start of the rx buffer, which is
			 * skb->data at this stage
			 */
			if (lrg_pkt) {
				__be64 *rxmss = (__be64 *)(skb->data + 8);

				mss = (u16)be64_to_cpu(*rxmss);
			}

			new_skb = NULL;
			if (length < rx_copybreak)
				new_skb = netdev_alloc_skb(netdev, length);

			if (new_skb) {
				skb_copy_to_linear_data(new_skb,
							skb->data + offset,
							length);
				if (rx_flush)
					ibmveth_flush_buffer(skb->data,
							     length + offset);
				rc = ibmveth_rxq_harvest_buffer(adapter, queue_index, true);
				if (unlikely(rc))
					break;
				skb = new_skb;
			} else {
				rc = ibmveth_rxq_harvest_buffer(adapter, queue_index, false);
				if (unlikely(rc))
					break;
				skb_reserve(skb, offset);
			}

			skb_put(skb, length);
			skb->protocol = eth_type_trans(skb, netdev);

			/* PHYP without PLSO support places a -1 in the ip
			 * checksum for large send frames.
			 */
			if (skb->protocol == cpu_to_be16(ETH_P_IP)) {
				struct iphdr *iph = (struct iphdr *)skb->data;

				iph_check = iph->check;
			}

			if ((length > netdev->mtu + ETH_HLEN) ||
			    lrg_pkt || iph_check == 0xffff) {
				ibmveth_rx_mss_helper(skb, mss, lrg_pkt);
				if (adapter->rx_qstats)
					adapter->rx_qstats[queue_index].large_packets++;
				else
					adapter->rx_large_packets++;
			}

			if (csum_good) {
				skb->ip_summed = CHECKSUM_UNNECESSARY;
				ibmveth_rx_csum_helper(skb, adapter);
			}

			napi_gro_receive(napi, skb);	/* send it up */

			if (adapter->rx_qstats) {
				adapter->rx_qstats[queue_index].packets++;
				adapter->rx_qstats[queue_index].bytes += length;
			}

			frames_processed++;
		}
	}

	ibmveth_replenish_task(adapter, queue_index);

	if (frames_processed == budget)
		goto out;

	if (!napi_complete_done(napi, frames_processed))
		goto out;

	/* We think we are done - reenable interrupts,
	 * then check once more to make sure we are done.
	 */
	lpar_rc = ibmveth_enable_irq(adapter, queue_index);
	if (lpar_rc != H_SUCCESS) {
		netdev_err(netdev,
			   "Failed to enable IRQ for queue %d (rc=0x%lx), scheduling reset\n",
			   queue_index, lpar_rc);
		schedule_work(&adapter->work);
		goto out;
	}

	if (ibmveth_rxq_pending_buffer(adapter, queue_index) &&
	    napi_schedule(napi)) {
		lpar_rc = ibmveth_disable_irq(adapter, queue_index);
		WARN_ON(lpar_rc != H_SUCCESS);
		goto restart_poll;
	}

out:
	return frames_processed;
}

static irqreturn_t ibmveth_interrupt(int irq, void *dev_instance)
{
	struct napi_struct *napi = dev_instance;
	struct net_device *netdev = napi->dev;
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	unsigned long lpar_rc;
	int qindex;

	qindex = napi - adapter->napi;

	if (WARN_ON(qindex < 0 || qindex >= adapter->num_rx_queues))
		return IRQ_NONE;

	if (adapter->rx_qstats)
		adapter->rx_qstats[qindex].interrupts++;

	if (napi_schedule_prep(napi)) {
		lpar_rc = ibmveth_disable_irq(adapter, qindex);
		WARN_ON(lpar_rc != H_SUCCESS);
		__napi_schedule(napi);
	}
	return IRQ_HANDLED;
}

static void ibmveth_set_multicast_list(struct net_device *netdev)
{
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	unsigned long lpar_rc;

	if ((netdev->flags & IFF_PROMISC) ||
	    (netdev_mc_count(netdev) > adapter->mcastFilterSize)) {
		lpar_rc = h_multicast_ctrl(adapter->vdev->unit_address,
					   IbmVethMcastEnableRecv |
					   IbmVethMcastDisableFiltering,
					   0);
		if (lpar_rc != H_SUCCESS) {
			netdev_err(netdev, "h_multicast_ctrl rc=%ld when "
				   "entering promisc mode\n", lpar_rc);
		}
	} else {
		struct netdev_hw_addr *ha;
		/* clear the filter table & disable filtering */
		lpar_rc = h_multicast_ctrl(adapter->vdev->unit_address,
					   IbmVethMcastEnableRecv |
					   IbmVethMcastDisableFiltering |
					   IbmVethMcastClearFilterTable,
					   0);
		if (lpar_rc != H_SUCCESS) {
			netdev_err(netdev, "h_multicast_ctrl rc=%ld when "
				   "attempting to clear filter table\n",
				   lpar_rc);
		}
		/* add the addresses to the filter table */
		netdev_for_each_mc_addr(ha, netdev) {
			/* add the multicast address to the filter table */
			u64 mcast_addr;
			mcast_addr = ether_addr_to_u64(ha->addr);
			lpar_rc = h_multicast_ctrl(adapter->vdev->unit_address,
						   IbmVethMcastAddFilter,
						   mcast_addr);
			if (lpar_rc != H_SUCCESS) {
				netdev_err(netdev, "h_multicast_ctrl rc=%ld "
					   "when adding an entry to the filter "
					   "table\n", lpar_rc);
			}
		}

		/* re-enable filtering */
		lpar_rc = h_multicast_ctrl(adapter->vdev->unit_address,
					   IbmVethMcastEnableFiltering,
					   0);
		if (lpar_rc != H_SUCCESS) {
			netdev_err(netdev, "h_multicast_ctrl rc=%ld when "
				   "enabling filtering\n", lpar_rc);
		}
	}
}

static int ibmveth_change_mtu(struct net_device *dev, int new_mtu)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	struct vio_dev *viodev = adapter->vdev;
	int new_mtu_oh = new_mtu + IBMVETH_BUFF_OH;
	int i, rc;
	int need_restart = 0;

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++)
		if (new_mtu_oh <= adapter->rx_buff_pool[0][i].buff_size)
			break;

	if (i == IBMVETH_NUM_BUFF_POOLS)
		return -EINVAL;

	/* Deactivate all the buffer pools so that the next loop can activate
	   only the buffer pools necessary to hold the new MTU */
	if (netif_running(adapter->netdev)) {
		need_restart = 1;
		ibmveth_close(adapter->netdev);
	}

	/* Look for an active buffer pool that can hold the new MTU */
	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
		adapter->rx_buff_pool[0][i].active = 1;

		if (new_mtu_oh <= adapter->rx_buff_pool[0][i].buff_size) {
			WRITE_ONCE(dev->mtu, new_mtu);
			vio_cmo_set_dev_desired(viodev,
						ibmveth_get_desired_dma
						(viodev));
			if (need_restart) {
				return ibmveth_open(adapter->netdev);
			}
			return 0;
		}
	}

	if (need_restart && (rc = ibmveth_open(adapter->netdev)))
		return rc;

	return -EINVAL;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void ibmveth_poll_controller(struct net_device *dev)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	int i;

	for (i = 0; i < adapter->num_rx_queues; i++)
		ibmveth_replenish_task(adapter, i);

	for (i = 0; i < adapter->num_rx_queues; i++)
		ibmveth_interrupt(adapter->queue_irq[i], &adapter->napi[i]);
}
#endif

/**
 * ibmveth_get_desired_dma - Calculate IO memory desired by the driver
 *
 * @vdev: struct vio_dev for the device whose desired IO mem is to be returned
 *
 * Return value:
 *	Number of bytes of IO data the driver will need to perform well.
 */
static unsigned long ibmveth_get_desired_dma(struct vio_dev *vdev)
{
	struct net_device *netdev = dev_get_drvdata(&vdev->dev);
	struct ibmveth_adapter *adapter;
	struct iommu_table *tbl;
	unsigned long ret;
	int i, q;

	tbl = get_iommu_table_base(&vdev->dev);

	/* netdev inits at probe time along with the structures we need below*/
	if (netdev == NULL)
		return IOMMU_PAGE_ALIGN(IBMVETH_IO_ENTITLEMENT_DEFAULT, tbl);

	adapter = netdev_priv(netdev);

	ret = IBMVETH_BUFF_LIST_SIZE + IBMVETH_FILT_LIST_SIZE;
	ret += IOMMU_PAGE_ALIGN(netdev->mtu, tbl);
	/* add size of mapped tx buffers */
	ret += IOMMU_PAGE_ALIGN(IBMVETH_MAX_TX_BUF_SIZE, tbl);

	for (q = 0; q < adapter->num_rx_queues; q++) {
		int rxqentries = 1;

		for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
			/* add the size of the active receive buffers */
			if (adapter->rx_buff_pool[q][i].active)
				ret += adapter->rx_buff_pool[q][i].size *
					IOMMU_PAGE_ALIGN(adapter->rx_buff_pool[q][i].buff_size,
							 tbl);
			rxqentries += adapter->rx_buff_pool[q][i].size;
		}

		/* add the size of the receive queue entries */
		ret += IOMMU_PAGE_ALIGN(rxqentries *
					sizeof(struct ibmveth_rx_q_entry), tbl);
	}

	return ret;
}

static int ibmveth_set_mac_addr(struct net_device *dev, void *p)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	struct sockaddr *addr = p;
	u64 mac_address;
	int rc;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	mac_address = ether_addr_to_u64(addr->sa_data);
	rc = h_change_logical_lan_mac(adapter->vdev->unit_address, mac_address);
	if (rc) {
		netdev_err(adapter->netdev, "h_change_logical_lan_mac failed with rc=%d\n", rc);
		return rc;
	}

	eth_hw_addr_set(dev, addr->sa_data);

	return 0;
}

static netdev_features_t ibmveth_features_check(struct sk_buff *skb,
						struct net_device *dev,
						netdev_features_t features)
{
	/* Some physical adapters do not support segmentation offload with
	 * MSS < 224. Disable GSO for such packets to avoid adapter freeze.
	 * Note: Single-segment packets (gso_segs == 1) don't need this check
	 * as they bypass the LSO path and are transmitted without segmentation.
	 */
	if (skb_is_gso(skb)) {
		if (skb_shinfo(skb)->gso_size < IBMVETH_MIN_LSO_MSS) {
			netdev_warn_once(dev,
					 "MSS %u too small for LSO, disabling GSO\n",
					 skb_shinfo(skb)->gso_size);
			features &= ~NETIF_F_GSO_MASK;
		}
	}

	return vlan_features_check(skb, features);
}

/**
 * ibmveth_get_stats64 - Return aggregated per-queue statistics
 * @dev: network device
 * @stats: rtnl link statistics storage
 *
 * Sums per-queue rx_qstats and tx_qstats into the rtnl counters.
 * Callers use ndo_get_stats64(); avoid updating netdev->stats on the
 * xmit/poll paths to keep per-queue counters off the hot cache line.
 */
static void ibmveth_get_stats64(struct net_device *dev,
				struct rtnl_link_stats64 *stats)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	int i;

	if (adapter->rx_qstats) {
		for (i = 0; i < adapter->num_rx_queues; i++) {
			stats->rx_packets += adapter->rx_qstats[i].packets;
			stats->rx_bytes += adapter->rx_qstats[i].bytes;
		}
	}

	if (adapter->tx_qstats) {
		for (i = 0; i < dev->real_num_tx_queues; i++) {
			stats->tx_packets += adapter->tx_qstats[i].packets;
			stats->tx_bytes += adapter->tx_qstats[i].bytes;
			stats->tx_dropped += adapter->tx_qstats[i].dropped_packets;
		}
	}

	stats->tx_errors = dev->stats.tx_errors;
}

static const struct net_device_ops ibmveth_netdev_ops = {
	.ndo_open		= ibmveth_open,
	.ndo_stop		= ibmveth_close,
	.ndo_start_xmit		= ibmveth_start_xmit,
	.ndo_set_rx_mode	= ibmveth_set_multicast_list,
	.ndo_eth_ioctl		= ibmveth_ioctl,
	.ndo_change_mtu		= ibmveth_change_mtu,
	.ndo_fix_features	= ibmveth_fix_features,
	.ndo_set_features	= ibmveth_set_features,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address    = ibmveth_set_mac_addr,
	.ndo_features_check	= ibmveth_features_check,
	.ndo_get_stats64	= ibmveth_get_stats64,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= ibmveth_poll_controller,
#endif
};

static const struct attribute_group ibmveth_attr_group;

static ssize_t buffer_pools_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct net_device *netdev = dev_get_drvdata(dev);
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	int len = 0;
	int i, j;

	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "Queue  Pool  Size  BuffSize  Active  Available\n");
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "-----  ----  ----  --------  ------  ---------\n");

	for (i = 0; i < adapter->num_rx_queues; i++) {
		for (j = 0; j < IBMVETH_NUM_BUFF_POOLS; j++) {
			struct ibmveth_buff_pool *pool =
				&adapter->rx_buff_pool[i][j];

			len += scnprintf(buf + len, PAGE_SIZE - len,
					 "%5d  %4d  %4u  %8u  %6d  %9d\n",
					 i, j, pool->size, pool->buff_size,
					 pool->active,
					 atomic_read(&pool->available));

			if (len >= PAGE_SIZE - 100)
				goto out;
		}
	}

out:
	return len;
}
static DEVICE_ATTR_RO(buffer_pools);

static struct attribute *ibmveth_attrs[] = {
	&dev_attr_buffer_pools.attr,
	NULL,
};

static const struct attribute_group ibmveth_attr_group = {
	.attrs = ibmveth_attrs,
};

static int ibmveth_probe(struct vio_dev *dev, const struct vio_device_id *id)
{
	int rc, i, mac_len;
	struct net_device *netdev;
	struct ibmveth_adapter *adapter;
	unsigned char *mac_addr_p;
	__be32 *mcastFilterSize_p;
	long ret;
	unsigned long ret_attr;

	dev_dbg(&dev->dev, "entering ibmveth_probe for UA 0x%x\n",
		dev->unit_address);

	mac_addr_p = (unsigned char *)vio_get_attribute(dev, VETH_MAC_ADDR,
							&mac_len);
	if (!mac_addr_p) {
		dev_err(&dev->dev, "Can't find VETH_MAC_ADDR attribute\n");
		return -EINVAL;
	}
	/* Workaround for old/broken pHyp */
	if (mac_len == 8)
		mac_addr_p += 2;
	else if (mac_len != 6) {
		dev_err(&dev->dev, "VETH_MAC_ADDR attribute wrong len %d\n",
			mac_len);
		return -EINVAL;
	}

	mcastFilterSize_p = (__be32 *)vio_get_attribute(dev,
							VETH_MCAST_FILTER_SIZE,
							NULL);
	if (!mcastFilterSize_p) {
		dev_err(&dev->dev, "Can't find VETH_MCAST_FILTER_SIZE "
			"attribute\n");
		return -EINVAL;
	}

	netdev = alloc_etherdev_mqs(sizeof(struct ibmveth_adapter),
				    IBMVETH_MAX_QUEUES, IBMVETH_MAX_RX_QUEUES);
	if (!netdev)
		return -ENOMEM;

	adapter = netdev_priv(netdev);
	dev_set_drvdata(&dev->dev, netdev);

	adapter->vdev = dev;
	adapter->netdev = netdev;
	INIT_WORK(&adapter->work, ibmveth_reset);
	adapter->mcastFilterSize = be32_to_cpu(*mcastFilterSize_p);
	ibmveth_init_link_settings(netdev);

	for (i = 0; i < IBMVETH_MAX_RX_QUEUES; i++)
		netif_napi_add_weight(netdev, &adapter->napi[i], ibmveth_poll, 16);

	netdev->irq = dev->irq;
	netdev->netdev_ops = &ibmveth_netdev_ops;
	netdev->ethtool_ops = &netdev_ethtool_ops;
	SET_NETDEV_DEV(netdev, &dev->dev);
	netdev->hw_features = NETIF_F_SG;
	if (vio_get_attribute(dev, "ibm,illan-options", NULL) != NULL) {
		netdev->hw_features |= NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM |
				       NETIF_F_RXCSUM;
	}

	netdev->features |= netdev->hw_features;

	ret = h_illan_attributes(adapter->vdev->unit_address, 0, 0, &ret_attr);

	/* If running older firmware, TSO should not be enabled by default */
	if (ret == H_SUCCESS && (ret_attr & IBMVETH_ILLAN_LRG_SND_SUPPORT) &&
	    !old_large_send) {
		netdev->hw_features |= NETIF_F_TSO | NETIF_F_TSO6;
		netdev->features |= netdev->hw_features;
	} else {
		netdev->hw_features |= NETIF_F_TSO;
	}

	adapter->is_active_trunk = false;
	if (ret == H_SUCCESS && (ret_attr & IBMVETH_ILLAN_ACTIVE_TRUNK)) {
		adapter->is_active_trunk = true;
		netdev->hw_features |= NETIF_F_FRAGLIST;
		netdev->features |= NETIF_F_FRAGLIST;
	}

	if (ret == H_SUCCESS &&
	    (ret_attr & IBMVETH_ILLAN_RX_MULTI_QUEUE_SUPPORT)) {
		adapter->multi_queue = 1;
		adapter->num_rx_queues = min(num_online_cpus(), IBMVETH_DEFAULT_QUEUES);
		netdev_dbg(netdev, "RX multi queue mode enabled: %d queues\n",
			   adapter->num_rx_queues);
	} else {
		adapter->multi_queue = 0;
		adapter->num_rx_queues = 1;
	}

	if (ret == H_SUCCESS &&
	    (ret_attr & IBMVETH_ILLAN_RX_MULTI_BUFF_SUPPORT)) {
		if (adapter->multi_queue)
			adapter->rx_buffers_per_hcall = IBMVETH_MAX_RX_QUEUE;
		else
			adapter->rx_buffers_per_hcall = IBMVETH_MAX_RX_REGULAR;

		netdev_dbg(netdev,
			   "RX Multi-buffer hcall supported by FW, batch set to %u\n",
			   adapter->rx_buffers_per_hcall);
	} else {
		adapter->rx_buffers_per_hcall = 1;
		netdev_dbg(netdev,
			   "RX Single-buffer hcall mode, batch set to %u\n",
			   adapter->rx_buffers_per_hcall);
	}

	netdev->min_mtu = IBMVETH_MIN_MTU;
	netdev->max_mtu = ETH_MAX_MTU - IBMVETH_BUFF_OH;

	eth_hw_addr_set(netdev, mac_addr_p);

	if (firmware_has_feature(FW_FEATURE_CMO))
		memcpy(pool_count, pool_count_cmo, sizeof(pool_count));

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
		struct kobject *kobj = &adapter->rx_buff_pool[0][i].kobj;
		int error;

		ibmveth_init_buffer_pool(&adapter->rx_buff_pool[0][i], i,
					 pool_count[i], pool_size[i],
					 pool_active[i]);
		error = kobject_init_and_add(kobj, &ktype_veth_pool,
					     &dev->dev.kobj, "pool%d", i);
		if (!error)
			kobject_uevent(kobj, KOBJ_ADD);
	}

	rc = netif_set_real_num_tx_queues(netdev, min(num_online_cpus(),
						      IBMVETH_DEFAULT_QUEUES));
	if (rc) {
		netdev_dbg(netdev, "failed to set number of tx queues rc=%d\n",
			   rc);
		free_netdev(netdev);
		return rc;
	}
	adapter->tx_ltb_size = PAGE_ALIGN(IBMVETH_MAX_TX_BUF_SIZE);
	for (i = 0; i < IBMVETH_MAX_QUEUES; i++)
		adapter->tx_ltb_ptr[i] = NULL;

	netdev_dbg(netdev, "adapter @ 0x%p\n", adapter);
	netdev_dbg(netdev, "registering netdev...\n");

	ibmveth_set_features(netdev, netdev->features);

	rc = register_netdev(netdev);

	if (rc) {
		netdev_dbg(netdev, "failed to register netdev rc=%d\n", rc);
		free_netdev(netdev);
		return rc;
	}

	netdev_dbg(netdev, "registered\n");

	rc = sysfs_create_group(&dev->dev.kobj, &ibmveth_attr_group);
	if (rc) {
		netdev_err(netdev, "failed to create sysfs attributes rc=%d\n", rc);
		unregister_netdev(netdev);
		free_netdev(netdev);
		return rc;
	}

	return 0;
}

static void ibmveth_remove(struct vio_dev *dev)
{
	struct net_device *netdev = dev_get_drvdata(&dev->dev);
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	int i;

	cancel_work_sync(&adapter->work);

	sysfs_remove_group(&dev->dev.kobj, &ibmveth_attr_group);

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++)
		kobject_put(&adapter->rx_buff_pool[0][i].kobj);

	unregister_netdev(netdev);

	free_netdev(netdev);
	dev_set_drvdata(&dev->dev, NULL);
}

static struct attribute veth_active_attr;
static struct attribute veth_num_attr;
static struct attribute veth_size_attr;

static ssize_t veth_pool_show(struct kobject *kobj,
			      struct attribute *attr, char *buf)
{
	struct ibmveth_buff_pool *pool = container_of(kobj,
						      struct ibmveth_buff_pool,
						      kobj);

	if (attr == &veth_active_attr)
		return sprintf(buf, "%d\n", pool->active);
	else if (attr == &veth_num_attr)
		return sprintf(buf, "%d\n", pool->size);
	else if (attr == &veth_size_attr)
		return sprintf(buf, "%d\n", pool->buff_size);
	return 0;
}

/**
 * veth_pool_store - sysfs store handler for pool attributes
 * @kobj: kobject embedded in pool
 * @attr: attribute being changed
 * @buf: value being stored
 * @count: length of @buf in bytes
 *
 * Stores new value in pool attribute. Verifies the range of the new value for
 * size and buff_size. Verifies that at least one pool remains available to
 * receive MTU-sized packets.
 *
 * Context: Process context.
 *          Takes and releases rtnl_mutex to ensure correct ordering of close
 *	    and open calls.
 * Return:
 * * %-EPERM  - Not allowed to disabled all MTU-sized buffer pools
 * * %-EINVAL - New pool size or buffer size is out of range
 * * count    - Return count for success
 * * other    - Return value from a failed ibmveth_open call
 */
static ssize_t veth_pool_store(struct kobject *kobj, struct attribute *attr,
			       const char *buf, size_t count)
{
	struct ibmveth_buff_pool *pool = container_of(kobj,
						      struct ibmveth_buff_pool,
						      kobj);
	struct net_device *netdev = dev_get_drvdata(kobj_to_dev(kobj->parent));
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	long value = simple_strtol(buf, NULL, 10);
	bool change = false;
	u32 newbuff_size;
	u32 oldbuff_size;
	int newactive;
	int oldactive;
	u32 newsize;
	u32 oldsize;
	long rc;

	rtnl_lock();

	oldbuff_size = pool->buff_size;
	oldactive = pool->active;
	oldsize = pool->size;

	newbuff_size = oldbuff_size;
	newactive = oldactive;
	newsize = oldsize;

	if (attr == &veth_active_attr) {
		if (value && !oldactive) {
			newactive = 1;
			change = true;
		} else if (!value && oldactive) {
			int mtu = netdev->mtu + IBMVETH_BUFF_OH;
			int i;
			/* Make sure there is a buffer pool with buffers that
			   can hold a packet of the size of the MTU */
			for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
				if (pool == &adapter->rx_buff_pool[0][i])
					continue;
				if (!adapter->rx_buff_pool[0][i].active)
					continue;
				if (mtu <= adapter->rx_buff_pool[0][i].buff_size)
					break;
			}

			if (i == IBMVETH_NUM_BUFF_POOLS) {
				netdev_err(netdev, "no active pool >= MTU\n");
				rc = -EPERM;
				goto unlock_err;
			}

			newactive = 0;
			change = true;
		}
	} else if (attr == &veth_num_attr) {
		if (value <= 0 || value > IBMVETH_MAX_POOL_COUNT) {
			rc = -EINVAL;
			goto unlock_err;
		}
		if (value != oldsize) {
			newsize = value;
			change = true;
		}
	} else if (attr == &veth_size_attr) {
		if (value <= IBMVETH_BUFF_OH || value > IBMVETH_MAX_BUF_SIZE) {
			rc = -EINVAL;
			goto unlock_err;
		}
		if (value != oldbuff_size) {
			newbuff_size = value;
			change = true;
		}
	}

	if (change) {
		if (netif_running(netdev))
			ibmveth_close(netdev);

		pool->active = newactive;
		pool->buff_size = newbuff_size;
		pool->size = newsize;

		if (netif_running(netdev)) {
			rc = ibmveth_open(netdev);
			if (rc) {
				pool->active = oldactive;
				pool->buff_size = oldbuff_size;
				pool->size = oldsize;
				goto unlock_err;
			}
		}
	}
	rtnl_unlock();

	/* kick the interrupt handler to allocate/deallocate pools */
	ibmveth_interrupt(netdev->irq, &adapter->napi[0]);
	return count;

unlock_err:
	rtnl_unlock();
	return rc;
}


#define ATTR(_name, _mode)				\
	struct attribute veth_##_name##_attr = {	\
	.name = __stringify(_name), .mode = _mode,	\
	};

static ATTR(active, 0644);
static ATTR(num, 0644);
static ATTR(size, 0644);

static struct attribute *veth_pool_attrs[] = {
	&veth_active_attr,
	&veth_num_attr,
	&veth_size_attr,
	NULL,
};
ATTRIBUTE_GROUPS(veth_pool);

static const struct sysfs_ops veth_pool_ops = {
	.show   = veth_pool_show,
	.store  = veth_pool_store,
};

static struct kobj_type ktype_veth_pool = {
	.release        = NULL,
	.sysfs_ops      = &veth_pool_ops,
	.default_groups = veth_pool_groups,
};

static int ibmveth_resume(struct device *dev)
{
	struct net_device *netdev = dev_get_drvdata(dev);
	struct ibmveth_adapter *adapter = netdev_priv(netdev);

	ibmveth_interrupt(netdev->irq, &adapter->napi[0]);
	return 0;
}

static const struct vio_device_id ibmveth_device_table[] = {
	{ "network", "IBM,l-lan"},
	{ "", "" }
};
MODULE_DEVICE_TABLE(vio, ibmveth_device_table);

static const struct dev_pm_ops ibmveth_pm_ops = {
	.resume = ibmveth_resume
};

static struct vio_driver ibmveth_driver = {
	.id_table	= ibmveth_device_table,
	.probe		= ibmveth_probe,
	.remove		= ibmveth_remove,
	.get_desired_dma = ibmveth_get_desired_dma,
	.name		= ibmveth_driver_name,
	.pm		= &ibmveth_pm_ops,
};

static int __init ibmveth_module_init(void)
{
	printk(KERN_DEBUG "%s: %s %s\n", ibmveth_driver_name,
	       ibmveth_driver_string, ibmveth_driver_version);

	return vio_register_driver(&ibmveth_driver);
}

static void __exit ibmveth_module_exit(void)
{
	vio_unregister_driver(&ibmveth_driver);
}

module_init(ibmveth_module_init);
module_exit(ibmveth_module_exit);

#ifdef CONFIG_IBMVETH_KUNIT_TEST
#include <kunit/test.h>

/**
 * ibmveth_reset_kunit - reset routine for running in KUnit environment
 *
 * @w: pointer to work_struct embedded in adapter structure
 *
 * Context: Called in the KUnit environment. Does nothing.
 *
 * Return: void
 */
static void ibmveth_reset_kunit(struct work_struct *w)
{
	netdev_dbg(NULL, "reset_kunit starting\n");
	netdev_dbg(NULL, "reset_kunit complete\n");
}

/**
 * ibmveth_remove_buffer_from_pool_test - unit test for some of
 *                                        ibmveth_remove_buffer_from_pool
 * @test: pointer to kunit structure
 *
 * Tests the error returns from ibmveth_remove_buffer_from_pool.
 * ibmveth_remove_buffer_from_pool also calls WARN_ON, so dmesg should be
 * checked to see that these warnings happened.
 *
 * Return: void
 */
static void ibmveth_remove_buffer_from_pool_test(struct kunit *test)
{
	struct ibmveth_adapter *adapter = kunit_kzalloc(test, sizeof(*adapter), GFP_KERNEL);
	struct ibmveth_buff_pool *pool;
	u64 correlator;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, adapter);

	INIT_WORK(&adapter->work, ibmveth_reset_kunit);

	/* Set sane values for buffer pools */
	for (int i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++)
		ibmveth_init_buffer_pool(&adapter->rx_buff_pool[0][i], i,
					 pool_count[i], pool_size[i],
					 pool_active[i]);

	pool = &adapter->rx_buff_pool[0][0];
	pool->skbuff = kunit_kcalloc(test, pool->size, sizeof(void *), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pool->skbuff);

	correlator = ((u64)IBMVETH_NUM_BUFF_POOLS << 32) | 0;
	KUNIT_EXPECT_EQ(test, -EINVAL,
			ibmveth_remove_buffer_from_pool(adapter, correlator, 0, false));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			ibmveth_remove_buffer_from_pool(adapter, correlator, 0, true));

	correlator = ((u64)0 << 32) | adapter->rx_buff_pool[0][0].size;
	KUNIT_EXPECT_EQ(test, -EINVAL,
			ibmveth_remove_buffer_from_pool(adapter, correlator, 0, false));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			ibmveth_remove_buffer_from_pool(adapter, correlator, 0, true));

	correlator = (u64)0 | 0;
	pool->skbuff[0] = NULL;
	KUNIT_EXPECT_EQ(test, -EFAULT,
			ibmveth_remove_buffer_from_pool(adapter, correlator, 0, false));
	KUNIT_EXPECT_EQ(test, -EFAULT,
			ibmveth_remove_buffer_from_pool(adapter, correlator, 0, true));

	flush_work(&adapter->work);
}

/**
 * ibmveth_rxq_get_buffer_test - unit test for ibmveth_rxq_get_buffer
 * @test: pointer to kunit structure
 *
 * Tests ibmveth_rxq_get_buffer. ibmveth_rxq_get_buffer also calls WARN_ON for
 * the NULL returns, so dmesg should be checked to see that these warnings
 * happened.
 *
 * Return: void
 */
static void ibmveth_rxq_get_buffer_test(struct kunit *test)
{
	struct ibmveth_adapter *adapter = kunit_kzalloc(test, sizeof(*adapter), GFP_KERNEL);
	struct sk_buff *skb = kunit_kzalloc(test, sizeof(*skb), GFP_KERNEL);
	struct ibmveth_buff_pool *pool;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, adapter);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, skb);

	INIT_WORK(&adapter->work, ibmveth_reset_kunit);

	adapter->rx_queue[0].queue_len = 1;
	adapter->rx_queue[0].index = 0;
	adapter->rx_queue[0].queue_addr =
		kunit_kzalloc(test, sizeof(struct ibmveth_rx_q_entry),
			      GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, adapter->rx_queue[0].queue_addr);

	/* Set sane values for buffer pools */
	for (int i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++)
		ibmveth_init_buffer_pool(&adapter->rx_buff_pool[0][i], i,
					 pool_count[i], pool_size[i],
					 pool_active[i]);

	pool = &adapter->rx_buff_pool[0][0];
	pool->skbuff = kunit_kcalloc(test, pool->size, sizeof(void *), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pool->skbuff);

	adapter->rx_queue[0].queue_addr[0].correlator = (u64)IBMVETH_NUM_BUFF_POOLS << 32 | 0;
	KUNIT_EXPECT_PTR_EQ(test, NULL, ibmveth_rxq_get_buffer(adapter, 0));

	adapter->rx_queue[0].queue_addr[0].correlator =
		(u64)0 << 32 | adapter->rx_buff_pool[0][0].size;
	KUNIT_EXPECT_PTR_EQ(test, NULL, ibmveth_rxq_get_buffer(adapter, 0));

	pool->skbuff[0] = skb;
	adapter->rx_queue[0].queue_addr[0].correlator = (u64)0 << 32 | 0;
	KUNIT_EXPECT_PTR_EQ(test, skb, ibmveth_rxq_get_buffer(adapter, 0));

	flush_work(&adapter->work);
}

static struct kunit_case ibmveth_test_cases[] = {
	KUNIT_CASE(ibmveth_remove_buffer_from_pool_test),
	KUNIT_CASE(ibmveth_rxq_get_buffer_test),
	{}
};

static struct kunit_suite ibmveth_test_suite = {
	.name = "ibmveth-kunit-test",
	.test_cases = ibmveth_test_cases,
};

kunit_test_suite(ibmveth_test_suite);
#endif
