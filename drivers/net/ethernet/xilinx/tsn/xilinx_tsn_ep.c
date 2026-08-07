// SPDX-License-Identifier: GPL-2.0
/*
 * AMD/Xilinx TSN Endpoint MAC driver.
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */

#include <linux/bitops.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/types.h>

#include "xilinx_tsn.h"

#define DRIVER_NAME			"xilinx_tsn_ep"

#define TSN_DMA_CH_INVALID		0xFFU
#define TSN_MAX_TX_QUEUE		8
#define TSN_MAX_RX_QUEUE		16

#define TSN_MAX_VLAN_FRAME_SIZE		(ETH_DATA_LEN + VLAN_ETH_HLEN + \
					 ETH_FCS_LEN)

/**
 * struct xlnx_tsn_ep - EP MAC private data, embedded in net_device priv area
 * @ndev: the conduit netdev ("ep")
 * @dev: backing device
 * @regs: EP MAC register window
 * @num_tx_queues: number of TX DMA channels (one per priority)
 * @num_rx_queues: number of RX DMA channels
 * @tx_dma_chan_map: logical TX queue index -> physical DMA channel number
 * @max_frm_size: maximum frame size accepted on RX
 */
struct xlnx_tsn_ep {
	struct net_device *ndev;
	struct device *dev;
	void __iomem *regs;

	u32 num_tx_queues;
	u32 num_rx_queues;
	u32 tx_dma_chan_map[TSN_MAX_TX_QUEUE];
	u32 max_frm_size;
};

static netdev_tx_t ep_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	dev_kfree_skb(skb);
	DEV_STATS_INC(ndev, tx_dropped);
	return NETDEV_TX_OK;
}

static int ep_open(struct net_device *ndev)
{
	netif_tx_start_all_queues(ndev);

	return 0;
}

static int ep_stop(struct net_device *ndev)
{
	netif_tx_disable(ndev);

	return 0;
}

static void ep_get_drvinfo(struct net_device *ndev, struct ethtool_drvinfo *ed)
{
	strscpy(ed->driver, DRIVER_NAME, sizeof(ed->driver));
}

static const struct net_device_ops ep_netdev_ops = {
	.ndo_open		= ep_open,
	.ndo_stop		= ep_stop,
	.ndo_start_xmit		= ep_start_xmit,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= eth_mac_addr,
};

static const struct ethtool_ops ep_ethtool_ops = {
	.get_drvinfo	= ep_get_drvinfo,
};

/*
 * Parse the "tx-queues-config" child of the EP node. The logical queue
 * index is taken from the "queue<N>" node name, so the mapping does not
 * depend on the order the child nodes appear in the device tree.
 */
static int ep_parse_tx_queue_config(struct xlnx_tsn_ep *ep,
				    struct device_node *txcfg_np)
{
	DECLARE_BITMAP(chan_seen, TSN_MAX_TX_QUEUE) = {};
	DECLARE_BITMAP(queue_seen, TSN_MAX_TX_QUEUE) = {};
	unsigned int count = 0;
	int ret;

	for_each_child_of_node_scoped(txcfg_np, qnode) {
		u32 chan, queue;

		if (!str_has_prefix(qnode->name, "queue") ||
		    kstrtou32(qnode->name + strlen("queue"), 10, &queue) ||
		    queue >= ep->num_tx_queues)
			return dev_err_probe(ep->dev, -EINVAL,
					     "tx-config: invalid queue node %pOFn (have %u queues)\n",
					     qnode, ep->num_tx_queues);

		if (test_and_set_bit(queue, queue_seen))
			return dev_err_probe(ep->dev, -EINVAL,
					     "tx-config: queue %u described twice\n",
					     queue);

		ret = of_property_read_u32(qnode, "xlnx,dma-channel-num", &chan);
		if (ret)
			return dev_err_probe(ep->dev, ret,
					     "tx-config: queue %u missing xlnx,dma-channel-num\n",
					     queue);

		if (chan >= ep->num_tx_queues)
			return dev_err_probe(ep->dev, -EINVAL,
					     "tx-config: queue %u channel %u has no matching tx_chan (have %u)\n",
					     queue, chan, ep->num_tx_queues);

		if (test_and_set_bit(chan, chan_seen))
			return dev_err_probe(ep->dev, -EINVAL,
					     "tx-config: channel %u already assigned to another queue\n",
					     chan);

		ep->tx_dma_chan_map[queue] = chan;
		count++;
	}

