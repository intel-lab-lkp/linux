/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2024 Broadcom Corporation
 * XGMAC4 definitions.
 */
#ifndef __STMMAC_DWXGMAC4_H__
#define __STMMAC_DWXGMAC4_H__

/* DMA Indirect Registers*/
#define XGMAC4_DMA_CH_IND_CONTROL	0X00003080
#define XGMAC4_MODE_SELECT		GENMASK(27, 24)
#define XGMAC4_MSEL_SHIFT		24
enum dma_ch_ind_modes {
	MODE_TXEXTCFG	 = 0x0,	  /* Tx Extended Config */
	MODE_RXEXTCFG	 = 0x1,	  /* Rx Extended Config */
	MODE_TXDBGSTS	 = 0x2,	  /* Tx Debug Status */
	MODE_RXDBGSTS	 = 0x3,	  /* Rx Debug Status */
	MODE_TXDESCCTRL	 = 0x4,	  /* Tx Descriptor control */
	MODE_RXDESCCTRL	 = 0x5,	  /* Rx Descriptor control */
};

#define XGMAC4_ADDR_OFFSET		GENMASK(14, 8)
#define XGMAC4_AOFF_SHIFT		8
#define XGMAC4_AUTO_INCR		GENMASK(5, 4)
#define XGMAC4_AUTO_SHIFT		4
#define XGMAC4_CMD_TYPE			BIT(1)
#define XGMAC4_OB			BIT(0)
#define XGMAC4_DMA_CH_IND_DATA		0X00003084

/* TX Config definitions */
#define XGMAC4_TXPBL			GENMASK(29, 24)
#define XGMAC4_TXPBL_SHIFT		24
#define XGMAC4_TPBLX8_MODE		BIT(19)
#define XGMAC4_TP2TCMP			GENMASK(18, 16)
#define XGMAC4_TP2TCMP_SHIFT		16
#define XGMAC4_ORRQ			GENMASK(13, 8)
/* RX Config definitions */
#define XGMAC4_RXPBL			GENMASK(29, 24)
#define XGMAC4_RXPBL_SHIFT		24
#define XGMAC4_RPBLX8_MODE		BIT(19)
#define XGMAC4_RP2TCMP			GENMASK(18, 16)
#define XGMAC4_RP2TCMP_SHIFT		16
#define XGMAC4_OWRQ			GENMASK(13, 8)
/* Tx Descriptor control */
#define XGMAC4_TXDCSZ			GENMASK(2, 0)
#define XGMAC4_TDPS			GENMASK(5, 3)
#define XGMAC4_TDPS_SHIFT		3
/* Rx Descriptor control */
#define XGMAC4_RXDCSZ			GENMASK(2, 0)
#define XGMAC4_RDPS			GENMASK(5, 3)
#define XGMAC4_RDPS_SHIFT		3

/* DWCXG_DMA_CH(#i) Registers*/
#define XGMAC4_DSL			GENMASK(20, 18)
#define XGMAC4_MSS			GENMASK(13, 0)
#define XGMAC4_TFSEL			GENMASK(30, 29)
#define XGMAC4_TQOS			GENMASK(27, 24)
#define XGMAC4_IPBL			BIT(15)
#define XGMAC4_TVDMA2TCMP		GENMASK(6, 4)
#define XGMAC4_TVDMA2TCMP_SHIFT		4
#define XGMAC4_RPF			BIT(31)
#define XGMAC4_RVDMA2TCMP		GENMASK(30, 28)
#define XGMAC4_RVDMA2TCMP_SHIFT		28
#define XGMAC4_RQOS			GENMASK(27, 24)

/* PDMA Channel count */
#define PDMA_TX_CH_COUNT		8
#define PDMA_RX_CH_COUNT		10
#define PDMA_MAX_TC_COUNT		8

/* VDMA channel count */
#define VDMA_TOTAL_CH_COUNT		32

void dwxgmac4_dma_init(void __iomem *ioaddr,
		       struct stmmac_dma_cfg *dma_cfg, int atds);

void dwxgmac4_dma_init_tx_chan(struct stmmac_priv *priv,
			       void __iomem *ioaddr,
			       struct stmmac_dma_cfg *dma_cfg,
			       dma_addr_t dma_addr, u32 chan);
void dwxgmac4_dma_init_rx_chan(struct stmmac_priv *priv,
			       void __iomem *ioaddr,
			       struct stmmac_dma_cfg *dma_cfg,
			       dma_addr_t dma_addr, u32 chan);
#endif /* __STMMAC_DWXGMAC4_H__ */
