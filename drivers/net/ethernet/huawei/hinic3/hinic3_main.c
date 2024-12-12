// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/netdevice.h>
#include <linux/etherdevice.h>

#include "hinic3_common.h"
#include "hinic3_hwdev.h"
#include "hinic3_nic_cfg.h"
#include "hinic3_tx.h"
#include "hinic3_rx.h"
#include "hinic3_lld.h"
#include "hinic3_nic_dev.h"
#include "hinic3_nic_io.h"
#include "hinic3_hw_comm.h"
#include "hinic3_rss.h"
#include "hinic3_hwif.h"

#define HINIC3_NIC_DRV_DESC  "Intelligent Network Interface Card Driver"

#define HINIC3_RX_BUFF_LEN           2048
#define HINIC3_RX_BUFF_NUM_PER_PAGE  2
#define HINIC3_LRO_REPLENISH_THLD    256
#define HINIC3_NIC_DEV_WQ_NAME       "hinic3_nic_dev_wq"

#define HINIC3_SQ_DEPTH              1024
#define HINIC3_RQ_DEPTH              1024

#define HINIC3_DEAULT_TXRX_MSIX_PENDING_LIMIT       2
#define HINIC3_DEAULT_TXRX_MSIX_COALESC_TIMER_CFG   25
#define HINIC3_DEAULT_TXRX_MSIX_RESEND_TIMER_CFG    7

static void init_intr_coal_param(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_intr_coal_info *info;
	u16 i;

	for (i = 0; i < nic_dev->max_qps; i++) {
		info = &nic_dev->intr_coalesce[i];
		info->pending_limt = HINIC3_DEAULT_TXRX_MSIX_PENDING_LIMIT;
		info->coalesce_timer_cfg = HINIC3_DEAULT_TXRX_MSIX_COALESC_TIMER_CFG;
		info->resend_timer_cfg = HINIC3_DEAULT_TXRX_MSIX_RESEND_TIMER_CFG;
	}
}

static int hinic3_init_intr_coalesce(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_hwdev *hwdev = nic_dev->hwdev;
	u64 size;

	size = sizeof(*nic_dev->intr_coalesce) * nic_dev->max_qps;
	if (!size) {
		dev_err(hwdev->dev, "Cannot allocate zero size intr coalesce\n");
		return -EINVAL;
	}
	nic_dev->intr_coalesce = kzalloc(size, GFP_KERNEL);
	if (!nic_dev->intr_coalesce)
		return -ENOMEM;

	init_intr_coal_param(netdev);
	return 0;
}

static void hinic3_free_intr_coalesce(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);

	kfree(nic_dev->intr_coalesce);
}

static int hinic3_alloc_txrxqs(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_hwdev *hwdev = nic_dev->hwdev;
	int err;

	err = hinic3_alloc_txqs(netdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to alloc txqs\n");
		return err;
	}

	err = hinic3_alloc_rxqs(netdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to alloc rxqs\n");
		goto err_alloc_rxqs;
	}

	err = hinic3_init_intr_coalesce(netdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to init_intr_coalesce\n");
		goto err_init_intr;
	}

	return 0;

err_init_intr:
	hinic3_free_rxqs(netdev);

err_alloc_rxqs:
	hinic3_free_txqs(netdev);

	return err;
}

static void hinic3_free_txrxqs(struct net_device *netdev)
{
	hinic3_free_intr_coalesce(netdev);
	hinic3_free_rxqs(netdev);
	hinic3_free_txqs(netdev);
}

static int hinic3_init_nic_dev(struct net_device *netdev,
			       struct hinic3_hwdev *hwdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct pci_dev *pdev = hwdev->pdev;
	u32 page_num;

	nic_dev->netdev = netdev;
	SET_NETDEV_DEV(netdev, &pdev->dev);
	nic_dev->hwdev = hwdev;
	nic_dev->pdev = pdev;

	nic_dev->rx_buff_len = HINIC3_RX_BUFF_LEN;
	nic_dev->dma_rx_buff_size = HINIC3_RX_BUFF_NUM_PER_PAGE * nic_dev->rx_buff_len;
	page_num = nic_dev->dma_rx_buff_size / HINIC3_MIN_PAGE_SIZE;
	nic_dev->page_order = page_num > 0 ? ilog2(page_num) : 0;
	nic_dev->lro_replenish_thld = HINIC3_LRO_REPLENISH_THLD;
	nic_dev->nic_cap = hwdev->cfg_mgmt->svc_cap.nic_cap;

	return 0;
}

