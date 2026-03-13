// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright Aspeed Technology Inc.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/reset.h>

#include "aspeed-espi.h"
#include "aspeed-espi-comm.h"
#include "ast2600-espi.h"

static void ast2600_espi_perif_isr(struct aspeed_espi *espi)
{
	u32 sts;

	sts = readl(espi->regs + ESPI_INT_STS);

	if (sts & ESPI_INT_STS_PERIF_PC_RX_CMPLT)
		writel(ESPI_INT_STS_PERIF_PC_RX_CMPLT, espi->regs + ESPI_INT_STS);
}

static void ast2600_espi_perif_sw_reset(struct aspeed_espi *espi)
{
	u32 reg;

	reg = readl(espi->regs + ESPI_CTRL);
	reg &= ~(ESPI_CTRL_PERIF_NP_TX_SW_RST
		 | ESPI_CTRL_PERIF_NP_RX_SW_RST
		 | ESPI_CTRL_PERIF_PC_TX_SW_RST
		 | ESPI_CTRL_PERIF_PC_RX_SW_RST
		 | ESPI_CTRL_PERIF_NP_TX_DMA_EN
		 | ESPI_CTRL_PERIF_PC_TX_DMA_EN
		 | ESPI_CTRL_PERIF_PC_RX_DMA_EN
		 | ESPI_CTRL_PERIF_SW_RDY);
	writel(reg, espi->regs + ESPI_CTRL);

	udelay(1);

	reg |= (ESPI_CTRL_PERIF_NP_TX_SW_RST
		| ESPI_CTRL_PERIF_NP_RX_SW_RST
		| ESPI_CTRL_PERIF_PC_TX_SW_RST
		| ESPI_CTRL_PERIF_PC_RX_SW_RST);
	writel(reg, espi->regs + ESPI_CTRL);
}

static void ast2600_espi_perif_reset(struct aspeed_espi *espi)
{
	u32 reg;

	writel(ESPI_INT_EN_PERIF, espi->regs + ESPI_INT_EN_CLR);
	writel(ESPI_INT_STS_PERIF, espi->regs + ESPI_INT_STS);

	writel(0x0, espi->regs + ESPI_MMBI_INT_EN);
	writel(0xffffffff, espi->regs + ESPI_MMBI_INT_STS);

	reg = readl(espi->regs + ESPI_CTRL2);
	reg &= ~(ESPI_CTRL2_MCYC_RD_DIS_WDT | ESPI_CTRL2_MCYC_WR_DIS_WDT);
	writel(reg, espi->regs + ESPI_CTRL2);

	reg = readl(espi->regs + ESPI_CTRL);
	reg &= ~(ESPI_CTRL_PERIF_NP_TX_DMA_EN
		 | ESPI_CTRL_PERIF_PC_TX_DMA_EN
		 | ESPI_CTRL_PERIF_PC_RX_DMA_EN
		 | ESPI_CTRL_PERIF_SW_RDY);
	writel(reg, espi->regs + ESPI_CTRL);

	reg = readl(espi->regs + ESPI_CTRL) | ESPI_CTRL_PERIF_SW_RDY;
	writel(reg, espi->regs + ESPI_CTRL);
}

int ast2600_espi_perif_probe(struct aspeed_espi *espi)
{
	ast2600_espi_perif_reset(espi);
	return 0;
}

int ast2600_espi_perif_remove(struct aspeed_espi *espi)
{
	u32 reg;

	writel(ESPI_INT_EN_PERIF, espi->regs + ESPI_INT_EN_CLR);

	reg = readl(espi->regs + ESPI_CTRL2);
	reg |= (ESPI_CTRL2_MCYC_RD_DIS | ESPI_CTRL2_MCYC_WR_DIS);
	writel(reg, espi->regs + ESPI_CTRL2);

	reg = readl(espi->regs + ESPI_CTRL);
	reg &= ~(ESPI_CTRL_PERIF_NP_TX_DMA_EN
		 | ESPI_CTRL_PERIF_PC_TX_DMA_EN
		 | ESPI_CTRL_PERIF_PC_RX_DMA_EN
		 | ESPI_CTRL_PERIF_SW_RDY);
	writel(reg, espi->regs + ESPI_CTRL);
	return 0;
}

