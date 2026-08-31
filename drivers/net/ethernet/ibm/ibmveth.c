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
#include <linux/debugfs.h>
#include <asm/hvcall.h>
#include <linux/atomic.h>
#include <asm/vio.h>
#include <asm/iommu.h>
#include <asm/firmware.h>
#include <net/tcp.h>
#include <net/ip6_checksum.h>
#include <net/netdev_queues.h>

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

/**
 * enum ibmveth_stat_src - where an ethtool -S counter is stored
 * @IBMVETH_STAT_ADAPTER: plain u64 in struct ibmveth_adapter
 * @IBMVETH_STAT_RX_QSUM: per-queue u64, summed over rx_qstats[]
 * @IBMVETH_STAT_TX_QSUM: per-queue u64, summed over tx_qstats[]
 * @IBMVETH_STAT_RX_NO_BUFFER: rx_qstats[] live-page absolute plus the
 *	absolutes carried over from pages the queue has already retired
 *
 * Counters live per-queue so multi-queue writers never share a field.
 * The adapter is only ever read from ethtool, so summing there is free.
 */
enum ibmveth_stat_src {
	IBMVETH_STAT_ADAPTER,
	IBMVETH_STAT_RX_QSUM,
	IBMVETH_STAT_TX_QSUM,
	IBMVETH_STAT_RX_NO_BUFFER,
};

struct ibmveth_stat {
	char name[ETH_GSTRING_LEN];
	enum ibmveth_stat_src src;
	/* Offset into the struct named by @src. */
	size_t off;
};

#define IBMVETH_STAT_OFF(stat) offsetof(struct ibmveth_adapter, stat)
#define IBMVETH_RXQ_OFF(stat) offsetof(struct ibmveth_rx_queue_stats, stat)
#define IBMVETH_TXQ_OFF(stat) offsetof(struct ibmveth_tx_queue_stats, stat)
#define IBMVETH_GET_STAT(a, off) *((u64 *)(((unsigned long)(a)) + off))

#define IBMVETH_ADAPTER_STAT(key, field) \
	{ key, IBMVETH_STAT_ADAPTER, IBMVETH_STAT_OFF(field) }
#define IBMVETH_RXQ_STAT(key, field) \
	{ key, IBMVETH_STAT_RX_QSUM, IBMVETH_RXQ_OFF(field) }
#define IBMVETH_TXQ_STAT(key, field) \
	{ key, IBMVETH_STAT_TX_QSUM, IBMVETH_TXQ_OFF(field) }

/*
 * Key names and their order are ABI. Do not reorder or rename; append
 * only, and only when the counter is worth a permanent interface.
 */
static struct ibmveth_stat ibmveth_stats[] = {
	IBMVETH_RXQ_STAT("replenish_task_cycles", replenish_task_cycles),
	IBMVETH_RXQ_STAT("replenish_no_mem", replenish_no_mem),
	IBMVETH_RXQ_STAT("replenish_add_buff_failure",
			 replenish_add_buff_failure),
	IBMVETH_RXQ_STAT("replenish_add_buff_success",
			 replenish_add_buff_success),
	IBMVETH_RXQ_STAT("rx_invalid_buffer", invalid_buffers),
	{ "rx_no_buffer", IBMVETH_STAT_RX_NO_BUFFER,
	  IBMVETH_RXQ_OFF(no_buffer_drops) },
	IBMVETH_ADAPTER_STAT("tx_map_failed", tx_map_failed),
	IBMVETH_TXQ_STAT("tx_send_failed", send_failures),
	IBMVETH_ADAPTER_STAT("fw_enabled_ipv4_csum", fw_ipv4_csum_support),
	IBMVETH_ADAPTER_STAT("fw_enabled_ipv6_csum", fw_ipv6_csum_support),
	IBMVETH_TXQ_STAT("tx_large_packets", large_packets),
	IBMVETH_RXQ_STAT("rx_large_packets", large_packets),
	IBMVETH_ADAPTER_STAT("fw_enabled_large_send", fw_large_send_support),
};

/**
 * struct ibmveth_qstat - a per-queue counter exposed through ethtool -S
 * @fmt: key name, taking the queue index as its only argument
 * @off: offset into the matching per-queue stats struct
 *
 * Driving the strings and the values from one table keeps the two in
 * step; get_sset_count() derives its length from ARRAY_SIZE() so the
 * three cannot drift apart.
 */
struct ibmveth_qstat {
	const char *fmt;
	size_t off;
};

/*
 * Only counters with no home in the standard interfaces belong here.
 * packets, bytes and drops are reported through netdev_stat_ops.
 */
static const struct ibmveth_qstat ibmveth_rx_qstat_keys[] = {
	{ "rx%d_interrupts", IBMVETH_RXQ_OFF(interrupts) },
	{ "rx%d_polls", IBMVETH_RXQ_OFF(polls) },
	{ "rx%d_large_packets", IBMVETH_RXQ_OFF(large_packets) },
	{ "rx%d_invalid_buffers", IBMVETH_RXQ_OFF(invalid_buffers) },
	{ "rx%d_no_buffer_drops", IBMVETH_RXQ_OFF(no_buffer_drops) },
};

static const struct ibmveth_qstat ibmveth_tx_qstat_keys[] = {
	{ "tx%d_large_packets", IBMVETH_TXQ_OFF(large_packets) },
	{ "tx%d_send_failures", IBMVETH_TXQ_OFF(send_failures) },
	{ "tx%d_checksum_offload", IBMVETH_TXQ_OFF(checksum_offload) },
};

#define IBMVETH_NUM_RX_QSTATS ARRAY_SIZE(ibmveth_rx_qstat_keys)
#define IBMVETH_NUM_TX_QSTATS ARRAY_SIZE(ibmveth_tx_qstat_keys)

/* simple methods of getting data from the current rxq entry */
static u32 ibmveth_rxq_flags(struct ibmveth_adapter *adapter,
			     int queue_index)
{
	struct ibmveth_rx_q *rxq = &adapter->rx_queue[queue_index];

	return be32_to_cpu(rxq->queue_addr[rxq->index].flags_off);
}

static int ibmveth_rxq_toggle(struct ibmveth_adapter *adapter,
			      int queue_index)
{
	return (ibmveth_rxq_flags(adapter, queue_index) & IBMVETH_RXQ_TOGGLE) >>
		IBMVETH_RXQ_TOGGLE_SHIFT;
}

static int ibmveth_rxq_pending_buffer(struct ibmveth_adapter *adapter,
				      int queue_index)
{
	return ibmveth_rxq_toggle(adapter, queue_index) ==
		adapter->rx_queue[queue_index].toggle;
}

static int ibmveth_rxq_buffer_valid(struct ibmveth_adapter *adapter,
				    int queue_index)
{
	return ibmveth_rxq_flags(adapter, queue_index) & IBMVETH_RXQ_VALID;
}

static int ibmveth_rxq_frame_offset(struct ibmveth_adapter *adapter,
				    int queue_index)
{
	return ibmveth_rxq_flags(adapter, queue_index) & IBMVETH_RXQ_OFF_MASK;
}

static int ibmveth_rxq_large_packet(struct ibmveth_adapter *adapter,
				    int queue_index)
{
	return ibmveth_rxq_flags(adapter, queue_index) & IBMVETH_RXQ_LRG_PKT;
}

static int ibmveth_rxq_frame_length(struct ibmveth_adapter *adapter,
				    int queue_index)
{
	struct ibmveth_rx_q *rxq = &adapter->rx_queue[queue_index];

	return be32_to_cpu(rxq->queue_addr[rxq->index].length);
}

static int ibmveth_rxq_csum_good(struct ibmveth_adapter *adapter,
				 int queue_index)
{
	return ibmveth_rxq_flags(adapter, queue_index) & IBMVETH_RXQ_CSUM_GOOD;
}

/* Lockless IRQ/poll readers vs resize publishers. */
static unsigned int
ibmveth_get_num_rx_queues(const struct ibmveth_adapter *adapter)
{
	/*
	 * Pairs with the release in ibmveth_publish_num_rx_queues(): a reader
	 * that sees the new count also sees the per-queue state behind it.
	 */
	return smp_load_acquire(&adapter->num_rx_queues);
}

