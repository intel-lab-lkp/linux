// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/netdevice.h>
#include <linux/etherdevice.h>

#include "hinic3_nic_dev.h"
#include "hinic3_nic_cfg.h"
#include "hinic3_nic_io.h"
#include "hinic3_tx.h"
#include "hinic3_rx.h"
#include "hinic3_rss.h"
#include "hinic3_hwif.h"

/* try to modify the number of irq to the target number,
 * and return the actual number of irq.
 */
static u16 hinic3_qp_irq_change(struct net_device *netdev,
				u16 dst_num_qp_irq)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	u16 resp_irq_num, irq_num_gap, i;
	struct irq_info *qps_irq_info;
	u16 idx;
	int err;

	qps_irq_info = nic_dev->qps_irq_info;
	if (dst_num_qp_irq > nic_dev->num_qp_irq) {
		irq_num_gap = dst_num_qp_irq - nic_dev->num_qp_irq;
		err = hinic3_alloc_irqs(nic_dev->hwdev, irq_num_gap,
					&qps_irq_info[nic_dev->num_qp_irq],
					&resp_irq_num);
		if (err) {
			netdev_err(netdev, "Failed to alloc irqs\n");
			return nic_dev->num_qp_irq;
		}

		nic_dev->num_qp_irq += resp_irq_num;
	} else if (dst_num_qp_irq < nic_dev->num_qp_irq) {
		irq_num_gap = nic_dev->num_qp_irq - dst_num_qp_irq;
		for (i = 0; i < irq_num_gap; i++) {
			idx = (nic_dev->num_qp_irq - i) - 1;
			hinic3_free_irq(nic_dev->hwdev,
					qps_irq_info[idx].irq_id);
			qps_irq_info[idx].irq_id = 0;
			qps_irq_info[idx].msix_entry_idx = 0;
		}
		nic_dev->num_qp_irq = dst_num_qp_irq;
	}

	return nic_dev->num_qp_irq;
}

static void hinic3_config_num_qps(struct net_device *netdev,
				  struct hinic3_dyna_txrxq_params *q_params)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	u16 alloc_num_irq, cur_num_irq;
	u16 dst_num_irq;

	if (!test_bit(HINIC3_RSS_ENABLE, &nic_dev->flags))
		q_params->num_qps = 1;

	if (nic_dev->num_qp_irq >= q_params->num_qps)
		goto out;

	cur_num_irq = nic_dev->num_qp_irq;

	alloc_num_irq = hinic3_qp_irq_change(netdev, q_params->num_qps);
	if (alloc_num_irq < q_params->num_qps) {
		q_params->num_qps = alloc_num_irq;
		netdev_warn(netdev, "Can not get enough irqs, adjust num_qps to %u\n",
			    q_params->num_qps);

		/* The current irq may be in use, we must keep it */
		dst_num_irq = max_t(u16, cur_num_irq, q_params->num_qps);
		hinic3_qp_irq_change(netdev, dst_num_irq);
	}

out:
	netdev_dbg(netdev, "Finally num_qps: %u\n", q_params->num_qps);
}

static int hinic3_setup_num_qps(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	u32 irq_size;

	nic_dev->num_qp_irq = 0;

	irq_size = sizeof(*nic_dev->qps_irq_info) * nic_dev->max_qps;
	if (!irq_size) {
		netdev_err(netdev, "Cannot allocate zero size entries\n");
		return -EINVAL;
	}
	nic_dev->qps_irq_info = kzalloc(irq_size, GFP_KERNEL);
	if (!nic_dev->qps_irq_info)
		return -ENOMEM;

	hinic3_config_num_qps(netdev, &nic_dev->q_params);

	return 0;
}

static void hinic3_destroy_num_qps(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	u16 i;

	for (i = 0; i < nic_dev->num_qp_irq; i++)
		hinic3_free_irq(nic_dev->hwdev,
				nic_dev->qps_irq_info[i].irq_id);

	kfree(nic_dev->qps_irq_info);
}

