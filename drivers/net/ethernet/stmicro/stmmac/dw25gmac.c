// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Broadcom Corporation
 */
#include "stmmac.h"
#include "dwxgmac2.h"
#include "dw25gmac.h"

static u32 decode_vdma_count(u32 regval)
{
	/* compressed encoding for vdma count
	 * regval: VDMA count
	 * 0-15	 : 1 - 16
	 * 16-19 : 20, 24, 28, 32
	 * 20-23 : 40, 48, 56, 64
	 * 24-27 : 80, 96, 112, 128
	 */
	if (regval < 16)
		return regval + 1;
	return (4 << ((regval - 16) / 4)) * ((regval % 4) + 5);
}

static void dw25gmac_read_hdma_limits(void __iomem *ioaddr,
				      struct stmmac_hdma_cfg *hdma)
{
	u32 hw_cap;

	/* Get VDMA/PDMA counts from HW */
	hw_cap = readl(ioaddr + XGMAC_HW_FEATURE2);
	hdma->tx_vdmas = decode_vdma_count(FIELD_GET(XXVGMAC_HWFEAT_VDMA_TXCNT,
						     hw_cap));
	hdma->rx_vdmas = decode_vdma_count(FIELD_GET(XXVGMAC_HWFEAT_VDMA_RXCNT,
						     hw_cap));
	hdma->tx_pdmas = FIELD_GET(XGMAC_HWFEAT_TXQCNT, hw_cap) + 1;
	hdma->rx_pdmas = FIELD_GET(XGMAC_HWFEAT_RXQCNT, hw_cap) + 1;
}

int dw25gmac_hdma_cfg_init(struct stmmac_priv *priv)
{
	struct plat_stmmacenet_data *plat = priv->plat;
	struct device *dev = priv->device;
	struct stmmac_hdma_cfg *hdma;
	int i;

	hdma = devm_kzalloc(dev,
			    sizeof(*plat->dma_cfg->hdma_cfg),
			    GFP_KERNEL);
	if (!hdma)
		return -ENOMEM;

	dw25gmac_read_hdma_limits(priv->ioaddr, hdma);

	hdma->tvdma_tc = devm_kzalloc(dev,
				      sizeof(*hdma->tvdma_tc) * hdma->tx_vdmas,
				      GFP_KERNEL);
	if (!hdma->tvdma_tc)
		return -ENOMEM;

	hdma->rvdma_tc = devm_kzalloc(dev,
				      sizeof(*hdma->rvdma_tc) * hdma->rx_vdmas,
				      GFP_KERNEL);
	if (!hdma->rvdma_tc)
		return -ENOMEM;

	hdma->tpdma_tc = devm_kzalloc(dev,
				      sizeof(*hdma->tpdma_tc) * hdma->tx_pdmas,
				      GFP_KERNEL);
	if (!hdma->tpdma_tc)
		return -ENOMEM;

	hdma->rpdma_tc = devm_kzalloc(dev,
				      sizeof(*hdma->rpdma_tc) * hdma->rx_pdmas,
				      GFP_KERNEL);
	if (!hdma->rpdma_tc)
		return -ENOMEM;

	/* Initialize one-to-one mapping for each of the used queues */
	for (i = 0; i < plat->tx_queues_to_use; i++) {
		hdma->tvdma_tc[i] = i;
		hdma->tpdma_tc[i] = i;
	}
	for (i = 0; i < plat->rx_queues_to_use; i++) {
		hdma->rvdma_tc[i] = i;
		hdma->rpdma_tc[i] = i;
	}
	plat->dma_cfg->hdma_cfg = hdma;

	return 0;
}

static int rd_dma_ch_ind(void __iomem *ioaddr, u8 mode, u32 channel)
{
	u32 reg_val = 0;

	reg_val |= FIELD_PREP(XXVGMAC_MODE_SELECT, mode);
	reg_val |= FIELD_PREP(XXVGMAC_ADDR_OFFSET, channel);
	reg_val |= XXVGMAC_CMD_TYPE | XXVGMAC_OB;
	writel(reg_val, ioaddr + XXVGMAC_DMA_CH_IND_CONTROL);
	return readl(ioaddr + XXVGMAC_DMA_CH_IND_DATA);
}

static void wr_dma_ch_ind(void __iomem *ioaddr, u8 mode, u32 channel, u32 val)
{
	u32 reg_val = 0;

	writel(val, ioaddr + XXVGMAC_DMA_CH_IND_DATA);
	reg_val |= FIELD_PREP(XXVGMAC_MODE_SELECT, mode);
	reg_val |= FIELD_PREP(XXVGMAC_ADDR_OFFSET, channel);
	reg_val |= XGMAC_OB;
	writel(reg_val, ioaddr + XXVGMAC_DMA_CH_IND_CONTROL);
}

static void xgmac4_tp2tc_map(void __iomem *ioaddr, u8 pdma_ch, u32 tc_num)
{
	u32 val = 0;

	val = rd_dma_ch_ind(ioaddr, MODE_TXEXTCFG, pdma_ch);
	val &= ~XXVGMAC_TP2TCMP;
	val |= FIELD_PREP(XXVGMAC_TP2TCMP, tc_num);
	wr_dma_ch_ind(ioaddr, MODE_TXEXTCFG, pdma_ch, val);
}

