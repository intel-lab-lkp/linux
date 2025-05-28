// SPDX-License-Identifier: GPL-2.0+
/*
 * Black Sesame Technologies SDHCI driver
 *
 * Copyright (C) 2024 Black Sesame Technologies. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/of_reserved_mem.h>
#include <linux/regulator/consumer.h>
#include "sdhci.h"
#include "sdhci-pltfm.h"

struct dwcmshc_priv {
	u32 phy_crm_reg_base;
	u32 phy_crm_reg_size;
};

#define SDHCI_CLOCK_PLL_EN		0x0008
#define SDHCI_TUNING_COUNT		0x20
#define SDHCI_VENDOR_PTR_R		0xE8
#define MBIU_CTRL			0x510
#define BURST_INCR16_EN			BIT(3)
#define BURST_INCR8_EN			BIT(2)
#define BURST_INCR4_EN			BIT(1)
#define BURST_EN			(BURST_INCR16_EN | BURST_INCR8_EN | BURST_INCR4_EN)

/* Synopsys vendor specific registers */
#define reg_offset_addr_vendor		(sdhci_readw(host, SDHCI_VENDOR_PTR_R))
#define SDHC_MHSC_VER_ID_R		(reg_offset_addr_vendor)
#define SDHC_MHSC_VER_TPYE_R		(reg_offset_addr_vendor + 0x4)
#define SDHC_MHSC_CTRL_R		(reg_offset_addr_vendor + 0x8)
#define SDHC_MBIU_CTRL_R		(reg_offset_addr_vendor + 0x10)
#define SDHC_EMMC_CTRL_R		(reg_offset_addr_vendor + 0x2C)
#define SDHC_BOOT_CTRL_R		(reg_offset_addr_vendor + 0x2E)
#define SDHC_GP_IN_R			(reg_offset_addr_vendor + 0x30)
#define SDHC_GP_OUT_R			(reg_offset_addr_vendor + 0x34)
#define SDHC_AT_CTRL_R			(reg_offset_addr_vendor + 0x40)
#define SDHC_AT_STAT_R			(reg_offset_addr_vendor + 0x44)

