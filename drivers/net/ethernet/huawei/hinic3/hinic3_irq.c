// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/debugfs.h>

#include "hinic3_hw_comm.h"
#include "hinic3_hwif.h"
#include "hinic3_nic_io.h"
#include "hinic3_nic_dev.h"
#include "hinic3_tx.h"
#include "hinic3_rx.h"

#define HINIC3_RX_RATE_THRESH  50000
#define HINIC3_AVG_PKT_SMALL   256U
#define HINIC3_LOWEST_LATENCY  3

static int hinic3_poll(struct napi_struct *napi, int budget)
{
	struct hinic3_irq *irq_cfg =
		container_of(napi, struct hinic3_irq, napi);
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

static void qp_add_napi(struct hinic3_irq *irq_cfg)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(irq_cfg->netdev);

	netif_napi_add(nic_dev->netdev, &irq_cfg->napi, hinic3_poll);
	napi_enable(&irq_cfg->napi);
}

static void qp_del_napi(struct hinic3_irq *irq_cfg)
{
	napi_disable(&irq_cfg->napi);
	netif_napi_del(&irq_cfg->napi);
}

static irqreturn_t qp_irq(int irq, void *data)
{
	struct hinic3_irq *irq_cfg = data;
	struct hinic3_nic_dev *nic_dev;

	nic_dev = netdev_priv(irq_cfg->netdev);
	hinic3_misx_intr_clear_resend_bit(nic_dev->hwdev, irq_cfg->msix_entry_idx, 1);

	napi_schedule(&irq_cfg->napi);

	return IRQ_HANDLED;
}

static int hinic3_request_irq(struct hinic3_irq *irq_cfg, u16 q_id)
{
	struct interrupt_info info = {};
	struct hinic3_nic_dev *nic_dev;
	struct net_device *netdev;
	int err;

	netdev = irq_cfg->netdev;
	nic_dev = netdev_priv(netdev);
	qp_add_napi(irq_cfg);

	info.msix_index = irq_cfg->msix_entry_idx;
	info.lli_set = 0;
	info.interrupt_coalesc_set = 1;
	info.pending_limt = nic_dev->intr_coalesce[q_id].pending_limt;
	info.coalesc_timer_cfg =
		nic_dev->intr_coalesce[q_id].coalesce_timer_cfg;
	info.resend_timer_cfg = nic_dev->intr_coalesce[q_id].resend_timer_cfg;
	nic_dev->rxqs[q_id].last_coalesc_timer_cfg =
			nic_dev->intr_coalesce[q_id].coalesce_timer_cfg;
	nic_dev->rxqs[q_id].last_pending_limt =
			nic_dev->intr_coalesce[q_id].pending_limt;
	err = hinic3_set_interrupt_cfg(nic_dev->hwdev, info);
	if (err) {
		netdev_err(netdev, "Failed to set RX interrupt coalescing attribute.\n");
		qp_del_napi(irq_cfg);
		return err;
	}

	err = request_irq(irq_cfg->irq_id, &qp_irq, 0, irq_cfg->irq_name, irq_cfg);
	if (err) {
		netdev_err(netdev, "Failed to request Rx irq\n");
		qp_del_napi(irq_cfg);
		return err;
	}

	irq_set_affinity_hint(irq_cfg->irq_id, &irq_cfg->affinity_mask);

	return 0;
}

static void hinic3_release_irq(struct hinic3_irq *irq_cfg)
{
	irq_set_affinity_hint(irq_cfg->irq_id, NULL);
	synchronize_irq(irq_cfg->irq_id);
	free_irq(irq_cfg->irq_id, irq_cfg);
	qp_del_napi(irq_cfg);
}

static int set_interrupt_moder(struct net_device *netdev, u16 q_id,
			       u8 coalesc_timer_cfg, u8 pending_limt)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct interrupt_info info;
	int err;

	memset(&info, 0, sizeof(info));

	if (coalesc_timer_cfg == nic_dev->rxqs[q_id].last_coalesc_timer_cfg &&
	    pending_limt == nic_dev->rxqs[q_id].last_pending_limt)
		return 0;

	if (!HINIC3_CHANNEL_RES_VALID(nic_dev) ||
	    q_id >= nic_dev->q_params.num_qps)
		return 0;

	info.lli_set = 0;
	info.interrupt_coalesc_set = 1;
	info.coalesc_timer_cfg = coalesc_timer_cfg;
	info.pending_limt = pending_limt;
	info.msix_index = nic_dev->q_params.irq_cfg[q_id].msix_entry_idx;
	info.resend_timer_cfg =
		nic_dev->intr_coalesce[q_id].resend_timer_cfg;

	err = hinic3_set_interrupt_cfg(nic_dev->hwdev, info);
	if (err) {
		netdev_err(netdev, "Failed to modify moderation for Queue: %u\n", q_id);
	} else {
		nic_dev->rxqs[q_id].last_coalesc_timer_cfg = coalesc_timer_cfg;
		nic_dev->rxqs[q_id].last_pending_limt = pending_limt;
	}

	return err;
}

