// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Copyright(c) 2020 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 *
 */

/*
 * This file contains HFI2 support for netdev RX functionality
 */

#include "sdma.h"
#include "verbs.h"
#include "netdev.h"
#include "hfi2.h"

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <rdma/ib_verbs.h>

static void hfi2_netdev_rxq_deinit(struct hfi2_netdev_rx *rx);

static int hfi2_netdev_setup_ctxt(struct hfi2_netdev_rx *rx,
				  struct hfi2_ctxtdata *uctxt)
{
	unsigned int rcvctrl_ops;
	struct hfi2_devdata *dd = rx->dd;
	int ret;

	uctxt->rhf_rcv_function_map = hfi2_netdev_rhf_rcv_functions;
	uctxt->do_interrupt = &hfi2_handle_receive_interrupt_napi_sp;

	/* Now allocate the RcvHdr queue and eager buffers. */
	ret = hfi2_create_rcvhdrq(dd, uctxt);
	if (ret)
		goto done;

	ret = hfi2_setup_eagerbufs(uctxt);
	if (ret)
		goto done;

	clear_rcvhdrtail(uctxt);

	rcvctrl_ops = HFI2_RCVCTRL_CTXT_DIS;
	rcvctrl_ops |= HFI2_RCVCTRL_INTRAVAIL_DIS;

	if (!HFI2_CAP_KGET_MASK(uctxt->flags, MULTI_PKT_EGR))
		rcvctrl_ops |= HFI2_RCVCTRL_ONE_PKT_EGR_ENB;
	if (HFI2_CAP_KGET_MASK(uctxt->flags, NODROP_EGR_FULL))
		rcvctrl_ops |= HFI2_RCVCTRL_NO_EGR_DROP_ENB;
	if (HFI2_CAP_KGET_MASK(uctxt->flags, NODROP_RHQ_FULL))
		rcvctrl_ops |= HFI2_RCVCTRL_NO_RHQ_DROP_ENB;
	if (HFI2_CAP_KGET_MASK(uctxt->flags, DMA_RTAIL))
		rcvctrl_ops |= HFI2_RCVCTRL_TAILUPD_ENB;

	hfi2_rcvctrl(uctxt->dd, rcvctrl_ops, uctxt);
done:
	return ret;
}

static int hfi2_netdev_allocate_ctxt(struct hfi2_pportdata *ppd,
				     struct hfi2_ctxtdata **ctxt)
{
	struct hfi2_devdata *dd = ppd->dd;
	struct hfi2_ctxtdata *uctxt;
	int ret;

	if (dd->flags & HFI2_FROZEN)
		return -EIO;

	ret = hfi2_create_ctxtdata(ppd, dd->node, DYNAMIC_CONTEXT, &uctxt);
	if (ret < 0) {
		dd_dev_err(dd, "Unable to create ctxtdata, failing open\n");
		return -ENOMEM;
	}

	uctxt->flags =
		HFI2_CAP_KGET(MULTI_PKT_EGR) | HFI2_CAP_KGET(NODROP_RHQ_FULL) |
		HFI2_CAP_KGET(NODROP_EGR_FULL) | HFI2_CAP_KGET(DMA_RTAIL);
	/* Netdev contexts are always NO_RDMA_RTAIL */
	uctxt->fast_handler = hfi2_handle_receive_interrupt_napi_fp;
	uctxt->slow_handler = hfi2_handle_receive_interrupt_napi_sp;
	hfi2_set_seq_cnt(uctxt, 1);

	hfi2_stats.sps_ctxts++;

	dd_dev_info(dd, "created netdev context %d\n", uctxt->ctxt);
	*ctxt = uctxt;

	return 0;
}

static void hfi2_netdev_deallocate_ctxt(struct hfi2_pportdata *ppd,
					struct hfi2_ctxtdata *uctxt)
{
	struct hfi2_devdata *dd = ppd->dd;

	flush_wc();

	/*
	 * Disable receive context and interrupt available, reset all
	 * RcvCtxtCtrl bits to default values.
	 */
	hfi2_rcvctrl(dd,
		     HFI2_RCVCTRL_CTXT_DIS | HFI2_RCVCTRL_TIDFLOW_DIS |
			     HFI2_RCVCTRL_INTRAVAIL_DIS |
			     HFI2_RCVCTRL_ONE_PKT_EGR_DIS |
			     HFI2_RCVCTRL_NO_RHQ_DROP_DIS |
			     HFI2_RCVCTRL_NO_EGR_DROP_DIS,
		     uctxt);