static void ast2600_espi_flash_isr(struct aspeed_espi *espi)
{
	struct aspeed_espi_flash *flash;
	u32 sts;

	flash = &espi->flash;

	sts = readl(espi->regs + ESPI_INT_STS);

	if (sts & ESPI_INT_STS_FLASH_RX_CMPLT) {
		writel(ESPI_INT_STS_FLASH_RX_CMPLT, espi->regs + ESPI_INT_STS);
		queue_work(system_wq, &flash->rx_work);
	}
}

static void ast2600_espi_flash_reset(struct aspeed_espi *espi)
{
	struct aspeed_espi_flash *flash;
	u32 reg;

	flash = &espi->flash;

	writel(ESPI_INT_EN_FLASH, espi->regs + ESPI_INT_EN_CLR);
	writel(ESPI_INT_STS_FLASH, espi->regs + ESPI_INT_STS);

	reg = readl(espi->regs + ESPI_CTRL);
	reg &= ~(ESPI_CTRL_FLASH_TX_SW_RST
		 | ESPI_CTRL_FLASH_RX_SW_RST
		 | ESPI_CTRL_FLASH_TX_DMA_EN
		 | ESPI_CTRL_FLASH_RX_DMA_EN
		 | ESPI_CTRL_FLASH_SW_RDY);
	writel(reg, espi->regs + ESPI_CTRL);

	udelay(1);

	reg |= (ESPI_CTRL_FLASH_TX_SW_RST | ESPI_CTRL_FLASH_RX_SW_RST);
	writel(reg, espi->regs + ESPI_CTRL);

	flash->tafs.mode = TAFS_MODE_SW;
	reg = readl(espi->regs + ESPI_CTRL) & ~ESPI_CTRL_FLASH_TAFS_MODE;
	reg |= FIELD_PREP(ESPI_CTRL_FLASH_TAFS_MODE, flash->tafs.mode);
	writel(reg, espi->regs + ESPI_CTRL);

	if (flash->dma.enable) {
		writel(flash->dma.tx_addr, espi->regs + ESPI_FLASH_TX_DMA);
		writel(flash->dma.rx_addr, espi->regs + ESPI_FLASH_RX_DMA);

		reg = readl(espi->regs + ESPI_CTRL)
		      | ESPI_CTRL_FLASH_TX_DMA_EN
		      | ESPI_CTRL_FLASH_RX_DMA_EN;
		writel(reg, espi->regs + ESPI_CTRL);
	}

	writel(ESPI_INT_EN_FLASH_RX_CMPLT, espi->regs + ESPI_INT_EN);

	reg = readl(espi->regs + ESPI_CTRL) | ESPI_CTRL_FLASH_SW_RDY;
	writel(reg, espi->regs + ESPI_CTRL);
}

int ast2600_espi_flash_probe(struct aspeed_espi *espi)
{
	u32 regs;

	regs = readl(espi->regs + ESPI_STS);
	if (regs & (ESPI_STS_FLASH_TX_BUSY | ESPI_STS_FLASH_RX_BUSY)) {
		dev_warn(espi->dev, "eSPI flash channel is busy, deferring...\n");
		return -EPROBE_DEFER;
	}

	ast2600_espi_flash_reset(espi);
	return 0;
}

int ast2600_espi_flash_remove(struct aspeed_espi *espi)
{
	struct aspeed_espi_flash *flash;
	u32 reg;

	flash = &espi->flash;

	writel(ESPI_INT_EN_FLASH, espi->regs + ESPI_INT_EN_CLR);

	reg = readl(espi->regs + ESPI_CTRL);
	reg &= ~(ESPI_CTRL_FLASH_TX_DMA_EN
		 | ESPI_CTRL_FLASH_RX_DMA_EN
		 | ESPI_CTRL_FLASH_SW_RDY);
	writel(reg, espi->regs + ESPI_CTRL);

	return 0;
}

int ast2600_espi_flash_get_hdr(struct aspeed_espi *espi,
			       struct espi_comm_hdr *hdr)
{
	u32 reg, len;

