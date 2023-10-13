// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: linux_base.c
 *
 * Abstract: Driver Entry  for Linux
 *
 * Version: 1.00
 *
 * Author: Peter.Guo
 *
 * Environment:	Linux
 *
 * History:
 *
 * 5/20/2015		Creation	Peter.Guo
 */

#include <linux/module.h>
#include <linux/init.h>
#include "../include/basic.h"
#include "../include/debug.h"
#include "../include/reqapi.h"
#include "../include/hostapi.h"
#include "../include/hostvenapi.h"

#include "linux_scsi.h"
#if BHT_LINUX_ENABLE_RTD3
#include <linux/pm_runtime.h>
#endif

#define VENDOR_O2MICRO 0x1217
#define BHT_SD_MAX_DEV	8
#define BHT_USE_PCI_MSI 1
#define SRB_MIN_REQ	64

#define INPUT_TUNING_PASS_WINDOW	0x44

extern u32 g_dbg_module;
extern u32 g_dbg_feature;
extern u32 g_dbg_ctrl;

extern uint m_sd_3v3_clk_driver_strength;
extern uint m_sd_3v3_cmddata_driver_strength;
extern uint m_sd_1v8_clk_driver_strength;
extern uint m_sd_1v8_cmddata_driver_strength;
extern uint m_ram_ema;
extern uint m_vdd1_vdd2_source;
extern uint m_vdd18_debounce_time;
extern uint m_ssc_enable;
extern uint m_cnfg_drv;
extern uint m_cnfg_trm_code_tx;
extern uint m_cnfg_trm_code_rx;
extern uint m_cnfg_rint_code;

struct kmem_cache *bht_srb_ext_cachep;
mempool_t *bht_sd_mem_pool;

typedef struct {
	int used;
	bht_dev_ext_t data;
} bht_sd_slot_t;

static bht_sd_slot_t bht_slot[BHT_SD_MAX_DEV];

static bht_sd_slot_t *bht_slot_getfree(void)
{
	int i = 0;

	for (i = 0; i < BHT_SD_MAX_DEV; i++) {
		if (bht_slot[i].used == 0) {
			os_memset(&bht_slot[i].data, 0, sizeof(bht_dev_ext_t));
			DbgInfo(MODULE_OS_ENTRY, FEATURE_DRIVER_INIT,
				NOT_TO_RAM, "Get Slot=%p i=%d\n", &bht_slot[i],
				i);
			return &bht_slot[i];
		}
	}
	return NULL;
}

static void bht_global_init(void)
{
	int i = 0;

	for (i = 0; i < BHT_SD_MAX_DEV; i++) {
		os_memset(&bht_slot[i].data, 0, sizeof(bht_dev_ext_t));
		bht_slot[i].used = 0;
	}

	DbgRamInit();
#if DBG || _DEBUG
	g_dbg_module = DBG_MODULE_CONTROL;
	g_dbg_feature = DBG_FEATURE_CONTROL;
	g_dbg_ctrl = DBG_CTRL_CONTROL;

	DbgErr("Dbg Module=0x%08X feature=0x%08X control=0x%08X\n",
	       g_dbg_module, g_dbg_feature, g_dbg_ctrl);
#else
	g_dbg_module = 0;
	g_dbg_feature = 0;
	g_dbg_ctrl = 0;
#endif
	cfgmng_init();
}

static void bht_global_uninit(void)
{
	int i;

	DbgInfo(MODULE_OS_ENTRY, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"bht sd free slots\n");
	for (i = 0; i < BHT_SD_MAX_DEV; i++) {
		os_memset(&bht_slot[i].data, 0, sizeof(bht_dev_ext_t));
		bht_slot[i].used = 0;
	}

	DbgRamFree();
}

struct pci_device_id bht_id_table[] = {
	{
	 /* SDS0 */
	 .vendor = VENDOR_O2MICRO,
	 .device = 0x8420,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	  },

	{
	 /* SDS1 */
	 .vendor = VENDOR_O2MICRO,
	 .device = 0x8421,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	  },

	{
	 /* Fujin2A Fjin2B */
	 .vendor = VENDOR_O2MICRO,
	 .device = 0x8520,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	  },

	{
	 /* Searbird */
	 .vendor = VENDOR_O2MICRO,
	 .device = 0x8620,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	  },

