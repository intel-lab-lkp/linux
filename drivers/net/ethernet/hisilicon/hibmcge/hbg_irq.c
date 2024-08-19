// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2024 Hisilicon Limited.

#include <linux/interrupt.h>
#include "hbg_irq.h"
#include "hbg_hw.h"

static void hbg_irq_handle_err(struct hbg_priv *priv,
			       struct hbg_irq_info *irq_info)
{
	if (irq_info->need_print)
		dev_err(&priv->pdev->dev,
			"receive abnormal interrupt: %s\n", irq_info->name);
}

#define HBG_TXRX_IRQ_I(name, mask, handle) {name, mask, false, false, 0, handle}
#define HBG_ERR_IRQ_I(name, mask, need_print) \
			{name, mask, true, need_print, 0, hbg_irq_handle_err}

static struct hbg_irq_info hbg_irqs[] = {
	HBG_TXRX_IRQ_I("RX", HBG_INT_MSK_RX_B, NULL),
	HBG_TXRX_IRQ_I("TX", HBG_INT_MSK_TX_B, NULL),
	HBG_ERR_IRQ_I("MAC_MII_FIFO_ERR", HBG_INT_MSK_MAC_MII_FF_ERR_B, true),
	HBG_ERR_IRQ_I("MAC_PCS_RX_FIFO_ERR", HBG_INT_MSK_MAC_PCS_RXFF_ERR_B, true),
	HBG_ERR_IRQ_I("MAC_PCS_TX_FIFO_ERR", HBG_INT_MSK_MAC_PCS_TXFF_ERR_B, true),
	HBG_ERR_IRQ_I("MAC_APP_RX_FIFO_ERR", HBG_INT_MSK_MAC_APP_RXFF_ERR_B, true),
	HBG_ERR_IRQ_I("MAC_APP_TX_FIFO_ERR", HBG_INT_MSK_MAC_APP_TXFF_ERR_B, true),
	HBG_ERR_IRQ_I("SRAM_PARITY_ERR", HBG_INT_MSK_SRAM_PARITY_ERR_B, true),
	HBG_ERR_IRQ_I("TX_AHB_ERR", HBG_INT_MSK_TX_AHB_ERR_B, true),
	HBG_ERR_IRQ_I("RX_BUF_AVL", HBG_INT_MSK_BUF_AVL_B, false),
	HBG_ERR_IRQ_I("REL_BUF_ERR", HBG_INT_MSK_REL_ERR_B, true),
	HBG_ERR_IRQ_I("TXCFG_AVL", HBG_INT_MSK_TXCFG_AVL_B, false),
	HBG_ERR_IRQ_I("TX_DROP", HBG_INT_MSK_TX_DROP_B, false),
	HBG_ERR_IRQ_I("RX_DROP", HBG_INT_MSK_RX_DROP_B, false),
	HBG_ERR_IRQ_I("RX_AHB_ERR", HBG_INT_MSK_RX_AHB_ERR_B, true),
	HBG_ERR_IRQ_I("MAC_FIFO_ERR", HBG_INT_MSK_MAC_FIFO_ERR_B, false),
	HBG_ERR_IRQ_I("RBREQ_ERR", HBG_INT_MSK_RBREQ_ERR_B, false),
	HBG_ERR_IRQ_I("WE_ERR", HBG_INT_MSK_WE_ERR_B, false),
};

void hbg_irq_enable(struct hbg_priv *priv, u32 mask, bool enable)
{
	u32 irq_mask;

	if (mask == HBG_INT_MSK_TX_B)
		return hbg_hw_set_txrx_intr_enable(priv, HBG_DIR_TX, enable);

	if (mask == HBG_INT_MSK_RX_B)
		return hbg_hw_set_txrx_intr_enable(priv, HBG_DIR_RX, enable);

	irq_mask = hbg_hw_get_err_intr_mask(priv);
	if (enable)
		irq_mask |= mask;
	else
		irq_mask &= ~mask;

	hbg_hw_set_err_intr_mask(priv, irq_mask);
}

bool hbg_irq_is_enabled(struct hbg_priv *priv, u32 mask)
{
	if (mask == HBG_INT_MSK_TX_B)
		return hbg_hw_txrx_intr_is_enabled(priv, HBG_DIR_TX);

	if (mask == HBG_INT_MSK_RX_B)
		return hbg_hw_txrx_intr_is_enabled(priv, HBG_DIR_RX);

	return hbg_hw_get_err_intr_mask(priv) & mask;
}

static void hbg_irq_clear_src(struct hbg_priv *priv, u32 mask)
{
	if (mask == HBG_INT_MSK_TX_B)
		return hbg_hw_set_txrx_intr_clear(priv, HBG_DIR_TX);

	if (mask == HBG_INT_MSK_RX_B)
		return hbg_hw_set_txrx_intr_clear(priv, HBG_DIR_RX);

	hbg_hw_set_err_intr_clear(priv, mask);
}

static void hbg_irq_info_handle(struct hbg_priv *priv,
				struct hbg_irq_info *irq_info)
{
	if (!hbg_irq_is_enabled(priv, irq_info->mask))
		return;

	hbg_irq_enable(priv, irq_info->mask, false);
	hbg_irq_clear_src(priv, irq_info->mask);

	irq_info->count++;
	if (irq_info->irq_handle)
		irq_info->irq_handle(priv, irq_info);

	if (irq_info->reenable)
		hbg_irq_enable(priv, irq_info->mask, true);
}

static irqreturn_t hbg_irq_handle(int irq_num, void *p)
{
	u32 status;
	struct hbg_irq_info *irq_info;
	struct hbg_priv *priv = p;
	u32 i;

	status = hbg_hw_get_err_intr_status(priv);
	status |= hbg_hw_get_txrx_intr_status(priv);

	for (i = 0; i < priv->vectors.info_array_len; i++) {
		irq_info = &priv->vectors.info_array[i];
		if (status & irq_info->mask)
			hbg_irq_info_handle(priv, irq_info);
	}

	return IRQ_HANDLED;
}

static const char *irq_names_map[HBG_VECTOR_NUM] = { "tx", "rx", "err", "mdio" };

int hbg_irq_init(struct hbg_priv *priv)
{
	struct hbg_vector *vectors = &priv->vectors;
	struct device *dev = &priv->pdev->dev;
	int ret, id;
	u32 i;

	ret = pci_alloc_irq_vectors(priv->pdev, HBG_VECTOR_NUM, HBG_VECTOR_NUM,
				    PCI_IRQ_MSI | PCI_IRQ_MSIX);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to allocate MSI vectors\n");

	if (ret != HBG_VECTOR_NUM)
		return dev_err_probe(dev, -EINVAL,
				     "requested %u MSI, but allocated %d MSI\n",
				     HBG_VECTOR_NUM, ret);

	/* mdio irq not request, so the number of requested interrupts
	 * is HBG_VECTOR_NUM - 1.
	 */
	for (i = 0; i < HBG_VECTOR_NUM - 1; i++) {
		id = pci_irq_vector(priv->pdev, i);
		if (id < 0)
			return dev_err_probe(dev, id, "failed to get irq number\n");

		snprintf(vectors->name[i], sizeof(vectors->name[i]), "%s-%s-%s",
			 dev_driver_string(dev), pci_name(priv->pdev),
			 irq_names_map[i]);

		ret = devm_request_irq(dev, id, hbg_irq_handle, 0,
				       vectors->name[i], priv);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to requset irq(%d)\n", id);
	}

	vectors->info_array = hbg_irqs;
	vectors->info_array_len = ARRAY_SIZE(hbg_irqs);
	return 0;
}
