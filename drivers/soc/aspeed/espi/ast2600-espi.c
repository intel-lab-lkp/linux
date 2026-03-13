// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright Aspeed Technology Inc.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/reset.h>

#include "aspeed-espi.h"
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

	if (sts & ESPI_INT_STS_RST_DEASSERT) {
		/* this will clear all interrupt enable and status */
		reset_control_assert(espi->rst);
		reset_control_deassert(espi->rst);

		ast2600_espi_perif_sw_reset(espi);
		ast2600_espi_perif_reset(espi);

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
