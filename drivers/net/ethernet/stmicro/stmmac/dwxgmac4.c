// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Broadcom Corporation
 */
#include "dwxgmac2.h"
#include "dwxgmac4.h"

static int rd_dma_ch_ind(void __iomem *ioaddr, u8 mode, u32 channel)
{
	u32 reg_val = 0;
	u32 val = 0;

	reg_val |= mode << XGMAC4_MSEL_SHIFT & XGMAC4_MODE_SELECT;
	reg_val |= channel << XGMAC4_AOFF_SHIFT & XGMAC4_ADDR_OFFSET;
	reg_val |= XGMAC4_CMD_TYPE | XGMAC4_OB;
	writel(reg_val, ioaddr + XGMAC4_DMA_CH_IND_CONTROL);
	val = readl(ioaddr + XGMAC4_DMA_CH_IND_DATA);
	return val;
}

static void wr_dma_ch_ind(void __iomem *ioaddr, u8 mode, u32 channel, u32 val)
{
	u32 reg_val = 0;

	writel(val, ioaddr + XGMAC4_DMA_CH_IND_DATA);
	reg_val |= mode << XGMAC4_MSEL_SHIFT & XGMAC4_MODE_SELECT;
	reg_val |= channel << XGMAC4_AOFF_SHIFT & XGMAC4_ADDR_OFFSET;
	reg_val |= XGMAC_OB;
	writel(reg_val, ioaddr + XGMAC4_DMA_CH_IND_CONTROL);
}

static void xgmac4_tp2tc_map(void __iomem *ioaddr, u8 pdma_ch, u32 tc_num)
{
	u32 val = 0;

	val = rd_dma_ch_ind(ioaddr, MODE_TXEXTCFG, pdma_ch);
	val &= ~XGMAC4_TP2TCMP;
	val |= tc_num << XGMAC4_TP2TCMP_SHIFT & XGMAC4_TP2TCMP;
	wr_dma_ch_ind(ioaddr, MODE_TXEXTCFG, pdma_ch, val);
}

static void xgmac4_rp2tc_map(void __iomem *ioaddr, u8 pdma_ch, u32 tc_num)
{
	u32 val = 0;

	val = rd_dma_ch_ind(ioaddr, MODE_RXEXTCFG, pdma_ch);
	val &= ~XGMAC4_RP2TCMP;
	val |= tc_num << XGMAC4_RP2TCMP_SHIFT & XGMAC4_RP2TCMP;
	wr_dma_ch_ind(ioaddr, MODE_RXEXTCFG, pdma_ch, val);
}

void dwxgmac4_dma_init(void __iomem *ioaddr,
		       struct stmmac_dma_cfg *dma_cfg, int atds)
{
	u32 value;
	u32 i;

	value = readl(ioaddr + XGMAC_DMA_SYSBUS_MODE);

	if (dma_cfg->aal)
		value |= XGMAC_AAL;

	if (dma_cfg->eame)
		value |= XGMAC_EAME;

	writel(value, ioaddr + XGMAC_DMA_SYSBUS_MODE);

	for (i = 0; i < VDMA_TOTAL_CH_COUNT; i++) {
		value = rd_dma_ch_ind(ioaddr, MODE_TXDESCCTRL, i);
		value &= ~XGMAC4_TXDCSZ;
		value |= 0x3;
		value &= ~XGMAC4_TDPS;
		value |= (3 << XGMAC4_TDPS_SHIFT) & XGMAC4_TDPS;
		wr_dma_ch_ind(ioaddr, MODE_TXDESCCTRL, i, value);

		value = rd_dma_ch_ind(ioaddr, MODE_RXDESCCTRL, i);
		value &= ~XGMAC4_RXDCSZ;
		value |= 0x3;
		value &= ~XGMAC4_RDPS;
		value |= (3 << XGMAC4_RDPS_SHIFT) & XGMAC4_RDPS;
		wr_dma_ch_ind(ioaddr, MODE_RXDESCCTRL, i, value);
	}

	for (i = 0; i < PDMA_TX_CH_COUNT; i++) {
		value = rd_dma_ch_ind(ioaddr, MODE_TXEXTCFG, i);
		value &= ~(XGMAC4_TXPBL | XGMAC4_TPBLX8_MODE);
		if (dma_cfg->pblx8)
			value |= XGMAC4_TPBLX8_MODE;
		value |= (dma_cfg->pbl << XGMAC4_TXPBL_SHIFT) & XGMAC4_TXPBL;
		wr_dma_ch_ind(ioaddr, MODE_TXEXTCFG, i, value);
		xgmac4_tp2tc_map(ioaddr, i, i);
	}

	for (i = 0; i < PDMA_RX_CH_COUNT; i++) {
		value = rd_dma_ch_ind(ioaddr, MODE_RXEXTCFG, i);
		value &= ~(XGMAC4_RXPBL | XGMAC4_RPBLX8_MODE);
		if (dma_cfg->pblx8)
			value |= XGMAC4_RPBLX8_MODE;
		value |= (dma_cfg->pbl << XGMAC4_RXPBL_SHIFT) & XGMAC4_RXPBL;
		wr_dma_ch_ind(ioaddr, MODE_RXEXTCFG, i, value);
		if (i < PDMA_MAX_TC_COUNT)
			xgmac4_rp2tc_map(ioaddr, i, i);
		else
			xgmac4_rp2tc_map(ioaddr, i, PDMA_MAX_TC_COUNT - 1);
	}
}

void dwxgmac4_dma_init_tx_chan(struct stmmac_priv *priv,
			       void __iomem *ioaddr,
			       struct stmmac_dma_cfg *dma_cfg,
			       dma_addr_t dma_addr, u32 chan)
{
	u32 value;

	value = readl(ioaddr + XGMAC_DMA_CH_TX_CONTROL(chan));
	value &= ~XGMAC4_TVDMA2TCMP;
	value |= (chan << XGMAC4_TVDMA2TCMP_SHIFT) & XGMAC4_TVDMA2TCMP;
	writel(value, ioaddr + XGMAC_DMA_CH_TX_CONTROL(chan));

	writel(upper_32_bits(dma_addr),
	       ioaddr + XGMAC_DMA_CH_TxDESC_HADDR(chan));
	writel(lower_32_bits(dma_addr),
	       ioaddr + XGMAC_DMA_CH_TxDESC_LADDR(chan));
}

void dwxgmac4_dma_init_rx_chan(struct stmmac_priv *priv,
			       void __iomem *ioaddr,
			       struct stmmac_dma_cfg *dma_cfg,
			       dma_addr_t dma_addr, u32 chan)
{
	u32 value;

	value = readl(ioaddr + XGMAC_DMA_CH_RX_CONTROL(chan));
	value &= ~XGMAC4_RVDMA2TCMP;
	value |= (chan << XGMAC4_RVDMA2TCMP_SHIFT) & XGMAC4_RVDMA2TCMP;
	writel(value, ioaddr + XGMAC_DMA_CH_RX_CONTROL(chan));

	writel(upper_32_bits(dma_addr),
	       ioaddr + XGMAC_DMA_CH_RxDESC_HADDR(chan));
	writel(lower_32_bits(dma_addr),
	       ioaddr + XGMAC_DMA_CH_RxDESC_LADDR(chan));
}
