// SPDX-License-Identifier: GPL-2.0
#include "xilinx_tsn.h"

#define DRIVER_NAME             "xilinx_tsn_ep"
#define DRIVER_DESCRIPTION      "Xilinx TSN driver"
#define DRIVER_VERSION          "1.0"

/**
 * ep_iow - Memory mapped TSN endpoint register write
 * @ep: Pointer to TSN endpoint structure
 * @off: Address offset from the base address of endpoint registers
 * @val: Value to be written into the endpoint register
 *
 * This function writes the desired value into the corresponding TSN
 * endpoint register.
 */
static inline void ep_iow(struct tsn_endpoint *ep, off_t off, u32 val)
{
	iowrite32(val, ep->regs + off);
}

/**
 * ep_ior - Memory mapped TSN endpoint register read
 * @ep: Pointer to TSN endpoint structure
 * @off: Address offset from the base address of endpoint registers
 *
 * This function reads a value from the corresponding TSN endpoint
 * register.
 *
 * Return: Value read from the endpoint register
 */
static inline u32 ep_ior(struct tsn_endpoint *ep, u32 off)
{
	return ioread32(ep->regs + off);
}

/**
 * tsn_ep_get_drvinfo - Get driver information for ethtool
 * @ndev: Pointer to the net_device structure
 * @ed: Pointer to ethtool_drvinfo structure
 *
 * This function populates driver name and version information
 * for ethtool driver information display.
 */
static void tsn_ep_get_drvinfo(struct net_device *ndev,
			       struct ethtool_drvinfo *ed)
{
	strscpy(ed->driver, DRIVER_NAME, sizeof(ed->driver));
	strscpy(ed->version, DRIVER_VERSION, sizeof(ed->version));
}

/**
 * tsn_ep_start_xmit_dmaengine - Transmit packet using DMA engine
 * @skb: Socket buffer containing packet data
 * @ndev: Pointer to the net_device structure
 *
 * Return: NETDEV_TX_OK on success, NETDEV_TX_BUSY if ring is full
 *
 * This function handles packet transmission using DMA engine. It maps
 * SKB to scatterlist, prepares DMA descriptor, and submits for transmission.
 */
static netdev_tx_t tsn_ep_start_xmit(struct sk_buff *skb,
				     struct net_device *ndev)
{
	struct tsn_endpoint *ep = netdev_priv(ndev);

	return tsn_start_xmit_dmaengine(ep->common, skb, ndev);
}

/**
 * tsn_ep_open - Open TSN endpoint network interface
 * @ndev: Pointer to the net_device structure
 *
 * Return: 0 on success, negative error code on failure
 *
 * This function opens the network interface by initializing DMA engine,
 * setting maximum frame size, and starting all transmit queues.
 */
static int tsn_ep_open(struct net_device *ndev)
{
	netif_tx_start_all_queues(ndev);

	return 0;
}

/**
 * tsn_ep_stop - Stop TSN endpoint network interface
 * @ndev: Pointer to the net_device structure
 *
 * Return: 0 on success
 *
 * This function stops the network interface by stopping all transmit
 * queues and cleaning up DMA engine resources.
 */
static int tsn_ep_stop(struct net_device *ndev)
{
	netif_tx_stop_all_queues(ndev);
	netdev_info(ndev, "TSN endpoint stopped\n");

	return 0;
}

static const struct net_device_ops ep_netdev_ops = {
	.ndo_open		= tsn_ep_open,
	.ndo_stop		= tsn_ep_stop,
	.ndo_start_xmit		= tsn_ep_start_xmit,
	.ndo_set_mac_address	= tsn_ndo_set_mac_address,
};

static const struct ethtool_ops ep_ethtool_ops = {
	.get_drvinfo	= tsn_ep_get_drvinfo,
};

/**
 * tsn_ep_init - Initialize TSN endpoint subsystem
 * @pdev: Platform device pointer
 *
 * Return: 0 on success, negative error code on failure
 *
 * This function initializes TSN endpoint by parsing device tree,
 * allocating network device, configuring DMA channels, setting MAC
 * address, and registering network interface.
 */
int tsn_ep_init(struct platform_device *pdev)
{
	struct tsn_priv *common = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	struct device_node *ep_node;
	struct tsn_endpoint *ep;
	struct net_device *ndev;
	u8 mac_addr[ETH_ALEN];
	struct resource res;
	int ret;

	ep_node = of_get_child_by_name(dev->of_node, "ep-mac");
	if (!ep_node)
		return dev_err_probe(dev, -ENODEV, "missing ep-mac node\n");

	ret = of_address_to_resource(ep_node, 0, &res);
	if (ret) {
		of_node_put(ep_node);
		return dev_err_probe(dev, ret, "failed to get ep resource\n");
	}

	ndev = alloc_netdev_mqs(sizeof(struct tsn_endpoint), "ep", NET_NAME_UNKNOWN,
				ether_setup, common->num_tx_queues,
				common->num_rx_queues);
	if (!ndev) {
		of_node_put(ep_node);
		return dev_err_probe(dev, -ENOMEM, "failed to alloc net_device\n");
	}

	ndev->netdev_ops = &ep_netdev_ops;
	ndev->ethtool_ops = &ep_ethtool_ops;
	ndev->features = NETIF_F_SG;
	ndev->min_mtu = ETH_ZLEN - ETH_HLEN;
	ndev->max_mtu = ETH_DATA_LEN;
	ep = netdev_priv(ndev);
	memset(ep, 0, sizeof(*ep));
	ep->ndev = ndev;
	ep->regs = common->regs + res.start;
	ep->common = common;
	common->ep = ep;
	SET_NETDEV_DEV(ndev, common->dev);

	/* Retrieve the MAC address */
	ret = of_get_mac_address(ep_node, mac_addr);
	if (ret == 0 && is_valid_ether_addr(mac_addr))
		eth_hw_addr_set(ndev, mac_addr);

	of_node_put(ep_node);

	ret = register_netdev(ndev);
	if (ret) {
		dev_err(common->dev, "Failed to register net device\n");
		free_netdev(ndev);
		common->ep = NULL;
		return ret;
	}

	dev_info(common->dev, "TSN endpoint registered with %d TX queues and %d RX queues\n",
		 common->num_tx_queues, common->num_rx_queues);

	return 0;
}

/**
 * tsn_ep_exit - Clean up TSN endpoint subsystem
 * @pdev: Platform device pointer
 *
 * This function unregisters network device, frees network device
 * memory, and cleans up TSN endpoint resources during driver removal.
 */
void tsn_ep_exit(struct platform_device *pdev)
{
	struct tsn_priv *common = platform_get_drvdata(pdev);
	struct tsn_endpoint *ep;

	if (!common || !common->ep)
		return;

	ep = common->ep;
	if (ep->ndev) {
		unregister_netdev(ep->ndev);
		free_netdev(ep->ndev);
		ep->ndev = NULL;
	}
	common->ep = NULL;
}

MODULE_LICENSE("GPL");
