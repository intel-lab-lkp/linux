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

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include "sdhci.h"
#include "sdhci-pltfm.h"

struct dwcmshc_priv {
	void __iomem *crm_reg_base;
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
#define SDHC_EMMC_CTRL_R_OFFSET		0x2C

#define SDEMMC_CRM_BCLK_DIV_CTRL	0x08
#define SDEMMC_CRM_RX_CLK_CTRL		0x14
#define SDEMMC_CRM_TIMER_DIV_CTRL	0x0C
#define SDEMMC_CRM_VOL_CTRL			0x1C
#define REG_WR_PROTECT			0x88
#define REG_WR_PROTECT_KEY		0x1234abcd
#define DELAY_CHAIN_SEL			0x94
#define BST_VOL_STABLE_ON		BIT(7)
#define DEFAULT_MAX_FREQ		200000UL

static u32 bst_crm_read(struct sdhci_pltfm_host *pltfm_host, u32 offset)
{
	struct dwcmshc_priv *priv = sdhci_pltfm_priv(pltfm_host);

	return ioread32(priv->crm_reg_base + offset);
}

static void bst_crm_write(struct sdhci_pltfm_host *pltfm_host, u32 offset, u32 value)
{
	struct dwcmshc_priv *priv = sdhci_pltfm_priv(pltfm_host);

	iowrite32(value, priv->crm_reg_base + offset);
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

static void sdhci_enable_bst_clk(struct sdhci_host *host, unsigned int clk)
{
	struct sdhci_pltfm_host *pltfm_host;
	unsigned int div;
	u32 val;
	struct rx_ctrl rx_reg;

	pltfm_host = sdhci_priv(host);
	if (clk == 0) {
		div = clk;
	} else if (clk > DEFAULT_MAX_FREQ) {
		div = clk / 1000;
		div = DEFAULT_MAX_FREQ / div;
	} else if (clk < 1500) {
		div = clk;
	} else {
		div = DEFAULT_MAX_FREQ * 100;
		div = div / clk;
		div /= 100;
	}

	clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	clk &= ~SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	clk &= ~SDHCI_CLOCK_PLL_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	val = bst_crm_read(pltfm_host, SDEMMC_CRM_TIMER_DIV_CTRL);
	val &= ~BIT(8);
	bst_crm_write(pltfm_host, SDEMMC_CRM_TIMER_DIV_CTRL, val);

	val = bst_crm_read(pltfm_host, SDEMMC_CRM_TIMER_DIV_CTRL);
	val &= ~0xff;
	val |= 0x20;
	bst_crm_write(pltfm_host, SDEMMC_CRM_TIMER_DIV_CTRL, val);

	val = bst_crm_read(pltfm_host, SDEMMC_CRM_TIMER_DIV_CTRL);
	val |= BIT(8);
	bst_crm_write(pltfm_host, SDEMMC_CRM_TIMER_DIV_CTRL, val);

	val = bst_crm_read(pltfm_host, SDEMMC_CRM_RX_CLK_CTRL);
	val &= ~BIT(11);
	bst_crm_write(pltfm_host, SDEMMC_CRM_RX_CLK_CTRL, val);

	rx_reg.reg = bst_crm_read(pltfm_host, SDEMMC_CRM_RX_CLK_CTRL);

	rx_reg.bit.rx_revert = 0;
	rx_reg.bit.rx_clk_sel_sec = 1;
	rx_reg.bit.rx_clk_div = 4;
	rx_reg.bit.rx_clk_phase_inner = 2;
	rx_reg.bit.rx_clk_sel_first = 0;
	rx_reg.bit.rx_clk_phase_out = 2;

	bst_crm_write(pltfm_host, SDEMMC_CRM_RX_CLK_CTRL, rx_reg.reg);

	val = bst_crm_read(pltfm_host, SDEMMC_CRM_RX_CLK_CTRL);
	val |= BIT(11);
	bst_crm_write(pltfm_host, SDEMMC_CRM_RX_CLK_CTRL, val);

	/* Disable clock first */
	val = bst_crm_read(pltfm_host, SDEMMC_CRM_BCLK_DIV_CTRL);
	val &= ~BIT(10);
	bst_crm_write(pltfm_host, SDEMMC_CRM_BCLK_DIV_CTRL, val);

	/* Setup clock divider */
	val = bst_crm_read(pltfm_host, SDEMMC_CRM_BCLK_DIV_CTRL);
	val &= ~GENMASK(9, 0);
	val |= div;
	bst_crm_write(pltfm_host, SDEMMC_CRM_BCLK_DIV_CTRL, val);

	/* Enable clock */
	val = bst_crm_read(pltfm_host, SDEMMC_CRM_BCLK_DIV_CTRL);
	val |= BIT(10);
	bst_crm_write(pltfm_host, SDEMMC_CRM_BCLK_DIV_CTRL, val);

	sdhci_writew(host, (div & 0xff) << 8, SDHCI_CLOCK_CONTROL);

	clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	clk |= SDHCI_CLOCK_PLL_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	clk |= SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	clk |= SDHCI_CLOCK_INT_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
}

static void sdhci_set_bst_clock(struct sdhci_host *host, unsigned int clock)
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
	u16 vendor_ptr, emmc_ctrl_reg;

	if (host->mmc->caps2 & MMC_CAP2_NO_SD) {
		vendor_ptr = sdhci_readw(host, SDHCI_VENDOR_PTR_R);
		emmc_ctrl_reg = vendor_ptr + SDHC_EMMC_CTRL_R_OFFSET;

		sdhci_writew(host,
			     sdhci_readw(host, emmc_ctrl_reg) & (~BIT(2)),
			     emmc_ctrl_reg);
		sdhci_reset(host, mask);
		usleep_range(10, 20);
		sdhci_writew(host,
			     sdhci_readw(host, emmc_ctrl_reg) | BIT(2),
			     emmc_ctrl_reg);
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
	unsigned int clk = 0, timeout;
	int ret = 0, error;
	int start0 = -1, end0 = -1, best = 0;
	int start1 = -1, end1 = -1, flag = 0;
	int i;

	pltfm_host = sdhci_priv(host);

	for (i = 0; i < SDHCI_TUNING_COUNT; i++) {
		/* Protected write */
		bst_crm_write(pltfm_host, REG_WR_PROTECT, REG_WR_PROTECT_KEY);
		/* Write tuning value */
		bst_crm_write(pltfm_host, DELAY_CHAIN_SEL, (1ul << i) - 1);

		timeout = 20;
		while (!((clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL)) &
			SDHCI_CLOCK_INT_STABLE)) {
			if (timeout == 0) {
				dev_err(mmc_dev(host->mmc), "Internal clock never stabilised\n");
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

	bst_crm_write(pltfm_host, DELAY_CHAIN_SEL, (1ul << best) - 1);
	timeout = 20;

	while (!((clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL)) &
		SDHCI_CLOCK_INT_STABLE)) {
		if (timeout == 0) {
			dev_err(mmc_dev(host->mmc), "Internal clock never stabilised\n");
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

	/* vol stable power on */
	bst_crm_write(pltfm_host, SDEMMC_CRM_VOL_CTRL, BST_VOL_STABLE_ON);
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

static int bst_sdhci_reallocate_bounce_buffer(struct sdhci_host *host)
{
	struct mmc_host *mmc = host->mmc;
	unsigned int max_blocks;
	unsigned int bounce_size;
	int ret;

	/*
	 * Cap the bounce buffer at 32KB. Using a bigger bounce buffer
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
		dev_err(mmc_dev(mmc), "Failed to initialize reserved memory\n");
		return ret;
	}

	host->bounce_buffer = dma_alloc_coherent(mmc_dev(mmc), bounce_size,
						 &host->bounce_addr, GFP_KERNEL);
	if (!host->bounce_buffer)
		return -ENOMEM;

	host->bounce_buffer_size = bounce_size;

	/* Lie about this since we're bouncing */
	mmc->max_segs = max_blocks;
	mmc->max_seg_size = bounce_size;
	mmc->max_req_size = bounce_size;

	return 0;
}

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
		goto err;

	sdhci_get_of_property(pdev);

	/* Get CRM registers from the second reg entry */
	priv->crm_reg_base = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(priv->crm_reg_base)) {
		err = PTR_ERR(priv->crm_reg_base);
		goto err;
	}

	err = sdhci_add_host(host);
	if (err)
		goto err;

	/*
	 * Silicon constraints for BST C1200:
	 * - System RAM base is 0x800000000 (above 32-bit addressable range)
	 * - The eMMC controller DMA engine is limited to 32-bit addressing
	 * - SMMU cannot be used on this path due to hardware design flaws
	 * - These are fixed in silicon and cannot be changed in software
	 *
	 * Bus/controller mapping:
	 * - No registers are available to reprogram the address mapping
	 * - The 32-bit DMA limit is a hard constraint of the controller IP
	 *
	 * Given these constraints, an SRAM-based bounce buffer in the 32-bit
	 * address space is required to enable eMMC DMA on this platform.
	 */
	err = bst_sdhci_reallocate_bounce_buffer(host);
	if (err) {
		dev_err(&pdev->dev, "Failed to allocate bounce buffer: %d\n", err);
		goto err_remove_host;
	}

	return 0;

err_remove_host:
	sdhci_remove_host(host, 1);
err:
	sdhci_pltfm_free(pdev);
	return err;
}

static void dwcmshc_remove(struct platform_device *pdev)
{
	struct sdhci_host *host = platform_get_drvdata(pdev);

	/* Free bounce buffer if allocated */
	if (host->bounce_buffer) {
		dma_free_coherent(mmc_dev(host->mmc), host->bounce_buffer_size,
				  host->bounce_buffer, host->bounce_addr);
		host->bounce_buffer = NULL;
	}

	/* Release reserved memory */
	of_reserved_mem_device_release(mmc_dev(host->mmc));

	sdhci_remove_host(host, 0);
	sdhci_pltfm_free(pdev);
}

static const struct of_device_id sdhci_dwcmshc_dt_ids[] = {
	{ .compatible = "bst,c1200-dwcmshc-sdhci" },
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

MODULE_DESCRIPTION("Black Sesame Technologies DWCMSHC SDHCI driver");
MODULE_AUTHOR("Black Sesame Technologies Co., Ltd.");
MODULE_LICENSE("GPL");
