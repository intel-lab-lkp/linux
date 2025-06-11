// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2015 - 2025 Beijing WangXun Technology Co., Ltd. */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/string.h>
#include <linux/etherdevice.h>

#include "../libwx/wx_type.h"
#include "../libwx/wx_hw.h"
#include "../libwx/wx_lib.h"
#include "../libwx/wx_mbx.h"
#include "../libwx/wx_vf.h"
#include "../libwx/wx_vf_common.h"
#include "txgbevf_type.h"

/* txgbevf_pci_tbl - PCI Device ID Table
 *
 * Wildcard entries (PCI_ANY_ID) should come last
 * Last entry must be all 0s
 *
 * { Vendor ID, Device ID, SubVendor ID, SubDevice ID,
 *   Class, Class Mask, private data (not used) }
 */
static const struct pci_device_id txgbevf_pci_tbl[] = {
	{ PCI_VDEVICE(WANGXUN, TXGBEVF_DEV_ID_SP1000), 0},
	{ PCI_VDEVICE(WANGXUN, TXGBEVF_DEV_ID_WX1820), 0},
	{ PCI_VDEVICE(WANGXUN, TXGBEVF_DEV_ID_AML500F), 0},
	{ PCI_VDEVICE(WANGXUN, TXGBEVF_DEV_ID_AML510F), 0},
	{ PCI_VDEVICE(WANGXUN, TXGBEVF_DEV_ID_AML5024), 0},
	{ PCI_VDEVICE(WANGXUN, TXGBEVF_DEV_ID_AML5124), 0},
	{ PCI_VDEVICE(WANGXUN, TXGBEVF_DEV_ID_AML503F), 0},
	{ PCI_VDEVICE(WANGXUN, TXGBEVF_DEV_ID_AML513F), 0},
	/* required last entry */
	{ .device = 0 }
};

static const struct phylink_mac_ops txgbevf_mac_ops = {
	.mac_config = wxvf_mac_config,
	.mac_link_down = wxvf_mac_link_down,
	.mac_link_up = wxvf_mac_link_up,
};

static int txgbevf_interface_max_speed(phy_interface_t interface)
{
	switch (interface) {
	case PHY_INTERFACE_MODE_10GBASER:
		return SPEED_10000;
	case PHY_INTERFACE_MODE_25GBASER:
		return SPEED_25000;
	case PHY_INTERFACE_MODE_XLGMII:
		return SPEED_40000;
	default:
		/* No idea! Garbage in, unknown out */
		return SPEED_UNKNOWN;
	}

	return SPEED_UNKNOWN;
}

static int txgbevf_phylink_init(struct wx *wx)
{
	struct phylink_link_state link_state;
	struct phylink_config *phy_config;
	phy_interface_t phy_mode;
	struct phylink *phylink;
	int err;

	phy_config = &wx->phylink_config;
	phy_config->dev = &wx->netdev->dev;
	phy_config->type = PHYLINK_NETDEV;
	phy_config->mac_capabilities = MAC_40000FD | MAC_25000FD | MAC_10000FD |
				       MAC_1000FD | MAC_100FD | MAC_10FD;
	phy_config->get_fixed_state = wx_get_mac_link_vf;

	if (wx->mac.type == wx_mac_aml40) {
		phy_mode = PHY_INTERFACE_MODE_XLGMII;
		__set_bit(PHY_INTERFACE_MODE_XLGMII,
			  phy_config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_10GBASER,
			  phy_config->supported_interfaces);
	} else if (wx->mac.type == wx_mac_aml) {
		phy_mode = PHY_INTERFACE_MODE_25GBASER;
		__set_bit(PHY_INTERFACE_MODE_25GBASER,
			  phy_config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_10GBASER,
			  phy_config->supported_interfaces);
	} else if (wx->mac.type == wx_mac_sp) {
		phy_mode = PHY_INTERFACE_MODE_10GBASER;
		__set_bit(PHY_INTERFACE_MODE_10GBASER,
			  phy_config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_1000BASEX,
			  phy_config->supported_interfaces);
	} else {
		wx_err(wx, "Unsupported MAC type for VF\n");
		return -EINVAL;
	}

	/* Initialize the phylink */
	phylink = phylink_create(phy_config, NULL,
				 phy_mode, &txgbevf_mac_ops);
	if (IS_ERR(phylink)) {
		wx_err(wx, "Failed to create phylink\n");
		return PTR_ERR(phylink);
	}

	link_state.speed = txgbevf_interface_max_speed(phy_mode);
	link_state.duplex = DUPLEX_FULL;
	err = phylink_set_fixed_link(phylink, &link_state);
	if (err) {
		wx_err(wx, "Failed to set fixed link\n");
		return err;
	}

	wx->phylink = phylink;

	return 0;
}

