// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/if_vlan.h>
#include <linux/auxiliary_bus.h>

#include "hinic3_hw_cfg.h"
#include "hinic3_hw_comm.h"
#include "hinic3_lld.h"
#include "hinic3_hwdev.h"
#include "hinic3_nic_dev.h"
#include "hinic3_nic_cfg.h"
#include "hinic3_nic_io.h"
#include "hinic3_tx.h"
#include "hinic3_rx.h"
#include "hinic3_rss.h"

#define HINIC3_NIC_DRV_DESC  "Intelligent Network Interface Card Driver"

#define DEFAULT_MSG_ENABLE           (NETIF_MSG_DRV | NETIF_MSG_LINK)
#define HINIC3_RX_BUFF_LEN           2048
#define HINIC3_RX_BUFF_NUM_PER_PAGE  2
#define HINIC3_LRO_REPLENISH_THLD    256
#define HINIC3_NIC_DEV_WQ_NAME       "hinic3_nic_dev_wq"

#define HINIC3_SQ_DEPTH              1024
#define HINIC3_RQ_DEPTH              1024

#define HINIC3_DEAULT_TXRX_MSIX_PENDING_LIMIT       2
#define HINIC3_DEAULT_TXRX_MSIX_COALESC_TIMER_CFG   25
#define HINIC3_DEAULT_TXRX_MSIX_RESEND_TIMER_CFG    7

#define HINIC3_RX_RATE_LOW            200000
#define HINIC3_RX_COAL_TIME_LOW       25
#define HINIC3_RX_PENDING_LIMIT_LOW   2

#define HINIC3_RX_RATE_HIGH           700000
#define HINIC3_RX_COAL_TIME_HIGH      225
#define HINIC3_RX_PENDING_LIMIT_HIGH  8

#define HINIC3_WATCHDOG_TIMEOUT      5

#define HINIC3_MAX_VLAN_DEPTH_OFFLOAD_SUPPORT  1
#define HINIC3_VLAN_CLEAR_OFFLOAD \
	(NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM | \
	 NETIF_F_SCTP_CRC | NETIF_F_RXCSUM | NETIF_F_ALL_TSO)

static int hinic3_netdev_event(struct notifier_block *notifier, unsigned long event, void *ptr);

/* used for netdev notifier register/unregister */
static DEFINE_MUTEX(hinic3_netdev_notifiers_mutex);
static int hinic3_netdev_notifiers_ref_cnt;
static struct notifier_block hinic3_netdev_notifier = {
	.notifier_call = hinic3_netdev_event,
};

static u16 hinic3_get_vlan_depth(struct net_device *netdev)
{
	u16 vlan_depth = 0;

#if defined(CONFIG_VLAN_8021Q) || defined(CONFIG_VLAN_8021Q_MODULE)
	while (is_vlan_dev(netdev)) {
		netdev = vlan_dev_priv(netdev)->real_dev;
		vlan_depth++;
	}
#endif
	return vlan_depth;
}

static int hinic3_netdev_event(struct notifier_block *notifier, unsigned long event, void *ptr)
{
	struct net_device *ndev = netdev_notifier_info_to_dev(ptr);
	u16 vlan_depth;

	if (!is_vlan_dev(ndev))
		return NOTIFY_DONE;

	dev_hold(ndev);

	switch (event) {
	case NETDEV_REGISTER:
		vlan_depth = hinic3_get_vlan_depth(ndev);
		if (vlan_depth == HINIC3_MAX_VLAN_DEPTH_OFFLOAD_SUPPORT) {
			ndev->vlan_features &= (~HINIC3_VLAN_CLEAR_OFFLOAD);
		} else if (vlan_depth > HINIC3_MAX_VLAN_DEPTH_OFFLOAD_SUPPORT) {
			ndev->hw_features &= (~HINIC3_VLAN_CLEAR_OFFLOAD);
			ndev->features &= (~HINIC3_VLAN_CLEAR_OFFLOAD);
		}

		break;

	default:
		break;
	};

	dev_put(ndev);

	return NOTIFY_DONE;
}

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

		info->pkt_rate_high = HINIC3_RX_RATE_HIGH;
		info->rx_usecs_high = HINIC3_RX_COAL_TIME_HIGH;
		info->rx_pending_limt_high = HINIC3_RX_PENDING_LIMIT_HIGH;

		info->pkt_rate_low = HINIC3_RX_RATE_LOW;
		info->rx_usecs_low = HINIC3_RX_COAL_TIME_LOW;
		info->rx_pending_limt_low = HINIC3_RX_PENDING_LIMIT_LOW;
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

	if (test_bit(HINIC3_INTR_ADAPT, &nic_dev->flags))
		nic_dev->adaptive_rx_coal = 1;
	else
		nic_dev->adaptive_rx_coal = 0;

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