static int hinic3_sw_init(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_hwdev *hwdev = nic_dev->hwdev;
	int err;

	nic_dev->q_params.sq_depth = HINIC3_SQ_DEPTH;
	nic_dev->q_params.rq_depth = HINIC3_RQ_DEPTH;

	hinic3_try_to_enable_rss(netdev);

	eth_hw_addr_random(netdev);
	err = hinic3_set_mac(hwdev, netdev->dev_addr, 0,
			     hinic3_global_func_id(hwdev));
	if (err) {
		dev_err(hwdev->dev, "Failed to set default MAC\n");
		goto err_out;
	}

	err = hinic3_alloc_txrxqs(netdev);
	if (err) {
		dev_err(hwdev->dev, "Failed to alloc qps\n");
		goto err_alloc_qps;
	}

	return 0;

err_alloc_qps:
	hinic3_del_mac(hwdev, netdev->dev_addr, 0, hinic3_global_func_id(hwdev));

err_out:
	hinic3_clear_rss_config(netdev);

	return err;
}

static void hinic3_sw_deinit(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);

	hinic3_free_txrxqs(netdev);
	hinic3_del_mac(nic_dev->hwdev, netdev->dev_addr, 0,
		       hinic3_global_func_id(nic_dev->hwdev));
	hinic3_clear_rss_config(netdev);
}

static void hinic3_assign_netdev_ops(struct net_device *netdev)
{
	hinic3_set_netdev_ops(netdev);
}

static void netdev_feature_init(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	netdev_features_t cso_fts = 0;
	netdev_features_t tso_fts = 0;
	netdev_features_t dft_fts;

	dft_fts = NETIF_F_SG | NETIF_F_HIGHDMA;
	if (hinic3_test_support(nic_dev, NIC_F_CSUM))
		cso_fts |= NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM | NETIF_F_RXCSUM;
	if (hinic3_test_support(nic_dev, NIC_F_SCTP_CRC))
		cso_fts |= NETIF_F_SCTP_CRC;
	if (hinic3_test_support(nic_dev, NIC_F_TSO))
		tso_fts |= NETIF_F_TSO | NETIF_F_TSO6;

	netdev->features |= dft_fts | cso_fts | tso_fts;
}

static int hinic3_set_default_hw_feature(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_hwdev *hwdev = nic_dev->hwdev;
	int err;

	err = hinic3_set_nic_feature_to_hw(nic_dev);
	if (err) {
		dev_err(hwdev->dev, "Failed to set nic features\n");
		return err;
	}

	return 0;
}

static void hinic3_link_status_change(struct net_device *netdev, bool link_status_up)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);

	if (!HINIC3_CHANNEL_RES_VALID(nic_dev))
		return;

	if (link_status_up) {
		if (netif_carrier_ok(netdev))
			return;

		nic_dev->link_status_up = true;
		netif_carrier_on(netdev);
		netdev_dbg(netdev, "Link is up\n");
	} else {
		if (!netif_carrier_ok(netdev))
			return;

		nic_dev->link_status_up = false;
		netif_carrier_off(netdev);
		netdev_dbg(netdev, "Link is down\n");
	}
}

static void nic_event(struct auxiliary_device *adev, struct hinic3_event_info *event)
{
	struct hinic3_nic_dev *nic_dev = dev_get_drvdata(&adev->dev);
	struct net_device *netdev;

	netdev = nic_dev->netdev;

	switch (HINIC3_SRV_EVENT_TYPE(event->service, event->type)) {
	case HINIC3_SRV_EVENT_TYPE(EVENT_SRV_NIC, EVENT_NIC_LINK_UP):
		hinic3_link_status_change(netdev, true);
		break;
	case HINIC3_SRV_EVENT_TYPE(EVENT_SRV_NIC, EVENT_NIC_LINK_DOWN):
		hinic3_link_status_change(netdev, false);
		break;
	default:
		break;
	}
}