	{
	 /* SeaBird */
	 .vendor = VENDOR_O2MICRO,
	 .device = 0x8621,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	  },
	{
	 .vendor = VENDOR_O2MICRO,
	 /* 0x8720 to 0x8723 is SeaEagle2 */
	 .device = 0x8720,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	  },

	{
	 .vendor = VENDOR_O2MICRO,
	 .device = 0x8721,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	  },
	{
	 .vendor = VENDOR_O2MICRO,
	 .device = 0x8722,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	  },
	{
	 .vendor = VENDOR_O2MICRO,
	 .device = 0x8723,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	  },
	{
	 /* 0x8750,0x8751 and 0x8740 is SandStorm2 */
	 .vendor = VENDOR_O2MICRO,
	 .device = 0x8750,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	  },
	{
	 .vendor = VENDOR_O2MICRO,
	 .device = 0x8751,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	  },
	{
	 .vendor = VENDOR_O2MICRO,
	 .device = 0x8740,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	  },
	{
	 /* gg8 */
	 .vendor = VENDOR_O2MICRO,
	 .device = 0x9860,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	  },
	{
	 .vendor = VENDOR_O2MICRO,
	 .device = 0x9861,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	  },
	{
	 .vendor = VENDOR_O2MICRO,
	 .device = 0x9862,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	  },
	{
	 .vendor = VENDOR_O2MICRO,
	 .device = 0x9863,
	 .subvendor = PCI_ANY_ID,
	 .subdevice = PCI_ANY_ID,
	  },
	{ 0, 0 },
};

MODULE_DEVICE_TABLE(pci, bht_id_table);

/* ----------BIOS GUIDE setting part---------- */

void bht_bios_setting(bht_dev_ext_t *pdx)
{
	sd_host_t *host = &pdx->host;
	cfg_item_t *cfg = pdx->cfg;
	u32 regaddr;
	u32 regval;

	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s chip type = 0x%x\n", __func__, host->chip_type);

	switch (host->chip_type) {
	case CHIP_GG8:
		regval = pci_readl(host, 0x300);
		regval &= ~(0xff);
		regval |= 0x33;
		pci_writel(host, 0x300, regval);
		break;
	case CHIP_SEAEAGLE2:
		/* we need set this to enable hs400 function */
		if (cfg->card_item.emmc_mode.enable_force_hs400)
			pci_orl(host, 0x3f8, BIT13);
		else
			pci_andl(host, 0x3f8, ~BIT13);
		break;
	case CHIP_SDS2_SD0:
	case CHIP_SDS2_SD1:
		/* pcie & pm #2, should be the first operation after detect host type */
		pci_writel(host, 0x438, m_ram_ema);

		/* uhsi #1 */
		pci_orl(host, 0x3e4, 0x80000000);
		/* uhsi #3 */
		regaddr = (host->chip_type == CHIP_SDS2_SD0) ? 0x300 : 0x380;
		pci_writel(host, regaddr, INPUT_TUNING_PASS_WINDOW);

		/* ushi #4 */
		regval = pci_readl(host, 0x3f8);
		regval &= ~(0x03u << 19);
		regval |= (m_vdd18_debounce_time << 19);
		pci_orl(host, 0x3f8, regval);

		/* uhsi #6 */
		regaddr = host->chip_type == CHIP_SDS2_SD0 ? 0x304 : 0x384;
		regval = pci_readl(host, regaddr);
		regval &= 0xffff8881;
		regval |= ((m_sd_3v3_clk_driver_strength & 0x07) << 12 |
			   (m_sd_3v3_cmddata_driver_strength & 0x07) << 8 |
			   (m_sd_1v8_clk_driver_strength & 0x07) << 4 |
			   (m_sd_1v8_cmddata_driver_strength & 0x07) << 1 | 1);
		pci_writel(host, regaddr, regval);

		/* ushii #1  */
		regval = pci_readl(host, 0x3fc);
		regval &= ~0x7f000000;
		regval |= ((m_cnfg_drv & 0x7f) << 24);
		pci_writel(host, 0x3fc, regval);

		regval = pci_readl(host, 0x400);
		regval &= ~(0xfff);
		regval |= ((m_cnfg_trm_code_tx & 0x0f) << 8);
		regval |= ((m_cnfg_trm_code_rx & 0x0f) << 4);
		regval |= (m_cnfg_rint_code & 0x0f);
		pci_writel(host, 0x400, regval);

		/* uhsii 2 */
		switch (m_vdd1_vdd2_source) {
		case 0:
			regval = pci_readl(host, 0x508);
			/* bit[25:24] = 2'b00 */
			regval &= ~(0x3 << 24);
			/* bit[9:8] = 2'b00 */
			regval &= ~(0x3 << 8);
			regval &= ~(0xf << 2);
			/* bit[5:2]=4'b0101 */
			regval |= 0x14;
			break;
		case 1:
			regval = pci_readl(host, 0x508);
			/* bit[25:24] = 2'b11 */
			regval |= (0x3 << 24);
			/* bit[9:8] = 2'b11 */
			regval |= (0x3 << 8);
			/* bit[5:2]=4'b1111 */
			regval |= (0xf << 2);
			break;
		case 2:
			regval = pci_readl(host, 0x508);
			/* bit[25:24] = 2'b00 */
			regval &= ~(0x3 << 24);
			/* bit[25:24] = 2'b10 */
			regval |= (0x2 << 24);
			/* bit[9:8] = 2'b00 */
			regval &= ~(0x3 << 8);
			/* bit[9:8] = 2'b10 */
			regval |= (0x2 << 8);
			regval &= ~(0xf << 2);
			/* bit[5:2]=4'b1010 */
			regval |= 0x28;
			break;
		default:
			break;
		}
		pci_writel(host, 0x508, regval);

		/* clock & generator #1 */
		if (m_ssc_enable != 0) {
			pci_writel(host, 0x420, 0xa0005e1e);
			pci_writel(host, 0x424, 0x61180000);
		} else {
			pci_writel(host, 0x420, 0xa000561e);
			pci_writel(host, 0x424, 0x21180000);
		}
		/* clock & generator #2 */
		pci_writel(host, 0x428, 0x01f0f0fa);
		pci_writel(host, 0x42c, 0x01f0f0fa);
		break;
	default:
		break;
	}

}