static void
ibmveth_publish_num_rx_queues(struct ibmveth_adapter *adapter,
			      unsigned int num)
{
	/*
	 * Pairs with the acquire in ibmveth_get_num_rx_queues(): per-queue
	 * state must be visible to a reader before it observes the new count.
	 */
	smp_store_release(&adapter->num_rx_queues, num);
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
static int
ibmveth_alloc_filter_list(struct ibmveth_adapter *adapter)
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
		/* Do not leave DMA_MAPPING_ERROR for free_filter_list(). */
		adapter->filter_list_dma = 0;
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
static void
ibmveth_free_filter_list(struct ibmveth_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;

	/* Unmap by CPU pointer: SPAPR can return DMA address 0. */
	if (adapter->filter_list_addr) {
		dma_unmap_single(dev, adapter->filter_list_dma, 4096,
				 DMA_BIDIRECTIONAL);
		adapter->filter_list_dma = 0;
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
				     sizeof(*adapter->rx_qstats),
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
				     sizeof(*adapter->tx_qstats),
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

	for (i = 0; i < ibmveth_get_num_rx_queues(adapter); i++) {
		adapter->buffer_list_addr[i] =
			(void *)get_zeroed_page(GFP_KERNEL);
		if (!adapter->buffer_list_addr[i]) {
			netdev_err(netdev,
				   "unable to allocate buffer list for queue %d\n",
				   i);
			goto err_cleanup;
		}

		adapter->rx_queue[i].queue_len =
			sizeof(struct ibmveth_rx_q_entry) * rxq_entries;
		adapter->rx_queue[i].queue_addr =
			dma_alloc_coherent(dev, adapter->rx_queue[i].queue_len,
					   &adapter->rx_queue[i].queue_dma,
					   GFP_KERNEL);
		if (!adapter->rx_queue[i].queue_addr) {
			netdev_err(netdev,
				   "unable to allocate RX queue for queue %d\n",
				   i);
			free_page((unsigned long)adapter->buffer_list_addr[i]);
			adapter->buffer_list_addr[i] = NULL;
			goto err_cleanup;
		}

		adapter->buffer_list_dma[i] =
			dma_map_single(dev, adapter->buffer_list_addr[i],
				       4096, DMA_BIDIRECTIONAL);
		if (dma_mapping_error(dev, adapter->buffer_list_dma[i])) {
			netdev_err(netdev,
				   "unable to map buffer list for queue %d\n",
				   i);
			free_page((unsigned long)adapter->buffer_list_addr[i]);
			adapter->buffer_list_addr[i] = NULL;
			adapter->buffer_list_dma[i] = 0;
			goto err_cleanup;
		}

		adapter->rx_queue[i].index = 0;
		adapter->rx_queue[i].num_slots = rxq_entries;
		adapter->rx_queue[i].toggle = 1;

		netdev_dbg(netdev, "queue %d: buffer_list @ 0x%p (DMA: 0x%llx), rx_queue @ 0x%p (DMA: 0x%llx), %llu entries\n",
			   i, adapter->buffer_list_addr[i],
			   (unsigned long long)adapter->buffer_list_dma[i],
			   adapter->rx_queue[i].queue_addr,
			   (unsigned long long)adapter->rx_queue[i].queue_dma,
			   (unsigned long long)rxq_entries);
	}

	netdev_dbg(netdev, "allocated %u RX queue(s) with %d entries each\n",
		   ibmveth_get_num_rx_queues(adapter), rxq_entries);

	return 0;

err_cleanup:
	/*
	 * Every failure path above releases what it had already allocated
	 * for queue i, so each index here is either fully constructed or
	 * fully empty. Do not unmap buffer_list_dma[] without the matching
	 * buffer_list_addr[] check: the two are only ever set together.
	 */
	for (; i >= 0; i--) {
		if (adapter->buffer_list_addr[i]) {
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
static void
ibmveth_cleanup_rx_resources(struct ibmveth_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	int i;

	netdev_dbg(adapter->netdev, "cleaning up %u RX queue(s)\n",
		   ibmveth_get_num_rx_queues(adapter));

	for (i = 0; i < ibmveth_get_num_rx_queues(adapter); i++) {
		if (adapter->buffer_list_addr[i]) {
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
 * Return: 0 on success, negative errno on failure (never raw H_*).
 */
static int
ibmveth_toggle_irq(struct ibmveth_adapter *adapter, int queue_index,
		   bool enable)
{
	unsigned long h_rc;
	unsigned long irq = adapter->queue_irq[queue_index];
	const char *action = enable ? "enable" : "disable";

	if (queue_index == 0) {
		/* Primary queue: use h_vio_signal() */
		h_rc = h_vio_signal(adapter->vdev->unit_address,
				    enable ? VIO_IRQ_ENABLE : VIO_IRQ_DISABLE);
	} else {
		/* Subordinate queues: use H_VIOCTL with hardware IRQ */
		struct irq_data *irq_data = irq_get_irq_data(irq);
		irq_hw_number_t hwirq;
		u64 vioctl_cmd = enable ? H_ENABLE_VIO_INTERRUPT :
			H_DISABLE_VIO_INTERRUPT;

		if (!irq_data) {
			netdev_err(adapter->netdev,
				   "Failed to get IRQ data for queue %d (virq=%lu)\n",
				   queue_index, irq);
			return -EINVAL;
		}

		hwirq = irqd_to_hwirq(irq_data);
		h_rc = plpar_hcall_norets(H_VIOCTL,
					  adapter->vdev->unit_address,
					  vioctl_cmd,
					  hwirq, 0, 0);

		/*
		 * H_PARAMETER is ambiguous (already in requested state vs bad
		 * args). Fold only on disable as an idempotent mask. On enable
		 * keep it an error so a stuck-masked queue stays visible to
		 * poll/resize recovery.
		 */
		if (h_rc == H_PARAMETER && !enable) {
			dev_warn_ratelimited(&adapter->netdev->dev,
					     "H_VIOCTL %s IRQ returned H_PARAMETER for queue %d (hwirq=%lu)\n",
					     action, queue_index, hwirq);
			return 0;
		}
	}

	if (h_rc) {
		dev_err_ratelimited(&adapter->netdev->dev,
				    "Failed to %s IRQ for queue %d, rc=0x%lx\n",
				    action, queue_index, h_rc);
		return -EIO;
	}
	return 0;
}

/**
 * ibmveth_disable_irq - Disable interrupt for a specific queue
 * @adapter: ibmveth adapter structure
 * @queue_index: Index of the queue (0 for primary, 1+ for subordinate)
 *
 * Return: 0 on success, negative errno on failure
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
 * Return: 0 on success, negative errno on failure
 */
static int
ibmveth_enable_irq(struct ibmveth_adapter *adapter, int queue_index)
{
	return ibmveth_toggle_irq(adapter, queue_index, true);
}

/**
 * ibmveth_dispose_subordinate_irq_mapping - Drop one subordinate virq mapping
 * @adapter: ibmveth adapter structure
 * @queue_idx: RX queue index (1..N)
 *
 * Subordinate queues get mappings from irq_create_mapping() during PHYP
 * registration. Queue 0 uses netdev->irq from device tree and is left alone.
 *
 * Bound against IBMVETH_MAX_RX_QUEUES, not num_rx_queues: a caller may
 * dispose a queue that is no longer in the published live set but still
 * owns a virq in queue_irq[]. Contrast with the bulk helper, which only
 * walks 1..num_rx_queues-1 (close / open-fail cleanup of the live set).
 *
 * Linux virq lifetime is owned by interrupt cleanup helpers. Call this only
 * after free_irq() when a handler was installed, or from registration failure
 * cleanup before request_irq().
 */
static void
ibmveth_dispose_subordinate_irq_mapping(struct ibmveth_adapter *adapter,
					int queue_idx)
{
	if (queue_idx <= 0 || queue_idx >= IBMVETH_MAX_RX_QUEUES)
		return;

	if (adapter->queue_irq[queue_idx]) {
		irq_dispose_mapping(adapter->queue_irq[queue_idx]);
		adapter->queue_irq[queue_idx] = 0;
	}
}

/**
 * ibmveth_dispose_subordinate_irq_mappings - Drop virq mappings for queues 1..N
 * @adapter: ibmveth adapter structure
 *
 * Bulk helper for close / open-fail cleanup of the published live set
 * (queues 1..num_rx_queues-1). Paths that need a retired or not-yet-published
 * queue must call ibmveth_dispose_subordinate_irq_mapping() directly.
 */
static void
ibmveth_dispose_subordinate_irq_mappings(struct ibmveth_adapter *adapter)
{
	int i;

	for (i = 1; i < ibmveth_get_num_rx_queues(adapter); i++)
		ibmveth_dispose_subordinate_irq_mapping(adapter, i);
}

/**
 * ibmveth_setup_rx_interrupts - Register IRQs and enable NAPI
 * @adapter: ibmveth adapter structure
 *
 * Registers interrupt handlers for all RX queues, enables NAPI, then
 * enables hypervisor interrupt delivery for multi-queue mode after
 * every queue has a Linux handler installed. For multi-queue open the
 * caller should replenish RX buffers before this helper so traffic
 * during open is not dropped (PHYP only interrupts after a successful
 * enqueue, which needs buffers). Single-queue open leaves PHYP masked
 * here and kicks NAPI afterward (classic path: first poll posts then
 * enables).
 *
 * Return: 0 on success, negative error code on failure
 */
static int
ibmveth_setup_rx_interrupts(struct ibmveth_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int i, rc, num = ibmveth_get_num_rx_queues(adapter);

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
				for (; i >= 0; i--) {
					ibmveth_disable_irq(adapter, i);
					synchronize_irq(adapter->queue_irq[i]);
				}
				rc = -EIO;
				goto err_disable_napi;
			}
		}
	}

	/* Set only on full success; fail paths leave this false so a later
	 * close() / cleanup is a no-op.
	 */
	adapter->rx_irq_setup = true;
	return 0;

err_disable_napi:
	/* STOP: remask after napi_disable; an in-flight poll can re-arm. */
	for (i = 0; i < num; i++)
		napi_disable(&adapter->napi[i]);
	for (i = 0; i < num; i++) {
		if (!adapter->queue_irq[i])
			continue;
		ibmveth_disable_irq(adapter, i);
		synchronize_irq(adapter->queue_irq[i]);
	}
	for (i = 0; i < num; i++) {
		if (adapter->queue_irq[i])
			free_irq(adapter->queue_irq[i], &adapter->napi[i]);
	}
	goto err_dispose_mappings;

err_free_irqs:
	while (--i >= 0)
		free_irq(adapter->queue_irq[i], &adapter->napi[i]);
err_dispose_mappings:
	/* Both setup failure paths own subordinate virq disposal. */
	ibmveth_dispose_subordinate_irq_mappings(adapter);
	return rc;
}

/**
 * ibmveth_cleanup_rx_interrupts - Mask PHYP IRQs, stop NAPI, and free IRQs
 * @adapter: ibmveth adapter structure
 *
 * Mask and synchronize each queue IRQ before napi_disable() so the handler
 * cannot miss a PHYP mask while NAPI is already dead. Remask after
 * napi_disable() in case an in-flight poll re-armed PHYP while we waited.
 * free_irq() runs only after that. Safe for close and for open failure after
 * setup_rx_interrupts() already unmasked PHYP. No-op if setup never
 * succeeded (avoids double napi_disable / free_irq after a failed close+open
 * while IFF_UP remains set).
 */
static void
ibmveth_cleanup_rx_interrupts(struct ibmveth_adapter *adapter)
{
	int i;

	if (!adapter->rx_irq_setup)
		return;

	for (i = 0; i < ibmveth_get_num_rx_queues(adapter); i++) {
		if (!adapter->queue_irq[i])
			continue;
		ibmveth_disable_irq(adapter, i);
		synchronize_irq(adapter->queue_irq[i]);
	}

	for (i = 0; i < ibmveth_get_num_rx_queues(adapter); i++)
		napi_disable(&adapter->napi[i]);

	for (i = 0; i < ibmveth_get_num_rx_queues(adapter); i++) {
		if (!adapter->queue_irq[i])
			continue;
		ibmveth_disable_irq(adapter, i);
		synchronize_irq(adapter->queue_irq[i]);
	}

	for (i = 0; i < ibmveth_get_num_rx_queues(adapter); i++) {
		if (adapter->queue_irq[i])
			free_irq(adapter->queue_irq[i], &adapter->napi[i]);
	}

	ibmveth_dispose_subordinate_irq_mappings(adapter);

	/* Queue 0 uses netdev->irq; leave queue_irq[0] for next open. */
	adapter->rx_irq_setup = false;
}

/**
 * ibmveth_schedule_rx_queue - Mask PHYP IRQ and schedule NAPI for one RX queue
 * @adapter: ibmveth adapter structure
 * @qindex: RX queue index
 *
 * Shared by the IRQ handler and process-context kick sites (open, resume,
 * pool sysfs, poll_controller).
 *
 * Return: true if napi_schedule_prep() succeeded and NAPI was scheduled.
 * Mask is attempted in that case; a failed disable_irq() is logged by the
 * helper and does not change the return (queue may still be unmasked).
 * false if the index is out of range or prep failed (including NAPI
 * already scheduled).
 */
static bool ibmveth_schedule_rx_queue(struct ibmveth_adapter *adapter,
				      int qindex)
{
	struct napi_struct *napi = &adapter->napi[qindex];

	if (WARN_ON(qindex < 0 || qindex >= ibmveth_get_num_rx_queues(adapter)))
		return false;

	/*
	 * Only mask PHYP when NAPI will run. Masking on prep failure can
	 * race a completing poll that already re-enabled the queue, leaving
	 * NAPI idle with the IRQ masked (TX works, RX stalls) until reload.
	 * Storm prevention on teardown remains in cleanup/disable paths.
	 */
	if (napi_schedule_prep(napi)) {
		/* Failure is already logged with the hcall rc by the helper. */
		ibmveth_disable_irq(adapter, qindex);
		__napi_schedule(napi);
		return true;
	}
	return false;
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

	/*
	 * The MQ hcall takes six ioba words (12 packed addresses). The
	 * legacy hcall takes eight descriptors. The argument lists below
	 * are written out by hand; keep the defines matched to those lists.
	 */
	static_assert(IBMVETH_MAX_RX_PER_HCALL == 12);
	static_assert(IBMVETH_MAX_RX_REGULAR == 8);

	if (adapter->multi_queue) {
		unsigned long buffersznum = (buff_size << 32) | filled;
		unsigned long ioba[IBMVETH_MAX_RX_PER_HCALL / 2] = {0};
		unsigned long handle = adapter->queue_handle[queue_index];
		int i;

		/* Pack descriptor addresses into ioba pairs.
		 * Each ioba holds two 32-bit addresses packed into 64 bits:
		 * - Even descriptors (0,2,4...) go in high 32 bits
		 * - Odd descriptors (1,3,5...) go in low 32 bits
		 */
		for (i = 0; i < filled && i < IBMVETH_MAX_RX_PER_HCALL; i++) {
			int pair_idx = i / 2;
			int is_high = (i % 2 == 0);

			if (is_high)
				ioba[pair_idx] = (unsigned long)
					descs[i].fields.address << 32;
			else
				ioba[pair_idx] |= descs[i].fields.address;
		}

		rc = h_add_logical_lan_buffers_queue(vdev->unit_address,
						     handle,
						     buffersznum,
						     ioba[0], ioba[1], ioba[2],
						     ioba[3], ioba[4], ioba[5]);
	} else if (filled == 1) {
		rc = h_add_logical_lan_buffer(vdev->unit_address,
					      descs[0].desc);
	} else {
		/* Legacy 8-desc hcall; probe/mq_fallback keep batch <=
		 * IBMVETH_MAX_RX_REGULAR.
		 */
		rc = h_add_logical_lan_buffers(vdev->unit_address,
					       descs[0].desc, descs[1].desc,
					       descs[2].desc, descs[3].desc,
					       descs[4].desc, descs[5].desc,
					       descs[6].desc, descs[7].desc);
	}

	return rc;
}

/* Outcomes for ibmveth_replenish_buffer_pool(); logged after unlock. */
enum {
	IBMVETH_REPLENISH_OK = 0,
	IBMVETH_REPLENISH_RESET_MAP,
	IBMVETH_REPLENISH_RESET_MQ,
	IBMVETH_REPLENISH_HCALL_FAIL,
	IBMVETH_REPLENISH_BATCH_FALLBACK,
};

struct ibmveth_replenish_fail {
	unsigned long lpar_rc;
	u32 filled;
	u32 batch;
};

/* Replenish the buffers for a pool.
 * Caller must hold the per-queue replenish_lock. Do not printk here:
 * netconsole on the same device can re-enter replenish_task.
 */
static int ibmveth_replenish_buffer_pool(struct ibmveth_adapter *adapter,
					 struct ibmveth_buff_pool *pool,
					 int queue_index,
					 struct ibmveth_replenish_fail *fail)
{
	struct ibmveth_rx_queue_stats *qstats =
		&adapter->rx_qstats[queue_index];
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
	int outcome = IBMVETH_REPLENISH_OK;

	vdev = adapter->vdev;
	dev = &vdev->dev;

	mb();

	batch = adapter->rx_buffers_per_hcall;

	while (remaining > 0) {
		unsigned int free_index = pool->consumer_index;

		/* Fill a batch of descriptors */
		for (filled = 0; filled < min(remaining, batch); filled++) {
			index = pool->free_map[free_index];
			if (index == IBM_VETH_INVALID_MAP) {
				qstats->replenish_add_buff_failure++;
				outcome = IBMVETH_REPLENISH_RESET_MAP;
				break;
			}

			if (!pool->skbuff[index]) {
				struct sk_buff *skb = NULL;

				skb = netdev_alloc_skb(adapter->netdev,
						       pool->buff_size);
				if (!skb) {
					qstats->replenish_no_mem++;
					qstats->replenish_add_buff_failure++;
					break;
				}

				/* NO_WARN: hold replenish_lock; iommu
				 * printk can re-enter via netconsole.
				 */
				dma_addr =
					dma_map_single_attrs(dev,
							     skb->data,
							     pool->buff_size,
							     DMA_FROM_DEVICE,
							     DMA_ATTR_NO_WARN);
				if (dma_mapping_error(dev, dma_addr)) {
					dev_kfree_skb_any(skb);
					qstats->replenish_add_buff_failure++;
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

		if (outcome != IBMVETH_REPLENISH_OK)
			break;

		if (!filled)
			break;

		lpar_rc = ibmveth_add_logical_lan_buffers(adapter, descs,
							  filled,
							  pool->buff_size,
							  queue_index);

		if (lpar_rc != H_SUCCESS) {
			fail->lpar_rc = lpar_rc;
			fail->filled = filled;
			fail->batch = batch;
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
		qstats->replenish_add_buff_success += filled;
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
		qstats->replenish_add_buff_failure += filled;

		if (lpar_rc == H_FUNCTION) {
			if (adapter->multi_queue) {
				/*
				 * LPM / firmware may drop MQ buffer hcalls.
				 * Schedule reset so we do not sit forever in
				 * no-buffer with the link still up.
				 */
				outcome = IBMVETH_REPLENISH_RESET_MQ;
			} else if (batch > 1) {
				/*
				 * Live Partition Migration may drop multi-
				 * buffer support. Fall back to single-buffer
				 * on the next replenish; do not continue with
				 * a stale local batch size (infinite loop).
				 */
				adapter->rx_buffers_per_hcall = 1;
				outcome = IBMVETH_REPLENISH_BATCH_FALLBACK;
			} else {
				outcome = IBMVETH_REPLENISH_HCALL_FAIL;
			}
		} else {
			outcome = IBMVETH_REPLENISH_HCALL_FAIL;
		}
		break;
	}

	mb();
	atomic_add(buffers_added, &(pool->available));
	return outcome;
}

/*
 * The final 8 bytes of the buffer list is a counter of frames dropped
 * because there was not a buffer in the buffer list capable of holding
 * the frame.
 */
static void ibmveth_update_rx_no_buffer(struct ibmveth_adapter *adapter,
					int queue_index)
{
	struct ibmveth_rx_queue_stats *qstats;
	__be64 *p;
	u64 drops;

	if (queue_index < 0 ||
	    queue_index >= ibmveth_get_num_rx_queues(adapter) ||
	    !adapter->buffer_list_addr[queue_index])
		return;

	p = adapter->buffer_list_addr[queue_index] + 4096 - 8;
	drops = be64_to_cpup(p);

	/*
	 * PHYP's buffer-list page counter is absolute for that page. A new
	 * page (reopen / queue reuse after -L) starts near zero; fold the
	 * previous absolute into this queue's retired carry so sums stay
	 * monotonic. Both fields belong to the queue being updated, so this
	 * stays single-writer under the queue's replenish_lock.
	 */
	qstats = &adapter->rx_qstats[queue_index];

	if (drops < qstats->no_buffer_drops)
		qstats->no_buffer_retired += qstats->no_buffer_drops;
	qstats->no_buffer_drops = drops;
}

/* replenish routine */
static void ibmveth_replenish_task(struct ibmveth_adapter *adapter,
				   int queue_index)
{
	struct ibmveth_rx_q *rxq = &adapter->rx_queue[queue_index];
	struct ibmveth_replenish_fail fail = {};
	unsigned long flags;
	int i, rc;
	int need_reset = 0;
	int batch_fallback = 0;
	int hcall_fail = 0;

	if (queue_index >= ibmveth_get_num_rx_queues(adapter)) {
		netdev_dbg(adapter->netdev,
			   "Skipping replenish for freed queue %d (num_queues=%u)\n",
			   queue_index, ibmveth_get_num_rx_queues(adapter));
		return;
	}

	spin_lock_irqsave(&rxq->replenish_lock, flags);

	adapter->rx_qstats[queue_index].replenish_task_cycles++;

	for (i = (IBMVETH_NUM_BUFF_POOLS - 1); i >= 0; i--) {
		struct ibmveth_buff_pool *pool =
			&adapter->rx_buff_pool[queue_index][i];

		if (pool->active && pool->free_map &&
		    (atomic_read(&pool->available) < pool->threshold)) {
			rc = ibmveth_replenish_buffer_pool(adapter, pool,
							   queue_index, &fail);
			switch (rc) {
			case IBMVETH_REPLENISH_RESET_MAP:
			case IBMVETH_REPLENISH_RESET_MQ:
				need_reset = rc;
				goto out_unlock;
			case IBMVETH_REPLENISH_BATCH_FALLBACK:
				batch_fallback = 1;
				break;
			case IBMVETH_REPLENISH_HCALL_FAIL:
				hcall_fail = 1;
				break;
			default:
				break;
			}
		}
	}

out_unlock:
	ibmveth_update_rx_no_buffer(adapter, queue_index);

	spin_unlock_irqrestore(&rxq->replenish_lock, flags);

	/* Log and schedule reset only after dropping replenish_lock. */
	if (need_reset == IBMVETH_REPLENISH_RESET_MAP) {
		netdev_info(adapter->netdev,
			    "Invalid RX free_map entry on queue %d, reset\n",
			    queue_index);
		schedule_work(&adapter->work);
	} else if (need_reset == IBMVETH_REPLENISH_RESET_MQ) {
		dev_err_ratelimited(&adapter->netdev->dev,
				    "MQ buffer add H_FUNCTION (q=%d, batch=%u), reset\n",
				    queue_index, fail.batch);
		adapter->mq_fallback = true;
		schedule_work(&adapter->work);
	}

	if (batch_fallback)
		dev_warn_ratelimited(&adapter->netdev->dev,
				     "Legacy batch add H_FUNCTION (batch=%u), fallback\n",
				     fail.batch);

	if (hcall_fail)
		dev_warn_ratelimited(&adapter->netdev->dev,
				     "RX %s failed: filled=%u, rc=%lu, batch=%u\n",
				     adapter->multi_queue ?
				     "h_add_logical_lan_buffers_queue" :
				     (fail.batch == 1 ?
				      "h_add_logical_lan_buffer" :
				      "h_add_logical_lan_buffers"),
				     fail.filled, fail.lpar_rc, fail.batch);
}

/**
 * ibmveth_restart_rx_queue - Post buffers and ensure Q can take RX
 * @adapter: ibmveth adapter
 * @qindex: RX queue index
 *
 * SQ open leaves PHYP masked until the first poll. If schedule_prep fails,
 * NAPI never runs and the queue stays masked (TX OK, RX/ARP dead) until
 * reload. Replenish first so an enable_irq fallback can actually deliver.
 * Also used after every open (SQ and MQ) and after scale-down so a
 * queue is not left idle+masked.
 */
static void ibmveth_restart_rx_queue(struct ibmveth_adapter *adapter,
				     int qindex)
{
	ibmveth_replenish_task(adapter, qindex);
	if (ibmveth_schedule_rx_queue(adapter, qindex))
		return;

	ibmveth_enable_irq(adapter, qindex);
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

	/*
	 * Keep probe/sysfs geometry (active, size, buff_size, threshold).
	 * Only tear down runtime allocations; open reuses active pools.
	 */
	atomic_set(&pool->available, 0);
}

/**
 * ibmveth_free_queue_buffer_pools - Free buffer pools for a single queue
 * @adapter: ibmveth adapter structure
 * @queue: queue index
 *
 * Frees buffer pools that still hold allocations for the specified
 * queue (by free_map / dma_addr / skbuff presence), regardless of the
 * active flag.
 */
static void ibmveth_free_queue_buffer_pools(struct ibmveth_adapter *adapter,
					    int queue)
{
	int i;

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
		struct ibmveth_buff_pool *pool =
			&adapter->rx_buff_pool[queue][i];

		/* Free pool if it has allocated memory, regardless of
		 * active flag. Allocation and active can diverge on failure
		 * paths, so check for actual allocations.
		 */
		if (pool->free_map || pool->dma_addr || pool->skbuff)
			ibmveth_free_buffer_pool(adapter, pool);
	}
}

/**
 * ibmveth_alloc_queue_buffer_pools - Allocate buffer pools for a single queue
 * @adapter: ibmveth adapter structure
 * @queue: queue index
 *
 * Allocates backing storage for each active pool on @queue.
 * Inactive pools (!active) are skipped. Pool metadata must be
 * initialized before calling this function.
 *
 * Return: 0 on success, negative error code on failure
 */
static int ibmveth_alloc_queue_buffer_pools(struct ibmveth_adapter *adapter,
					    int queue)
{
	struct net_device *netdev = adapter->netdev;
	int i;

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
		struct ibmveth_buff_pool *bpool =
			&adapter->rx_buff_pool[queue][i];

		if (!bpool->active)
			continue;

		if (ibmveth_alloc_buffer_pool(bpool)) {
			netdev_err(netdev,
				   "queue %d pool %d alloc failed (size=%u count=%u)\n",
				   queue, i,
				   bpool->buff_size,
				   bpool->size);
			bpool->active = 0;
			/* Free by allocation presence, not pool->active.
			 * Needed on later resize paths (a pool can still
			 * hold memory after active is cleared). This
			 * open-fail slot is already empty:
			 * alloc_buffer_pool() undoes its own partials.
			 */
			ibmveth_free_queue_buffer_pools(adapter, queue);
			return -ENOMEM;
		}
	}

	return 0;
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
static int
ibmveth_alloc_buffer_pools(struct ibmveth_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int i, q, rc;

	/* Initialize pool metadata for queues 1..N from queue 0 settings */
	for (q = 1; q < ibmveth_get_num_rx_queues(adapter); q++) {
		for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
			struct ibmveth_buff_pool *src =
				&adapter->rx_buff_pool[0][i];
			struct ibmveth_buff_pool *dst =
				&adapter->rx_buff_pool[q][i];

			dst->size = src->size;
			dst->index = src->index;
			dst->buff_size = src->buff_size;
			dst->threshold = src->threshold;
			dst->active = src->active;
		}
	}

	/* Allocate actual buffers for all queues */
	for (q = 0; q < ibmveth_get_num_rx_queues(adapter); q++) {
		rc = ibmveth_alloc_queue_buffer_pools(adapter, q);
		if (rc) {
			/* Free pools for all previous queues */
			while (--q >= 0)
				ibmveth_free_queue_buffer_pools(adapter, q);
			return rc;
		}
	}

	netdev_dbg(netdev, "allocated buffer pools for %u queue(s)\n",
		   ibmveth_get_num_rx_queues(adapter));
	return 0;
}

/**
 * ibmveth_free_buffer_pools - Free buffer pools for all queues
 * @adapter: ibmveth adapter structure
 *
 * Frees buffer pools for all queues using the helper function.
 */
static void
ibmveth_free_buffer_pools(struct ibmveth_adapter *adapter)
{
	int q;

	/* Free buffer pools for all queues */
	for (q = 0; q < ibmveth_get_num_rx_queues(adapter); q++)
		ibmveth_free_queue_buffer_pools(adapter, q);

	netdev_dbg(adapter->netdev, "freed buffer pools for %u queue(s)\n",
		   ibmveth_get_num_rx_queues(adapter));
}

static bool ibmveth_rxq_correlator_valid(struct ibmveth_adapter *adapter,
					 int queue_index, u64 correlator)
{
	unsigned int pool = correlator >> 32;
	unsigned int index = correlator & 0xffffffffUL;
	struct ibmveth_buff_pool *bpool;

	if (pool >= IBMVETH_NUM_BUFF_POOLS)
		return false;

	bpool = &adapter->rx_buff_pool[queue_index][pool];

	/* Require a live pool with allocated arrays before indexing.
	 * Inactive pools still have size from init; free clears skbuff.
	 */
	if (!bpool->active || !bpool->skbuff || !bpool->free_map)
		return false;

	return index < bpool->size;
}

static void ibmveth_rxq_advance(struct ibmveth_rx_q *rxq)
{
	if (++rxq->index == rxq->num_slots) {
		rxq->index = 0;
		rxq->toggle = !rxq->toggle;
	}
}

/**
 * ibmveth_remove_buffer_from_pool - remove a buffer from a pool
 * @adapter: adapter instance
 * @correlator: identifies pool and index
 * @queue_index: RX queue index (0..num_rx_queues-1)
 * @reuse: whether to reuse buffer
 *
 * Context: may run concurrently with netpoll replenish_task on the same
 * queue; takes per-queue replenish_lock to serialize free_map /
 * producer_index / available against the producer.
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
	struct ibmveth_rx_q *rxq = &adapter->rx_queue[queue_index];
	unsigned int pool  = correlator >> 32;
	unsigned int index = correlator & 0xffffffffUL;
	unsigned int free_index;
	struct sk_buff *skb;
	unsigned long flags;
	int rc = 0;

	spin_lock_irqsave(&rxq->replenish_lock, flags);

	if (!ibmveth_rxq_correlator_valid(adapter, queue_index, correlator)) {
		rc = -EINVAL;
		goto out_unlock;
	}

	skb = adapter->rx_buff_pool[queue_index][pool].skbuff[index];
	if (!skb) {
		rc = -EFAULT;
		goto out_unlock;
	}

	/* if we are going to reuse the buffer then keep the pointers around
	 * but mark index as available. replenish will see the skb pointer and
	 * assume it is to be recycled.
	 */
	if (!reuse) {
		/* remove the skb pointer to mark free. actual freeing is done
		 * by upper level networking after gro_receive
		 */
		struct ibmveth_buff_pool *bpool =
			&adapter->rx_buff_pool[queue_index][pool];

		bpool->skbuff[index] = NULL;

		dma_unmap_single(&adapter->vdev->dev,
				 bpool->dma_addr[index],
				 bpool->buff_size,
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

out_unlock:
	spin_unlock_irqrestore(&rxq->replenish_lock, flags);
	return rc;
}

/* get the current buffer on the rx queue */
static struct sk_buff *
ibmveth_rxq_get_buffer(struct ibmveth_adapter *adapter,
		       int queue_index)
{
	struct ibmveth_rx_q *rxq = &adapter->rx_queue[queue_index];
	u64 correlator = rxq->queue_addr[rxq->index].correlator;
	unsigned int pool = correlator >> 32;
	unsigned int index = correlator & 0xffffffffUL;

	if (!ibmveth_rxq_correlator_valid(adapter, queue_index, correlator))
		return NULL;

	return adapter->rx_buff_pool[queue_index][pool].skbuff[index];
}

/**
 * ibmveth_rxq_harvest_buffer - Harvest buffer from pool
 *
 * @adapter: pointer to adapter
 * @queue_index: RX queue index to harvest from
 * @reuse:   whether to reuse buffer
 *
 * Context: called from ibmveth_poll
 *
 * On a bad correlator (-EINVAL/-EFAULT) the ring is still advanced so poll
 * cannot spin forever on one slot. The error is still returned: callers must
 * not treat it as a successful take from the pool (especially reuse=false,
 * which would hand the SKB to the stack while it remains pool-owned).
 *
 * Return:
 * * %0    - buffer removed from pool (or marked for reuse) and ring advanced
 * * other - non-zero return from ibmveth_remove_buffer_from_pool; ring has
 *           still been advanced for -EINVAL/-EFAULT
 */
static int ibmveth_rxq_harvest_buffer(struct ibmveth_adapter *adapter,
				      int queue_index, bool reuse)
{
	struct ibmveth_rx_q *rxq = &adapter->rx_queue[queue_index];
	u64 cor;
	int rc;

	cor = rxq->queue_addr[rxq->index].correlator;
	rc = ibmveth_remove_buffer_from_pool(adapter, cor, queue_index, reuse);
	if (unlikely(rc)) {
		/* Skip a corrupt slot without claiming pool ownership. */
		if (rc == -EINVAL || rc == -EFAULT)
			ibmveth_rxq_advance(rxq);
		return rc;
	}

	ibmveth_rxq_advance(rxq);

	return 0;
}

static void ibmveth_free_tx_ltb(struct ibmveth_adapter *adapter, int idx)
{
	void *ltb = adapter->tx_ltb_ptr[idx];
	dma_addr_t dma = adapter->tx_ltb_dma[idx];

	if (!ltb)
		return;

	/*
	 * Clear the slot before releasing it. start_xmit() tests
	 * tx_ltb_ptr[idx] to decide whether the LTB is usable.
	 */
	adapter->tx_ltb_ptr[idx] = NULL;
	adapter->tx_ltb_dma[idx] = 0;

	dma_unmap_single(&adapter->vdev->dev, dma, adapter->tx_ltb_size,
			 DMA_TO_DEVICE);
	kfree(ltb);
}

/* Caller must ensure tx_ltb_ptr[idx] is NULL. open() runs on
 * probe-zeroed slots; set_channels() skips populated indices.
 */
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
		adapter->tx_ltb_dma[idx] = 0;
		return -ENOMEM;
	}

	return 0;
}

/**
 * ibmveth_alloc_tx_resources - Allocate TX LTBs for real_num_tx_queues
 * @adapter: ibmveth adapter structure
 *
 * Allocates TX Long Term Buffers (LTBs) for real_num_tx_queues.
 *
 * Return: 0 on success, -ENOMEM on failure
 */
static int ibmveth_alloc_tx_resources(struct ibmveth_adapter *adapter)
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
 * ibmveth_free_tx_resources - Free TX LTBs for real_num_tx_queues
 * @adapter: ibmveth adapter structure
 *
 * Frees TX Long Term Buffers (LTBs) for real_num_tx_queues.
 */
static void ibmveth_free_tx_resources(struct ibmveth_adapter *adapter)
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
	unsigned long ua = adapter->vdev->unit_address;
	unsigned long buf_dma = adapter->buffer_list_dma[0];
	unsigned long filter_dma = adapter->filter_list_dma;
	unsigned long qh0;

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
		rc = h_register_logical_lan_with_handle(ua, buf_dma,
							rxq_desc.desc,
							filter_dma,
							mac_address,
							&qh0);
		if (rc == H_SUCCESS)
			adapter->queue_handle[0] = qh0;
	} else {
		rc = h_register_logical_lan(ua, buf_dma, rxq_desc.desc,
					    filter_dma, mac_address);
	}
	netdev_dbg(adapter->netdev, "h_register_logical_lan%s rc=%d\n",
		   adapter->multi_queue ? "_with_handle" : "", rc);

	if (rc != H_SUCCESS && try_again) {
		do {
			rc = h_free_logical_lan(adapter->vdev->unit_address);
		} while (H_IS_LONG_BUSY(rc) || (rc == H_BUSY));
		netdev_dbg(adapter->netdev, "h_free_logical_lan rc=%d\n", rc);

		try_again = 0;
		goto retry;
	}

	return rc;
}

/**
 * ibmveth_register_logical_lan_queue - Register subordinate queue with
 * hypervisor
 * @adapter: ibmveth adapter structure
 * @rxq_desc: Receive queue descriptor
 * @queue_index: RX queue index (1..N for subordinate queues)
 *
 * Registers a subordinate receive queue using H_REG_LOGICAL_LAN_QUEUE.
 * On success, stores the queue handle and virtual IRQ in the adapter.
 * If IRQ mapping fails after a successful hypervisor registration, the
 * queue is freed before returning.
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
	unsigned long ua = adapter->vdev->unit_address;
	unsigned long bl = adapter->buffer_list_dma[queue_index];

	netdev_dbg(adapter->netdev,
		   "register queue %d: ua=0x%lx bl=0x%lx rxq=0x%llx\n",
		   queue_index, ua, bl, rxq_desc.desc);
	do {
		lpar_rc = h_register_logical_lan_queue(ua, bl,
						       rxq_desc.desc, &handle,
						       &hwirq);
	} while (H_IS_LONG_BUSY(lpar_rc) || (lpar_rc == H_BUSY));
	netdev_dbg(adapter->netdev,
		   "h_register_logical_lan_queue queue %d rc=%ld\n",
		   queue_index, lpar_rc);

	if (lpar_rc == H_SUCCESS) {
		virq = irq_create_mapping(NULL, hwirq);
		if (!virq) {
			unsigned long free_rc;

			netdev_err(adapter->netdev,
				   "Failed to map IRQ for queue %d (hwirq=%lu)\n",
				   queue_index, hwirq);
			do {
				free_rc = h_free_logical_lan_queue(ua, handle);
			} while (H_IS_LONG_BUSY(free_rc) ||
				  (free_rc == H_BUSY));
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

	/*
	 * H_FUNCTION means firmware rejected this subordinate register
	 * (MQ unsupported / dropped after LPM). Caller fails this open and
	 * latches mq_fallback so the next open applies SQ; keep a specific
	 * log then the generic failure lines below.
	 */
	if (lpar_rc == H_FUNCTION)
		netdev_err(adapter->netdev,
			   "h_register_logical_lan_queue H_FUNCTION for queue %d (firmware MQ unsupported)\n",
			   queue_index);

	netdev_err(adapter->netdev,
		   "h_register_logical_lan_queue failed for queue %d with %ld\n",
		   queue_index, lpar_rc);
	netdev_err(adapter->netdev,
		   "queue %d params: unit_addr=0x%x buffer_list_dma=0x%llx rxq_desc=0x%llx\n",
		   queue_index, adapter->vdev->unit_address,
		   adapter->buffer_list_dma[queue_index],
		   rxq_desc.desc);

	return lpar_rc;
}

/**
 * ibmveth_register_single_rx_queue - Register one subordinate RX queue
 * @adapter: ibmveth adapter structure
 * @queue_idx: Queue index to register (1..N)
 *
 * Builds the queue descriptor and registers with the hypervisor via
 * ibmveth_register_logical_lan_queue().
 *
 * Return: 0 on success, -EINVAL if @queue_idx is invalid, -EOPNOTSUPP if
 *         firmware rejects MQ (H_FUNCTION), -EIO on other failures
 */
static int
ibmveth_register_single_rx_queue(struct ibmveth_adapter *adapter,
				 int queue_idx)
{
	struct net_device *netdev = adapter->netdev;
	union ibmveth_buf_desc rxq_desc;
	long lpar_rc;

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
		if (lpar_rc == H_FUNCTION)
			return -EOPNOTSUPP;
		return -EIO;
	}

	netdev_dbg(netdev, "Registered queue %d with handle 0x%llx\n",
		   queue_idx, adapter->queue_handle[queue_idx]);

	return 0;
}

/**
 * ibmveth_free_all_queues - Free all RX queues at once
 * @adapter: ibmveth adapter structure
 *
 * Issues one H_FREE_LOGICAL_LAN for full adapter teardown. Per PAPR/PHYP,
 * that drops the primary LAN and any subordinate queues registered under
 * it. Incremental scale-down uses H_FREE_LOGICAL_LAN_QUEUE per queue
 * instead; do not use this helper for partial live-set shrink.
 *
 * Used during interface close and registration error cleanup.
 *
 * Retries only H_BUSY and H_IS_LONG_BUSY. On other failures, logs and
 * returns; callers cannot observe hypercall status. queue_handle[] is
 * cleared regardless. Callers still run RX pool and DMA teardown
 * afterward (same as pre-helper close()).
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
	} while (H_IS_LONG_BUSY(lpar_rc) || (lpar_rc == H_BUSY));

	if (lpar_rc != H_SUCCESS) {
		netdev_err(adapter->netdev,
			   "h_free_logical_lan failed: %ld\n", lpar_rc);
	}

	for (i = 0; i < ibmveth_get_num_rx_queues(adapter); i++)
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
 * Return: 0 on success, -ENONET if queue 0 registration fails,
 *         -EOPNOTSUPP if firmware rejects a subordinate queue (H_FUNCTION;
 *         this function latches mq_fallback), -EIO on other subordinate
 *         failures
 */
static int
ibmveth_register_rx_queues(struct ibmveth_adapter *adapter, u64 mac_address)
{
	struct net_device *netdev = adapter->netdev;
	union ibmveth_buf_desc rxq_desc;
	unsigned long lpar_rc;
	unsigned int num;
	int i, rc;

	rxq_desc.fields.flags_len = IBMVETH_BUF_VALID |
				    adapter->rx_queue[0].queue_len;
	rxq_desc.fields.address = adapter->rx_queue[0].queue_dma;
	adapter->queue_irq[0] = netdev->irq;

	rc = ibmveth_disable_irq(adapter, 0);
	if (rc)
		netdev_dbg(netdev,
			   "Failed to disable IRQ for queue 0 before registration, rc=%d\n",
			   rc);

	lpar_rc = ibmveth_register_logical_lan(adapter, rxq_desc, mac_address);
	if (lpar_rc != H_SUCCESS) {
		netdev_err(netdev,
			   "h_register_logical_lan failed: %ld\n", lpar_rc);
		netdev_err(netdev,
			   "buffer TCE:0x%llx filter TCE:0x%llx rxq desc:0x%llx MAC:0x%llx\n",
			   adapter->buffer_list_dma[0],
			   adapter->filter_list_dma,
			   rxq_desc.desc, mac_address);
		return -ENONET;
	}

	num = ibmveth_get_num_rx_queues(adapter);
	if (num == 1 || !adapter->multi_queue) {
		netdev_dbg(netdev,
			   "registered 1 RX queue with hypervisor (single-queue mode)\n");
		return 0;
	}

	netdev_dbg(netdev, "Registering %u subordinate queues (1-%u)\n",
		   num - 1, num - 1);

	for (i = 1; i < num; i++) {
		rc = ibmveth_register_single_rx_queue(adapter, i);
		if (rc) {
			/* Firmware MQ gone: fall back to SQ on next open. */
			if (rc == -EOPNOTSUPP)
				adapter->mq_fallback = true;
			goto err_unregister;
		}
	}

	netdev_dbg(netdev,
		   "registered %u RX queues with hypervisor (multi-queue mode)\n",
		   num);

	return 0;

err_unregister:
	ibmveth_dispose_subordinate_irq_mappings(adapter);
	ibmveth_free_all_queues(adapter);
	return rc;
}

/**
 * ibmveth_apply_mq_fallback - Drop multi-queue mode after firmware rejection
 * @adapter: ibmveth adapter
 *
 * mq_fallback is set when firmware rejects MQ (subordinate register or
 * buffer-add H_FUNCTION). Apply only at the start of open after teardown so
 * num_rx_queues is not shrunk while IRQ/NAPI still reference higher queues.
 * Consumes the flag and clears multi_queue, which is what makes the
 * single-queue decision permanent for this device.
 */
static void ibmveth_apply_mq_fallback(struct ibmveth_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;

	if (!adapter->mq_fallback)
		return;

	adapter->mq_fallback = false;

	if (!adapter->multi_queue && ibmveth_get_num_rx_queues(adapter) == 1)
		return;

	netdev_warn(netdev,
		    "Falling back to single RX queue (firmware MQ unavailable)\n");
	adapter->multi_queue = false;
	ibmveth_publish_num_rx_queues(adapter, 1);
	/* real_num_rx_queues is set later in open after resources exist. */
	if (adapter->rx_buffers_per_hcall > IBMVETH_MAX_RX_REGULAR)
		adapter->rx_buffers_per_hcall = IBMVETH_MAX_RX_REGULAR;
}

static int ibmveth_open(struct net_device *netdev)
{
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	u64 mac_address = ether_addr_to_u64(netdev->dev_addr);
	int rxq_entries = 1;
	int rc;
	int i;

	netdev_dbg(netdev, "open starting\n");

	ibmveth_apply_mq_fallback(adapter);

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++)
		rxq_entries += adapter->rx_buff_pool[0][i].size;

	rc = ibmveth_alloc_filter_list(adapter);
	if (rc)
		goto out;

	rc = ibmveth_alloc_rx_queues(adapter, rxq_entries);
	if (rc)
		goto out_free_filter_list;

	rc = ibmveth_alloc_buffer_pools(adapter);
	if (rc)
		goto out_free_queue_mem;

	rc = ibmveth_register_rx_queues(adapter, mac_address);
	if (rc)
		goto out_free_buffer_pools;

	rc = netif_set_real_num_rx_queues(netdev,
					  ibmveth_get_num_rx_queues(adapter));

	if (rc) {
		netdev_err(netdev, "failed to set number of rx queues\n");
		goto out_unregister_queues;
	}

	/*
	 * Post buffers before setup_rx_interrupts(). MQ setup then unmasks
	 * PHYP; SQ setup leaves PHYP masked. Scheduling NAPI only when a
	 * descriptor is already pending is not enough: after ifdown/up
	 * (RX=8, no -L) NAPI can be idle with nothing pending and the
	 * queue stays dead (TX OK, ARP/RX fail).
	 * restart_rx_queue() replenishes, schedules NAPI, and unmasks if
	 * prep fails.
	 */
	for (i = 0; i < ibmveth_get_num_rx_queues(adapter); i++) {
		netdev_dbg(netdev, "initial replenish cycle for queue %d\n", i);
		ibmveth_replenish_task(adapter, i);
	}

	rc = ibmveth_setup_rx_interrupts(adapter);
	if (rc)
		goto out_free_all_queues; /* setup already disposed IRQs */

	for (i = 0; i < ibmveth_get_num_rx_queues(adapter); i++)
		ibmveth_restart_rx_queue(adapter, i);

	rc = ibmveth_alloc_tx_resources(adapter);
	if (rc)
		goto out_cleanup_rx_interrupts;

	netif_tx_start_all_queues(netdev);

	adapter->opened = true;
	netdev_dbg(netdev, "open complete\n");

	return 0;

out_cleanup_rx_interrupts:
	ibmveth_cleanup_rx_interrupts(adapter);
	goto out_free_all_queues; /* cleanup already disposed IRQs */
out_unregister_queues:
	ibmveth_dispose_subordinate_irq_mappings(adapter);
out_free_all_queues:
	ibmveth_free_all_queues(adapter);
out_free_buffer_pools:
	ibmveth_free_buffer_pools(adapter);
out_free_queue_mem:
	ibmveth_cleanup_rx_resources(adapter);
out_free_filter_list:
	ibmveth_free_filter_list(adapter);
out:
	return rc;
}

static int ibmveth_close(struct net_device *netdev)
{
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	int i;

	/* Gate on opened, not IFF_UP: pool_store/change_mtu close+open can
	 * leave IFF_UP set after a failed reopen.
	 */
	if (!adapter->opened)
		return 0;

	adapter->opened = false;

	netdev_dbg(netdev, "close starting\n");

	/* Disable and wait for in-flight ndo_start_xmit (stop_all_queues
	 * alone does not). Direct close() callers bypass synchronize_net().
	 */
	netif_tx_disable(netdev);

	ibmveth_cleanup_rx_interrupts(adapter);
	/* Wait for softirq/poll that already passed shutdown checks. */
	synchronize_net();

	for (i = 0; i < ibmveth_get_num_rx_queues(adapter); i++)
		ibmveth_update_rx_no_buffer(adapter, i);
	ibmveth_free_all_queues(adapter);
	/* Free TX LTBs after quiesce and after H_FREE_LOGICAL_LAN so xmit
	 * cannot touch unmapped bounce buffers while the LAN is live.
	 */
	ibmveth_free_tx_resources(adapter);
	ibmveth_free_buffer_pools(adapter);
	ibmveth_cleanup_rx_resources(adapter);
	ibmveth_free_filter_list(adapter);

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
	if (netdev->reg_state != NETREG_REGISTERED) {
		rtnl_unlock();
		return;
	}

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

/*
 * Sum per-queue counters for rare ethtool reads. The hot paths only ever
 * touch their own queue's slot, so nothing here needs an atomic; the cost
 * of aggregation is paid by the reader instead (ibmvnic-style).
 *
 * Every slot is summed, not just the live ones, so that shrinking the
 * queue count with ethtool -L cannot make a counter go backwards.
 */
static u64 ibmveth_sum_rx_qstat(struct ibmveth_adapter *adapter, size_t off)
{
	u64 total = 0;
	int i;

	for (i = 0; i < IBMVETH_MAX_RX_QUEUES; i++)
		total += *(u64 *)((u8 *)&adapter->rx_qstats[i] + off);

	return total;
}

static u64 ibmveth_sum_tx_qstat(struct ibmveth_adapter *adapter, size_t off)
{
	u64 total = 0;
	int i;

	for (i = 0; i < IBMVETH_MAX_QUEUES; i++)
		total += *(u64 *)((u8 *)&adapter->tx_qstats[i] + off);

	return total;
}

static u64 ibmveth_ethtool_adapter_stat(struct ibmveth_adapter *adapter,
					int index)
{
	const struct ibmveth_stat *stat = &ibmveth_stats[index];

	switch (stat->src) {
	case IBMVETH_STAT_RX_QSUM:
		return ibmveth_sum_rx_qstat(adapter, stat->off);
	case IBMVETH_STAT_TX_QSUM:
		return ibmveth_sum_tx_qstat(adapter, stat->off);
	case IBMVETH_STAT_RX_NO_BUFFER:
		/*
		 * PHYP's page counter is absolute for the page currently
		 * mapped, so a reopen or queue reuse restarts it near zero.
		 * ibmveth_update_rx_no_buffer() folds each decrease into the
		 * queue's retired carry; add both back to stay monotonic.
		 */
		return ibmveth_sum_rx_qstat(adapter, stat->off) +
		       ibmveth_sum_rx_qstat(adapter,
					    IBMVETH_RXQ_OFF(no_buffer_retired));
	case IBMVETH_STAT_ADAPTER:
		break;
	}

	return IBMVETH_GET_STAT(adapter, stat->off);
}

static void ibmveth_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	u8 *p = data;
	int i, j;

	if (stringset != ETH_SS_STATS)
		return;

	for (i = 0; i < ARRAY_SIZE(ibmveth_stats); i++) {
		memcpy(p, ibmveth_stats[i].name, ETH_GSTRING_LEN);
		p += ETH_GSTRING_LEN;
	}

	for (i = 0; i < ibmveth_get_num_rx_queues(adapter); i++)
		for (j = 0; j < IBMVETH_NUM_RX_QSTATS; j++)
			ethtool_sprintf(&p, ibmveth_rx_qstat_keys[j].fmt, i);

	for (i = 0; i < dev->real_num_tx_queues; i++)
		for (j = 0; j < IBMVETH_NUM_TX_QSTATS; j++)
			ethtool_sprintf(&p, ibmveth_tx_qstat_keys[j].fmt, i);
}

static int ibmveth_get_sset_count(struct net_device *dev, int sset)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);

	switch (sset) {
	case ETH_SS_STATS:
		return ARRAY_SIZE(ibmveth_stats) +
		       ibmveth_get_num_rx_queues(adapter) *
		       IBMVETH_NUM_RX_QSTATS +
		       dev->real_num_tx_queues * IBMVETH_NUM_TX_QSTATS;
	default:
		return -EOPNOTSUPP;
	}
}