	if (uctxt->msix_intr != CCE_NUM_MSIX_VECTORS)
		hfi2_msix_free_irq(dd, uctxt->msix_intr);

	uctxt->msix_intr = CCE_NUM_MSIX_VECTORS;
	uctxt->event_flags = 0;

	hfi2_clear_tids(uctxt);
	hfi2_clear_ctxt_pkey(dd, uctxt);

	hfi2_stats.sps_ctxts--;

	hfi2_free_ctxt(uctxt);
}

static int hfi2_netdev_allot_ctxt(struct hfi2_netdev_rx *rx,
				  struct hfi2_ctxtdata **ctxt)
{
	int rc;
	struct hfi2_devdata *dd = rx->dd;
	struct hfi2_pportdata *ppd = rx->ppd;

	rc = hfi2_netdev_allocate_ctxt(ppd, ctxt);
	if (rc) {
		dd_dev_err(dd, "netdev ctxt alloc failed %d\n", rc);
		return rc;
	}

	rc = hfi2_netdev_setup_ctxt(rx, *ctxt);
	if (rc) {
		dd_dev_err(dd, "netdev ctxt setup failed %d\n", rc);
		hfi2_netdev_deallocate_ctxt(ppd, *ctxt);
		*ctxt = NULL;
	}

	return rc;
}

/**
 * hfi2_num_netdev_contexts - Count of netdev recv contexts to use.
 * @dd: device on which to allocate netdev contexts
 * @available_contexts: count of available receive contexts
 * @cpu_mask: mask of possible cpus to include for contexts
 *
 * Return: count of physical cores on a node or the remaining available recv
 * contexts for netdev recv context usage up to the maximum of
 * HFI2_MAX_NETDEV_CTXTS.
 * A value of 0 can be returned when acceleration is explicitly turned off,
 * a memory allocation error occurs or when there are no available contexts.
 *
 */
u32 hfi2_num_netdev_contexts(struct hfi2_devdata *dd, u32 available_contexts,
			     const struct cpumask *cpu_mask)
{
	cpumask_var_t node_cpu_mask;
	unsigned int available_cpus;

	if (!HFI2_CAP_IS_KSET(AIP))
		return 0;

	/* Always give user contexts priority over netdev contexts */
	if (available_contexts == 0) {
		dd_dev_info(dd, "No receive contexts available for netdevs.\n");
		return 0;
	}

	if (!zalloc_cpumask_var(&node_cpu_mask, GFP_KERNEL)) {
		dd_dev_err(dd, "Unable to allocate cpu_mask for netdevs.\n");
		return 0;
	}

	cpumask_and(node_cpu_mask, cpu_mask, cpumask_of_node(dd->node));

	available_cpus = cpumask_weight(node_cpu_mask);

	free_cpumask_var(node_cpu_mask);

	return min3(available_cpus, available_contexts,
		    (u32)HFI2_MAX_NETDEV_CTXTS);
}

static int hfi2_netdev_rxq_init(struct hfi2_netdev_rx *rx)
{
	int i;
	int rc;
	struct hfi2_devdata *dd = rx->dd;
	struct hfi2_pportdata *ppd = rx->ppd;
	struct net_device *dev = rx->rx_napi;

	rx->num_rx_q = dd->rsrcs.ppr[rx->ppd->hw_pidx].num_netdev_contexts;
	rx->rxq = kcalloc_node(rx->num_rx_q, sizeof(*rx->rxq), GFP_KERNEL,
			       dd->node);

	if (!rx->rxq) {
		ppd_dev_err(ppd, "Unable to allocate netdev queue data\n");
		return (-ENOMEM);
	}

	for (i = 0; i < rx->num_rx_q; i++) {
		struct hfi2_netdev_rxq *rxq = &rx->rxq[i];

		rc = hfi2_netdev_allot_ctxt(rx, &rxq->rcd);
		if (rc)
			goto bail_context_irq_failure;

		hfi2_rcd_get(rxq->rcd);
		rxq->rx = rx;
		rxq->rcd->napi = &rxq->napi;
		ppd_dev_info(ppd, "Setting rcv queue %d napi to context %d\n",
			     i, rxq->rcd->ctxt);
		/*
		 * Disable BUSY_POLL on this NAPI as this is not supported
		 * right now.
		 */
		set_bit(NAPI_STATE_NO_BUSY_POLL, &rxq->napi.state);
		netif_napi_add(dev, &rxq->napi, hfi2_netdev_rx_napi);
		rc = hfi2_msix_netdev_request_rcd_irq(rxq->rcd);
		if (rc)
			goto bail_context_irq_failure;
	}

	return 0;

bail_context_irq_failure:
	ppd_dev_err(ppd, "Unable to allot receive context\n");
	hfi2_netdev_rxq_deinit(rx);
	return rc;
}