#if BHT_LINUX_ENABLE_RTD3

static void bht_sd_runtime_pm_allow(bht_dev_ext_t *pdx, struct device *dev)
{
	if (pdx->pm_state.rtd3_en == FALSE)
		return;

	pm_runtime_put_noidle(dev);
	pm_runtime_allow(dev);
	pm_runtime_set_autosuspend_delay(dev,
					 pdx->cfg->feature_item.psd_mode.disk_idle_time_s * 1000);
	pm_runtime_use_autosuspend(dev);
	pm_suspend_ignore_children(dev, 1);
}

static void bht_sd_runtime_pm_forbid(bht_dev_ext_t *pdx, struct device *dev)
{

	if (pdx->pm_state.rtd3_en == FALSE)
		return;

	pm_runtime_forbid(dev);
	pm_runtime_get_noresume(dev);
}

static int bht_sd_runtime_suspend(struct device *dev)
{
	struct pci_dev *pdev = container_of(dev, struct pci_dev, dev);
	int ret = 0;
	bht_sd_slot_t *slot = pci_get_drvdata(pdev);
	bht_dev_ext_t *pdx = 0;

	DbgInfo(MODULE_OS_ENTRY, FEATURE_PM_TRACE, NOT_TO_RAM,
		"BHT sd rt supspend begin\n");

	if (slot == NULL || slot->used == FALSE)
		goto exit;

	pdx = &slot->data;
	pdx->pm_state.rtd3_entered = TRUE;
	req_enter_d3(pdx);

exit:
	DbgInfo(MODULE_OS_ENTRY, FEATURE_PM_TRACE, NOT_TO_RAM,
		"BHT sd rt supspend exit\n");
	return ret;
}

static int bht_sd_runtime_resume(struct device *dev)
{
	struct pci_dev *pdev = container_of(dev, struct pci_dev, dev);
	int ret = 0;
	bht_sd_slot_t *slot = pci_get_drvdata(pdev);
	bht_dev_ext_t *pdx = 0;

	DbgInfo(MODULE_OS_ENTRY, FEATURE_PM_TRACE, NOT_TO_RAM,
		"BHT sd rt resume begin\n");

	if (slot == NULL || slot->used == FALSE)
		goto exit;

	pdx = &slot->data;
	req_enter_d0_sync(pdx);

exit:
	DbgInfo(MODULE_OS_ENTRY, FEATURE_PM_TRACE, NOT_TO_RAM,
		"BHT sd rt resume exit\n");
	return ret;
}

