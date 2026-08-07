/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/*
 * Copyright(c) 2020 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 *
 */

#ifndef HFI2_NETDEV_H
#define HFI2_NETDEV_H

#include "hfi2.h"

#include <linux/netdevice.h>
#include <linux/xarray.h>

/**
 * struct hfi2_netdev_rxq - Receive Queue for HFI
 * IPoIB netdevices will be working on the rx abstraction.
 * @napi: napi object
 * @rx: ptr to netdev_rx
 * @rcd:  ptr to receive context data
 */
struct hfi2_netdev_rxq {
	struct napi_struct napi;
	struct hfi2_netdev_rx *rx;
	struct hfi2_ctxtdata *rcd;
};

#define HFI2_MAX_NETDEV_CTXTS 8

/* Number of NETDEV RSM entries */
#define NUM_NETDEV_MAP_ENTRIES HFI2_MAX_NETDEV_CTXTS

/**
 * struct hfi2_netdev_rx: data required to setup and run HFI netdev.
 * @rx_napi:	the dummy netdevice to support "polling" the receive contexts
 * @dd:		hfi2_devdata
 * @ppd:	hfi2_pportdata
 * @rxq:	pointer to dummy netdev receive queues.
 * @num_rx_q:	number of receive queues
 * @rmt_start:	first allocated index in the RMT
 * @dev_tbl:	netdev table for unique identifier VNIC and IPoIb VLANs.
 * @enabled:	atomic counter of netdevs enabling receive queues.
 *		When 0 NAPI will be disabled.
 * @netdevs:	count of netdev_rx users, protected by hfi2_mutex.
 *		When 0 receive queues will be freed.
 */
struct hfi2_netdev_rx {
	struct net_device *rx_napi;
	struct hfi2_devdata *dd;
	struct hfi2_pportdata *ppd;
	struct hfi2_netdev_rxq *rxq;
	int num_rx_q;
	int rmt_start;
	struct xarray dev_tbl;
	/* count of enabled napi polls */
	atomic_t enabled;
	int netdevs;
};

static inline int hfi2_netdev_ctxt_count(struct hfi2_pportdata *ppd)
{
	return ppd->netdev_rx->num_rx_q;
}

static inline struct hfi2_ctxtdata *
hfi2_netdev_get_ctxt(struct hfi2_pportdata *ppd, int ctxt)
{
	return ppd->netdev_rx->rxq[ctxt].rcd;
}

static inline int hfi2_netdev_get_free_rmt_idx(struct hfi2_pportdata *ppd)
{
	return ppd->netdev_rx->rmt_start;
}

u32 hfi2_num_netdev_contexts(struct hfi2_devdata *dd, u32 available_contexts,
			     const struct cpumask *cpu_mask);

void hfi2_netdev_enable_queues(struct hfi2_pportdata *ppd);
void hfi2_netdev_disable_queues(struct hfi2_pportdata *ppd);
int hfi2_netdev_rx_init(struct hfi2_pportdata *ppd);
int hfi2_netdev_rx_destroy(struct hfi2_pportdata *ppd);
int hfi2_alloc_rx(struct hfi2_devdata *dd);
void hfi2_free_rx(struct hfi2_devdata *dd);
int hfi2_netdev_add_data(struct hfi2_pportdata *ppd, int id, void *data);
void *hfi2_netdev_remove_data(struct hfi2_pportdata *ppd, int id);
void *hfi2_netdev_get_data(struct hfi2_pportdata *ppd, int id);
void *hfi2_netdev_get_first_data(struct hfi2_pportdata *ppd, int *start_id);

/* chip.c  */
int hfi2_netdev_rx_napi(struct napi_struct *napi, int budget);

#endif /* HFI2_NETDEV_H */