static void hfi2_netdev_rxq_deinit(struct hfi2_netdev_rx *rx)
{
	int i;

	for (i = 0; i < rx->num_rx_q; i++) {
		struct hfi2_netdev_rxq *rxq = &rx->rxq[i];

		if (!rxq->rcd)
			continue;
		netif_napi_del(&rxq->napi);
		hfi2_netdev_deallocate_ctxt(rx->ppd, rxq->rcd);
		hfi2_rcd_put(rxq->rcd);
		rxq->rcd = NULL;
	}

	kfree(rx->rxq);
	rx->rxq = NULL;
	rx->num_rx_q = 0;
}

static void enable_queues(struct hfi2_netdev_rx *rx)
{
	int i;

	for (i = 0; i < rx->num_rx_q; i++) {
		struct hfi2_netdev_rxq *rxq = &rx->rxq[i];

		dd_dev_info(rx->dd, "enabling queue %d on context %d\n", i,
			    rxq->rcd->ctxt);
		napi_enable(&rxq->napi);
		hfi2_rcvctrl(rx->dd,
			     HFI2_RCVCTRL_CTXT_ENB | HFI2_RCVCTRL_INTRAVAIL_ENB,
			     rxq->rcd);
	}
}

static void disable_queues(struct hfi2_netdev_rx *rx)
{
	int i;

	hfi2_msix_netdev_synchronize_irq(rx->ppd);

	for (i = 0; i < rx->num_rx_q; i++) {
		struct hfi2_netdev_rxq *rxq = &rx->rxq[i];

		dd_dev_info(rx->dd, "disabling queue %d on context %d\n", i,
			    rxq->rcd->ctxt);

		/* wait for napi if it was scheduled */
		hfi2_rcvctrl(rx->dd,
			     HFI2_RCVCTRL_CTXT_DIS | HFI2_RCVCTRL_INTRAVAIL_DIS,
			     rxq->rcd);
		napi_synchronize(&rxq->napi);
		napi_disable(&rxq->napi);
	}
}

/**
 * hfi2_netdev_rx_init - Incrememnts netdevs counter. When called first time,
 * it allocates receive queue data and calls netif_napi_add for each queue.
 *
 * @ppd: hfi2 port data
 */
int hfi2_netdev_rx_init(struct hfi2_pportdata *ppd)
{
	struct hfi2_netdev_rx *rx = ppd->netdev_rx;
	int res = 0;

	mutex_lock(&hfi2_mutex);
	if (rx->netdevs++ == 0) {
		res = hfi2_netdev_rxq_init(rx);
		if (res)
			rx->netdevs--;
	}
	mutex_unlock(&hfi2_mutex);
	return res;
}

/**
 * hfi2_netdev_rx_destroy - Decrements netdevs counter, when it reaches 0
 * napi is deleted and receive queses memory is freed.
 *
 * @ppd: hfi2 port data
 */
int hfi2_netdev_rx_destroy(struct hfi2_pportdata *ppd)
{
	struct hfi2_netdev_rx *rx = ppd->netdev_rx;

	/* destroy the RX queues only if it is the last netdev going away */
	mutex_lock(&hfi2_mutex);
	if (--rx->netdevs == 0)
		hfi2_netdev_rxq_deinit(rx);
	mutex_unlock(&hfi2_mutex);

	return 0;
}

/**
 * hfi2_alloc_rx - Allocates the rx support structure
 * @dd: hfi2 dev data
 *
 * Allocate the rx structure to support gathering the receive
 * resources and the dummy netdev.
 *
 * Updates ppd struct pointers upon success.
 *
 * Return: 0 (success) -error on failure
 *
 */