	if (count != ep->num_tx_queues)
		return dev_err_probe(ep->dev, -EINVAL,
				     "tx-config: described %u queues but expected %u\n",
				     count, ep->num_tx_queues);

	return 0;
}

static int ep_count_dma_queues(struct device *dev, u32 *out_tx, u32 *out_rx)
{
	u32 tx = 0, rx = 0;
	int n, i;

	n = of_property_count_strings(dev->of_node, "dma-names");
	if (n < 0)
		return dev_err_probe(dev, n, "failed to read dma-names\n");

	for (i = 0; i < n; i++) {
		const char *name;

		if (of_property_read_string_index(dev->of_node, "dma-names",
						  i, &name))
			continue;
		if (str_has_prefix(name, "tx_chan"))
			tx++;
		else if (str_has_prefix(name, "rx_chan"))
			rx++;
	}

	if (!tx || tx > TSN_MAX_TX_QUEUE)
		return dev_err_probe(dev, -EINVAL,
				     "invalid TX queue count (%u, max %u)\n",
				     tx, TSN_MAX_TX_QUEUE);

	if (!rx || rx > TSN_MAX_RX_QUEUE)
		return dev_err_probe(dev, -EINVAL,
				     "invalid RX queue count (%u, max %u)\n",
				     rx, TSN_MAX_RX_QUEUE);

	*out_tx = tx;
	*out_rx = rx;

	return 0;
}

static int xlnx_tsn_ep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *txcfg_np;
	struct net_device *ndev;
	struct xlnx_tsn_ep *ep;
	u8 mac_addr[ETH_ALEN];
	u32 num_tx, num_rx;
	int ret;
	int i;

	ret = ep_count_dma_queues(dev, &num_tx, &num_rx);
	if (ret)
		return ret;

	ndev = alloc_netdev_mqs(sizeof(*ep), "ep", NET_NAME_UNKNOWN,
				ether_setup, num_tx, num_rx);
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, dev);
	ndev->netdev_ops = &ep_netdev_ops;
	ndev->ethtool_ops = &ep_ethtool_ops;
	ndev->features = NETIF_F_SG;

	ep = netdev_priv(ndev);
	ep->ndev = ndev;
	ep->dev = dev;
	ep->num_tx_queues = num_tx;
	ep->num_rx_queues = num_rx;
	ep->max_frm_size = TSN_MAX_VLAN_FRAME_SIZE;

	for (i = 0; i < TSN_MAX_TX_QUEUE; i++)
		ep->tx_dma_chan_map[i] = TSN_DMA_CH_INVALID;

	ep->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ep->regs)) {
		ret = PTR_ERR(ep->regs);
		goto err_free_ndev;
	}

	txcfg_np = of_get_child_by_name(dev->of_node, "tx-queues-config");
	if (!txcfg_np) {
		ret = dev_err_probe(dev, -EINVAL,
				    "missing tx-queues-config node\n");
		goto err_free_ndev;
	}
	ret = ep_parse_tx_queue_config(ep, txcfg_np);
	of_node_put(txcfg_np);
	if (ret)
		goto err_free_ndev;

	ret = of_get_mac_address(dev->of_node, mac_addr);
	if (ret == -EPROBE_DEFER) {
		goto err_free_ndev;
	} else if (!ret && is_valid_ether_addr(mac_addr)) {
		eth_hw_addr_set(ndev, mac_addr);
	} else {
		eth_hw_addr_random(ndev);
		dev_info(dev, "no valid MAC in DT, using random address %pM\n",
			 ndev->dev_addr);
	}

	platform_set_drvdata(pdev, ep);

	ret = register_netdev(ndev);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register net device\n");
		goto err_free_ndev;
	}

	return 0;

err_free_ndev:
	free_netdev(ndev);
	return ret;
}

static void xlnx_tsn_ep_remove(struct platform_device *pdev)
{
	struct xlnx_tsn_ep *ep = platform_get_drvdata(pdev);

	if (!ep)
		return;

	unregister_netdev(ep->ndev);
	free_netdev(ep->ndev);
}

static const struct of_device_id xlnx_tsn_ep_of_match[] = {
	{ .compatible = "xlnx,tsn-ep-mac" },
	{ }
};
MODULE_DEVICE_TABLE(of, xlnx_tsn_ep_of_match);

struct platform_driver xlnx_tsn_ep_driver = {
	.probe	= xlnx_tsn_ep_probe,
	.remove	= xlnx_tsn_ep_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= xlnx_tsn_ep_of_match,
	},
};
