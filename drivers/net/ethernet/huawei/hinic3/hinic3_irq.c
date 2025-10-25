// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.

#include <linux/netdevice.h>

#include "hinic3_hw_comm.h"
#include "hinic3_hwdev.h"
#include "hinic3_hwif.h"
#include "hinic3_nic_dev.h"
#include "hinic3_rx.h"
#include "hinic3_tx.h"

#define HINIC3_RX_RATE_THRESH  50000
#define HINIC3_AVG_PKT_SMALL   256U

static int hinic3_poll(struct napi_struct *napi, int budget)
{
	struct hinic3_irq_cfg *irq_cfg =
		container_of(napi, struct hinic3_irq_cfg, napi);
	struct hinic3_nic_dev *nic_dev;
	bool busy = false;
	int work_done;

	nic_dev = netdev_priv(irq_cfg->netdev);

	busy |= hinic3_tx_poll(irq_cfg->txq, budget);

	if (unlikely(!budget))
		return 0;

	work_done = hinic3_rx_poll(irq_cfg->rxq, budget);
	busy |= work_done >= budget;

	if (busy)
		return budget;

	if (likely(napi_complete_done(napi, work_done)))
		hinic3_set_msix_state(nic_dev->hwdev, irq_cfg->msix_entry_idx,
				      HINIC3_MSIX_ENABLE);

	return work_done;
}

static void qp_add_napi(struct hinic3_irq_cfg *irq_cfg)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(irq_cfg->netdev);

	netif_napi_add(nic_dev->netdev, &irq_cfg->napi, hinic3_poll);
	netif_queue_set_napi(irq_cfg->netdev, irq_cfg->irq_id,
			     NETDEV_QUEUE_TYPE_RX, &irq_cfg->napi);
	netif_queue_set_napi(irq_cfg->netdev, irq_cfg->irq_id,
			     NETDEV_QUEUE_TYPE_TX, &irq_cfg->napi);
	napi_enable(&irq_cfg->napi);
}

static void qp_del_napi(struct hinic3_irq_cfg *irq_cfg)
{
	napi_disable(&irq_cfg->napi);
	netif_queue_set_napi(irq_cfg->netdev, irq_cfg->irq_id,
			     NETDEV_QUEUE_TYPE_RX, NULL);
	netif_queue_set_napi(irq_cfg->netdev, irq_cfg->irq_id,
			     NETDEV_QUEUE_TYPE_TX, NULL);
	netif_stop_subqueue(irq_cfg->netdev, irq_cfg->irq_id);
	netif_napi_del(&irq_cfg->napi);
}

static irqreturn_t qp_irq(int irq, void *data)
{
	struct hinic3_irq_cfg *irq_cfg = data;
	struct hinic3_nic_dev *nic_dev;

	nic_dev = netdev_priv(irq_cfg->netdev);
	hinic3_msix_intr_clear_resend_bit(nic_dev->hwdev,
					  irq_cfg->msix_entry_idx, 1);

	napi_schedule(&irq_cfg->napi);

	return IRQ_HANDLED;
}

static int hinic3_request_irq(struct hinic3_irq_cfg *irq_cfg, u16 q_id)
{
	struct hinic3_interrupt_info info = {};
	struct hinic3_nic_dev *nic_dev;
	struct net_device *netdev;
	int err;

	netdev = irq_cfg->netdev;
	nic_dev = netdev_priv(netdev);
	qp_add_napi(irq_cfg);

	info.msix_index = irq_cfg->msix_entry_idx;
	info.interrupt_coalesc_set = 1;
	info.pending_limit = nic_dev->intr_coalesce[q_id].pending_limit;
	info.coalesc_timer_cfg =
		nic_dev->intr_coalesce[q_id].coalesce_timer_cfg;
	info.resend_timer_cfg = nic_dev->intr_coalesce[q_id].resend_timer_cfg;
	err = hinic3_set_interrupt_cfg(nic_dev->hwdev, info);
	if (err) {
		netdev_err(netdev, "Failed to set RX interrupt coalescing attribute.\n");
		qp_del_napi(irq_cfg);
		return err;
	}

	err = request_irq(irq_cfg->irq_id, qp_irq, 0, irq_cfg->irq_name,
			  irq_cfg);
	if (err) {
		qp_del_napi(irq_cfg);
		return err;
	}

	irq_set_affinity_hint(irq_cfg->irq_id, &irq_cfg->affinity_mask);

	return 0;
}