#define SDEMMC_CRM_BCLK_DIV_CTRL	0x08
#define SDEMMC_CRM_RX_CLK_CTRL		0x14
#define SDEMMC_CRM_TIMER_DIV_CTRL	0x0C
#define SDEMMC_CRM_VOL_CTRL		0x1C
#define DRIVER_NAME			"sdhci_bst"
#define BST_VOL_STABLE_ON		(0x1 << 7)
#define SDHCI_DUMP_BST(f, x...)		\
	pr_info("%s: " DRIVER_NAME ": " f, mmc_hostname(host->mmc), ##x)
#define SD_3_3V				0
#define SD_1_8V				1
#define default_max_freq		200000ul

void sdhci_bst_print_vendor(struct sdhci_host *host)
{
	SDHCI_DUMP_BST("============ SDHCI VENDOR REGISTER DUMP ===========\n");

	SDHCI_DUMP_BST("VER_ID:  0x%08x | VER_TPYE:  0x%08x\n",
		       sdhci_readl(host, SDHC_MHSC_VER_ID_R),
		       sdhci_readl(host, SDHC_MHSC_VER_TPYE_R));
	SDHCI_DUMP_BST("MHSC_CTRL:  0x%08x | MBIU_CTRL:  0x%08x\n",
		       sdhci_readw(host, SDHC_MHSC_CTRL_R),
		       sdhci_readw(host, SDHC_MBIU_CTRL_R));
	SDHCI_DUMP_BST("EMMC_CTRL:  0x%08x | BOOT_CTRL: 0x%08x\n",
		       sdhci_readl(host, SDHC_EMMC_CTRL_R),
		       sdhci_readw(host, SDHC_BOOT_CTRL_R));
	SDHCI_DUMP_BST("GP_IN:   0x%08x | GP_OUT: 0x%08x\n",
		       sdhci_readl(host, SDHC_GP_IN_R),
		       sdhci_readb(host, SDHC_GP_OUT_R));
	SDHCI_DUMP_BST("AT_CTRL:     0x%08x | AT_STAT:  0x%08x\n",
		       sdhci_readb(host, SDHC_AT_CTRL_R),
		       sdhci_readb(host, SDHC_AT_STAT_R));
}
EXPORT_SYMBOL_GPL(sdhci_bst_print_vendor);

static u32 bst_read_phys_bst(u32 phys_addr)
{
	u32 phys_addr_page = phys_addr & 0xFFFFE000;
	u32 phys_offset = phys_addr & 0x00001FFF;
	u32 map_size = phys_offset + sizeof(u32);
	u32 ret = 0xDEADBEEF;
	void *mem_mapped = ioremap(phys_addr_page, map_size);

	if (mem_mapped) {
		ret = (u32)ioread32(((u8 *)mem_mapped) + phys_offset);
		iounmap(mem_mapped);
	}

	return ret;
}

static void bst_write_phys_bst(u32 phys_addr, u32 value)
{
	u32 phys_addr_page = phys_addr & 0xFFFFE000;
	u32 phys_offset = phys_addr & 0x00001FFF;
	u32 map_size = phys_offset + sizeof(u32);
	void *mem_mapped = ioremap(phys_addr_page, map_size);

	if (mem_mapped) {
		iowrite32(value, ((u8 *)mem_mapped) + phys_offset);
		iounmap(mem_mapped);
	}
}

static unsigned int bst_get_max_clock(struct sdhci_host *host)
{
	return host->mmc->f_max;
}

static unsigned int bst_get_min_clock(struct sdhci_host *host)
{
	return host->mmc->f_min;
}

struct rx_ctrl {
	struct {
		u32 rx_revert:1;
		u32 rx_clk_sel_sec:1;
		u32 rx_clk_div:4;
		u32 rx_clk_phase_inner:2;
		u32 rx_clk_sel_first:1;
		u32 rx_clk_phase_out:2;
		u32 rx_clk_en:1;
		u32 res0:20;
	} bit;
	u32 reg;
};

struct sdmmc_iocfg {
	struct {
		u32 res0:16;
		u32 SC_SDMMC0_PVDD18POCSD0:2;
		u32 SC_SDMMC0_PVDD18POCSD1:2;
		u32 SC_SDMMC0_PVDD18POCSD2:2;
		u32 SC_SDMMC1_PVDD18POCSD0:2;
		u32 SC_SDMMC1_PVDD18POCSD1:2;
		u32 SC_SDMMC1_PVDD18POCSD2:2;
		u32 res1:4;
	} bit;
	u32 reg;
};

void sdhci_enable_bst_clk(struct sdhci_host *host, unsigned int clk)
{
	struct sdhci_pltfm_host *pltfm_host;
	struct dwcmshc_priv *priv;
	unsigned int div;
	u32 val;
	struct rx_ctrl rx_reg;

	pltfm_host = sdhci_priv(host);
	priv = sdhci_pltfm_priv(pltfm_host);
	if (clk == 0) {
		div = clk;
	} else if (clk > default_max_freq) {
		div = clk / 1000;
		div = default_max_freq / div;
	} else if (clk < 1500) {
		div = clk;
	} else {
		div = default_max_freq * 100;
		div = div / clk;
		div /= 100;
	}

	clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	clk &= ~SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	clk &= ~SDHCI_CLOCK_PLL_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	val = bst_read_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_TIMER_DIV_CTRL);
	val &= ~(1 << 8);
	bst_write_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_TIMER_DIV_CTRL, val);

	val = bst_read_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_TIMER_DIV_CTRL);
	val &= ~(0xff);
	val |= 0x20;
	bst_write_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_TIMER_DIV_CTRL, val);

	val = bst_read_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_TIMER_DIV_CTRL);
	val |= 1 << 8;
	bst_write_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_TIMER_DIV_CTRL, val);

	val = bst_read_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_RX_CLK_CTRL);
	val &= ~(1 << 11);
	bst_write_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_RX_CLK_CTRL, val);

	rx_reg.reg = bst_read_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_RX_CLK_CTRL);

	rx_reg.bit.rx_revert = 0;
	rx_reg.bit.rx_clk_sel_sec = 1;
	rx_reg.bit.rx_clk_div = 4;
	rx_reg.bit.rx_clk_phase_inner = 2;
	rx_reg.bit.rx_clk_sel_first = 0;
	rx_reg.bit.rx_clk_phase_out = 2;

	bst_write_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_RX_CLK_CTRL, rx_reg.reg);

	val = bst_read_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_RX_CLK_CTRL);
	val |= 1 << 11;
	bst_write_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_RX_CLK_CTRL, val);

	/* Disable clock first */
	val = bst_read_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_BCLK_DIV_CTRL);
	val &= ~0x0400;
	bst_write_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_BCLK_DIV_CTRL, val);

	/* Setup clock divider */
	val = bst_read_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_BCLK_DIV_CTRL);
	val &= ~0x03ff;
	val |= div;
	bst_write_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_BCLK_DIV_CTRL, val);

	/* Enable clock */
	val = bst_read_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_BCLK_DIV_CTRL);
	val |= 0x0400;
	bst_write_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_BCLK_DIV_CTRL, val);

	sdhci_writew(host, (div & 0xff) << 8, SDHCI_CLOCK_CONTROL);

	sdhci_writew(host, (div & 0xff) << 8, SDHCI_CLOCK_CONTROL);
	clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	clk |= SDHCI_CLOCK_PLL_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	clk |= SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	clk |= SDHCI_CLOCK_INT_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
}