static const struct net_device_ops txgbevf_netdev_ops = {
	.ndo_open               = wxvf_open,
	.ndo_stop               = wxvf_close,
	.ndo_start_xmit         = wx_xmit_frame,
	.ndo_validate_addr      = eth_validate_addr,
	.ndo_set_mac_address    = wx_set_mac_vf,
};

static void txgbevf_set_num_queues(struct wx *wx)
{
	u32 def_q = 0, num_tcs = 0;
	u16 rss, queue;
	int ret = 0;

	/* Start with base case */
	wx->num_rx_queues = 1;
	wx->num_tx_queues = 1;

	spin_lock_bh(&wx->mbx.mbx_lock);
	/* fetch queue configuration from the PF */
	ret = wx_get_queues_vf(wx, &num_tcs, &def_q);
	spin_unlock_bh(&wx->mbx.mbx_lock);

	if (ret)
		return;

	/* we need as many queues as traffic classes */
	if (num_tcs > 1) {
		wx->num_rx_queues = num_tcs;
	} else {
		rss = min_t(u16, num_online_cpus(), TXGBEVF_MAX_RSS_NUM);
		queue = min_t(u16, wx->mac.max_rx_queues, wx->mac.max_tx_queues);
		rss = min_t(u16, queue, rss);

		switch (wx->vfinfo->vf_api) {
		case wx_mbox_api_13:
			wx->num_rx_queues = rss;
			wx->num_tx_queues = rss;
		default:
			break;
		}
	}
}

static void txgbevf_init_type_code(struct wx *wx)
{
	switch (wx->device_id) {
	case TXGBEVF_DEV_ID_SP1000:
	case TXGBEVF_DEV_ID_WX1820:
		wx->mac.type = wx_mac_sp;
		break;
	case TXGBEVF_DEV_ID_AML500F:
	case TXGBEVF_DEV_ID_AML510F:
	case TXGBEVF_DEV_ID_AML5024:
	case TXGBEVF_DEV_ID_AML5124:
	case TXGBEVF_DEV_ID_AML503F:
	case TXGBEVF_DEV_ID_AML513F:
		wx->mac.type = wx_mac_aml;
		break;
	default:
		wx->mac.type = wx_mac_unknown;
		break;
	}
}

static int txgbevf_sw_init(struct wx *wx)
{
	struct net_device *netdev = wx->netdev;
	struct pci_dev *pdev = wx->pdev;
	int err;

	/* Initialize pcie info and common capability flags */
	err = wx_sw_init(wx);
	if (err < 0)
		goto err_wx_sw_init;

	/* Initialize the mailbox */
	err = wx_init_mbx_params_vf(wx);
	if (err)
		goto err_init_mbx_params;

	/* max q_vectors */
	wx->mac.max_msix_vectors = TXGBEVF_MAX_MSIX_VECTORS;
	/* Initialize the device type */
	txgbevf_init_type_code(wx);
	/* lock to protect mailbox accesses */
	spin_lock_init(&wx->mbx.mbx_lock);

	err = wx_reset_hw_vf(wx);
	if (err) {
		wx_err(wx, "PF still in reset state. Is the PF interface up?\n");
		goto err_reset_hw;
	}
	wx_init_hw_vf(wx);
	wx_negotiate_api_vf(wx);
	if (is_zero_ether_addr(wx->mac.addr))
		dev_info(&pdev->dev,
			 "MAC address not assigned by administrator.\n");
	eth_hw_addr_set(netdev, wx->mac.addr);

	if (!is_valid_ether_addr(netdev->dev_addr)) {
		dev_info(&pdev->dev, "Assigning random MAC address\n");
		eth_hw_addr_random(netdev);
		ether_addr_copy(wx->mac.addr, netdev->dev_addr);
		ether_addr_copy(wx->mac.perm_addr, netdev->dev_addr);
	}

	wx->mac.max_tx_queues = TXGBEVF_MAX_TX_QUEUES;
	wx->mac.max_rx_queues = TXGBEVF_MAX_RX_QUEUES;
	/* Enable dynamic interrupt throttling rates */
	wx->rx_itr_setting = 1;
	wx->tx_itr_setting = 1;
	/* set default ring sizes */
	wx->tx_ring_count = TXGBEVF_DEFAULT_TXD;
	wx->rx_ring_count = TXGBEVF_DEFAULT_RXD;
	/* set default work limits */
	wx->tx_work_limit = TXGBEVF_DEFAULT_TX_WORK;
	wx->rx_work_limit = TXGBEVF_DEFAULT_RX_WORK;

	wx->set_num_queues = txgbevf_set_num_queues;

	return 0;
err_reset_hw:
	kfree(wx->vfinfo);
err_init_mbx_params:
	kfree(wx->rss_key);
	kfree(wx->mac_table);
err_wx_sw_init:
	return err;
}

