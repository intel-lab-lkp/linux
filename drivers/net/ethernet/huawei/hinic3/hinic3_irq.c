// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.

#include <linux/netdevice.h>

#include "hinic3_hw_comm.h"
#include "hinic3_hwdev.h"
#include "hinic3_hwif.h"
#include "hinic3_nic_dev.h"
#include "hinic3_rx.h"
#include "hinic3_tx.h"

static int hinic3_poll(struct napi_struct *napi, int budget)
{
	struct hinic3_irq_cfg *irq_cfg =
		container_of(napi, struct hinic3_irq_cfg, napi);
	struct hinic3_nic_dev *nic_dev;
	int tx_pkts, rx_pkts;

	nic_dev = netdev_priv(irq_cfg->netdev);
	rx_pkts = hinic3_rx_poll(irq_cfg->rxq, budget);

	tx_pkts = hinic3_tx_poll(irq_cfg->txq, budget);
	if (tx_pkts >= budget || rx_pkts >= budget)
		return budget;

	napi_complete(napi);

	hinic3_set_msix_state(nic_dev->hwdev, irq_cfg->msix_entry_idx,
			      HINIC3_MSIX_ENABLE);

	return max(tx_pkts, rx_pkts);
}

void qp_add_napi(struct hinic3_irq_cfg *irq_cfg)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(irq_cfg->netdev);

	netif_napi_add(nic_dev->netdev, &irq_cfg->napi, hinic3_poll);
	napi_enable(&irq_cfg->napi);
}

void qp_del_napi(struct hinic3_irq_cfg *irq_cfg)
{
	napi_disable(&irq_cfg->napi);
	netif_napi_del(&irq_cfg->napi);
}