static void ibmveth_get_ethtool_stats(struct net_device *dev,
				      struct ethtool_stats *stats, u64 *data)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	int i, j, k;

	for (i = 0; i < ARRAY_SIZE(ibmveth_stats); i++)
		data[i] = ibmveth_ethtool_adapter_stat(adapter, i);

	for (j = 0; j < ibmveth_get_num_rx_queues(adapter); j++) {
		const u8 *q = (const u8 *)&adapter->rx_qstats[j];

		for (k = 0; k < IBMVETH_NUM_RX_QSTATS; k++)
			data[i++] = *(const u64 *)
				(q + ibmveth_rx_qstat_keys[k].off);
	}

	for (j = 0; j < dev->real_num_tx_queues; j++) {
		const u8 *q = (const u8 *)&adapter->tx_qstats[j];

		for (k = 0; k < IBMVETH_NUM_TX_QSTATS; k++)
			data[i++] = *(const u64 *)
				(q + ibmveth_tx_qstat_keys[k].off);
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
	channels->rx_count = ibmveth_get_num_rx_queues(adapter);
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

	/* Close / failed reopen can free LTBs while IFF_UP is still set. */
	if (unlikely(!adapter->tx_ltb_ptr[queue_num])) {
		adapter->tx_qstats[queue_num].dropped_packets++;
		goto out;
	}

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

static void ibmveth_poll_bump_invalid(struct ibmveth_adapter *adapter,
				      int queue_index)
{
	adapter->rx_qstats[queue_index].invalid_buffers++;
}

static bool ibmveth_poll_stopping(struct net_device *netdev,
				  struct napi_struct *napi)
{
	return !netif_running(netdev) || napi_disable_pending(napi);
}

static bool ibmveth_poll_harvest_slot(struct ibmveth_adapter *adapter,
				      int queue_index, bool reuse)
{
	int rc = ibmveth_rxq_harvest_buffer(adapter, queue_index, reuse);

	return !rc || rc == -EINVAL || rc == -EFAULT;
}

static bool ibmveth_poll_recycle_invalid(struct net_device *netdev,
					 struct ibmveth_adapter *adapter,
					 int queue_index)
{
	netdev_dbg(netdev, "recycling invalid buffer\n");
	ibmveth_poll_bump_invalid(adapter, queue_index);
	return ibmveth_poll_harvest_slot(adapter, queue_index, true);
}

static bool ibmveth_poll_skip_bad_correlator(struct net_device *netdev,
					     struct ibmveth_adapter *adapter,
					     int queue_index)
{
	if (net_ratelimit())
		netdev_err(netdev,
			   "bad correlator on queue %d, skipping slot\n",
			   queue_index);
	/* Residual stale slot after resize: recover via reset rather
	 * than spinning forever. Always escalate; only the log is
	 * rate-limited.
	 */
	schedule_work(&adapter->work);
	ibmveth_poll_bump_invalid(adapter, queue_index);
	return ibmveth_poll_harvest_slot(adapter, queue_index, true);
}

static bool ibmveth_poll_drop_oversize(struct net_device *netdev,
				       struct ibmveth_adapter *adapter,
				     int queue_index, unsigned int off,
				     unsigned int len, unsigned int room)
{
	if (net_ratelimit())
		netdev_err(netdev,
			   "RX frame %u+%u exceeds buffer %u on queue %d, dropping\n",
			   off, len, room, queue_index);
	ibmveth_poll_bump_invalid(adapter, queue_index);
	return ibmveth_poll_harvest_slot(adapter, queue_index, true);
}

/**
 * ibmveth_poll_deliver_frame - Build SKB from one valid RX slot and GRO it
 * @napi: NAPI context for this RX queue
 * @adapter: ibmveth adapter
 * @netdev: net_device for @adapter
 * @queue_index: RX queue index
 *
 * Return: 1 frame delivered, 0 if the slot was skipped cleanly, -1 on error.
 */
static int ibmveth_poll_deliver_frame(struct napi_struct *napi,
				      struct ibmveth_adapter *adapter,
				      struct net_device *netdev,
				      int queue_index)
{
	struct sk_buff *skb, *new_skb;
	unsigned int room, off, len;
	int length, offset, csum_good, lrg_pkt;
	__sum16 iph_check = 0;
	u16 mss = 0;
	int rc;

	length = ibmveth_rxq_frame_length(adapter, queue_index);
	offset = ibmveth_rxq_frame_offset(adapter, queue_index);
	csum_good = ibmveth_rxq_csum_good(adapter, queue_index);
	lrg_pkt = ibmveth_rxq_large_packet(adapter, queue_index);

	skb = ibmveth_rxq_get_buffer(adapter, queue_index);
	if (unlikely(!skb)) {
		if (!ibmveth_poll_skip_bad_correlator(netdev, adapter,
						      queue_index))
			return -1;
		return 0;
	}

	room = skb_tailroom(skb);
	off = offset;
	len = length;
	if (unlikely(off >= room || len > room - off)) {
		if (!ibmveth_poll_drop_oversize(netdev, adapter, queue_index,
						off, len, room))
			return -1;
		return 0;
	}

	if (lrg_pkt) {
		__be64 *rxmss = (__be64 *)(skb->data + 8);

		mss = (u16)be64_to_cpu(*rxmss);
	}

	new_skb = NULL;
	if (length < rx_copybreak)
		new_skb = netdev_alloc_skb(netdev, length);

	if (new_skb) {
		skb_copy_to_linear_data(new_skb, skb->data + offset, length);
		if (rx_flush)
			ibmveth_flush_buffer(skb->data, length + offset);
		rc = ibmveth_rxq_harvest_buffer(adapter, queue_index, true);
		if (unlikely(rc)) {
			kfree_skb(new_skb);
			return -1;
		}
		skb = new_skb;
	} else {
		rc = ibmveth_rxq_harvest_buffer(adapter, queue_index, false);
		if (unlikely(rc))
			return -1;
		skb_reserve(skb, offset);
	}

	skb_put(skb, length);
	skb->protocol = eth_type_trans(skb, netdev);

	if (skb->protocol == cpu_to_be16(ETH_P_IP))
		iph_check = ((struct iphdr *)skb->data)->check;

	if ((length > netdev->mtu + ETH_HLEN) || lrg_pkt ||
	    iph_check == 0xffff) {
		ibmveth_rx_mss_helper(skb, mss, lrg_pkt);
		adapter->rx_qstats[queue_index].large_packets++;
	}

	if (csum_good) {
		skb->ip_summed = CHECKSUM_UNNECESSARY;
		ibmveth_rx_csum_helper(skb, adapter);
	}

	skb_record_rx_queue(skb, queue_index);
	napi_gro_receive(napi, skb);

	adapter->rx_qstats[queue_index].packets++;
	adapter->rx_qstats[queue_index].bytes += length;

	return 1;
}

static int ibmveth_poll(struct napi_struct *napi, int budget)
{
	struct net_device *netdev = napi->dev;
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	int frames_processed = 0;
	int queue_index, rc;

	queue_index = napi - adapter->napi;

	if (WARN_ON(queue_index < 0 ||
		    queue_index >= ibmveth_get_num_rx_queues(adapter))) {
		if (budget)
			napi_complete_done(napi, 0);
		return 0;
	}

	if (ibmveth_poll_stopping(netdev, napi)) {
		if (budget)
			napi_complete_done(napi, 0);
		return 0;
	}

	adapter->rx_qstats[queue_index].polls++;

restart_poll:
	while (frames_processed < budget) {
		if (ibmveth_poll_stopping(netdev, napi))
			break;

		if (!ibmveth_rxq_pending_buffer(adapter, queue_index))
			break;

		smp_rmb();
		if (!ibmveth_rxq_buffer_valid(adapter, queue_index)) {
			wmb(); /* suggested by larson1 */
			if (!ibmveth_poll_recycle_invalid(netdev, adapter,
							  queue_index))
				break;
		} else {
			rc = ibmveth_poll_deliver_frame(napi, adapter, netdev,
							queue_index);
			if (rc < 0)
				break;
			if (rc > 0)
				frames_processed++;
		}
	}

	ibmveth_replenish_task(adapter, queue_index);

	if (ibmveth_poll_stopping(netdev, napi)) {
		/* budget 0 is netpoll, which must not complete NAPI.
		 * Otherwise returning budget after completing would ask
		 * NAPI to reschedule, so cap the return at budget - 1.
		 */
		if (budget) {
			napi_complete_done(napi, frames_processed);
			return min(frames_processed, budget - 1);
		}
		return 0;
	}

	if (frames_processed == budget)
		goto out;

	if (!napi_complete_done(napi, frames_processed))
		goto out;

	/*
	 * napi_disable() sets DISABLE then waits for this poll. Without a
	 * second stopping check here, enable_irq() can re-arm PHYP after
	 * resize already masked the queue; late IRQs then hit the handler
	 * after num_rx_queues was published lower (lab WARN at interrupt).
	 */
	if (ibmveth_poll_stopping(netdev, napi))
		goto out;

	rc = ibmveth_enable_irq(adapter, queue_index);
	if (rc) {
		netdev_err(netdev,
			   "Failed to enable IRQ for queue %d (rc=%d), scheduling reset\n",
			   queue_index, rc);
		schedule_work(&adapter->work);
		goto out;
	}

	if (ibmveth_rxq_pending_buffer(adapter, queue_index) &&
	    napi_schedule(napi)) {
		ibmveth_disable_irq(adapter, queue_index);
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
	int qindex;

	qindex = napi - adapter->napi;
	/*
	 * Quiet on out-of-range: scale-down publishes a lower live count
	 * before free_irq(). A residual IRQ must not WARN-storm; return
	 * IRQ_NONE until the handler is removed.
	 */
	if (qindex < 0 || qindex >= ibmveth_get_num_rx_queues(adapter))
		return IRQ_NONE;

	adapter->rx_qstats[qindex].interrupts++;

	ibmveth_schedule_rx_queue(adapter, qindex);
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
	unsigned int num = ibmveth_get_num_rx_queues(adapter);
	int i;

	for (i = 0; i < num; i++)
		ibmveth_replenish_task(adapter, i);

	for (i = 0; i < num; i++)
		ibmveth_schedule_rx_queue(adapter, i);
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

	/* One buffer list page per RX queue; filter list is shared. */
	ret = IBMVETH_BUFF_LIST_SIZE * ibmveth_get_num_rx_queues(adapter) +
	      IBMVETH_FILT_LIST_SIZE;
	ret += IOMMU_PAGE_ALIGN(netdev->mtu, tbl);
	/* add size of mapped tx buffers */
	ret += IOMMU_PAGE_ALIGN(IBMVETH_MAX_TX_BUF_SIZE, tbl);

	/*
	 * Pool metadata for queues 1+ is copied from queue 0 at open.
	 * Always size from pool 0 x num_rx_queues (valid at probe before that
	 * copy).
	 */
	for (q = 0; q < ibmveth_get_num_rx_queues(adapter); q++) {
		int rxqentries = 1;

		for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
			struct ibmveth_buff_pool *bpool =
				&adapter->rx_buff_pool[0][i];

			if (bpool->active)
				ret += bpool->size *
					IOMMU_PAGE_ALIGN(bpool->buff_size, tbl);
			rxqentries += bpool->size;
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
 * Walk the full allocated arrays (not the live queue count) so shrinking
 * channels cannot make the totals go backwards.
 * Callers use ndo_get_stats64(); avoid updating netdev->stats on the
 * xmit/poll paths to keep per-queue counters off the hot cache line.
 */
static void ibmveth_get_stats64(struct net_device *dev,
				struct rtnl_link_stats64 *stats)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	int i;

	for (i = 0; i < IBMVETH_MAX_RX_QUEUES; i++) {
		stats->rx_packets += adapter->rx_qstats[i].packets;
		stats->rx_bytes += adapter->rx_qstats[i].bytes;
	}

	for (i = 0; i < IBMVETH_MAX_QUEUES; i++) {
		stats->tx_packets += adapter->tx_qstats[i].packets;
		stats->tx_bytes += adapter->tx_qstats[i].bytes;
		stats->tx_dropped += adapter->tx_qstats[i].dropped_packets;
	}
}

static void ibmveth_get_queue_stats_rx(struct net_device *dev, int idx,
				       struct netdev_queue_stats_rx *stats)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);

	stats->packets = adapter->rx_qstats[idx].packets;
	stats->bytes = adapter->rx_qstats[idx].bytes;
	/*
	 * All three are frames that entered the device and never left it,
	 * which is what rx-hw-drops is specified to cover: no_buffer_drops
	 * is PHYP dropping for lack of buffer space on the page mapped now,
	 * no_buffer_retired the same for pages this queue has already
	 * released, and invalid_buffers is a processing error.
	 */
	stats->hw_drops = adapter->rx_qstats[idx].no_buffer_drops +
			  adapter->rx_qstats[idx].no_buffer_retired +
			  adapter->rx_qstats[idx].invalid_buffers;
	stats->alloc_fail = adapter->rx_qstats[idx].replenish_no_mem;
}

static void ibmveth_get_queue_stats_tx(struct net_device *dev, int idx,
				       struct netdev_queue_stats_tx *stats)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);

	stats->packets = adapter->tx_qstats[idx].packets;
	stats->bytes = adapter->tx_qstats[idx].bytes;
	stats->hw_drops = adapter->tx_qstats[idx].dropped_packets;
}