	reg = readl(espi->regs + ESPI_FLASH_RX_CTRL);
	hdr->cyc = FIELD_GET(ESPI_FLASH_RX_CTRL_CYC, reg);
	hdr->tag = FIELD_GET(ESPI_FLASH_RX_CTRL_TAG, reg);
	len = FIELD_GET(ESPI_FLASH_RX_CTRL_LEN, reg);
	hdr->len_h = (len >> 8) & 0xff;
	hdr->len_l = len & 0xff;

	return 0;
}

int ast2600_espi_flash_get_pkt(struct aspeed_espi *espi, void *pkt_buf,
			       size_t pkt_size)
{
	u32 i;
	u8 *pkt;

	pkt = (u8 *)pkt_buf;

	if (espi->flash.dma.enable) {
		memcpy(pkt, espi->flash.dma.rx_virt, pkt_size);
	} else {
		for (i = 0; i < pkt_size; ++i)
			pkt[i] = readl(espi->regs + ESPI_FLASH_RX_DATA) & 0xff;
	}

	return 0;
}

int ast2600_espi_flash_put_pkt(struct aspeed_espi *espi,
			       struct espi_flash_cmplt hdr, void *pkt_buf,
			       size_t pkt_size)
{
	u32 i, cyc, tag, len, reg;
	u8 *pkt;

	pkt = (u8 *)pkt_buf;

	if (pkt_buf && pkt_size > 0) {
		if (espi->flash.dma.enable) {
			memcpy(espi->flash.dma.tx_virt, pkt, pkt_size);
			dma_wmb();
		} else {
			for (i = 0; i < pkt_size; ++i)
				writel(pkt[i], espi->regs + ESPI_FLASH_TX_DATA);
		}
	}

	cyc = hdr.cyc;
	tag = hdr.tag;
	len = (hdr.len_h << 8) | hdr.len_l;
	reg = FIELD_PREP(ESPI_FLASH_TX_CTRL_CYC, cyc) |
	      FIELD_PREP(ESPI_FLASH_TX_CTRL_TAG, tag) |
	      FIELD_PREP(ESPI_FLASH_TX_CTRL_LEN, len) |
	      ESPI_FLASH_TX_CTRL_TRIG_PEND;
	writel(reg, espi->regs + ESPI_FLASH_TX_CTRL);

	return 0;
}

void ast2600_espi_flash_clr_pkt(struct aspeed_espi *espi)
{
	writel(ESPI_FLASH_RX_CTRL_SERV_PEND, espi->regs + ESPI_FLASH_RX_CTRL);
}

/* global control */
irqreturn_t ast2600_espi_isr(int irq, void *arg)
{
	struct aspeed_espi *espi;
	u32 sts;

	espi = (struct aspeed_espi *)arg;
	sts = readl(espi->regs + ESPI_INT_STS);

	if (!sts)
		return IRQ_NONE;

	if (sts & ESPI_INT_STS_PERIF)
		ast2600_espi_perif_isr(espi);

	if (sts & ESPI_INT_STS_FLASH_RX_CMPLT)
		ast2600_espi_flash_isr(espi);

	if (sts & ESPI_INT_STS_RST_DEASSERT) {
		/* this will clear all interrupt enable and status */
		reset_control_assert(espi->rst);
		reset_control_deassert(espi->rst);

		ast2600_espi_perif_sw_reset(espi);
		ast2600_espi_perif_reset(espi);
		ast2600_espi_flash_reset(espi);

		/* re-enable eSPI_RESET# interrupt */
		writel(ESPI_INT_EN_RST_DEASSERT, espi->regs + ESPI_INT_EN);
	}

	return IRQ_HANDLED;
}

void ast2600_espi_pre_init(struct aspeed_espi *espi)
{
	writel(ESPI_INT_EN_RST_DEASSERT, espi->regs + ESPI_INT_EN_CLR);
}

void ast2600_espi_post_init(struct aspeed_espi *espi)
{
	writel(ESPI_INT_EN_RST_DEASSERT, espi->regs + ESPI_INT_EN);
}

void ast2600_espi_deinit(struct aspeed_espi *espi)
{
	writel(ESPI_INT_EN_RST_DEASSERT, espi->regs + ESPI_INT_EN_CLR);
}