static int bht_sd_runtime_idle(struct device *dev)
{
	return 0;
}

#endif

static irqreturn_t bht_sd_irq(int irq, void *dev_id)
{
	irqreturn_t irqret = IRQ_NONE;
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) dev_id;
	bool ret = FALSE;

	ret = sdhci_irq(pdx);

	if (pdx->host.pci_dev.use_msi)
		irqret |= IRQ_HANDLED;
	else {
		if (ret)
			irqret |= IRQ_HANDLED;
	}

	return irqret;
}

#ifdef CONFIG_PM
static int bht_sd_suspend(struct device *device)
{
	struct pci_dev *pdev = to_pci_dev(device);
	bht_sd_slot_t *slot = pci_get_drvdata(pdev);
	bht_dev_ext_t *pdx = &slot->data;

	DbgInfo(MODULE_OS_ENTRY, FEATURE_PM_TRACE, NOT_TO_RAM,
		"BHT sd suspend begin\n");

	if (slot->used == 0)
		goto exit;

	pdx->pm_state.s3s4_entered = TRUE;
	req_pre_enter_d3(pdx);
	req_enter_d3(pdx);

exit:
	DbgInfo(MODULE_OS_ENTRY, FEATURE_PM_TRACE, NOT_TO_RAM,
		"BHT sd suspend end\n");
	return 0;
}

static int bht_sd_resume(struct device *device)
{
	struct pci_dev *pdev = to_pci_dev(device);
	bht_sd_slot_t *slot = pci_get_drvdata(pdev);
	bht_dev_ext_t *pdx = &slot->data;

	DbgInfo(MODULE_OS_ENTRY, FEATURE_PM_TRACE, NOT_TO_RAM,
		"BHT sd resume begin\n");

	if (slot->used == 0)
		goto exit;

	req_enter_d0(pdx);

exit:
	DbgInfo(MODULE_OS_ENTRY, FEATURE_PM_TRACE, NOT_TO_RAM,
		"BHT sd resume end\n");
	return 0;
}

#else
#define bht_sd_suspend	NULL
#define bht_sd_resume	NULL
#endif

/*
 * pci dev interrupt init
 */
static int bht_pci_init_irq(struct pci_dev *pdev, bht_dev_ext_t *pdx)
{
	int ret = -1;

	pdx->host.pci_dev.use_msi = 0;

	/* Firstly try MSI interrupt if enabled */
#if defined(BHT_USE_PCI_MSI) && defined(CONFIG_PCI_MSI)
	if (pci_find_capability(pdev, PCI_CAP_ID_MSI)) {
		int err;
		/* PCI device support MSI interrupt mode. */
		err = pci_enable_msi(pdev);
		if (!err) {
			pdx->host.pci_dev.irq = pdev->irq;
			pdx->host.pci_dev.use_msi = 1;
			DbgInfo(MODULE_OS_ENTRY, FEATURE_DRIVER_INIT,
				NOT_TO_RAM, "Use Msi irq irq=%d\n", pdev->irq);
		} else {
			DbgErr("MSI init failed %d\n", err);
		}

		if (request_irq(pdev->irq, bht_sd_irq, IRQF_SHARED,
				(const char *)"bht-sd", (void *)pdx)) {
			DbgErr("request msi irq %d for bht-sd failed",
			       pdev->irq);
			pci_disable_msi(pdev);
			pdx->host.pci_dev.use_msi = 0;
		}
		ret = 0;
		goto exit;
	}
#endif

	pdx->host.pci_dev.irq = pdev->irq;
	pdx->host.pci_dev.use_msi = 0;
	if (request_irq(pdev->irq, bht_sd_irq, IRQF_SHARED,
			(const char *)"bht-sd", (void *)pdx)) {
		DbgErr("request irq %d for bht-sd failed", pdev->irq);
		ret = -1;
		goto exit;
	}

	ret = 0;

exit:
	return ret;

}

/*
 * pci device basci info init
 */