/**
 * ibmveth_get_base_stats - account for traffic not on a live queue
 * @dev: network device
 * @rx: RX base statistics storage
 * @tx: TX base statistics storage
 *
 * get_queue_stats_{rx,tx}() only report queues the core still iterates,
 * i.e. below real_num_{rx,tx}_queues, while ibmveth_get_stats64() walks
 * the full arrays so device totals stay monotonic across a shrink.
 * Report the retired-queue remainder here, otherwise qstats and
 * rtnl_link_stats64 disagree by a delta that grows with every shrink.
 * Zeroing would not be neutral: per netdev_stat_ops it asserts the
 * per-queue sum is already exact.
 *
 * Bound the live side with real_num_*_queues rather than the adapter's
 * own count, so the split lines up with the core's iteration exactly.
 *
 * Every field the per-queue callbacks fill must also be initialised
 * here: netdev_nl_stats_add() starts the sum at NETDEV_STAT_NOT_SET and
 * only accumulates while both sides are set, so a field left unset here
 * is dropped from the device total even though the queues report it.
 */
static void ibmveth_get_base_stats(struct net_device *dev,
				   struct netdev_queue_stats_rx *rx,
				   struct netdev_queue_stats_tx *tx)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	unsigned int i;

	rx->packets = 0;
	rx->bytes = 0;
	rx->alloc_fail = 0;
	rx->hw_drops = 0;
	tx->packets = 0;
	tx->bytes = 0;
	tx->hw_drops = 0;

	for (i = dev->real_num_rx_queues; i < IBMVETH_MAX_RX_QUEUES; i++) {
		rx->packets += adapter->rx_qstats[i].packets;
		rx->bytes += adapter->rx_qstats[i].bytes;
		rx->hw_drops += adapter->rx_qstats[i].no_buffer_drops +
				adapter->rx_qstats[i].no_buffer_retired +
				adapter->rx_qstats[i].invalid_buffers;
		rx->alloc_fail += adapter->rx_qstats[i].replenish_no_mem;
	}

	for (i = dev->real_num_tx_queues; i < IBMVETH_MAX_QUEUES; i++) {
		tx->packets += adapter->tx_qstats[i].packets;
		tx->bytes += adapter->tx_qstats[i].bytes;
		tx->hw_drops += adapter->tx_qstats[i].dropped_packets;
	}
}