void sdhci_set_bst_clock(struct sdhci_host *host, unsigned int clock)
{
	if (clock == 0)
		return;
	sdhci_enable_bst_clk(host, clock);
}

/**
 * sdhci_bst_reset - Reset the SDHCI host controller
 * @host: SDHCI host controller
 * @mask: Reset mask
 *
 * Performs a reset of the SDHCI host controller with special handling for eMMC.
 */
static void sdhci_bst_reset(struct sdhci_host *host, u8 mask)
{
	if (host->mmc->caps2 & MMC_CAP2_NO_SD) {
		sdhci_writew(host,
			     sdhci_readw(host, SDHC_EMMC_CTRL_R) & (~BIT(2)),
			     SDHC_EMMC_CTRL_R);
		sdhci_reset(host, mask);
		usleep_range(10, 20);
		sdhci_writew(host,
			     sdhci_readw(host, SDHC_EMMC_CTRL_R) | BIT(2),
			     SDHC_EMMC_CTRL_R);
	} else {
		sdhci_reset(host, mask);
	}
}

/**
 * sdhci_bst_timeout - Set timeout value for commands
 * @host: SDHCI host controller
 * @cmd: MMC command
 *
 * Sets the timeout control register to maximum value (0xE).
 */
static void sdhci_bst_timeout(struct sdhci_host *host, struct mmc_command *cmd)
{
	sdhci_writeb(host, 0xE, SDHCI_TIMEOUT_CONTROL);
}

/**
 * sdhci_bst_set_power - Set power mode and voltage
 * @host: SDHCI host controller
 * @mode: Power mode to set
 * @vdd: Voltage to set
 *
 * Sets power mode and voltage, also configures MBIU control register.
 */
static void sdhci_bst_set_power(struct sdhci_host *host, unsigned char mode,
				unsigned short vdd)
{
	sdhci_set_power(host, mode, vdd);
	sdhci_writeb(host, 0xF, SDHCI_POWER_CONTROL);
	sdhci_writew(host,
		     (sdhci_readw(host, MBIU_CTRL) & (~0xf)) | BURST_EN,
		     MBIU_CTRL);
}

/**
 * bst_sdhci_execute_tuning - Execute tuning procedure
 * @host: SDHCI host controller
 * @opcode: Opcode to use for tuning
 *
 * Performs tuning procedure by trying different values and selecting the best one.
 *
 * Return: 0 on success, negative errno on failure
 */