static int bht_pci_init(struct pci_dev *pdev, bht_dev_ext_t *pdx)
{
	int ret = -1;
	u32 bus_address;

	pdx->host.pci_dev.pci_dev = pdev;
	ret = pci_enable_device(pdev);
	if (ret) {
		DbgErr("pci enable device failed\n");
		return ret;
	}

	pci_set_master(pdev);

	bus_address = pci_resource_start(pdev, 0);
	if (pci_request_regions(pdev, "bht-sd") < 0) {
		DbgErr("o2sd request_region failed(%d)\n", 0);
		ret = -1;
		goto exit;
	}
	/* BAR 0 and BAR1 */
	bus_address = pci_resource_start(pdev, 0);
	pdx->host.pci_dev.membase =
	    (void *)ioremap(bus_address, pci_resource_len(pdev, 0));
	bus_address = pci_resource_start(pdev, 1);
	pdx->host.pci_dev.membase2 =
	    (void *)ioremap(bus_address, pci_resource_len(pdev, 1));

	DbgInfo(MODULE_OS_ENTRY, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"membase1=%p membase2=%p\n", pdx->host.pci_dev.membase,
		pdx->host.pci_dev.membase2);
	pdx->host.vendor_id = pdev->vendor;
	pdx->host.device_id = pdev->device;
	pdx->host.revision_id = pdev->revision;

	ret = 0;
exit:
	if (ret != 0)
		pci_disable_device(pdev);
	return ret;

}

/*
 * pci dev dma resource init
 */
static bool bht_sd_dma_init(bht_dev_ext_t *pdx)
{
	bool ret = FALSE;
	struct pci_dev *pdev = pdx->host.pci_dev.pci_dev;

#if	CONFIG_X86_64

#else

	/* Set to 32Bit DMA currently */
	cfg_dma_addr_range_dec(pdx->cfg, 0);
#endif

	if (pdx->cfg->host_item.test_dma_mode_setting.enable_dma_64bit_address) {
		if (dma_set_mask(&pdev->dev, DMA_BIT_MASK(64))
		    || dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(64)))
			DbgErr("dma_set_mask 64bit failed\n");
		else
			goto next;
	}

	if (dma_set_mask(&pdev->dev, DMA_BIT_MASK(32))
	    || dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32)))
		DbgErr("dma_set_mask 32bit failed\n");
next:

	if (pdx->signature != BHT_PDX_SIGNATURE) {
		u32 max_req_numb = 0;
		u32 dma_mode = 0;
		u32 buf_size = 0x10;
		u32 node_size = 0;

		max_req_numb =
		    pdx->cfg->host_item.test_tag_queue_capability.max_srb;
		dma_mode = pdx->cfg->host_item.test_dma_mode_setting.dma_mode;

		if (cfg_dma_need_sdma_like_buffer(dma_mode) == TRUE)
			node_size = MAX_SDMA_LIKE_MODE_NODE_BUF_SIZE;
		else
			node_size = MAX_NODE_BUF_SIZE;

		buf_size = (max_req_numb + TQ_RESERVED_NODE_SIZE) * node_size;

		while (1) {
			/* preseved for ADMA2 API DMA buffer */
			buf_size += MIN_DMA_API_BUF_SIZE;

			/* less than minimum buf size  will failed. */
			if (buf_size <
			    (MIN_DMA_API_BUF_SIZE + MAX_NODE_BUF_SIZE)) {
				DbgErr
				    ("dma buffer less than min(0x%x),so Failed\n",
				     (MIN_DMA_API_BUF_SIZE +
				      MAX_NODE_BUF_SIZE));
				break;
			}
			DbgInfo(MODULE_OS_ENTRY, FEATURE_DRIVER_INIT,
				NOT_TO_RAM, "os alloc dma buf len %x\n",
				buf_size);
			ret =
			    os_alloc_dma_buffer(pdx, NULL, buf_size,
						&pdx->dma_buff);
			if (ret == TRUE)
				break;
			/* small requirement size */
			buf_size = buf_size / 2;
		}
	}

	return ret;

}

/*
 * Release pci layer resource
 */