static const struct netdev_stat_ops ibmveth_stat_ops = {
	.get_queue_stats_rx	= ibmveth_get_queue_stats_rx,
	.get_queue_stats_tx	= ibmveth_get_queue_stats_tx,
	.get_base_stats		= ibmveth_get_base_stats,
};

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

static int ibmveth_buffer_pools_show(struct seq_file *m, void *v)
{
	struct ibmveth_adapter *adapter = m->private;
	int i, j;

	/*
	 * size / buff_size / pool->active are written under RTNL
	 * (veth_pool_store, open template copy). Take the same lock so
	 * those columns are not a torn snapshot. available is updated
	 * from NAPI/softirq; only atomic_read() keeps it from tearing.
	 * Not required for memory safety; embedded arrays only.
	 */
	rtnl_lock();

	seq_puts(m, "Queue  Pool  Count  BuffSize  Active  Available\n");
	seq_puts(m, "-----  ----  -----  --------  ------  ---------\n");
	if (!adapter->opened) {
		seq_puts(m, "# down: Active/Available 0 unless allocated\n");
		seq_puts(m, "# down: geometry above queue 0 set at open\n");
	}

	for (i = 0; i < ibmveth_get_num_rx_queues(adapter); i++) {
		for (j = 0; j < IBMVETH_NUM_BUFF_POOLS; j++) {
			struct ibmveth_buff_pool *pool =
				&adapter->rx_buff_pool[i][j];
			bool live = pool->skbuff && pool->free_map;
			int active = live ? pool->active : 0;
			int available = live ? atomic_read(&pool->available)
					     : 0;

			seq_printf(m, "%5d  %4d  %5u  %8u  %6d  %9d\n",
				   i, j, pool->size, pool->buff_size,
				   active, available);
		}
	}

	rtnl_unlock();
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ibmveth_buffer_pools);