static int hinic3_alloc_txrxq_resources(struct net_device *netdev,
					struct hinic3_dyna_txrxq_params *q_params)
{
	u32 size;
	int err;

	size = sizeof(*q_params->txqs_res) * q_params->num_qps;
	q_params->txqs_res = kzalloc(size, GFP_KERNEL);
	if (!q_params->txqs_res)
		return -ENOMEM;

	size = sizeof(*q_params->rxqs_res) * q_params->num_qps;
	q_params->rxqs_res = kzalloc(size, GFP_KERNEL);
	if (!q_params->rxqs_res) {
		err = -ENOMEM;
		goto err_alloc_rxqs_res_arr;
	}

	size = sizeof(*q_params->irq_cfg) * q_params->num_qps;
	q_params->irq_cfg = kzalloc(size, GFP_KERNEL);
	if (!q_params->irq_cfg) {
		err = -ENOMEM;
		goto err_alloc_irq_cfg;
	}

	err = hinic3_alloc_txqs_res(netdev, q_params->num_qps,
				    q_params->sq_depth, q_params->txqs_res);
	if (err) {
		netdev_err(netdev, "Failed to alloc txqs resource\n");
		goto err_alloc_txqs_res;
	}

	err = hinic3_alloc_rxqs_res(netdev, q_params->num_qps,
				    q_params->rq_depth, q_params->rxqs_res);
	if (err) {
		netdev_err(netdev, "Failed to alloc rxqs resource\n");
		goto err_alloc_rxqs_res;
	}

	return 0;

err_alloc_rxqs_res:
	hinic3_free_txqs_res(netdev, q_params->num_qps, q_params->sq_depth,
			     q_params->txqs_res);

err_alloc_txqs_res:
	kfree(q_params->irq_cfg);
	q_params->irq_cfg = NULL;

err_alloc_irq_cfg:
	kfree(q_params->rxqs_res);
	q_params->rxqs_res = NULL;

err_alloc_rxqs_res_arr:
	kfree(q_params->txqs_res);
	q_params->txqs_res = NULL;

	return err;
}

static void hinic3_free_txrxq_resources(struct net_device *netdev,
					struct hinic3_dyna_txrxq_params *q_params)
{
	hinic3_free_rxqs_res(netdev, q_params->num_qps, q_params->rq_depth,
			     q_params->rxqs_res);
	hinic3_free_txqs_res(netdev, q_params->num_qps, q_params->sq_depth,
			     q_params->txqs_res);

	kfree(q_params->irq_cfg);
	q_params->irq_cfg = NULL;

	kfree(q_params->rxqs_res);
	q_params->rxqs_res = NULL;

	kfree(q_params->txqs_res);
	q_params->txqs_res = NULL;
}

static int hinic3_configure_txrxqs(struct net_device *netdev,
				   struct hinic3_dyna_txrxq_params *q_params)
{
	int err;

	err = hinic3_configure_txqs(netdev, q_params->num_qps,
				    q_params->sq_depth, q_params->txqs_res);
	if (err) {
		netdev_err(netdev, "Failed to configure txqs\n");
		return err;
	}

	err = hinic3_configure_rxqs(netdev, q_params->num_qps,
				    q_params->rq_depth, q_params->rxqs_res);
	if (err) {
		netdev_err(netdev, "Failed to configure rxqs\n");
		return err;
	}

	return 0;
}

static int hinic3_configure(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	int err;

	err = hinic3_set_port_mtu(netdev, (u16)netdev->mtu);
	if (err) {
		netdev_err(netdev, "Failed to set mtu\n");
		return err;
	}

	/* Ensure DCB is disabled */
	hinic3_sync_dcb_state(nic_dev->hwdev, 1, 0);

	if (test_bit(HINIC3_RSS_ENABLE, &nic_dev->flags)) {
		err = hinic3_rss_init(netdev);
		if (err) {
			netdev_err(netdev, "Failed to init rss\n");
			return -EFAULT;
		}
	}

	return 0;
}

