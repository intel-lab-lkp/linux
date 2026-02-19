// SPDX-License-Identifier: GPL-2.0

/*
 * Time Sensitive Networking (TSN) Ethernet MAC driver
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include "xilinx_tsn.h"

static const char * const tsn_clk_names[TSN_NUM_CLOCKS] = {
	"gtx",
	"gtx90",
	"host_rxfifo",
	"host_txfifo",
	"ref",
	"s_axi",
};

/*
 * Helper to parse TX queue config subnode referenced by
 * xlnx,tsn-tx-config. This version enumerates child nodes in order and
 * assigns DMA channels sequentially (queue0 == first child, etc.)
 */
static int tsn_parse_tx_queue_config(struct device *dev, struct tsn_priv *common,
				     struct device_node *txcfg_np)
{
	struct device_node *qnode;
	unsigned int queue = 0;
	int ret = 0;

	for_each_available_child_of_node(txcfg_np, qnode) {
		u32 chan;

		if (queue >= common->num_tx_queues) {
			dev_err(dev, "tx-config: extra child nodes beyond %u ignored\n",
				common->num_tx_queues);
			of_node_put(qnode);
			return -EINVAL;
		}

		ret = of_property_read_u32(qnode, "xlnx,dma-channel-num", &chan);
		if (ret) {
			dev_err(dev, "tx-config: Q%u missing xlnx,dma-channel-num\n", queue);
			of_node_put(qnode);
			return ret;
		}

		if (chan > TSN_DMA_MAX_TX_CH) {
			dev_err(dev, "tx-config: Q%u channel %u exceeds max %lu\n",
				queue, chan, TSN_DMA_MAX_TX_CH);
			of_node_put(qnode);
			return -EINVAL;
		}
		common->tx_dma_chan_map[queue++] = chan;
	}

	if (queue != common->num_tx_queues) {
		dev_err(dev, "tx-config: described %u queues but expected %u\n",
			queue, common->num_tx_queues);
		return -EINVAL;
	}

	return 0;
}

/**
 * tsn_parse_device_tree - Parse device tree configuration for TSN device
 * @pdev: Platform device pointer
 *
 * Return: 0 on success, negative error code on failure
 */
static int tsn_parse_device_tree(struct platform_device *pdev)
{
	struct tsn_priv *common = platform_get_drvdata(pdev);
	struct device_node *txcfg_np = NULL;
	struct device *dev = &pdev->dev;
	int i, ret;

	/* Read number of priorities */
	ret = of_property_read_u32(dev->of_node, "xlnx,num-priorities", &common->num_priorities);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get xlnx,num-priorities\n");

	if (common->num_priorities < TSN_MIN_PRIORITIES ||
	    common->num_priorities > TSN_MAX_PRIORITIES)
		return dev_err_probe(dev, -EINVAL, "Invalid xlnx,num-priorities (%u)\n",
				     common->num_priorities);

	/* Count TX and RX queues from dma-names property */
	ret = of_property_count_strings(dev->of_node, "dma-names");
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get dma-names\n");

	common->num_tx_queues = 0;
	common->num_rx_queues = 0;

	for (i = 0; i < ret; i++) {
		const char *dma_name;

		if (of_property_read_string_index(dev->of_node, "dma-names", i, &dma_name))
			continue;

		if (strncmp(dma_name, "tx_chan", 7) == 0)
			common->num_tx_queues++;
		else if (strncmp(dma_name, "rx_chan", 7) == 0)
			common->num_rx_queues++;
	}

	if (!common->num_tx_queues || common->num_tx_queues > TSN_MAX_TX_QUEUE)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid TX queue count (%u, max %u)\n",
				     common->num_tx_queues, TSN_MAX_TX_QUEUE);

	if (!common->num_rx_queues)
		return dev_err_probe(dev, -EINVAL, "No RX DMA channels found\n");

	/* Setup clock IDs */
	for (i = 0; i < TSN_NUM_CLOCKS; i++)
		common->clks[i].id = tsn_clk_names[i];

	/* Get all clocks */
	ret = devm_clk_bulk_get(dev, TSN_NUM_CLOCKS, common->clks);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get clocks\n");

	/* Enable clocks */
	ret = clk_bulk_prepare_enable(TSN_NUM_CLOCKS, common->clks);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable clocks\n");

	for (i = 0; i < TSN_MAX_TX_QUEUE; i++)
		common->tx_dma_chan_map[i] = TSN_DMA_CH_INVALID;

	txcfg_np = of_parse_phandle(dev->of_node, "xlnx,tsn-tx-config", 0);
	if (txcfg_np) {
		ret = tsn_parse_tx_queue_config(dev, common, txcfg_np);
		of_node_put(txcfg_np);
		if (ret)
			goto err_disable_clks;
	}

	return 0;

err_disable_clks:
	clk_bulk_disable_unprepare(TSN_NUM_CLOCKS, common->clks);
	return ret;
}

/**
 * tsn_ip_probe - Probe TSN IP core device
 * @pdev: Platform device pointer
 *
 * Return: 0 on success, negative error code on failure
 */
static int tsn_ip_probe(struct platform_device *pdev)
{
	struct tsn_priv *common;
	int ret;

	common = devm_kzalloc(&pdev->dev, sizeof(*common), GFP_KERNEL);
	if (!common)
		return -ENOMEM;

	platform_set_drvdata(pdev, common);
	common->pdev = pdev;
	common->dev = &pdev->dev;

	/* Initialize synchronization primitives */
	spin_lock_init(&common->tx_lock);
	spin_lock_init(&common->rx_lock);
	mutex_init(&common->mdio_lock);

	common->res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!common->res)
		return -ENODEV;
	common->regs_start = common->res->start;
	common->regs = devm_ioremap_resource(&pdev->dev, common->res);
	if (IS_ERR(common->regs))
		return PTR_ERR(common->regs);

	ret = tsn_parse_device_tree(pdev);
	if (ret)
		return ret;

	return 0;
}

/**
 * tsn_ip_remove - Remove TSN IP core device
 * @pdev: Platform device pointer
 */
static void tsn_ip_remove(struct platform_device *pdev)
{
	struct tsn_priv *common = platform_get_drvdata(pdev);

	clk_bulk_disable_unprepare(TSN_NUM_CLOCKS, common->clks);
}

static const struct of_device_id tsn_of_match[] = {
	{ .compatible = "xlnx,tsn-endpoint-ethernet-mac-3.0", },
	{ }
};
MODULE_DEVICE_TABLE(of, tsn_of_match);

static struct platform_driver tsn_driver = {
	.probe = tsn_ip_probe,
	.remove = tsn_ip_remove,
	.driver = {
		.name = "xilinx-tsn",
		.of_match_table = tsn_of_match,
	},
};
module_platform_driver(tsn_driver);

MODULE_AUTHOR("Neeli Srinivas <srinivas.neeli@amd.com>");
MODULE_DESCRIPTION("Time Sensitive Networking (TSN) Ethernet MAC driver");
MODULE_LICENSE("GPL");