static void hinic3_fault_event_report(struct hinic3_hwdev *hwdev, u16 src, u16 level)
{
	dev_info(hwdev->dev, "Fault event report, src: %u, level: %u\n", src, level);
}

static void hinic3_periodic_work_handler(struct work_struct *work)
{
	struct delayed_work *delay = to_delayed_work(work);
	struct hinic3_nic_dev *nic_dev;

	nic_dev = container_of(delay, struct hinic3_nic_dev, periodic_work);
	if (test_and_clear_bit(EVENT_WORK_TX_TIMEOUT, &nic_dev->event_flag))
		hinic3_fault_event_report(nic_dev->hwdev, HINIC3_FAULT_SRC_TX_TIMEOUT,
					  FAULT_LEVEL_SERIOUS_FLR);

	queue_delayed_work(nic_dev->workq, &nic_dev->periodic_work, HZ);
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

	nic_dev->msg_enable = DEFAULT_MSG_ENABLE;
	nic_dev->rx_buff_len = HINIC3_RX_BUFF_LEN;
	nic_dev->dma_rx_buff_size = HINIC3_RX_BUFF_NUM_PER_PAGE * nic_dev->rx_buff_len;
	page_num = nic_dev->dma_rx_buff_size / HINIC3_MIN_PAGE_SIZE;
	nic_dev->page_order = page_num > 0 ? ilog2(page_num) : 0;
	nic_dev->lro_replenish_thld = HINIC3_LRO_REPLENISH_THLD;
	nic_dev->vlan_bitmap = kzalloc(VLAN_BITMAP_SIZE(nic_dev), GFP_KERNEL);
	if (!nic_dev->vlan_bitmap)
		return -ENOMEM;
	set_bit(HINIC3_INTR_ADAPT, &nic_dev->flags);
	nic_dev->nic_cap = hwdev->cfg_mgmt->svc_cap.nic_cap;

	nic_dev->workq = create_singlethread_workqueue(HINIC3_NIC_DEV_WQ_NAME);
	if (!nic_dev->workq) {
		dev_err(hwdev->dev, "Failed to initialize nic workqueue\n");
		kfree(nic_dev->vlan_bitmap);
		return -ENOMEM;
	}

	INIT_DELAYED_WORK(&nic_dev->periodic_work, hinic3_periodic_work_handler);

	INIT_LIST_HEAD(&nic_dev->uc_filter_list);
	INIT_LIST_HEAD(&nic_dev->mc_filter_list);
	INIT_WORK(&nic_dev->rx_mode_work, hinic3_set_rx_mode_work);

	return 0;
}

static void hinic3_free_nic_dev(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);

	destroy_workqueue(nic_dev->workq);
	kfree(nic_dev->vlan_bitmap);
}