static void xgmac4_rp2tc_map(void __iomem *ioaddr, u8 pdma_ch, u32 tc_num)
{
	u32 val = 0;

	val = rd_dma_ch_ind(ioaddr, MODE_RXEXTCFG, pdma_ch);
	val &= ~XXVGMAC_RP2TCMP;
	val |= FIELD_PREP(XXVGMAC_RP2TCMP, tc_num);
	wr_dma_ch_ind(ioaddr, MODE_RXEXTCFG, pdma_ch, val);
}

void dw25gmac_dma_init(void __iomem *ioaddr,
		       struct stmmac_dma_cfg *dma_cfg)
{
	u32 value;
	u32 i;

	value = readl(ioaddr + XGMAC_DMA_SYSBUS_MODE);
	value &= ~(XGMAC_AAL | XGMAC_EAME);
	if (dma_cfg->aal)
		value |= XGMAC_AAL;
	if (dma_cfg->eame)
		value |= XGMAC_EAME;
	writel(value, ioaddr + XGMAC_DMA_SYSBUS_MODE);

	for (i = 0; i < dma_cfg->hdma_cfg->tx_vdmas; i++) {
		value = rd_dma_ch_ind(ioaddr, MODE_TXDESCCTRL, i);
		value &= ~XXVGMAC_TXDCSZ;
		value |= FIELD_PREP(XXVGMAC_TXDCSZ,
				    XXVGMAC_TXDCSZ_256BYTES);
		value &= ~XXVGMAC_TDPS;
		value |= FIELD_PREP(XXVGMAC_TDPS, XXVGMAC_TDPS_HALF);
		wr_dma_ch_ind(ioaddr, MODE_TXDESCCTRL, i, value);
	}

	for (i = 0; i < dma_cfg->hdma_cfg->rx_vdmas; i++) {
		value = rd_dma_ch_ind(ioaddr, MODE_RXDESCCTRL, i);
		value &= ~XXVGMAC_RXDCSZ;
		value |= FIELD_PREP(XXVGMAC_RXDCSZ,
				    XXVGMAC_RXDCSZ_256BYTES);
		value &= ~XXVGMAC_RDPS;
		value |= FIELD_PREP(XXVGMAC_TDPS, XXVGMAC_RDPS_HALF);
		wr_dma_ch_ind(ioaddr, MODE_RXDESCCTRL, i, value);
	}

	for (i = 0; i < dma_cfg->hdma_cfg->tx_pdmas; i++) {
		value = rd_dma_ch_ind(ioaddr, MODE_TXEXTCFG, i);
		value &= ~(XXVGMAC_TXPBL | XXVGMAC_TPBLX8_MODE);
		if (dma_cfg->pblx8)
			value |= XXVGMAC_TPBLX8_MODE;
		value |= FIELD_PREP(XXVGMAC_TXPBL, dma_cfg->pbl);
		wr_dma_ch_ind(ioaddr, MODE_TXEXTCFG, i, value);
		xgmac4_tp2tc_map(ioaddr, i, dma_cfg->hdma_cfg->tpdma_tc[i]);
	}

	for (i = 0; i < dma_cfg->hdma_cfg->rx_pdmas; i++) {
		value = rd_dma_ch_ind(ioaddr, MODE_RXEXTCFG, i);
		value &= ~(XXVGMAC_RXPBL | XXVGMAC_RPBLX8_MODE);
		if (dma_cfg->pblx8)
			value |= XXVGMAC_RPBLX8_MODE;
		value |= FIELD_PREP(XXVGMAC_RXPBL, dma_cfg->pbl);
		wr_dma_ch_ind(ioaddr, MODE_RXEXTCFG, i, value);
		xgmac4_rp2tc_map(ioaddr, i, dma_cfg->hdma_cfg->rpdma_tc[i]);
	}
}

void dw25gmac_dma_init_tx_chan(struct stmmac_priv *priv,
			       void __iomem *ioaddr,
			       struct stmmac_dma_cfg *dma_cfg,
			       dma_addr_t dma_addr, u32 chan)
{
	u32 value;

	value = readl(ioaddr + XGMAC_DMA_CH_TX_CONTROL(chan));
	value &= ~XXVGMAC_TVDMA2TCMP;
	value |= FIELD_PREP(XXVGMAC_TVDMA2TCMP,
			    dma_cfg->hdma_cfg->tvdma_tc[chan]);
	writel(value, ioaddr + XGMAC_DMA_CH_TX_CONTROL(chan));

	writel(upper_32_bits(dma_addr),
	       ioaddr + XGMAC_DMA_CH_TxDESC_HADDR(chan));
	writel(lower_32_bits(dma_addr),
	       ioaddr + XGMAC_DMA_CH_TxDESC_LADDR(chan));
}

void dw25gmac_dma_init_rx_chan(struct stmmac_priv *priv,
			       void __iomem *ioaddr,
			       struct stmmac_dma_cfg *dma_cfg,
			       dma_addr_t dma_addr, u32 chan)
{
	u32 value;

	value = readl(ioaddr + XGMAC_DMA_CH_RX_CONTROL(chan));
	value &= ~XXVGMAC_RVDMA2TCMP;
	value |= FIELD_PREP(XXVGMAC_RVDMA2TCMP,
			    dma_cfg->hdma_cfg->rvdma_tc[chan]);
	writel(value, ioaddr + XGMAC_DMA_CH_RX_CONTROL(chan));

	writel(upper_32_bits(dma_addr),
	       ioaddr + XGMAC_DMA_CH_RxDESC_HADDR(chan));
	writel(lower_32_bits(dma_addr),
	       ioaddr + XGMAC_DMA_CH_RxDESC_LADDR(chan));
}