/* Driver-owned root so per-adapter dirs use a stable vio name, not the
 * mutable netdev->name (avoids stale names / eth0 collisions after rename).
 */
static struct dentry *ibmveth_dbg_root;

static void ibmveth_debugfs_init(struct ibmveth_adapter *adapter)
{
	adapter->debugfs_dir =
		debugfs_create_dir(dev_name(&adapter->vdev->dev),
				   ibmveth_dbg_root);
	debugfs_create_file("buffer_pools", 0400, adapter->debugfs_dir,
			    adapter, &ibmveth_buffer_pools_fops);
}

static void ibmveth_debugfs_exit(struct ibmveth_adapter *adapter)
{
	debugfs_remove_recursive(adapter->debugfs_dir);
	adapter->debugfs_dir = NULL;
}

static void ibmveth_put_pool_kobjs(struct ibmveth_adapter *adapter,
				   int pools_ready)
{
	int i;

	for (i = 0; i < pools_ready; i++)
		kobject_put(&adapter->rx_buff_pool[0][i].kobj);
}

static void ibmveth_probe_cleanup(struct ibmveth_adapter *adapter,
				  int pools_ready)
{
	struct net_device *netdev = adapter->netdev;

	cancel_work_sync(&adapter->work);
	ibmveth_put_pool_kobjs(adapter, pools_ready);

	ibmveth_free_tx_qstats(adapter);
	ibmveth_free_rx_qstats(adapter);
	/* Probe failure never reaches ibmveth_remove(); clear before free so
	 * CMO get_desired_dma() cannot see a freed netdev on rebind.
	 */
	dev_set_drvdata(&adapter->vdev->dev, NULL);
	free_netdev(netdev);
}