static int bst_sdhci_execute_tuning(struct sdhci_host *host, u32 opcode)
{
	struct sdhci_pltfm_host *pltfm_host;
	struct dwcmshc_priv *priv;
	unsigned int clk = 0, timeout;
	int ret = 0, error;
	int start0 = -1, end0 = -1, best = 0;
	int start1 = -1, end1 = -1, flag = 0;
	int i;
	u32 val;

	pltfm_host = sdhci_priv(host);
	priv = sdhci_pltfm_priv(pltfm_host);

	for (i = 0; i < SDHCI_TUNING_COUNT; i++) {
		/* Protected write */
		bst_write_phys_bst(priv->phy_crm_reg_base + 0x88, 0x1234abcd);
		/* Write tuning value */
		bst_write_phys_bst(priv->phy_crm_reg_base + 0x94,
				   (1ul << i) - 1);

		timeout = 20;
		while (!((clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL)) &
			SDHCI_CLOCK_INT_STABLE)) {
			if (timeout == 0) {
				pr_err("%s: Internal clock never stabilised.\n",
				       __func__);
				return -EBUSY;
			}
			timeout--;
			usleep_range(1000, 1100);
		}

		ret = mmc_send_tuning(host->mmc, opcode, &error);
		if (ret != 0) {
			flag = 1;
		} else {
			if (flag == 0) {
				if (start0 == -1)
					start0 = i;
				end0 = i;
			} else {
				if (start1 == -1)
					start1 = i;
				end1 = i;
			}
		}
	}

	/* Calculate best tuning value */
	if (end0 - start0 >= end1 - start1)
		best = ((end0 - start0) >> 1) + start0;
	else
		best = ((end1 - start1) >> 1) + start1;

	if (best < 0)
		best = 0;

	bst_write_phys_bst(priv->phy_crm_reg_base + 0x94, (1ul << best) - 1);
	timeout = 20;

	while (!((clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL)) &
		SDHCI_CLOCK_INT_STABLE)) {
		if (timeout == 0) {
			pr_err("%s: Internal clock never stabilised.\n",
			       __func__);
			return -EBUSY;
		}
		timeout--;
		usleep_range(1000, 1100);
	}

	return 0;
}

/**
 * sdhci_bst_voltage_switch - Perform voltage switch
 * @host: SDHCI host controller
 *
 * Enables voltage stable power.
 */
static void sdhci_bst_voltage_switch(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct dwcmshc_priv *priv = sdhci_pltfm_priv(pltfm_host);

	/* vol stable power on */
	bst_write_phys_bst(priv->phy_crm_reg_base + SDEMMC_CRM_VOL_CTRL,
			   BST_VOL_STABLE_ON);
}

static const struct sdhci_ops sdhci_dwcmshc_ops = {
	.set_clock		= sdhci_set_bst_clock,
	.set_bus_width		= sdhci_set_bus_width,
	.set_uhs_signaling	= sdhci_set_uhs_signaling,
	.get_min_clock		= bst_get_min_clock,
	.get_max_clock		= bst_get_max_clock,
	.reset			= sdhci_bst_reset,
	.set_power		= sdhci_bst_set_power,
	.set_timeout		= sdhci_bst_timeout,
	.platform_execute_tuning = bst_sdhci_execute_tuning,
	.voltage_switch		= sdhci_bst_voltage_switch,
};

static const struct sdhci_pltfm_data sdhci_dwcmshc_pdata = {
	.ops = &sdhci_dwcmshc_ops,
	.quirks = SDHCI_QUIRK_DELAY_AFTER_POWER |
		  SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN |
		  SDHCI_QUIRK_INVERTED_WRITE_PROTECT,
	.quirks2 = SDHCI_QUIRK2_BROKEN_DDR50 |
		   SDHCI_QUIRK2_TUNING_WORK_AROUND |
		   SDHCI_QUIRK2_ACMD23_BROKEN,
};

static void bst_sdhci_allocate_bounce_buffer(struct sdhci_host *host)
{
	struct mmc_host *mmc = host->mmc;
	unsigned int max_blocks;
	unsigned int bounce_size;
	int ret;

	/*
	 * Cap the bounce buffer at 64KB. Using a bigger bounce buffer
	 * has diminishing returns, this is probably because SD/MMC
	 * cards are usually optimized to handle this size of requests.
	 */
	bounce_size = SZ_32K;
	/*
	 * Adjust downwards to maximum request size if this is less
	 * than our segment size, else hammer down the maximum
	 * request size to the maximum buffer size.
	 */
	if (mmc->max_req_size < bounce_size)
		bounce_size = mmc->max_req_size;
	max_blocks = bounce_size / 512;

	ret = of_reserved_mem_device_init_by_idx(mmc_dev(mmc), mmc_dev(mmc)->of_node, 0);
	if (ret) {
		dev_err(mmc_dev(mmc), "of_reserved_mem_device_init error\n");
		return;
	}
	host->bounce_buffer = dma_alloc_coherent(mmc_dev(mmc), bounce_size,
						 &host->bounce_addr, GFP_KERNEL);

	ret = dma_mapping_error(mmc_dev(mmc), host->bounce_addr);
	if (ret) {
		devm_kfree(mmc_dev(mmc), host->bounce_buffer);
		host->bounce_buffer = NULL;
		/* Again fall back to max_segs == 1 */
		return;
	}

	host->bounce_buffer_size = bounce_size;

	/* Lie about this since we're bouncing */
	mmc->max_segs = max_blocks;
	mmc->max_seg_size = bounce_size;
	mmc->max_req_size = bounce_size;

	pr_info("BST reallocate %s bounce up to %u segments into one, max segment size %u bytes\n",
		mmc_hostname(mmc), max_blocks, bounce_size);
}