static void hinic3_release_irq(struct hinic3_irq_cfg *irq_cfg)
{
	irq_set_affinity_hint(irq_cfg->irq_id, NULL);
	free_irq(irq_cfg->irq_id, irq_cfg);
}

static int hinic3_set_interrupt_moder(struct net_device *netdev, u16 q_id,
				      u8 coalesc_timer_cfg, u8 pending_limit)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_interrupt_info info = {};
	int err;

	if (coalesc_timer_cfg == nic_dev->rxqs[q_id].last_coalesc_timer_cfg &&
	    pending_limit == nic_dev->rxqs[q_id].last_pending_limit)
		return 0;

	if (q_id >= nic_dev->q_params.num_qps)
		return 0;

	info.interrupt_coalesc_set = 1;
	info.coalesc_timer_cfg = coalesc_timer_cfg;
	info.pending_limit = pending_limit;
	info.msix_index = nic_dev->q_params.irq_cfg[q_id].msix_entry_idx;
	info.resend_timer_cfg =
		nic_dev->intr_coalesce[q_id].resend_timer_cfg;

	err = hinic3_set_interrupt_cfg(nic_dev->hwdev, info);
	if (err) {
		netdev_err(netdev,
			   "Failed to modify moderation for Queue: %u\n", q_id);
	} else {
		nic_dev->rxqs[q_id].last_coalesc_timer_cfg = coalesc_timer_cfg;
		nic_dev->rxqs[q_id].last_pending_limit = pending_limit;
	}

	return err;
}

static void hinic3_calc_coal_para(struct hinic3_intr_coal_info *q_coal,
				  u64 rx_rate, u8 *coalesc_timer_cfg,
				  u8 *pending_limit)
{
	if (rx_rate < q_coal->pkt_rate_low) {
		*coalesc_timer_cfg = q_coal->rx_usecs_low;
		*pending_limit = q_coal->rx_pending_limit_low;
	} else if (rx_rate > q_coal->pkt_rate_high) {
		*coalesc_timer_cfg = q_coal->rx_usecs_high;
		*pending_limit = q_coal->rx_pending_limit_high;
	} else {
		*coalesc_timer_cfg =
			(u8)((rx_rate - q_coal->pkt_rate_low) *
			     (q_coal->rx_usecs_high - q_coal->rx_usecs_low) /
			     (q_coal->pkt_rate_high - q_coal->pkt_rate_low) +
			     q_coal->rx_usecs_low);

		*pending_limit =
			(u8)((rx_rate - q_coal->pkt_rate_low) *
			     (q_coal->rx_pending_limit_high -
			      q_coal->rx_pending_limit_low) /
			     (q_coal->pkt_rate_high - q_coal->pkt_rate_low) +
			     q_coal->rx_pending_limit_low);
	}
}

static void hinic3_update_queue_coal(struct net_device *netdev, u16 qid,
				     u64 rx_rate, u64 avg_pkt_size, u64 tx_rate)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_intr_coal_info *q_coal;
	u8 coalesc_timer_cfg, pending_limit;

	q_coal = &nic_dev->intr_coalesce[qid];

	if (rx_rate > HINIC3_RX_RATE_THRESH &&
	    avg_pkt_size > HINIC3_AVG_PKT_SMALL) {
		hinic3_calc_coal_para(q_coal, rx_rate, &coalesc_timer_cfg,
				      &pending_limit);
	} else {
		coalesc_timer_cfg = 3;
		pending_limit = q_coal->rx_pending_limit_low;
	}

	hinic3_set_interrupt_moder(netdev, qid,
				   coalesc_timer_cfg, pending_limit);
}

