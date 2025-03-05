// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2024 Intel Corporation */

#include <net/libeth/xdp.h>

#include "idpf.h"
#include "xdp.h"

static int idpf_rxq_for_each(const struct idpf_vport *vport,
			     int (*fn)(struct idpf_rx_queue *rxq, void *arg),
			     void *arg)
{
	bool splitq = idpf_is_queue_model_split(vport->rxq_model);

	if (!vport->rxq_grps)
		return -ENETDOWN;

	for (u32 i = 0; i < vport->num_rxq_grp; i++) {
		const struct idpf_rxq_group *rx_qgrp = &vport->rxq_grps[i];
		u32 num_rxq;

		if (splitq)
			num_rxq = rx_qgrp->splitq.num_rxq_sets;
		else
			num_rxq = rx_qgrp->singleq.num_rxq;

		for (u32 j = 0; j < num_rxq; j++) {
			struct idpf_rx_queue *q;
			int err;

			if (splitq)
				q = &rx_qgrp->splitq.rxq_sets[j]->rxq;
			else
				q = rx_qgrp->singleq.rxqs[j];

			err = fn(q, arg);
			if (err)
				return err;
		}
	}

	return 0;
}

/**
 * __idpf_xdp_rxq_info_init - Setup XDP RxQ info for a given Rx queue
 * @rxq: Rx queue for which the resources are setup
 * @arg: flag indicating if the HW works in split queue mode
 *
 * Return: 0 on success, negative on failure.
 */
static int __idpf_xdp_rxq_info_init(struct idpf_rx_queue *rxq, void *arg)
{
	const struct idpf_vport *vport = rxq->q_vector->vport;
	bool split = idpf_is_queue_model_split(vport->rxq_model);
	const struct page_pool *pp;
	int err;

	err = __xdp_rxq_info_reg(&rxq->xdp_rxq, vport->netdev, rxq->idx,
				 rxq->q_vector->napi.napi_id,
				 rxq->rx_buf_size);
	if (err)
		return err;

	pp = split ? rxq->bufq_sets[0].bufq.pp : rxq->pp;
	xdp_rxq_info_attach_page_pool(&rxq->xdp_rxq, pp);

	if (!split)
		return 0;

	rxq->xdpqs = &vport->txqs[vport->xdp_txq_offset];
	rxq->num_xdp_txq = vport->num_xdp_txq;

	return 0;
}

/**
 * idpf_xdp_rxq_info_init_all - initialize RxQ info for all Rx queues in vport
 * @vport: vport to setup the info
 *
 * Return: 0 on success, negative on failure.
 */
int idpf_xdp_rxq_info_init_all(const struct idpf_vport *vport)
{
	return idpf_rxq_for_each(vport, __idpf_xdp_rxq_info_init, NULL);
}

/**
 * __idpf_xdp_rxq_info_deinit - Deinit XDP RxQ info for a given Rx queue
 * @rxq: Rx queue for which the resources are destroyed
 * @arg: flag indicating if the HW works in split queue mode
 *
 * Return: always 0.
 */
static int __idpf_xdp_rxq_info_deinit(struct idpf_rx_queue *rxq, void *arg)
{
	if (idpf_is_queue_model_split((size_t)arg)) {
		rxq->xdpqs = NULL;
		rxq->num_xdp_txq = 0;
	}

	xdp_rxq_info_detach_mem_model(&rxq->xdp_rxq);
	xdp_rxq_info_unreg(&rxq->xdp_rxq);

	return 0;
}

/**
 * idpf_xdp_rxq_info_deinit_all - deinit RxQ info for all Rx queues in vport
 * @vport: vport to setup the info
 */
void idpf_xdp_rxq_info_deinit_all(const struct idpf_vport *vport)
{
	idpf_rxq_for_each(vport, __idpf_xdp_rxq_info_deinit,
			  (void *)(size_t)vport->rxq_model);
}

int idpf_vport_xdpq_get(const struct idpf_vport *vport)
{
	struct libeth_xdpsq_timer **timers __free(kvfree) = NULL;
	struct net_device *dev;
	u32 sqs;

	if (!idpf_xdp_is_prog_ena(vport))
		return 0;

	timers = kvcalloc(vport->num_xdp_txq, sizeof(*timers), GFP_KERNEL);
	if (!timers)
		return -ENOMEM;

	for (u32 i = 0; i < vport->num_xdp_txq; i++) {
		timers[i] = kzalloc_node(sizeof(*timers[i]), GFP_KERNEL,
					 cpu_to_mem(i));
		if (!timers[i]) {
			for (int j = i - 1; j >= 0; j--)
				kfree(timers[j]);

			return -ENOMEM;
		}
	}

	dev = vport->netdev;
	sqs = vport->xdp_txq_offset;

	for (u32 i = sqs; i < vport->num_txq; i++) {
		struct idpf_tx_queue *xdpq = vport->txqs[i];

		xdpq->complq = xdpq->txq_grp->complq;

		idpf_queue_clear(FLOW_SCH_EN, xdpq);
		idpf_queue_clear(FLOW_SCH_EN, xdpq->complq);
		idpf_queue_set(NOIRQ, xdpq);
		idpf_queue_set(XDP, xdpq);
		idpf_queue_set(XDP, xdpq->complq);

		xdpq->timer = timers[i - sqs];
		libeth_xdpsq_get(&xdpq->xdp_lock, dev, vport->xdpq_share);

		xdpq->pending = 0;
		xdpq->xdp_tx = 0;
		xdpq->thresh = libeth_xdp_queue_threshold(xdpq->desc_count);
	}

	return 0;
}

void idpf_vport_xdpq_put(const struct idpf_vport *vport)
{
	struct net_device *dev;
	u32 sqs;

	if (!idpf_xdp_is_prog_ena(vport))
		return;

	dev = vport->netdev;
	sqs = vport->xdp_txq_offset;

	for (u32 i = sqs; i < vport->num_txq; i++) {
		struct idpf_tx_queue *xdpq = vport->txqs[i];

		if (!idpf_queue_has_clear(XDP, xdpq))
			continue;

		libeth_xdpsq_put(&xdpq->xdp_lock, dev);

		kfree(xdpq->timer);
		idpf_queue_clear(NOIRQ, xdpq);
	}
}