static int hinic3_sw_init(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_hwdev *hwdev = nic_dev->hwdev;
	u8 mac_addr[ETH_ALEN];
	int err;

	sema_init(&nic_dev->port_state_sem, 1);

	nic_dev->q_params.sq_depth = HINIC3_SQ_DEPTH;
	nic_dev->q_params.rq_depth = HINIC3_RQ_DEPTH;

	hinic3_try_to_enable_rss(netdev);

	if (HINIC3_IS_VF(hwdev)) {
		eth_hw_addr_random(netdev);
	} else {
		err = hinic3_get_default_mac(hwdev, mac_addr);
		if (err) {
			dev_err(hwdev->dev, "Failed to get MAC address\n");
			goto err_out;
		}
		eth_hw_addr_set(netdev, mac_addr);
	}

	err = hinic3_set_mac(hwdev, netdev->dev_addr, 0,
			     hinic3_global_func_id(hwdev));
	/* Failure to set MAC is not a fatal error for VF since its MAC may have
	 * already been set by PF
	 */
	if (err && err != HINIC3_PF_SET_VF_ALREADY) {
		dev_err(hwdev->dev, "Failed to set default MAC\n");
		goto err_out;
	}

	netdev->min_mtu = HINIC3_MIN_MTU_SIZE;
	netdev->max_mtu = HINIC3_MAX_JUMBO_FRAME_SIZE;

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

	hinic3_clean_mac_list_filter(netdev);

	hinic3_del_mac(nic_dev->hwdev, netdev->dev_addr, 0,
		       hinic3_global_func_id(nic_dev->hwdev));

	hinic3_clear_rss_config(netdev);
}

static void hinic3_assign_netdev_ops(struct net_device *netdev)
{
	hinic3_set_netdev_ops(netdev);
	hinic3_set_ethtool_ops(netdev);

	netdev->watchdog_timeo = HINIC3_WATCHDOG_TIMEOUT * HZ;
}

static void netdev_feature_init(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	netdev_features_t hw_features = 0;
	netdev_features_t vlan_fts = 0;
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

	if (hinic3_test_support(nic_dev, NIC_F_RX_VLAN_STRIP | NIC_F_TX_VLAN_INSERT))
		vlan_fts |= NETIF_F_HW_VLAN_CTAG_TX | NETIF_F_HW_VLAN_CTAG_RX;

	if (hinic3_test_support(nic_dev, NIC_F_RX_VLAN_FILTER))
		vlan_fts |= NETIF_F_HW_VLAN_CTAG_FILTER;

	if (hinic3_test_support(nic_dev, NIC_F_VXLAN_OFFLOAD))
		tso_fts |= NETIF_F_GSO_UDP_TUNNEL | NETIF_F_GSO_UDP_TUNNEL_CSUM;

	/* LRO is disabled by default, only set hw features */
	if (hinic3_test_support(nic_dev, NIC_F_LRO))
		hw_features |= NETIF_F_LRO;

	netdev->features |= dft_fts | cso_fts | tso_fts | vlan_fts;
	netdev->vlan_features |= dft_fts | cso_fts | tso_fts;
	hw_features |= netdev->hw_features | netdev->features;
	netdev->hw_features = hw_features;
	netdev->priv_flags |= IFF_UNICAST_FLT;
	netdev->hw_enc_features |= dft_fts;
	if (hinic3_test_support(nic_dev, NIC_F_VXLAN_OFFLOAD))
		netdev->hw_enc_features |= cso_fts | tso_fts | NETIF_F_TSO_ECN;
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

	err = hinic3_set_hw_features(netdev);
	if (err) {
		hinic3_update_nic_feature(nic_dev, 0);
		hinic3_set_nic_feature_to_hw(nic_dev);
		return err;
	}

	return 0;
}

static void hinic3_register_notifier(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	int err;

	mutex_lock(&hinic3_netdev_notifiers_mutex);
	hinic3_netdev_notifiers_ref_cnt++;
	if (hinic3_netdev_notifiers_ref_cnt == 1) {
		err = register_netdevice_notifier(&hinic3_netdev_notifier);
		if (err) {
			dev_dbg(nic_dev->hwdev->dev,
				"Register netdevice notifier failed, err: %d\n",
				err);
			hinic3_netdev_notifiers_ref_cnt--;
		}
	}
	mutex_unlock(&hinic3_netdev_notifiers_mutex);
}