static void hinic3_remove_configure(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);

	if (test_bit(HINIC3_RSS_ENABLE, &nic_dev->flags))
		hinic3_rss_deinit(netdev);
}

static int hinic3_alloc_channel_resources(struct net_device *netdev,
					  struct hinic3_dyna_qp_params *qp_params,
					  struct hinic3_dyna_txrxq_params *trxq_params)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	int err;

	qp_params->num_qps = trxq_params->num_qps;
	qp_params->sq_depth = trxq_params->sq_depth;
	qp_params->rq_depth = trxq_params->rq_depth;

	err = hinic3_alloc_qps(nic_dev, qp_params);
	if (err) {
		netdev_err(netdev, "Failed to alloc qps\n");
		return err;
	}

	err = hinic3_alloc_txrxq_resources(netdev, trxq_params);
	if (err) {
		netdev_err(netdev, "Failed to alloc txrxq resources\n");
		hinic3_free_qps(nic_dev, qp_params);
		return err;
	}

	return 0;
}

static void hinic3_free_channel_resources(struct net_device *netdev,
					  struct hinic3_dyna_qp_params *qp_params,
					  struct hinic3_dyna_txrxq_params *trxq_params)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);

	hinic3_free_txrxq_resources(netdev, trxq_params);
	hinic3_free_qps(nic_dev, qp_params);
}

static int hinic3_open_channel(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	int err;

	err = hinic3_init_qp_ctxts(nic_dev);
	if (err) {
		netdev_err(netdev, "Failed to init qps\n");
		return err;
	}

	err = hinic3_configure_txrxqs(netdev, &nic_dev->q_params);
	if (err) {
		netdev_err(netdev, "Failed to configure txrxqs\n");
		goto err_cfg_txrxqs;
	}

	err = hinic3_qps_irq_init(netdev);
	if (err) {
		netdev_err(netdev, "Failed to init txrxq irq\n");
		goto err_cfg_txrxqs;
	}

	err = hinic3_configure(netdev);
	if (err) {
		netdev_err(netdev, "Failed to init txrxq irq\n");
		goto err_configure;
	}

	return 0;

err_configure:
	hinic3_qps_irq_deinit(netdev);

err_cfg_txrxqs:
	hinic3_free_qp_ctxts(nic_dev);

	return err;
}

static void hinic3_close_channel(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);

	hinic3_remove_configure(netdev);
	hinic3_qps_irq_deinit(netdev);
	hinic3_free_qp_ctxts(nic_dev);
}

static int hinic3_vport_up(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	bool link_status_up;
	u16 glb_func_id;
	int err;

	glb_func_id = hinic3_global_func_id(nic_dev->hwdev);
	err = hinic3_set_vport_enable(nic_dev->hwdev, glb_func_id, true);
	if (err) {
		netdev_err(netdev, "Failed to enable vport\n");
		goto err_vport_enable;
	}

	netif_set_real_num_tx_queues(netdev, nic_dev->q_params.num_qps);
	netif_set_real_num_rx_queues(netdev, nic_dev->q_params.num_qps);
	netif_tx_wake_all_queues(netdev);

	err = hinic3_get_link_status(nic_dev->hwdev, &link_status_up);
	if (!err && link_status_up)
		netif_carrier_on(netdev);

	return 0;

err_vport_enable:
	hinic3_flush_qps_res(nic_dev->hwdev);
	/* wait to guarantee that no packets will be sent to host */
	msleep(100);

	return err;
}

static void hinic3_vport_down(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	u16 glb_func_id;

	netif_carrier_off(netdev);
	netif_tx_disable(netdev);

	glb_func_id = hinic3_global_func_id(nic_dev->hwdev);
	hinic3_set_vport_enable(nic_dev->hwdev, glb_func_id, false);

	hinic3_flush_txqs(netdev);
	/* wait to guarantee that no packets will be sent to host */
	msleep(100);
	hinic3_flush_qps_res(nic_dev->hwdev);
}

