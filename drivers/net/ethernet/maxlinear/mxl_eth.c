// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025  MaxLinear, Inc.
 */
#include <linux/clk.h>
#include <linux/etherdevice.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#define ETH_TX_TIMEOUT		(10 * HZ)
#define MXL_NUM_TX_RING		8
#define MXL_NUM_RX_RING		8
#define MXL_NUM_PORT		4

struct mxl_eth_drvdata {
	struct net_device *ndevs[MXL_NUM_PORT];
	void __iomem *port_base;
	void __iomem *ctrl_base;
	struct clk *clks;
};

struct eth_priv {
	struct platform_device *pdev;
	struct device_node *np;
	int port_id;
};

static int mxl_eth_open(struct net_device *ndev)
{
	netif_carrier_on(ndev);
	netif_start_queue(ndev);
	return 0;
}

static int mxl_eth_stop(struct net_device *ndev)
{
	netif_stop_queue(ndev);
	netif_carrier_off(ndev);
	return 0;
}

static int mxl_eth_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops mxl_eth_netdev_ops = {
	.ndo_open       = mxl_eth_open,
	.ndo_stop       = mxl_eth_stop,
	.ndo_start_xmit = mxl_eth_start_xmit,
};

static int mxl_eth_create_ndev(struct platform_device *pdev,
			       struct device_node *np,
			       struct net_device **ndev_out)
{
	struct net_device *ndev;
	struct eth_priv *priv;
	int ret;

	ndev = devm_alloc_etherdev_mqs(&pdev->dev, sizeof(struct eth_priv),
				       MXL_NUM_TX_RING, MXL_NUM_RX_RING);
	if (!ndev) {
		dev_err(&pdev->dev, "alloc_etherdev_mq failed\n");
		return -ENOMEM;
	}

	ndev->netdev_ops = &mxl_eth_netdev_ops;
	ndev->watchdog_timeo = ETH_TX_TIMEOUT;
	ndev->max_mtu = ETH_FRAME_LEN;
	ndev->min_mtu = ETH_MIN_MTU;
	SET_NETDEV_DEV(ndev, &pdev->dev);

	priv = netdev_priv(ndev);
	priv->pdev = pdev;
	priv->np = np;

	if (of_property_read_u32(np, "reg", &priv->port_id) < 0) {
		dev_err(&pdev->dev, "failed to get port id\n");
		return -EINVAL;
	}

	ret = register_netdev(ndev);
	if (ret) {
		dev_err(&pdev->dev, "failed to register net device\n");
		return ret;
	}

	*ndev_out = ndev;
	return 0;
}

static void mxl_eth_cleanup(struct mxl_eth_drvdata *drvdata)
{
	int i;

	for (i = 0; i < MXL_NUM_PORT && drvdata->ndevs[i]; i++) {
		unregister_netdev(drvdata->ndevs[i]);
		drvdata->ndevs[i] = NULL;
	}
}

static int mxl_eth_probe(struct platform_device *pdev)
{
	struct mxl_eth_drvdata *drvdata;
	struct device_node *eth_np, *np;
	struct reset_control *rst;
	int ret, i = 0;

	drvdata = devm_kzalloc(&pdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->port_base =
		devm_platform_ioremap_resource_byname(pdev, "port");
	if (IS_ERR(drvdata->port_base))
		return PTR_ERR(drvdata->port_base);

	drvdata->ctrl_base =
		devm_platform_ioremap_resource_byname(pdev, "ctrl");
	if (IS_ERR(drvdata->ctrl_base))
		return PTR_ERR(drvdata->ctrl_base);

	drvdata->clks = devm_clk_get_enabled(&pdev->dev, "ethif");
	if (IS_ERR(drvdata->clks))
		return dev_err_probe(&pdev->dev, PTR_ERR(drvdata->clks),
				     "failed to get/enable clock\n");

	rst = devm_reset_control_get(&pdev->dev, NULL);
	if (IS_ERR(rst))
		return dev_err_probe(&pdev->dev, PTR_ERR(rst),
				     "failed to get reset control\n");

	reset_control_assert(rst);
	udelay(1);
	reset_control_deassert(rst);

	platform_set_drvdata(pdev, drvdata);

	eth_np = of_get_child_by_name(pdev->dev.of_node, "ethernet-ports");
	if (!eth_np)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "no ethernet-ports node found!\n");

	for_each_available_child_of_node(eth_np, np) {
		ret = mxl_eth_create_ndev(pdev, np, &drvdata->ndevs[i++]);
		if (ret) {
			of_node_put(np);
			goto err_cleanup;
		}

		if (i >= MXL_NUM_PORT) {
			of_node_put(np);
			break;
		}
	}

	if (!i)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "no valid ethernet port\n");

	return 0;

err_cleanup:
	mxl_eth_cleanup(drvdata);
	return ret;
}

static void mxl_eth_remove(struct platform_device *pdev)
{
	struct mxl_eth_drvdata *drvdata = platform_get_drvdata(pdev);

	mxl_eth_cleanup(drvdata);
}

/* Device Tree match table */
static const struct of_device_id mxl_eth_of_match[] = {
	{ .compatible = "maxlinear,lgm-eth" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mxl_eth_of_match);

/* Platform driver struct */
static struct platform_driver mxl_eth_drv = {
	.probe    = mxl_eth_probe,
	.remove   = mxl_eth_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = mxl_eth_of_match,
	},
};

module_platform_driver(mxl_eth_drv);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ethernet driver for MxL SoC");