static int ibmveth_probe(struct vio_dev *dev, const struct vio_device_id *id)
{
	int rc, i, mac_len, pools_ready = 0;
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

	/*
	 * Initialise the replenish locks once. open() is re-entered on
	 * MTU and offload changes without netpoll_poll_disable(), so a
	 * lock set up there could be reinitialised while poll_controller()
	 * holds it.
	 */
	for (i = 0; i < IBMVETH_MAX_RX_QUEUES; i++)
		spin_lock_init(&adapter->rx_queue[i].replenish_lock);

	adapter->mcastFilterSize = be32_to_cpu(*mcastFilterSize_p);
	ibmveth_init_link_settings(netdev);

	for (i = 0; i < IBMVETH_MAX_RX_QUEUES; i++)
		netif_napi_add_weight(netdev, &adapter->napi[i],
				      ibmveth_poll, 16);

	if (ibmveth_alloc_rx_qstats(adapter) ||
	    ibmveth_alloc_tx_qstats(adapter)) {
		ibmveth_probe_cleanup(adapter, 0);
		return -ENOMEM;
	}

	netdev->irq = dev->irq;
	netdev->netdev_ops = &ibmveth_netdev_ops;
	netdev->ethtool_ops = &netdev_ethtool_ops;
	netdev->stat_ops = &ibmveth_stat_ops;
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
		adapter->multi_queue = true;
		ibmveth_publish_num_rx_queues(adapter,
					      min(num_online_cpus(),
						  IBMVETH_DEFAULT_QUEUES));
		netdev_dbg(netdev, "RX multi queue mode enabled: %u queues\n",
			   ibmveth_get_num_rx_queues(adapter));
	} else {
		adapter->multi_queue = false;
		ibmveth_publish_num_rx_queues(adapter,
					      IBMVETH_DEFAULT_RX_QUEUES);
	}

	if (ret == H_SUCCESS &&
	    (ret_attr & IBMVETH_ILLAN_RX_MULTI_BUFF_SUPPORT)) {
		if (adapter->multi_queue)
			adapter->rx_buffers_per_hcall =
				IBMVETH_MAX_RX_PER_HCALL;
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

		ibmveth_init_buffer_pool(&adapter->rx_buff_pool[0][i], i,
					 pool_count[i], pool_size[i],
					 pool_active[i]);
		rc = kobject_init_and_add(kobj, &ktype_veth_pool,
					  &dev->dev.kobj, "pool%d", i);
		if (rc) {
			dev_err(&dev->dev,
				"failed to create pool%d kobject: %d\n", i, rc);
			/* init_and_add takes a ref even on failure */
			kobject_put(kobj);
			ibmveth_probe_cleanup(adapter, pools_ready);
			return rc;
		}

		pools_ready++;
		kobject_uevent(kobj, KOBJ_ADD);
	}

	/*
	 * VIO CMO entitlement was set before probe (netdev NULL, so default).
	 * Recompute now that num_rx_queues and pool 0 metadata are known.
	 */
	if (firmware_has_feature(FW_FEATURE_CMO))
		vio_cmo_set_dev_desired(dev, ibmveth_get_desired_dma(dev));

	rc = netif_set_real_num_tx_queues(netdev, min(num_online_cpus(),
						      IBMVETH_DEFAULT_QUEUES));
	if (rc) {
		netdev_dbg(netdev, "failed to set number of tx queues rc=%d\n",
			   rc);
		ibmveth_probe_cleanup(adapter, pools_ready);
		return rc;
	}

	/*
	 * alloc_etherdev_mqs() sized real_num_rx_queues to MAX. Match the
	 * advertised default (or SQ 1) before register_netdev so down-state
	 * readers agree with adapter->num_rx_queues / ethtool -l.
	 */
	rc = netif_set_real_num_rx_queues(netdev,
					  ibmveth_get_num_rx_queues(adapter));

	if (rc) {
		netdev_dbg(netdev, "failed to set number of rx queues rc=%d\n",
			   rc);
		ibmveth_probe_cleanup(adapter, pools_ready);
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
		ibmveth_probe_cleanup(adapter, pools_ready);
		return rc;
	}

	netdev_dbg(netdev, "registered\n");

	ibmveth_debugfs_init(adapter);

	return 0;
}

