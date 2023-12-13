// SPDX-License-Identifier: GPL-2.0-only
/* DMA support for Intel FPGA Quad-Speed Ethernet MAC driver
 * Copyright (C) 2023 Intel Corporation. All rights reserved
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/platform_device.h>

#include "altera_eth_dma.h"
#include "altera_utils.h"

/* Probe DMA */
int altera_eth_dma_probe(struct platform_device *pdev, struct altera_dma_private *priv,
			 enum altera_dma_type type)
{
	void __iomem *descmap;

	/* xSGDMA Rx Dispatcher address space */
	priv->rx_dma_csr = devm_platform_ioremap_resource_byname(pdev, "rx_csr");
	if (IS_ERR(priv->rx_dma_csr))
		return PTR_ERR(priv->rx_dma_csr);

	/* mSGDMA Tx Dispatcher address space */
	priv->tx_dma_csr = devm_platform_ioremap_resource_byname(pdev, "tx_csr");
	if (IS_ERR(priv->rx_dma_csr))
		return PTR_ERR(priv->rx_dma_csr);

	switch (type) {
	case ALTERA_DTYPE_SGDMA:
		/* Get the mapped address to the SGDMA descriptor memory */
		descmap = devm_platform_ioremap_resource_byname(pdev, "s1");
		if (IS_ERR(descmap))
			return PTR_ERR(descmap);
		break;
	case ALTERA_DTYPE_MSGDMA:
		priv->rx_dma_resp = devm_platform_ioremap_resource_byname(pdev, "rx_resp");
		if (IS_ERR(priv->rx_dma_resp))
			return PTR_ERR(priv->rx_dma_resp);

		priv->tx_dma_desc = devm_platform_ioremap_resource_byname(pdev, "tx_desc");
		if (IS_ERR(priv->tx_dma_desc))
			return PTR_ERR(priv->tx_dma_desc);

		priv->rx_dma_desc = devm_platform_ioremap_resource_byname(pdev, "rx_desc");
		if (IS_ERR(priv->rx_dma_desc))
			return PTR_ERR(priv->rx_dma_desc);
		break;
	default:
		return -ENODEV;
	}

	return 0;

};
EXPORT_SYMBOL_NS(altera_eth_dma_probe, NET_ALTERA);
MODULE_LICENSE("GPL");