static int nic_probe(struct auxiliary_device *adev, const struct auxiliary_device_id *id)
{
	struct hinic3_hwdev *hwdev = adev_get_hwdev(adev);
	struct pci_dev *pdev = hwdev->pdev;
	struct hinic3_nic_dev *nic_dev;
	struct net_device *netdev;
	u16 max_qps, glb_func_id;
	int err;

	if (!hinic3_support_nic(hwdev)) {
		dev_dbg(&adev->dev, "Hw doesn't support nic\n");
		return 0;
	}

	err = hinic3_adev_event_register(adev, nic_event);
	if (err) {
		err = -EINVAL;
		goto err_out;
	}

	glb_func_id = hinic3_global_func_id(hwdev);
	err = hinic3_func_reset(hwdev, glb_func_id, RESET_TYPE_NIC);
	if (err) {
		dev_err(&adev->dev, "Failed to reset function\n");
		goto err_out;
	}

	max_qps = hinic3_func_max_qnum(hwdev);
	netdev = alloc_etherdev_mq(sizeof(*nic_dev), max_qps);
	if (!netdev) {
		dev_err(&adev->dev, "Failed to allocate netdev\n");
		err = -ENOMEM;
		goto err_out;
	}

	nic_dev = netdev_priv(netdev);
	err = hinic3_init_nic_dev(netdev, hwdev);
	if (err)
		goto err_undo_alloc_etherdev;

	err = hinic3_init_nic_io(nic_dev);
	if (err)
		goto err_undo_alloc_etherdev;

	err = hinic3_sw_init(netdev);
	if (err)
		goto err_sw_init;

	hinic3_assign_netdev_ops(netdev);

	netdev_feature_init(netdev);
	err = hinic3_set_default_hw_feature(netdev);
	if (err)
		goto err_set_features;

	err = register_netdev(netdev);
	if (err) {
		err = -ENOMEM;
		goto err_netdev;
	}

	netif_carrier_off(netdev);

	dev_set_drvdata(&adev->dev, nic_dev);

	return 0;

err_netdev:
	hinic3_update_nic_feature(nic_dev, 0);
	hinic3_set_nic_feature_to_hw(nic_dev);

err_set_features:
	hinic3_sw_deinit(netdev);

err_sw_init:
	hinic3_free_nic_io(nic_dev);

err_undo_alloc_etherdev:
	free_netdev(netdev);

err_out:
	dev_err(&pdev->dev, "NIC service probe failed\n");

	return err;
}

static void nic_remove(struct auxiliary_device *adev)
{
	struct hinic3_nic_dev *nic_dev = dev_get_drvdata(&adev->dev);
	struct net_device *netdev;

	if (!hinic3_support_nic(nic_dev->hwdev))
		return;

	netdev = nic_dev->netdev;
	unregister_netdev(netdev);

	hinic3_update_nic_feature(nic_dev, 0);
	hinic3_set_nic_feature_to_hw(nic_dev);
	hinic3_sw_deinit(netdev);

	hinic3_free_nic_io(nic_dev);

	free_netdev(netdev);
}

static const struct auxiliary_device_id nic_id_table[] = {
	{
		.name = HINIC3_NIC_DRV_NAME ".nic",
	},
	{},
};

static struct auxiliary_driver nic_driver = {
	.probe    = nic_probe,
	.remove   = nic_remove,
	.suspend  = NULL,
	.resume   = NULL,
	.name     = "nic",
	.id_table = nic_id_table,
};

static __init int hinic3_nic_lld_init(void)
{
	int err;

	pr_info("%s: %s\n", HINIC3_NIC_DRV_NAME, HINIC3_NIC_DRV_DESC);

	err = hinic3_lld_init();
	if (err)
		return err;

	err = auxiliary_driver_register(&nic_driver);
	if (err) {
		hinic3_lld_exit();
		return err;
	}

	return 0;
}

static __exit void hinic3_nic_lld_exit(void)
{
	auxiliary_driver_unregister(&nic_driver);

	hinic3_lld_exit();
}

module_init(hinic3_nic_lld_init);
module_exit(hinic3_nic_lld_exit);

MODULE_AUTHOR("Huawei Technologies CO., Ltd");
MODULE_DESCRIPTION(HINIC3_NIC_DRV_DESC);
MODULE_LICENSE("GPL");