static int bst_sdhci_set_dma_mask(struct sdhci_host *host)
{
	struct mmc_host *mmc = host->mmc;
	struct device *dev = mmc_dev(mmc);
	int ret = -EINVAL;

	if (host->quirks2 & SDHCI_QUIRK2_BROKEN_64_BIT_DMA)
		host->flags &= ~SDHCI_USE_64_BIT_DMA;

	/* Try 64-bit mask if hardware is capable  of it */
	if (host->flags & SDHCI_USE_64_BIT_DMA) {
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
		if (ret) {
			pr_warn("%s: Failed to set 64-bit DMA mask.\n",
				mmc_hostname(mmc));
			host->flags &= ~SDHCI_USE_64_BIT_DMA;
		}
	}

	/* 32-bit mask as default & fallback */
	if (ret) {
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret)
			pr_warn("%s: Failed to set 32-bit DMA mask.\n",
				mmc_hostname(mmc));
	}

	return ret;
}

int bst_sdhci_setup_host(struct sdhci_host *host)
{
	struct mmc_host *mmc;
	u32 max_current_caps;
	unsigned int ocr_avail;
	unsigned int override_timeout_clk;
	u32 max_clk;
	int ret = 0;
	bool enable_vqmmc = false;

	WARN_ON(!host);
	if (!host)
		return -EINVAL;

	mmc = host->mmc;

	/*
	 * If there are external regulators, get them. Note this must be done
	 * early before resetting the host and reading the capabilities so that
	 * the host can take the appropriate action if regulators are not
	 * available.
	 */
	if (!mmc->supply.vqmmc) {
		ret = mmc_regulator_get_supply(mmc);
		if (ret)
			return ret;
		enable_vqmmc  = true;
	}

	pr_info("Version:   0x%08x | Present:  0x%08x\n",
		sdhci_readw(host, SDHCI_HOST_VERSION),
		sdhci_readl(host, SDHCI_PRESENT_STATE));
	pr_info("Caps:      0x%08x | Caps_1:   0x%08x\n",
		sdhci_readl(host, SDHCI_CAPABILITIES),
		sdhci_readl(host, SDHCI_CAPABILITIES_1));

	sdhci_read_caps(host);

	override_timeout_clk = host->timeout_clk;

	host->flags |= SDHCI_USE_SDMA;

	if (host->flags & (SDHCI_USE_SDMA | SDHCI_USE_ADMA)) {
		if (host->ops->set_dma_mask)
			ret = host->ops->set_dma_mask(host);
		else
			ret = bst_sdhci_set_dma_mask(host);

		if (!ret && host->ops->enable_dma)
			ret = host->ops->enable_dma(host);

		if (ret) {
			pr_warn("%s: No suitable DMA available - falling back to PIO\n",
				mmc_hostname(mmc));
			host->flags &= ~(SDHCI_USE_SDMA | SDHCI_USE_ADMA);

			ret = 0;
		}
	}

	if (host->flags & SDHCI_USE_ADMA) {
		dma_addr_t dma;
		void *buf;

		if (!(host->flags & SDHCI_USE_64_BIT_DMA))
			host->alloc_desc_sz = SDHCI_ADMA2_32_DESC_SZ;
		else if (!host->alloc_desc_sz)
			host->alloc_desc_sz = SDHCI_ADMA2_64_DESC_SZ(host);

		host->desc_sz = host->alloc_desc_sz;
		host->adma_table_sz = host->adma_table_cnt * host->desc_sz;

		host->align_buffer_sz = SDHCI_MAX_SEGS * SDHCI_ADMA2_ALIGN;
		/*
		 * Use zalloc to zero the reserved high 32-bits of 128-bit
		 * descriptors so that they never need to be written.
		 */
		buf = dma_alloc_coherent(mmc_dev(mmc),
					 host->align_buffer_sz + host->adma_table_sz,
					 &dma, GFP_KERNEL);
		if (!buf) {
			pr_warn("%s: Unable to allocate ADMA buffers - falling back to standard DMA\n",
				mmc_hostname(mmc));
			host->flags &= ~SDHCI_USE_ADMA;
		} else if ((dma + host->align_buffer_sz) &
			   (SDHCI_ADMA2_DESC_ALIGN - 1)) {
			pr_warn("%s: unable to allocate aligned ADMA descriptor\n",
				mmc_hostname(mmc));
			host->flags &= ~SDHCI_USE_ADMA;
			dma_free_coherent(mmc_dev(mmc), host->align_buffer_sz +
					  host->adma_table_sz, buf, dma);
		} else {
			host->align_buffer = buf;
			host->align_addr = dma;

			host->adma_table = buf + host->align_buffer_sz;
			host->adma_addr = dma + host->align_buffer_sz;
		}
	}

	host->max_clk = FIELD_GET(SDHCI_CLOCK_V3_BASE_MASK, host->caps);

	host->max_clk *= 1000000;
	if (host->max_clk == 0 || host->quirks &
			SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN) {
		if (!host->ops->get_max_clock) {
			pr_err("%s: Hardware doesn't specify base clock frequency.\n",
			       mmc_hostname(mmc));
			ret = -ENODEV;
			goto undma;
		}
		host->max_clk = host->ops->get_max_clock(host);
	}

	/*
	 * Set host parameters.
	 */
	max_clk = host->max_clk;

	if (host->ops->get_min_clock)
		mmc->f_min = host->ops->get_min_clock(host);

	if (!(host->quirks & SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK)) {
		host->timeout_clk = FIELD_GET(SDHCI_TIMEOUT_CLK_MASK, host->caps);

		if (host->caps & SDHCI_TIMEOUT_CLK_UNIT)
			host->timeout_clk *= 1000;

		if (host->timeout_clk == 0) {
			if (!host->ops->get_timeout_clock) {
				pr_err("%s: Hardware doesn't specify timeout clock frequency.\n",
				       mmc_hostname(mmc));
				ret = -ENODEV;
				goto undma;
			}

			host->timeout_clk =
				DIV_ROUND_UP(host->ops->get_timeout_clock(host),
					     1000);
		}

		if (override_timeout_clk)
			host->timeout_clk = override_timeout_clk;

		mmc->max_busy_timeout = host->ops->get_max_timeout_count ?
			host->ops->get_max_timeout_count(host) : 1 << 27;
		mmc->max_busy_timeout /= host->timeout_clk;
	}

	mmc->caps |= MMC_CAP_SDIO_IRQ | MMC_CAP_CMD23;
	mmc->caps2 |= MMC_CAP2_SDIO_IRQ_NOTHREAD;

	if (host->quirks & SDHCI_QUIRK_MULTIBLOCK_READ_ACMD12)
		host->flags |= SDHCI_AUTO_CMD12;

	/*
	 * A controller may support 8-bit width, but the board itself
	 * might not have the pins brought out.  Boards that support
	 * 8-bit width must set "mmc->caps |= MMC_CAP_8_BIT_DATA;" in
	 * their platform code before calling sdhci_add_host(), and we
	 * won't assume 8-bit width for hosts without that CAP.
	 */
	if (!(host->quirks & SDHCI_QUIRK_FORCE_1_BIT_DATA))
		mmc->caps |= MMC_CAP_4_BIT_DATA;

	if (host->quirks2 & SDHCI_QUIRK2_HOST_NO_CMD23)
		mmc->caps &= ~MMC_CAP_CMD23;

	if (host->caps & SDHCI_CAN_DO_HISPD)
		mmc->caps |= MMC_CAP_SD_HIGHSPEED | MMC_CAP_MMC_HIGHSPEED;

	/* Any UHS-I mode in caps implies SDR12 and SDR25 support. */
	if (host->caps1 & (SDHCI_SUPPORT_SDR104 | SDHCI_SUPPORT_SDR50 |
			   SDHCI_SUPPORT_DDR50))
		mmc->caps |= MMC_CAP_UHS_SDR12 | MMC_CAP_UHS_SDR25;

	/* SDR104 supports also implies SDR50 support */
	if (host->caps1 & SDHCI_SUPPORT_SDR104) {
		mmc->caps |= MMC_CAP_UHS_SDR104 | MMC_CAP_UHS_SDR50;
		/* SD3.0: SDR104 is supported so (for eMMC) the caps2
		 * field can be promoted to support HS200.
		 */
		if (!(host->quirks2 & SDHCI_QUIRK2_BROKEN_HS200))
			mmc->caps2 |= MMC_CAP2_HS200;
	}

	if ((mmc->caps2 & MMC_CAP2_HSX00_1_2V) &&
	    (IS_ERR(mmc->supply.vqmmc) ||
	     !regulator_is_supported_voltage(mmc->supply.vqmmc, 1100000,
					     1300000)))
		mmc->caps2 &= ~MMC_CAP2_HSX00_1_2V;

	/* Does the host need tuning for SDR50? */
	if (host->caps1 & SDHCI_USE_SDR50_TUNING)
		host->flags |= SDHCI_SDR50_NEEDS_TUNING;

	/* Driver Type(s) (A, C, D) supported by the host */
	if (host->caps1 & SDHCI_DRIVER_TYPE_A)
		mmc->caps |= MMC_CAP_DRIVER_TYPE_A;
	if (host->caps1 & SDHCI_DRIVER_TYPE_C)
		mmc->caps |= MMC_CAP_DRIVER_TYPE_C;
	if (host->caps1 & SDHCI_DRIVER_TYPE_D)
		mmc->caps |= MMC_CAP_DRIVER_TYPE_D;

	/* Initial value for re-tuning timer count */
	host->tuning_count = FIELD_GET(SDHCI_RETUNING_TIMER_COUNT_MASK,
				       host->caps1);

	/*
	 * In case Re-tuning Timer is not disabled, the actual value of
	 * re-tuning timer will be 2 ^ (n - 1).
	 */
	if (host->tuning_count)
		host->tuning_count = 1 << (host->tuning_count - 1);

	/* Re-tuning mode supported by the Host Controller */
	host->tuning_mode = FIELD_GET(SDHCI_RETUNING_MODE_MASK, host->caps1);

	ocr_avail = 0;

	if (host->caps & SDHCI_CAN_VDD_330) {
		ocr_avail |= MMC_VDD_32_33 | MMC_VDD_33_34;

		mmc->max_current_330 = FIELD_GET(SDHCI_MAX_CURRENT_330_MASK,
						 max_current_caps) *
						SDHCI_MAX_CURRENT_MULTIPLIER;
	}
	if (host->caps & SDHCI_CAN_VDD_300) {
		ocr_avail |= MMC_VDD_29_30 | MMC_VDD_30_31;

		mmc->max_current_300 = FIELD_GET(SDHCI_MAX_CURRENT_300_MASK,
						 max_current_caps) *
						SDHCI_MAX_CURRENT_MULTIPLIER;
	}
	if (host->caps & SDHCI_CAN_VDD_180) {
		ocr_avail |= MMC_VDD_165_195;

		mmc->max_current_180 = FIELD_GET(SDHCI_MAX_CURRENT_180_MASK,
						 max_current_caps) *
						SDHCI_MAX_CURRENT_MULTIPLIER;
	}

	/* If OCR set by host, use it instead. */
	if (host->ocr_mask)
		ocr_avail = host->ocr_mask;

	/* If OCR set by external regulators, give it highest prio. */
	if (mmc->ocr_avail)
		ocr_avail = mmc->ocr_avail;

	mmc->ocr_avail = ocr_avail;
	mmc->ocr_avail_sdio = ocr_avail;
	if (host->ocr_avail_sdio)
		mmc->ocr_avail_sdio &= host->ocr_avail_sdio;
	mmc->ocr_avail_sd = ocr_avail;
	if (host->ocr_avail_sd)
		mmc->ocr_avail_sd &= host->ocr_avail_sd;
	else /* normal SD controllers don't support 1.8V */
		mmc->ocr_avail_sd &= ~MMC_VDD_165_195;
	mmc->ocr_avail_mmc = ocr_avail;
	if (host->ocr_avail_mmc)
		mmc->ocr_avail_mmc &= host->ocr_avail_mmc;

	if ((mmc->caps & (MMC_CAP_UHS_SDR12 | MMC_CAP_UHS_SDR25 |
			  MMC_CAP_UHS_SDR50 | MMC_CAP_UHS_SDR104 |
			  MMC_CAP_UHS_DDR50 | MMC_CAP_1_8V_DDR)) ||
	    (mmc->caps2 & (MMC_CAP2_HS200_1_8V_SDR | MMC_CAP2_HS400_1_8V)))
		host->flags |= SDHCI_SIGNALING_180;

	spin_lock_init(&host->lock);

	/*
	 * Maximum number of sectors in one transfer. Limited by SDMA boundary
	 * size (512KiB). Note some tuning modes impose a 4MiB limit, but this
	 * is less anyway.
	 */
	mmc->max_req_size = 524288;
	/*
	 * Maximum number of segments. Depends on if the hardware
	 * can do scatter/gather or not.
	 */
	mmc->max_segs = 1;
	mmc->max_req_size = min_t(size_t, mmc->max_req_size,
				  dma_max_mapping_size(mmc_dev(mmc)));

	mmc->max_seg_size = mmc->max_req_size;

	mmc->max_blk_size = (host->caps & SDHCI_MAX_BLOCK_MASK) >>
			SDHCI_MAX_BLOCK_SHIFT;

	mmc->max_blk_size = 512 << mmc->max_blk_size;

	/*
	 * Maximum block count.
	 */
	mmc->max_blk_count = (host->quirks & SDHCI_QUIRK_NO_MULTIBLOCK) ? 1 : 65535;

	if (mmc->max_segs == 1)
		/* This may alter mmc->*_blk_* parameters */
		// bst sdhci must reallocate bounce buffer
		bst_sdhci_allocate_bounce_buffer(host);

	return 0;

undma:
	if (host->align_buffer)
		dma_free_coherent(mmc_dev(mmc), host->align_buffer_sz +
				  host->adma_table_sz, host->align_buffer,
				  host->align_addr);
	host->adma_table = NULL;
	host->align_buffer = NULL;

	return ret;
}