/**
 * txgbevf_probe - Device Initialization Routine
 * @pdev: PCI device information struct
 * @ent: entry in txgbevf_pci_tbl
 *
 * Return: return 0 on success, negative on failure
 *
 * txgbevf_probe initializes an adapter identified by a pci_dev structure.
 * The OS initialization, configuring of the adapter private structure,
 * and a hardware reset occur.
 **/
static int txgbevf_probe(struct pci_dev *pdev,
			 const struct pci_device_id __always_unused *ent)
{
	struct net_device *netdev;
	struct wx *wx = NULL;
	int err;

	err = pci_enable_device_mem(pdev);
	if (err)
		return err;

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (err) {
		dev_err(&pdev->dev,
			"No usable DMA configuration, aborting\n");
		goto err_pci_disable_dev;
	}

	err = pci_request_selected_regions(pdev,
					   pci_select_bars(pdev, IORESOURCE_MEM),
					   dev_driver_string(&pdev->dev));
	if (err) {
		dev_err(&pdev->dev,
			"pci_request_selected_regions failed 0x%x\n", err);
		goto err_pci_disable_dev;
	}

	pci_set_master(pdev);

	netdev = devm_alloc_etherdev_mqs(&pdev->dev,
					 sizeof(struct wx),
					 TXGBEVF_MAX_TX_QUEUES,
					 TXGBEVF_MAX_RX_QUEUES);
	if (!netdev) {
		err = -ENOMEM;
		goto err_pci_release_regions;
	}

	SET_NETDEV_DEV(netdev, &pdev->dev);

	wx = netdev_priv(netdev);
	wx->netdev = netdev;
	wx->pdev = pdev;

	wx->msg_enable = netif_msg_init(-1, NETIF_MSG_DRV |
					NETIF_MSG_PROBE | NETIF_MSG_LINK);
	wx->hw_addr = devm_ioremap(&pdev->dev,
				   pci_resource_start(pdev, 0),
				   pci_resource_len(pdev, 0));
	if (!wx->hw_addr) {
		err = -EIO;
		goto err_pci_release_regions;
	}

	wx->b4_addr = devm_ioremap(&pdev->dev,
				   pci_resource_start(pdev, 4),
				   pci_resource_len(pdev, 4));
	if (!wx->b4_addr) {
		err = -EIO;
		goto err_pci_release_regions;
	}

	netdev->netdev_ops = &txgbevf_netdev_ops;

	/* setup the private structure */
	err = txgbevf_sw_init(wx);
	if (err)
		goto err_pci_release_regions;

	netdev->features |= NETIF_F_HIGHDMA;

	eth_hw_addr_set(netdev, wx->mac.perm_addr);
	ether_addr_copy(netdev->perm_addr, wx->mac.addr);

	err = wx_init_interrupt_scheme(wx);
	if (err)
		goto err_free_sw_init;

	err = txgbevf_phylink_init(wx);
	if (err)
		goto err_clear_interrupt_scheme;

	err = register_netdev(netdev);
	if (err)
		goto err_register;

	pci_set_drvdata(pdev, wx);
	netif_tx_stop_all_queues(netdev);

	return 0;

err_register:
	phylink_destroy(wx->phylink);
err_clear_interrupt_scheme:
	wx_clear_interrupt_scheme(wx);
err_free_sw_init:
	kfree(wx->vfinfo);
	kfree(wx->rss_key);
	kfree(wx->mac_table);
err_pci_release_regions:
	pci_release_selected_regions(pdev,
				     pci_select_bars(pdev, IORESOURCE_MEM));
err_pci_disable_dev:
	pci_disable_device(pdev);
	return err;
}

/**
 * txgbevf_remove - Device Removal Routine
 * @pdev: PCI device information struct
 *
 * txgbevf_remove is called by the PCI subsystem to alert the driver
 * that it should release a PCI device.  The could be caused by a
 * Hot-Plug event, or because the driver is going to be removed from
 * memory.
 **/
static void txgbevf_remove(struct pci_dev *pdev)
{
	wxvf_remove(pdev);
}

static DEFINE_SIMPLE_DEV_PM_OPS(txgbevf_pm_ops, wxvf_suspend, wxvf_resume);

static struct pci_driver txgbevf_driver = {
	.name     = KBUILD_MODNAME,
	.id_table = txgbevf_pci_tbl,
	.probe    = txgbevf_probe,
	.remove   = txgbevf_remove,
	.shutdown = wxvf_shutdown,
	/* Power Management Hooks */
	.driver.pm	= pm_sleep_ptr(&txgbevf_pm_ops)
};

module_pci_driver(txgbevf_driver);

MODULE_DEVICE_TABLE(pci, txgbevf_pci_tbl);
MODULE_AUTHOR("Beijing WangXun Technology Co., Ltd, <software@trustnetic.com>");
MODULE_DESCRIPTION("WangXun(R) 10/25/40 Gigabit Virtual Function Network Driver");
MODULE_LICENSE("GPL");