static int hinic3_open(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_dyna_qp_params qp_params;
	int err;

	if (test_bit(HINIC3_INTF_UP, &nic_dev->flags)) {
		netdev_dbg(netdev, "Netdev already open, do nothing\n");
		return 0;
	}

	err = hinic3_init_nicio_res(nic_dev);
	if (err) {
		netdev_err(netdev, "Failed to init nicio resources\n");
		return err;
	}

	err = hinic3_setup_num_qps(netdev);
	if (err) {
		netdev_err(netdev, "Failed to setup num_qps\n");
		goto err_setup_qps;
	}

	err = hinic3_alloc_channel_resources(netdev, &qp_params,
					     &nic_dev->q_params);
	if (err)
		goto err_alloc_channel_res;

	hinic3_init_qps(nic_dev, &qp_params);

	err = hinic3_open_channel(netdev);
	if (err)
		goto err_open_channel;

	err = hinic3_vport_up(netdev);
	if (err)
		goto err_vport_up;

	set_bit(HINIC3_INTF_UP, &nic_dev->flags);

	return 0;

err_vport_up:
	hinic3_close_channel(netdev);

err_open_channel:
	hinic3_deinit_qps(nic_dev, &qp_params);
	hinic3_free_channel_resources(netdev, &qp_params, &nic_dev->q_params);

err_alloc_channel_res:
	hinic3_destroy_num_qps(netdev);

err_setup_qps:
	hinic3_deinit_nicio_res(nic_dev);

	return err;
}

static int hinic3_close(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_dyna_qp_params qp_params;

	if (!test_and_clear_bit(HINIC3_INTF_UP, &nic_dev->flags)) {
		netdev_dbg(netdev, "Netdev already close, do nothing\n");
		return 0;
	}

	if (test_and_clear_bit(HINIC3_CHANGE_RES_INVALID, &nic_dev->flags))
		goto out;

	hinic3_vport_down(netdev);
	hinic3_close_channel(netdev);
	hinic3_deinit_qps(nic_dev, &qp_params);
	hinic3_free_channel_resources(netdev, &qp_params, &nic_dev->q_params);

out:
	hinic3_deinit_nicio_res(nic_dev);
	hinic3_destroy_num_qps(netdev);

	return 0;
}

static int hinic3_change_mtu(struct net_device *netdev, int new_mtu)
{
	int err;

	err = hinic3_set_port_mtu(netdev, new_mtu);
	if (err) {
		netdev_err(netdev, "Failed to change port mtu to %d\n", new_mtu);
	} else {
		netdev_dbg(netdev, "Change mtu from %u to %d\n", netdev->mtu, new_mtu);
		WRITE_ONCE(netdev->mtu, new_mtu);
	}

	return err;
}

static int hinic3_set_mac_addr(struct net_device *netdev, void *addr)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct sockaddr *saddr = addr;
	int err;

	if (!is_valid_ether_addr(saddr->sa_data))
		return -EADDRNOTAVAIL;

	if (ether_addr_equal(netdev->dev_addr, saddr->sa_data))
		return 0;

	err = hinic3_update_mac(nic_dev->hwdev, netdev->dev_addr,
				saddr->sa_data, 0,
				hinic3_global_func_id(nic_dev->hwdev));

	if (err)
		return err;

	eth_hw_addr_set(netdev, saddr->sa_data);

	return 0;
}

static const struct net_device_ops hinic3_netdev_ops = {
	.ndo_open             = hinic3_open,
	.ndo_stop             = hinic3_close,
	.ndo_change_mtu       = hinic3_change_mtu,
	.ndo_set_mac_address  = hinic3_set_mac_addr,
	.ndo_start_xmit       = hinic3_xmit_frame,
};

void hinic3_set_netdev_ops(struct net_device *netdev)
{
	netdev->netdev_ops = &hinic3_netdev_ops;
}