static int bst_sdhci_add_host(struct sdhci_host *host)
{
	int ret;

	ret = bst_sdhci_setup_host(host);
	if (ret)
		return ret;

	ret = __sdhci_add_host(host);
	if (ret)
		goto cleanup;

	return 0;

cleanup:
	sdhci_cleanup_host(host);

	return ret;
}

/**
 * dwcmshc_probe - Platform driver probe
 * @pdev: Platform device
 *
 * Initializes the SDHCI host controller and registers it.
 *
 * Return: 0 on success, negative errno on failure
 */
static int dwcmshc_probe(struct platform_device *pdev)
{
	struct sdhci_pltfm_host *pltfm_host;
	struct sdhci_host *host;
	struct dwcmshc_priv *priv;
	int err;

	host = sdhci_pltfm_init(pdev, &sdhci_dwcmshc_pdata,
				sizeof(struct dwcmshc_priv));
	if (IS_ERR(host))
		return PTR_ERR(host);

	pltfm_host = sdhci_priv(host);
	priv = sdhci_pltfm_priv(pltfm_host);

	err = mmc_of_parse(host->mmc);
	if (err)
		goto err_clk;

	sdhci_get_of_property(pdev);
	device_property_read_u32(&pdev->dev, "mmc_crm_base",
				 &priv->phy_crm_reg_base);
	device_property_read_u32(&pdev->dev, "mmc_crm_size",
				 &priv->phy_crm_reg_size);

	err = bst_sdhci_add_host(host);
	if (err)
		goto err_clk;

	return 0;

err_clk:
	sdhci_pltfm_free(pdev);
	return err;
}

/**
 * dwcmshc_remove - Platform driver remove
 * @pdev: Platform device
 *
 * Removes the SDHCI host controller.
 *
 * Return: 0 on success
 */
static void dwcmshc_remove(struct platform_device *pdev)
{
	struct sdhci_host *host = platform_get_drvdata(pdev);

	sdhci_remove_host(host, 0);
	sdhci_pltfm_free(pdev);
}

static const struct of_device_id sdhci_dwcmshc_dt_ids[] = {
	{ .compatible = "bst,dwcmshc-sdhci" },
	{}
};
MODULE_DEVICE_TABLE(of, sdhci_dwcmshc_dt_ids);

static struct platform_driver sdhci_dwcmshc_driver = {
	.driver = {
		.name = "sdhci-dwcmshc",
		.of_match_table = sdhci_dwcmshc_dt_ids,
	},
	.probe = dwcmshc_probe,
	.remove = dwcmshc_remove,
};
module_platform_driver(sdhci_dwcmshc_driver);

MODULE_DESCRIPTION("SDHCI platform driver for BST DWC MSHC");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("BST Ltd.");