static void hinic3_unregister_notifier(void)
{
	mutex_lock(&hinic3_netdev_notifiers_mutex);
	if (hinic3_netdev_notifiers_ref_cnt == 1)
		unregister_netdevice_notifier(&hinic3_netdev_notifier);

	if (hinic3_netdev_notifiers_ref_cnt)
		hinic3_netdev_notifiers_ref_cnt--;
	mutex_unlock(&hinic3_netdev_notifiers_mutex);
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

static void hinic3_port_module_event_handler(struct net_device *netdev,
					     struct hinic3_event_info *event)
{
	const char *g_hinic3_module_link_err[LINK_ERR_NUM] = { "Unrecognized module" };
	struct hinic3_port_module_event *module_event;
	enum port_module_event_type type;
	enum link_err_type err_type;

	module_event = (struct hinic3_port_module_event *)event->event_data;
	type = module_event->type;
	err_type = module_event->err_type;

	switch (type) {
	case HINIC3_PORT_MODULE_CABLE_PLUGGED:
	case HINIC3_PORT_MODULE_CABLE_UNPLUGGED:
		netdev_info(netdev, "Port module event: Cable %s\n",
			    type == HINIC3_PORT_MODULE_CABLE_PLUGGED ? "plugged" : "unplugged");
		break;
	case HINIC3_PORT_MODULE_LINK_ERR:
		if (err_type >= LINK_ERR_NUM) {
			netdev_info(netdev, "Link failed, Unknown error type: 0x%x\n", err_type);
		} else {
			netdev_info(netdev, "Link failed, error type: 0x%x: %s\n",
				    err_type, g_hinic3_module_link_err[err_type]);
		}
		break;
	default:
		netdev_err(netdev, "Unknown port module type %d\n", type);
		break;
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
	case HINIC3_SRV_EVENT_TYPE(EVENT_SRV_NIC, EVENT_NIC_PORT_MODULE_EVENT):
		hinic3_port_module_event_handler(netdev, event);
		break;
	case HINIC3_SRV_EVENT_TYPE(EVENT_SRV_NIC, EVENT_NIC_LINK_DOWN):
	case HINIC3_SRV_EVENT_TYPE(EVENT_SRV_COMM, EVENT_COMM_FAULT):
	case HINIC3_SRV_EVENT_TYPE(EVENT_SRV_COMM, EVENT_COMM_PCIE_LINK_DOWN):
	case HINIC3_SRV_EVENT_TYPE(EVENT_SRV_COMM, EVENT_COMM_HEART_LOST):
	case HINIC3_SRV_EVENT_TYPE(EVENT_SRV_COMM, EVENT_COMM_MGMT_WATCHDOG):
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
	err = hinic3_func_reset(hwdev, glb_func_id, HINIC3_NIC_RES);
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
		goto err_setup_dev;

	err = hinic3_init_nic_io(nic_dev);
	if (err)
		goto err_init_nic_io;

	err = hinic3_sw_init(netdev);
	if (err)
		goto err_sw_init;

	hinic3_assign_netdev_ops(netdev);

	netdev_feature_init(netdev);
	err = hinic3_set_default_hw_feature(netdev);
	if (err)
		goto err_set_features;

	hinic3_register_notifier(netdev);

	err = register_netdev(netdev);
	if (err) {
		err = -ENOMEM;
		goto err_netdev;
	}

	queue_delayed_work(nic_dev->workq, &nic_dev->periodic_work, HZ);
	netif_carrier_off(netdev);

	dev_set_drvdata(&adev->dev, nic_dev);

	return 0;

err_netdev:
	hinic3_unregister_notifier();
	hinic3_update_nic_feature(nic_dev, 0);
	hinic3_set_nic_feature_to_hw(nic_dev);

err_set_features:
	hinic3_sw_deinit(netdev);

err_sw_init:
	hinic3_free_nic_io(nic_dev);

err_init_nic_io:
	hinic3_free_nic_dev(netdev);

err_setup_dev:
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
	hinic3_unregister_notifier();

	cancel_delayed_work_sync(&nic_dev->periodic_work);
	cancel_work_sync(&nic_dev->rx_mode_work);
	destroy_workqueue(nic_dev->workq);

	hinic3_update_nic_feature(nic_dev, 0);
	hinic3_set_nic_feature_to_hw(nic_dev);
	hinic3_sw_deinit(netdev);

	hinic3_free_nic_io(nic_dev);

	kfree(nic_dev->vlan_bitmap);
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