static void hinic3_auto_moderation_work(struct work_struct *work)
{
	u64 rx_packets, rx_bytes, rx_pkt_diff, rx_rate, avg_pkt_size;
	u64 tx_packets, tx_bytes, tx_pkt_diff, tx_rate;
	struct hinic3_nic_dev *nic_dev;
	struct delayed_work *delay;
	struct net_device *netdev;
	unsigned long period;
	u16 qid;

	delay = to_delayed_work(work);
	nic_dev = container_of(delay, struct hinic3_nic_dev, moderation_task);
	period = (unsigned long)(jiffies - nic_dev->last_moder_jiffies);
	netdev = nic_dev->netdev;

	queue_delayed_work(nic_dev->workq, &nic_dev->moderation_task,
			   HINIC3_MODERATONE_DELAY);

	for (qid = 0; qid < nic_dev->q_params.num_qps; qid++) {
		rx_packets = nic_dev->rxqs[qid].rxq_stats.packets;
		rx_bytes = nic_dev->rxqs[qid].rxq_stats.bytes;
		tx_packets = nic_dev->txqs[qid].txq_stats.packets;
		tx_bytes = nic_dev->txqs[qid].txq_stats.bytes;

		rx_pkt_diff =
			rx_packets - nic_dev->rxqs[qid].last_moder_packets;
		avg_pkt_size = rx_pkt_diff ?
			((unsigned long)(rx_bytes -
			 nic_dev->rxqs[qid].last_moder_bytes)) /
			 rx_pkt_diff : 0;

		rx_rate = rx_pkt_diff * HZ / period;
		tx_pkt_diff =
			tx_packets - nic_dev->txqs[qid].last_moder_packets;
		tx_rate = tx_pkt_diff * HZ / period;

		hinic3_update_queue_coal(netdev, qid, rx_rate, avg_pkt_size,
					 tx_rate);
		nic_dev->rxqs[qid].last_moder_packets = rx_packets;
		nic_dev->rxqs[qid].last_moder_bytes = rx_bytes;
		nic_dev->txqs[qid].last_moder_packets = tx_packets;
		nic_dev->txqs[qid].last_moder_bytes = tx_bytes;
	}

	nic_dev->last_moder_jiffies = jiffies;
}

int hinic3_qps_irq_init(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct pci_dev *pdev = nic_dev->pdev;
	struct hinic3_irq_cfg *irq_cfg;
	struct msix_entry *msix_entry;
	u32 local_cpu;
	u16 q_id;
	int err;

	for (q_id = 0; q_id < nic_dev->q_params.num_qps; q_id++) {
		msix_entry = &nic_dev->qps_msix_entries[q_id];
		irq_cfg = &nic_dev->q_params.irq_cfg[q_id];

		irq_cfg->irq_id = msix_entry->vector;
		irq_cfg->msix_entry_idx = msix_entry->entry;
		irq_cfg->netdev = netdev;
		irq_cfg->txq = &nic_dev->txqs[q_id];
		irq_cfg->rxq = &nic_dev->rxqs[q_id];
		nic_dev->rxqs[q_id].irq_cfg = irq_cfg;

		local_cpu = cpumask_local_spread(q_id, dev_to_node(&pdev->dev));
		cpumask_set_cpu(local_cpu, &irq_cfg->affinity_mask);

		snprintf(irq_cfg->irq_name, sizeof(irq_cfg->irq_name),
			 "%s_qp%u", netdev->name, q_id);

		err = hinic3_request_irq(irq_cfg, q_id);
		if (err) {
			netdev_err(netdev, "Failed to request Rx irq\n");
			goto err_release_irqs;
		}

		hinic3_set_msix_auto_mask_state(nic_dev->hwdev,
						irq_cfg->msix_entry_idx,
						HINIC3_SET_MSIX_AUTO_MASK);
		hinic3_set_msix_state(nic_dev->hwdev, irq_cfg->msix_entry_idx,
				      HINIC3_MSIX_ENABLE);
	}

	INIT_DELAYED_WORK(&nic_dev->moderation_task,
			  hinic3_auto_moderation_work);

	return 0;

err_release_irqs:
	while (q_id > 0) {
		q_id--;
		irq_cfg = &nic_dev->q_params.irq_cfg[q_id];
		qp_del_napi(irq_cfg);
		hinic3_set_msix_state(nic_dev->hwdev, irq_cfg->msix_entry_idx,
				      HINIC3_MSIX_DISABLE);
		hinic3_set_msix_auto_mask_state(nic_dev->hwdev,
						irq_cfg->msix_entry_idx,
						HINIC3_CLR_MSIX_AUTO_MASK);
		hinic3_release_irq(irq_cfg);
	}

	return err;
}

void hinic3_qps_irq_uninit(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_irq_cfg *irq_cfg;
	u16 q_id;

	for (q_id = 0; q_id < nic_dev->q_params.num_qps; q_id++) {
		irq_cfg = &nic_dev->q_params.irq_cfg[q_id];
		qp_del_napi(irq_cfg);
		hinic3_set_msix_state(nic_dev->hwdev, irq_cfg->msix_entry_idx,
				      HINIC3_MSIX_DISABLE);
		hinic3_set_msix_auto_mask_state(nic_dev->hwdev,
						irq_cfg->msix_entry_idx,
						HINIC3_CLR_MSIX_AUTO_MASK);
		hinic3_release_irq(irq_cfg);
	}
}