static void calc_coal_para(struct hinic3_intr_coal_info *q_coal, u64 rx_rate,
			   u8 *coalesc_timer_cfg, u8 *pending_limt)
{
	if (rx_rate < q_coal->pkt_rate_low) {
		*coalesc_timer_cfg = q_coal->rx_usecs_low;
		*pending_limt = q_coal->rx_pending_limt_low;
	} else if (rx_rate > q_coal->pkt_rate_high) {
		*coalesc_timer_cfg = q_coal->rx_usecs_high;
		*pending_limt = q_coal->rx_pending_limt_high;
	} else {
		*coalesc_timer_cfg =
			(u8)((rx_rate - q_coal->pkt_rate_low) *
			     (q_coal->rx_usecs_high - q_coal->rx_usecs_low) /
			     (q_coal->pkt_rate_high - q_coal->pkt_rate_low) +
			     q_coal->rx_usecs_low);

		*pending_limt =
			(u8)((rx_rate - q_coal->pkt_rate_low) *
			     (q_coal->rx_pending_limt_high - q_coal->rx_pending_limt_low) /
			     (q_coal->pkt_rate_high - q_coal->pkt_rate_low) +
			     q_coal->rx_pending_limt_low);
	}
}

static void update_queue_coal(struct net_device *netdev, u16 qid,
			      u64 rx_rate, u64 avg_pkt_size, u64 tx_rate)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_intr_coal_info *q_coal;
	u8 coalesc_timer_cfg, pending_limt;

	q_coal = &nic_dev->intr_coalesce[qid];

	if (rx_rate > HINIC3_RX_RATE_THRESH && avg_pkt_size > HINIC3_AVG_PKT_SMALL) {
		calc_coal_para(q_coal, rx_rate, &coalesc_timer_cfg, &pending_limt);
	} else {
		coalesc_timer_cfg = HINIC3_LOWEST_LATENCY;
		pending_limt = q_coal->rx_pending_limt_low;
	}

	set_interrupt_moder(netdev, qid, coalesc_timer_cfg, pending_limt);
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
	if (!test_bit(HINIC3_INTF_UP, &nic_dev->flags))
		return;

	queue_delayed_work(nic_dev->workq, &nic_dev->moderation_task,
			   HINIC3_MODERATONE_DELAY);

	if (!nic_dev->adaptive_rx_coal || !period)
		return;

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

		update_queue_coal(netdev, qid, rx_rate, avg_pkt_size,
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
	struct irq_info *qp_irq_info;
	struct hinic3_irq *irq_cfg;
	u32 local_cpu;
	u16 q_id, i;
	int err;

	for (q_id = 0; q_id < nic_dev->q_params.num_qps; q_id++) {
		qp_irq_info = &nic_dev->qps_irq_info[q_id];
		irq_cfg = &nic_dev->q_params.irq_cfg[q_id];

		irq_cfg->irq_id = qp_irq_info->irq_id;
		irq_cfg->msix_entry_idx = qp_irq_info->msix_entry_idx;
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
			goto err_req_tx_irq;
		}

		hinic3_set_msix_auto_mask_state(nic_dev->hwdev, irq_cfg->msix_entry_idx,
						HINIC3_SET_MSIX_AUTO_MASK);
		hinic3_set_msix_state(nic_dev->hwdev, irq_cfg->msix_entry_idx, HINIC3_MSIX_ENABLE);
	}

	INIT_DELAYED_WORK(&nic_dev->moderation_task, hinic3_auto_moderation_work);

	return 0;

err_req_tx_irq:
	for (i = 0; i < q_id; i++) {
		irq_cfg = &nic_dev->q_params.irq_cfg[i];
		hinic3_set_msix_state(nic_dev->hwdev, irq_cfg->msix_entry_idx, HINIC3_MSIX_DISABLE);
		hinic3_set_msix_auto_mask_state(nic_dev->hwdev, irq_cfg->msix_entry_idx,
						HINIC3_CLR_MSIX_AUTO_MASK);
		hinic3_release_irq(irq_cfg);
	}

	return err;
}

void hinic3_qps_irq_deinit(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_irq *irq_cfg;
	u16 q_id;

	for (q_id = 0; q_id < nic_dev->q_params.num_qps; q_id++) {
		irq_cfg = &nic_dev->q_params.irq_cfg[q_id];
		hinic3_set_msix_state(nic_dev->hwdev, irq_cfg->msix_entry_idx,
				      HINIC3_MSIX_DISABLE);
		hinic3_set_msix_auto_mask_state(nic_dev->hwdev,
						irq_cfg->msix_entry_idx,
						HINIC3_CLR_MSIX_AUTO_MASK);
		hinic3_release_irq(irq_cfg);
	}
}