int hfi2_alloc_rx(struct hfi2_devdata *dd)
{
	struct hfi2_pportdata *ppd;
	struct hfi2_netdev_rx *rx;
	int i;

	dd_dev_info(dd, "hfi2 rx allocating, size %ld\n", sizeof(*rx));

	for (i = 0; i < dd->num_pports; i++) {
		ppd = &dd->pport[i];

		rx = kzalloc_node(sizeof(*rx), GFP_KERNEL, dd->node);

		if (!rx) {
			hfi2_free_rx(dd);
			return -ENOMEM;
		}
		rx->dd = dd;
		rx->ppd = ppd;
		rx->rx_napi = alloc_netdev_dummy(0);
		if (!rx->rx_napi) {
			kfree(rx);
			hfi2_free_rx(dd);
			return -ENOMEM;
		}

		xa_init(&rx->dev_tbl);
		atomic_set(&rx->enabled, 0);
		/* rx->netdevs is already zero from kzalloc */
		ppd->netdev_rx = rx;
	}

	return 0;
}

void hfi2_free_rx(struct hfi2_devdata *dd)
{
	struct hfi2_pportdata *ppd;
	int i;

	dd_dev_info(dd, "hfi2 rx freed\n");
	for (i = 0; i < dd->num_pports; i++) {
		struct hfi2_netdev_rx *rx;

		ppd = &dd->pport[i];
		rx = ppd->netdev_rx;
		if (!rx)
			continue;
		if (rx->rx_napi)
			free_netdev(rx->rx_napi);
		xa_destroy(&rx->dev_tbl);
		kfree(rx);
		ppd->netdev_rx = NULL;
	}
}

/**
 * hfi2_netdev_enable_queues - This is napi enable function.
 * It enables napi objects associated with queues.
 * When at least one device has called it, it increments atomic counter.
 * Disable function decrements counter and when it is 0,
 * calls napi_disable for every queue.
 *
 * @ppd: hfi2 port data
 */
void hfi2_netdev_enable_queues(struct hfi2_pportdata *ppd)
{
	struct hfi2_netdev_rx *rx = ppd->netdev_rx;

	if (!rx)
		return;

	if (atomic_fetch_inc(&rx->enabled))
		return;

	mutex_lock(&hfi2_mutex);
	enable_queues(rx);
	mutex_unlock(&hfi2_mutex);
}

void hfi2_netdev_disable_queues(struct hfi2_pportdata *ppd)
{
	struct hfi2_netdev_rx *rx = ppd->netdev_rx;

	if (!rx)
		return;

	if (atomic_dec_if_positive(&rx->enabled))
		return;

	mutex_lock(&hfi2_mutex);
	disable_queues(rx);
	mutex_unlock(&hfi2_mutex);
}

/**
 * hfi2_netdev_add_data - Registers data with unique identifier
 * to be requested later this is needed for IPoIB VLANs
 * implementations.
 * This call is protected by mutex idr_lock.
 *
 * @ppd: hfi2 port data
 * @id: requested integer id up to INT_MAX
 * @data: data to be associated with index
 */
int hfi2_netdev_add_data(struct hfi2_pportdata *ppd, int id, void *data)
{
	struct hfi2_netdev_rx *rx = ppd->netdev_rx;

	return xa_insert(&rx->dev_tbl, id, data, GFP_NOWAIT);
}

/**
 * hfi2_netdev_remove_data - Removes data with previously given id.
 * Returns the reference to removed entry.
 *
 * @ppd: hfi2 port data
 * @id: requested integer id up to INT_MAX
 */
void *hfi2_netdev_remove_data(struct hfi2_pportdata *ppd, int id)
{
	struct hfi2_netdev_rx *rx = ppd->netdev_rx;

	return xa_erase(&rx->dev_tbl, id);
}

/**
 * hfi2_netdev_get_data - Gets data with given id
 *
 * @ppd: hfi2 port data
 * @id: requested integer id up to INT_MAX
 */
void *hfi2_netdev_get_data(struct hfi2_pportdata *ppd, int id)
{
	struct hfi2_netdev_rx *rx = ppd->netdev_rx;

	return xa_load(&rx->dev_tbl, id);
}

/**
 * hfi2_netdev_get_first_data - Gets first entry with greater or equal id.
 *
 * @ppd: hfi2 port data
 * @start_id: requested integer id up to INT_MAX
 */
void *hfi2_netdev_get_first_data(struct hfi2_pportdata *ppd, int *start_id)
{
	struct hfi2_netdev_rx *rx = ppd->netdev_rx;
	unsigned long index = *start_id;
	void *ret;

	ret = xa_find(&rx->dev_tbl, &index, UINT_MAX, XA_PRESENT);
	*start_id = (int)index;
	return ret;
}