static void ibmveth_remove(struct vio_dev *dev)
{
	struct net_device *netdev = dev_get_drvdata(&dev->dev);
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	int i;

	ibmveth_debugfs_exit(adapter);

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++)
		kobject_put(&adapter->rx_buff_pool[0][i].kobj);

	/*
	 * Unregister first so NAPI/xmit cannot re-arm reset work after we
	 * cancel it. cancel_work_sync() before unregister left a window
	 * where poll could schedule_work() and the worker ran after
	 * free_netdev().
	 */
	unregister_netdev(netdev);
	cancel_work_sync(&adapter->work);

	ibmveth_free_tx_qstats(adapter);
	ibmveth_free_rx_qstats(adapter);

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
				if (mtu <=
				    adapter->rx_buff_pool[0][i].buff_size)
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

	/* kick RX processing to allocate/deallocate pools */
	ibmveth_schedule_rx_queue(adapter, 0);
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
	unsigned int num = ibmveth_get_num_rx_queues(adapter);
	int i;

	for (i = 0; i < num; i++)
		ibmveth_schedule_rx_queue(adapter, i);

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
	int rc;

	printk(KERN_DEBUG "%s: %s %s\n", ibmveth_driver_name,
	       ibmveth_driver_string, ibmveth_driver_version);

	ibmveth_dbg_root = debugfs_create_dir(ibmveth_driver_name, NULL);

	rc = vio_register_driver(&ibmveth_driver);
	if (rc) {
		debugfs_remove_recursive(ibmveth_dbg_root);
		ibmveth_dbg_root = NULL;
	}

	return rc;
}

static void __exit ibmveth_module_exit(void)
{
	vio_unregister_driver(&ibmveth_driver);
	debugfs_remove_recursive(ibmveth_dbg_root);
	ibmveth_dbg_root = NULL;
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
 * Bad correlators return -EINVAL/-EFAULT (no WARN_ON).
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

	spin_lock_init(&adapter->rx_queue[0].replenish_lock);

	/* Set sane values for buffer pools */
	for (int i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++)
		ibmveth_init_buffer_pool(&adapter->rx_buff_pool[0][i], i,
					 pool_count[i], pool_size[i],
					 pool_active[i]);

	pool = &adapter->rx_buff_pool[0][0];
	pool->skbuff = kunit_kcalloc(test, pool->size, sizeof(void *), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pool->skbuff);
	pool->free_map = kunit_kcalloc(test, pool->size, sizeof(u16),
				       GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pool->free_map);

	correlator = ((u64)IBMVETH_NUM_BUFF_POOLS << 32) | 0;
	KUNIT_EXPECT_EQ(test, -EINVAL,
			ibmveth_remove_buffer_from_pool(adapter,
							correlator, 0, false));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			ibmveth_remove_buffer_from_pool(adapter,
							correlator, 0, true));

	correlator = ((u64)0 << 32) | adapter->rx_buff_pool[0][0].size;
	KUNIT_EXPECT_EQ(test, -EINVAL,
			ibmveth_remove_buffer_from_pool(adapter,
							correlator, 0, false));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			ibmveth_remove_buffer_from_pool(adapter,
							correlator, 0, true));

	correlator = (u64)0 | 0;
	pool->skbuff[0] = NULL;
	KUNIT_EXPECT_EQ(test, -EFAULT,
			ibmveth_remove_buffer_from_pool(adapter,
							correlator, 0, false));
	KUNIT_EXPECT_EQ(test, -EFAULT,
			ibmveth_remove_buffer_from_pool(adapter,
							correlator, 0, true));

	flush_work(&adapter->work);
}

/**
 * ibmveth_rxq_get_buffer_test - unit test for ibmveth_rxq_get_buffer
 * @test: pointer to kunit structure
 *
 * Tests ibmveth_rxq_get_buffer invalid correlator returns NULL without WARN.
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
	pool->free_map = kunit_kcalloc(test, pool->size, sizeof(u16),
				       GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pool->free_map);

	adapter->rx_queue[0].queue_addr[0].correlator =
		(u64)IBMVETH_NUM_BUFF_POOLS << 32 | 0;
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