static void bht_sd_pci_release(bht_dev_ext_t *pdx)
{
	struct pci_dev *pdev = pdx->host.pci_dev.pci_dev;

	free_irq(pdx->host.pci_dev.irq, pdx);
	if (pdx->host.pci_dev.use_msi)
		pci_disable_msi(pdev);
	os_free_dma_buffer(pdx, &pdx->dma_buff);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

/*
 * Update Linux Cfg Setting
 */
static void bht_linux_cfg_update(bht_dev_ext_t *pdx)
{
	u32 dma_mode = pdx->cfg->host_item.test_dma_mode_setting.dma_mode;

	switch (dma_mode) {
	case CFG_TRANS_MODE_SDMA:
	case CFG_TRANS_MODE_ADMA2_SDMA_LIKE:
	case CFG_TRANS_MODE_PIO:
	case CFG_TRANS_MODE_ADMA_MIX_SDMA_LIKE:
		/* don't support tagqueue */
		pdx->cfg->host_item.test_tag_queue_capability.max_srb = 2;
		pdx->cfg->host_item.test_tag_queue_capability.enable_srb_merge =
		    0;
		break;
	case CFG_TRANS_MODE_ADMA2:
	case CFG_TRANS_MODE_ADMA3:
		break;
		/* Linux don't support adma3_sdma like mode */
	case CFG_TRANS_MODE_ADMA3_SDMA_LIKE:
		pdx->cfg->host_item.test_dma_mode_setting.dma_mode =
		    CFG_TRANS_MODE_ADMA3;
		break;
	default:
		break;
	}
	/* This function is add for degrade frequency for HS400 */
	if ((*(u32 *) (&pdx->cfg->card_item.emmc_mode)) & BIT13)
		pdx->cfg->dmdn_tbl[4] = 0x23040000;

	bht_bios_setting(pdx);
}

/*
 * pci device remove init
 */
static int bht_sd_probe(struct pci_dev *pdev, const struct pci_device_id *pid)
{
	bht_sd_slot_t *slot = bht_slot_getfree();
	bht_dev_ext_t *pdx = &slot->data;
	int ret = -1;

	DbgInfo(MODULE_OS_ENTRY, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"BHT sd probe begin\n");
	if (slot == NULL) {
		DbgErr("all slot is used\n");
		goto exit;
	}

	ret = bht_pci_init(pdev, &slot->data);
	if (ret)
		goto exit;

	pdx->host.pdx = pdx;
	hostven_chip_type_check(&pdx->host);
	/* Get the configuration pointer for the specified chip */
	pdx->cfg = cfgmng_get((void *)pdx, pdx->host.chip_type, FALSE);
	pdx->host.cfg = pdx->cfg;
	/*
	 * Linux Adjust the input parameter
	 */
	bht_linux_cfg_update(pdx);
	os_memset(&pdx->testcase, 0, sizeof(testcase_t));
	pdx->testcase.test_type = 0;

	if (bht_sd_dma_init(pdx) == FALSE) {
		ret = -1;
		DbgErr("DMA Init failed\n");
		pci_release_regions(pdev);
		pci_disable_device(pdev);
		goto exit;
	}

	req_global_init(pdx);
	ret = bht_pci_init_irq(pdev, pdx);
	if (ret) {
		ret = -1;
		DbgErr("Irq Init failed\n");
		os_free_dma_buffer(pdx, &pdx->dma_buff);
		pci_release_regions(pdev);
		pci_disable_device(pdev);
		goto exit;
	}

	if (pdx->card.card_present)
		os_set_event(&pdx->os, EVENT_CARD_CHG);

	ret = 0;

exit:
	if (ret == 0) {
		slot->used = 1;
		pci_set_drvdata(pdev, slot);
#if BHT_LINUX_ENABLE_RTD3
		bht_sd_runtime_pm_allow(pdx, &pdev->dev);
#endif
	}

	DbgInfo(MODULE_OS_ENTRY, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"BHT sd probe end ret=%d\n", ret);
	return ret;

}

/*
 * pci device remove interface
 * Thomas change for direct remove
 */
void bht_sd_remove(struct pci_dev *pdev)
{
	bht_sd_slot_t *slot = pci_get_drvdata(pdev);
	bht_dev_ext_t *pdx = &slot->data;

	DbgInfo(MODULE_OS_ENTRY, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"BHT sd remove begin\n");

	if (slot->used == FALSE)
		goto exit;

#if BHT_LINUX_ENABLE_RTD3
	bht_sd_runtime_pm_forbid(pdx, &pdev->dev);
#endif
	bht_scsi_uinit(pdx);

	/*
	 * Thomas comment because it cause AER 0xe6 and block the remove.
	 *
	 * if(os_stop_thread(&pdx->os, &pdx->os.thread) == FALSE)
	 * {
	 *      DbgErr("stop thread timeout\n");
	 *      os_kill_thread(&pdx->os, &pdx->os.thread);
	 * }
	 *
	 */

	req_global_uninit(pdx);
	bht_sd_pci_release(pdx);
	os_memset(slot, 0, sizeof(bht_sd_slot_t));
	pci_set_drvdata(pdev, NULL);

exit:
	DbgInfo(MODULE_OS_ENTRY, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"BHT sd remove end\n");

}

/*
 * pci_dev shutdown interface
 */
static void bht_sd_shutdown(struct pci_dev *pdev)
{
	bht_sd_slot_t *slot = pci_get_drvdata(pdev);
	bht_dev_ext_t *pdx = &slot->data;

	if (slot->used == FALSE)
		return;

	if (os_stop_thread(&pdx->os, &pdx->os.thread) == FALSE) {
		DbgErr("stop thread timeout\n");
		os_kill_thread(&pdx->os, &pdx->os.thread);
	}
	req_global_uninit(pdx);
	bht_sd_pci_release(pdx);
	pci_set_drvdata(pdev, NULL);
	os_memset(slot, 0, sizeof(bht_sd_slot_t));
}

static const struct dev_pm_ops bht_sd_pm_ops = {
	.suspend = bht_sd_suspend,
	.resume = bht_sd_resume,
	.freeze = bht_sd_suspend,
	.thaw = bht_sd_resume,
	.poweroff = bht_sd_suspend,
	.restore = bht_sd_resume,
#if	BHT_LINUX_ENABLE_RTD3
	SET_RUNTIME_PM_OPS(bht_sd_runtime_suspend,
			   bht_sd_runtime_resume, bht_sd_runtime_idle)
#endif
};

static struct pci_driver bht_sd_pci_driver = {
	.name = "bht-sd",
	.id_table = bht_id_table,
	.probe = bht_sd_probe,
	.remove = bht_sd_remove,
	.shutdown = bht_sd_shutdown,
	.driver = {
		   .pm = &bht_sd_pm_ops },
};

/*
 * Uninit linux global resource
 */
static void bht_linux_global_uninit(void)
{
	if (bht_sd_mem_pool)
		mempool_destroy(bht_sd_mem_pool);

	if (bht_srb_ext_cachep)
		kmem_cache_destroy(bht_srb_ext_cachep);

	bht_srb_ext_cachep = NULL;
	bht_sd_mem_pool = NULL;
}

/*
 * init linux global resource
 */
static int bht_linux_global_init(void)
{
	int ret = 0;

	bht_srb_ext_cachep = NULL;
	bht_sd_mem_pool = NULL;

	bht_srb_ext_cachep =
	    kmem_cache_create("bht-sd-srbext", sizeof(srb_ext_t), 0,
			      SLAB_HWCACHE_ALIGN, NULL);

	if (bht_srb_ext_cachep == NULL) {
		ret = -1;
		DbgErr("Allocate Memory cache failed\n");
		goto exit;
	}

	bht_sd_mem_pool =
	    mempool_create(LINUX_SCSI_MAX_QUEUE_DPETH * 2, mempool_alloc_slab,
			   mempool_free_slab, bht_srb_ext_cachep);
exit:
	if (bht_sd_mem_pool == NULL) {
		DbgErr("Allocate Memory Pool failed\n");
		bht_linux_global_uninit();
		ret = -1;
	}

	return ret;

}

/*
 * bht_sd_init - First called function when driver load/insert
 */
static int __init bht_sd_init(void)
{
	int ret = 0;

	ret = bht_linux_global_init();
	if (ret)
		goto exit;

	bht_global_init();

	ret = pci_register_driver(&bht_sd_pci_driver);

exit:
	if (ret != 0) {
		bht_linux_global_uninit();
		DbgErr("Register bht pci driver failed\n");
		bht_global_uninit();
	}

	return ret;
}

/*
 * bht_sd_exit - First called when dirver unload/remove
 */
static void __exit bht_sd_exit(void)
{
	pci_unregister_driver(&bht_sd_pci_driver);
	bht_linux_global_uninit();
	bht_global_uninit();
}

/*
 * Driver parameter and Information
 */

/* set first call function name when driver insert/load. */
module_init(bht_sd_init);
/* set first call function name when driver remove/unload. */
module_exit(bht_sd_exit);
MODULE_DESCRIPTION("BayHub SD Card reader device driver");
MODULE_AUTHOR("BayHub Inc.");
#if BHT_LINUX_ENABLE_RTD3
MODULE_LICENSE("GPL");
#else
MODULE_LICENSE("GPL");
#endif

MODULE_VERSION("v1008_00_00");
