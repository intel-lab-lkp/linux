// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: hostven.c
 *
 * Abstract: Include host vendor defined operations
 *
 * Version: 1.00
 *
 * Author: Samuel
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 9/2/2014   Creation    Samuel
 */

#include "../include/basic.h"
#include "hostreg.h"
#include "../include/hostapi.h"
#include "../include/debug.h"
#include "../include/hostvenapi.h"
#include "../include/cfgmng.h"
#include "../include/reqapi.h"

void host_enable_clock(sd_host_t *host, bool on);
void host_internal_clk_setup(sd_host_t *host, bool on);
void host_init_clock(sd_host_t *host, u32 value);
void host_init_400k_clock(sd_host_t *host);

/* PCI 16bit access */
u16 pci_readw(sd_host_t *host, u16 offset)
{
	u32 i = 0;
	u32 tmp[3] = { 0 };
	u16 reg_val = 0;

	if ((host->chip_type == CHIP_SDS0) ||
	    (host->chip_type == CHIP_SDS1) ||
	    (host->chip_type == CHIP_FUJIN2) ||
	    (host->chip_type == CHIP_SEABIRD) ||
	    (host->chip_type == CHIP_SEAEAGLE)) {
		/*
		 * For Sandstorm, HW implement a mapping method by
		 * memory space reg to access PCI reg.
		 */

		/* Enable mapping */

		/* Check function conflict */
		if ((host->chip_type == CHIP_SDS0) ||
		    (host->chip_type == CHIP_FUJIN2) ||
		    (host->chip_type == CHIP_SEABIRD) ||
		    (host->chip_type == CHIP_SEAEAGLE)) {
			i = 0;
			ven_writel(host, VEN_PCIRMappingEn, 0x40000000);
			while ((ven_readl(host, VEN_PCIRMappingEn) & 0x40000000)
			       == 0) {
				if (i == 5)
					goto RD_DIS_MAPPING;

				os_mdelay(1);
				i++;
				ven_writel(host, VEN_PCIRMappingEn, 0x40000000);

			}
		} else if (host->chip_type == CHIP_SDS1) {
			i = 0;

			ven_writel(host, VEN_PCIRMappingEn, 0x20000000);
			while ((ven_readl(host, VEN_PCIRMappingEn) & 0x20000000)
			       == 0) {
				if (i == 5)
					goto RD_DIS_MAPPING;

				os_mdelay(1);
				i++;
				ven_writel(host, VEN_PCIRMappingEn, 0x20000000);
			}
		}

		/* Check last operation is complete */
		i = 0;
		while (ven_readl(host, VEN_PCIRMappingCtl) & 0xc0000000) {
			if ((i == 5)
			    || ven_readl(host,
					 VEN_PCIRMappingCtl) == 0xffffffff) {
				goto RD_DIS_MAPPING;
			}

			os_mdelay(1);
			i += 1;
		}

		/* Set register address due to hardware constraint */

		tmp[0] |= 0x40000000;
		tmp[0] |= offset & 0xfffc;
		ven_writel(host, VEN_PCIRMappingCtl, tmp[0]);

		/* Check read is complete */
		i = 0;
		while (ven_readl(host, VEN_PCIRMappingCtl) & 0x40000000) {
			if ((i == 5)
			    || ven_readl(host,
					 VEN_PCIRMappingCtl) == 0xffffffff) {
				goto RD_DIS_MAPPING;
			}

			os_mdelay(1);
			i += 1;
		}

		/* Get PCIR value */
		tmp[1] = ven_readl(host, VEN_PCIRMappingVal);

		if (offset & 0x2)
			tmp[1] >>= 16;

RD_DIS_MAPPING:
		/* Disable mapping */
		ven_writel(host, VEN_PCIRMappingEn, 0x80000000);

		DbgInfo(MODULE_VEN_HOST, FEATURE_PCIREG_TRACER, NOT_TO_RAM,
			"%s offset=%x Value:%x\n", __func__, offset,
			(u16) tmp[1]);
		return (u16) tmp[1];
	} else if (host->chip_type == CHIP_SEAEAGLE2) {
		reg_val = ven_readw(host, offset);
		DbgInfo(MODULE_VEN_HOST, FEATURE_PCIREG_TRACER, NOT_TO_RAM,
			"%s offset=%x Value:%x\n", __func__, offset,
			reg_val);
		return reg_val;
	} else if (host->chip_type == CHIP_GG8
		   || host->chip_type == CHIP_ALBATROSS) {
		reg_val = ven_readw(host, offset);
		DbgInfo(MODULE_VEN_HOST, FEATURE_PCIREG_TRACER, NOT_TO_RAM,
			"%s offset=%x Value:%x\n", __func__, offset,
			reg_val);
		return reg_val;
	}

	DbgInfo(MODULE_VEN_HOST, FEATURE_PCIREG_TRACER, NOT_TO_RAM,
		"%s offset=%x Value:%x\n", __func__, offset, (u16) tmp[0]);
	return (u16) tmp[0];

}

void pci_writew(sd_host_t *host, u16 offset, u16 value)
{
	u32 tmp = 0;
	u32 i = 0;

	DbgInfo(MODULE_VEN_HOST, FEATURE_PCIREG_TRACEW, NOT_TO_RAM,
		"%s, Addr:%x, Value: %x\n", __func__, offset, value);

	if ((host->chip_type == CHIP_SDS0) ||
	    (host->chip_type == CHIP_SDS1) ||
	    (host->chip_type == CHIP_FUJIN2) ||
	    (host->chip_type == CHIP_SEABIRD) ||
	    (host->chip_type == CHIP_SEAEAGLE)) {
		/*
		 * For Sandstorm, HW implement a mapping method by
		 * memory space reg to access PCI reg.
		 * Upper caller doesn't need to set 0xD0.
		 */

		/* Enable mapping */

		/* Check function conflict */
		if ((host->chip_type == CHIP_SDS0) ||
		    (host->chip_type == CHIP_FUJIN2) ||
		    (host->chip_type == CHIP_SEABIRD) ||
		    (host->chip_type == CHIP_SEAEAGLE)) {
			i = 0;
			ven_writel(host, VEN_PCIRMappingEn, 0x40000000);
			while ((ven_readl(host, VEN_PCIRMappingEn) & 0x40000000)
			       == 0) {
				if (i == 5)
					goto WR_DIS_MAPPING;

				os_mdelay(1);
				i++;
				ven_writel(host, VEN_PCIRMappingEn, 0x40000000);
			}
		} else if (host->chip_type == CHIP_SDS1) {
			i = 0;
			ven_writel(host, VEN_PCIRMappingEn, 0x20000000);

			while ((ven_readl(host, VEN_PCIRMappingEn) & 0x20000000)
			       == 0) {
				if (i == 5)
					goto WR_DIS_MAPPING;

				os_mdelay(1);
				i++;
				ven_writel(host, VEN_PCIRMappingEn, 0x20000000);
			}
		}

		/* Enable MEM access */
		ven_writel(host, VEN_PCIRMappingVal, 0x80000000);
		ven_writel(host, VEN_PCIRMappingCtl, 0x800000D0);

		/* Check last operation is complete */
		i = 0;
		while (ven_readl(host, VEN_PCIRMappingCtl) & 0xc0000000) {
			if ((i == 5)
			    || ven_readl(host,
					 VEN_PCIRMappingCtl) == 0xffffffff) {
				goto WR_DIS_MAPPING;
			}

			os_mdelay(1);
			i += 1;
		}

		/* Set write value */
		if (offset & 0x2) {
			u32 val32 = value;

			val32 <<= 16;
			offset = offset & 0xfffc;
			ven_writel(host, VEN_PCIRMappingVal, val32);
			/* Set register address */
			tmp |= 0x80030000;
			tmp |= offset;
		} else {
			ven_writel(host, VEN_PCIRMappingVal, value);
			/* Set register address */
			tmp |= 0x800c0000;
			tmp |= offset;
		}
		ven_writel(host, VEN_PCIRMappingCtl, tmp);

		/* Check write is complete */
		i = 0;
		while (ven_readl(host, VEN_PCIRMappingCtl) & 0x80000000) {
			if ((i == 5)
			    || ven_readl(host,
					 VEN_PCIRMappingCtl) == 0xffffffff) {
				goto WR_DIS_MAPPING;
			}

			os_mdelay(1);
			i += 1;
		}

WR_DIS_MAPPING:
		/* Disable MEM access */
		ven_writel(host, VEN_PCIRMappingVal, 0x80000001);
		ven_writel(host, VEN_PCIRMappingCtl, 0x800000D0);

		/* Check last operation is complete */
		i = 0;
		while (ven_readl(host, VEN_PCIRMappingCtl) & 0xc0000000) {
			if ((i == 5)
			    || ven_readl(host,
					 VEN_PCIRMappingCtl) == 0xffffffff) {
				break;
			}

			os_mdelay(1);
			i += 1;
		}

		/* Disable function conflict */

		/* Disable mapping */
		ven_writel(host, VEN_PCIRMappingEn, 0x80000000);
	} else if (host->chip_type == CHIP_SEAEAGLE2) {
		ven_writew(host, offset, value);
	} else if (host->chip_type == CHIP_GG8
		   || host->chip_type == CHIP_ALBATROSS) {
		ven_writew(host, offset, value);
	}
}

void pci_orw(sd_host_t *host, u16 offset, u16 value)
{
	u16 reg_val = 0;

	reg_val = pci_readw(host, offset);
	reg_val |= value;
	pci_writew(host, offset, reg_val);
	DbgInfo(MODULE_SD_HOST, FEATURE_VENREG_TRACER, NOT_TO_RAM,
		"[PCI] pci orw(0x%08X): 0x%08X\n", offset, reg_val);
}

void pci_andw(sd_host_t *host, u16 offset, u16 value)
{
	u16 reg_val = 0;

	reg_val = pci_readw(host, offset);
	reg_val &= value;
	pci_writew(host, offset, reg_val);
	DbgInfo(MODULE_SD_HOST, FEATURE_VENREG_TRACER, NOT_TO_RAM,
		"[PCI] pci andw(0x%08X): 0x%08X\n", offset, reg_val);
}

/* PCI 32bit access */
u32 pci_readl(sd_host_t *host, u16 offset)
{
	u32 result = 0;
	u32 i = 0;
	u32 tmp[3] = { 0 };
	u32 reg_val = 0;

	if ((host->chip_type == CHIP_SDS0) ||
	    (host->chip_type == CHIP_SDS1) ||
	    (host->chip_type == CHIP_FUJIN2) ||
	    (host->chip_type == CHIP_SEABIRD) ||
	    (host->chip_type == CHIP_SEAEAGLE)) {
		/*
		 * For Sandstorm, HW implement a mapping method by
		 * memory space reg to access PCI reg.
		 */

		/* Enable mapping */

		/* Check function conflict */
		if ((host->chip_type == CHIP_SDS0) ||
		    (host->chip_type == CHIP_FUJIN2) ||
		    (host->chip_type == CHIP_SEABIRD) ||
		    (host->chip_type == CHIP_SEAEAGLE)) {
			i = 0;
			ven_writel(host, VEN_PCIRMappingEn, 0x40000000);
			while ((ven_readl(host, VEN_PCIRMappingEn) & 0x40000000)
			       == 0) {
				if (i == 5)
					goto RD_DIS_MAPPING;

				os_mdelay(1);
				i++;
				ven_writel(host, VEN_PCIRMappingEn, 0x40000000);

			}
		} else if (host->chip_type == CHIP_SDS1) {
			i = 0;

			ven_writel(host, VEN_PCIRMappingEn, 0x20000000);
			while ((ven_readl(host, VEN_PCIRMappingEn) & 0x20000000)
			       == 0) {
				if (i == 5)
					goto RD_DIS_MAPPING;

				os_mdelay(1);
				i++;
				ven_writel(host, VEN_PCIRMappingEn, 0x20000000);
			}
		}

		/* Check last operation is complete */
		i = 0;
		while (ven_readl(host, VEN_PCIRMappingCtl) & 0xc0000000) {
			if ((i == 5)
			    || ven_readl(host,
					 VEN_PCIRMappingCtl) == 0xffffffff) {
				goto RD_DIS_MAPPING;
			}

			os_mdelay(1);
			i += 1;
		}

		/* Set register address */
		tmp[0] |= 0x40000000;
		tmp[0] |= offset;
		ven_writel(host, VEN_PCIRMappingCtl, tmp[0]);

		/* Check read is complete */
		i = 0;
		while (ven_readl(host, VEN_PCIRMappingCtl) & 0x40000000) {
			if ((i == 5)
			    || ven_readl(host,
					 VEN_PCIRMappingCtl) == 0xffffffff) {
				goto RD_DIS_MAPPING;
			}

			os_mdelay(1);
			i += 1;
		}

		/* Get PCIR value */
		tmp[1] = ven_readl(host, VEN_PCIRMappingVal);

RD_DIS_MAPPING:
		/* Disable mapping */
		ven_writel(host, VEN_PCIRMappingEn, 0x80000000);

		DbgInfo(MODULE_VEN_HOST, FEATURE_PCIREG_TRACER, NOT_TO_RAM,
			"%s offset=%x Value:%x\n", __func__, offset,
			tmp[1]);
		result = tmp[1];
	} else if (host->chip_type == CHIP_SEAEAGLE2) {
		reg_val = ven_readl(host, offset);
		DbgInfo(MODULE_VEN_HOST, FEATURE_PCIREG_TRACER, NOT_TO_RAM,
			"%s offset=%x Value:%x\n", __func__, offset,
			reg_val);
		result = reg_val;
	} else if (host->chip_type == CHIP_GG8
		   || host->chip_type == CHIP_ALBATROSS) {
		reg_val = ven_readl(host, offset);
		DbgInfo(MODULE_VEN_HOST, FEATURE_PCIREG_TRACER, NOT_TO_RAM,
			"%s offset=%x Value:%x\n", __func__, offset,
			reg_val);
		result = reg_val;
	} else {

		DbgInfo(MODULE_VEN_HOST, FEATURE_PCIREG_TRACER, NOT_TO_RAM,
			"%s offset=%x Value:%x\n", __func__, offset,
			tmp[0]);
		result = tmp[0];
	}

	return result;
}

void pci_writel(sd_host_t *host, u16 offset, u32 value)
{
	u32 tmp = 0;
	u32 i = 0;

	DbgInfo(MODULE_VEN_HOST, FEATURE_PCIREG_TRACEW, NOT_TO_RAM,
		"%s, Addr:%x, Value:%x\n", __func__, offset, value);
	if ((host->chip_type == CHIP_SDS0) || (host->chip_type == CHIP_SDS1)
	    || (host->chip_type == CHIP_FUJIN2)
	    || (host->chip_type == CHIP_SEABIRD)
	    || (host->chip_type == CHIP_SEAEAGLE)) {
		/*
		 * For Sandstorm, HW implement a mapping method by
		 * memory space reg to access PCI reg.
		 * Upper caller doesn't need to set 0xD0.
		 */

		/* Enable mapping */

		/* Check function conflict */
		if ((host->chip_type == CHIP_SDS0) ||
		    (host->chip_type == CHIP_FUJIN2) ||
		    (host->chip_type == CHIP_SEABIRD) ||
		    (host->chip_type == CHIP_SEAEAGLE)) {
			i = 0;
			ven_writel(host, VEN_PCIRMappingEn, 0x40000000);
			while ((ven_readl(host, VEN_PCIRMappingEn) & 0x40000000)
			       == 0) {
				if (i == 5)
					goto WR_DIS_MAPPING;

				os_mdelay(1);
				i++;
				ven_writel(host, VEN_PCIRMappingEn, 0x40000000);
			}
		} else if (host->chip_type == CHIP_SDS1) {
			i = 0;
			ven_writel(host, VEN_PCIRMappingEn, 0x20000000);

			while ((ven_readl(host, VEN_PCIRMappingEn) & 0x20000000)
			       == 0) {
				if (i == 5)
					goto WR_DIS_MAPPING;

				os_mdelay(1);
				i++;
				ven_writel(host, VEN_PCIRMappingEn, 0x20000000);
			}
		}

		/* Enable MEM access */
		ven_writel(host, VEN_PCIRMappingVal, 0x80000000);
		ven_writel(host, VEN_PCIRMappingCtl, 0x800000D0);

		/* Check last operation is complete */
		i = 0;
		while (ven_readl(host, VEN_PCIRMappingCtl) & 0xc0000000) {
			if ((i == 5)
			    || ven_readl(host,
					 VEN_PCIRMappingCtl) == 0xffffffff) {
				goto WR_DIS_MAPPING;
			}

			os_mdelay(1);
			i += 1;
		}

		/* Set write value */
		ven_writel(host, VEN_PCIRMappingVal, value);
		/* Set register address */
		tmp |= 0x80000000;
		tmp |= offset;
		ven_writel(host, VEN_PCIRMappingCtl, tmp);

		/* Check write is complete */
		i = 0;
		while (ven_readl(host, VEN_PCIRMappingCtl) & 0x80000000) {
			if ((i == 5)
			    || ven_readl(host,
					 VEN_PCIRMappingCtl) == 0xffffffff) {
				goto WR_DIS_MAPPING;
			}

			os_mdelay(1);
			i += 1;
		}

WR_DIS_MAPPING:
		/* Disable MEM access */
		ven_writel(host, VEN_PCIRMappingVal, 0x80000001);
		ven_writel(host, VEN_PCIRMappingCtl, 0x800000D0);

		/* Check last operation is complete */
		i = 0;
		while (ven_readl(host, VEN_PCIRMappingCtl) & 0xc0000000) {
			if ((i == 5)
			    || ven_readl(host,
					 VEN_PCIRMappingCtl) == 0xffffffff) {
				break;
			}

			os_mdelay(1);
			i += 1;
		}

		/* Disable function conflict */

		/* Disable mapping */
		ven_writel(host, VEN_PCIRMappingEn, 0x80000000);
	} else if (host->chip_type == CHIP_SEAEAGLE2) {
		ven_writel(host, offset, value);
	} else if (host->chip_type == CHIP_GG8
		   || host->chip_type == CHIP_ALBATROSS) {
		ven_writel(host, offset, value);
	}

}

void pci_orl(sd_host_t *host, u16 offset, u32 value)
{
	u32 reg_val = 0;

	reg_val = pci_readl(host, offset);
	reg_val |= value;
	pci_writel(host, offset, reg_val);
}

void pci_andl(sd_host_t *host, u16 offset, u32 value)
{
	u32 reg_val = 0;

	reg_val = pci_readl(host, offset);
	reg_val &= value;
	pci_writel(host, offset, reg_val);

}

void hostven_update_dmdn(sd_host_t *host, u32 dmdn)
{
	u32 regval;

	DbgInfo(MODULE_VEN_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, dmdn = %x\n", __func__, dmdn);
	if ((host->chip_type == CHIP_SDS0) || (host->chip_type == CHIP_FUJIN2)
	    || (host->chip_type == CHIP_SEABIRD)
	    || (host->chip_type == CHIP_SEAEAGLE)
	    || (host->chip_type == CHIP_SEAEAGLE2)
	    || (host->chip_type == CHIP_GG8)
	    || (host->chip_type == CHIP_ALBATROSS)) {
		/* 0x304[28:16] */
		regval = pci_readl(host, 0x304);
		regval &= 0x0000ffff;
		regval |= (dmdn << 16);
		pci_writel(host, 0x304, regval);
	} else if (host->chip_type == CHIP_SDS1) {
		/* 0x384[28:16] */
		regval = pci_readl(host, 0x384);
		regval &= 0x0000ffff;
		regval |= (dmdn << 16);
		pci_writel(host, 0x384, regval);
	}

	DbgInfo(MODULE_VEN_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);

}

static bool dma_need_host_infinite_support(u32 dma_mode)
{
	if ((dma_mode == CFG_TRANS_MODE_ADMA2) ||
	    (dma_mode == CFG_TRANS_MODE_ADMA2_SDMA_LIKE) ||
	    (dma_mode == CFG_TRANS_MODE_ADMA_MIX_SDMA_LIKE) ||
	    (dma_mode == CFG_TRANS_MODE_ADMA_MIX))
		return TRUE;
	else
		return FALSE;
}

void hostven_drive_strength_cfg(sd_host_t *host)
{
	u32 regval;
	u32 i;

	DbgInfo(MODULE_VEN_HOST, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	for (i = 0; i < MAX_PCR_SETTING_SIZE; i++) {
		if ((host->cfg->pcr_item.pcr_tb[i].valid_flg == 0)
		    && (host->cfg->pcr_item.pcr_tb[i].addr == 0x304)) {
			DbgInfo(MODULE_VEN_HOST, FEATURE_RW_TRACE, NOT_TO_RAM,
				"PCR 0x304 set is invalid.\n");
			goto exit;
		}
	}

	regval = pci_readl(host, 0x304);

	if (regval & BIT7) {
		DbgInfo(MODULE_VEN_HOST, FEATURE_RW_TRACE, NOT_TO_RAM,
			"BIOS setting enable.\n");
	} else if (host->cfg->host_item.host_drive_strength.ds_selection_enable) {
		DbgInfo(MODULE_VEN_HOST, FEATURE_RW_TRACE, NOT_TO_RAM,
			"Host's SD IO drive setting enable.\n");

		/* Unlock write protect */
		pci_andl(host, 0xd0, ~BIT31);

		/*
		 * If detect PCR 0x304[7] = 0,
		 * please set host_drive_strength[14:12, 10:8, 6:4, 3:1] value
		 * to PCR 0x304[14:12, 10, 8, 6 : 4, 3 : 1] when driver loading
		 */

		regval &= ~0x0000777E;
		regval |=
		    (host->cfg->host_item.host_drive_strength.clk_driver_strength_3_3v << 12 |
			host->cfg->host_item.host_drive_strength.data_cmd_driver_strength_3_3v << 8 |
			host->cfg->host_item.host_drive_strength.clk_driver_strength_1_8v << 4 |
			host->cfg->host_item.host_drive_strength.cmd_driver_strength_1_8v << 1);

		pci_writel(host, 0x304, regval);

		/* Lock write protect  */
		pci_orl(host, 0xd0, BIT31);
	} else {
		DbgInfo(MODULE_VEN_HOST, FEATURE_RW_TRACE, NOT_TO_RAM,
			"Host's SD IO drive setting disable.\n");
	}

exit:
	DbgInfo(MODULE_VEN_HOST, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

void hostven_transfer_init(sd_host_t *host, bool enable)
{
	DbgInfo(MODULE_VEN_HOST, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s, enable = %x\n", __func__, enable);

	/* If not ADMA2 infinite transfer return */
	if (FALSE ==
	    dma_need_host_infinite_support(
			host->cfg->host_item.test_dma_mode_setting.dma_mode))
		return;

	/* when don't use inf while inifinte is enable */
	if (enable) {
		sdhci_or32(host, SDHCI_DRIVER_CTRL_REG,
			   SDHCI_DRIVER_CTRL_ADMA2_ENABLE_INF);
	} else {
		sdhci_and32(host, SDHCI_DRIVER_CTRL_REG,
			    ~(SDHCI_DRIVER_CTRL_ADMA2_ENABLE_INF));
	}

	DbgInfo(MODULE_VEN_HOST, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);

}

static void hostven_bios_cfg(sd_host_t *host)
{
	bht_dev_ext_t *pdx = host->pdx;
	u16 regval;
	u32 pcr_item_index = 0;

	cfg_vdd_power_source_item_t *cfg =
		&host->cfg->host_item.vdd_power_source_item;

	/* initial setting for hsmux_vcme  */
	cfg_hsmux_vcme_enable_item_t *cfg_hsmux_vcme =
		&host->cfg->feature_item.hsmux_vcme_enable;

	DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/*
	 * Stop the soft L0 request for d3silence sub mode 2 because
	 * the sub mode 2 will open the soft L0 request at the card removal handle
	 */
	if (pdx->pm_state.d3_silc_en && pdx->pm_state.d3_silc_submode2_en)
		pci_andl(host, 0x3e4, ~(1 << 23));

	if (host->cfg->driver_item.camera_mode_ctrl_vdd1_vdd2_cd == 1)
		goto camera_mode;
	else
		goto pc_mode;

pc_mode:
	/* for GG8: thomas.hu add for VDD 1,2,3 power source default setting. */

	DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"vdd_power_source_item: 0x%08X\n", *(u32 *) cfg);

	/* set vdd1 power source select internal/external and polarity  */
	regval = ven_readw(host, SDBAR1_GPIO_FUNC_SEL_508);

	/* clear field for vdd1 power source select internal/external and polarity */
	regval &= ~(7 << 2);

	/* bit4: external, bit3: internal */
	regval |=
	    (cfg->vdd1_power_source ==
	     VDDX_PWR_SOURCE_EXTERNAL) ? (1 << 4) : (1 << 3);
	/* bit2: polarity: 1 for active high, 0 for low */
	regval |=
	    (cfg->vdd1_onoff_polarity ==
	     VDDX_POLARITY_ACTIVE_HIGH) ? (1 << 2) : (0 << 2);
	ven_writew(host, SDBAR1_GPIO_FUNC_SEL_508, regval);

	DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"cfg->vdd1_power_source: 0x%X\n", cfg->vdd1_power_source);
	DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"cfg->vdd1_onoff_polarity: 0x%X\n", cfg->vdd1_onoff_polarity);

	/* set Vdd2 power source */
	regval = ven_readw(host, SDBAR1_GPIO_FUNC_SEL_508);
	/* use internal LDO */
	if (cfg->vdd2_power_source == VDDX_PWR_SOURCE_INTERNAL) {
		/* enable vdd2 internal powersource */
		regval &= ~(1 << 9);
	}
	/* use external power source. */
	else {
		/* enable vdd2 external powersource */
		regval |= (1 << 9);
	}
	ven_writew(host, SDBAR1_GPIO_FUNC_SEL_508, regval);

	DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"cfg->vdd2_power_source: 0x%X\n", cfg->vdd2_power_source);

	/* set Vdd3 power source */
	regval = ven_readw(host, SDBAR1_GPIO_FUNC_SEL_508);

	/* use internal LDO */
	if (cfg->vdd3_power_source == VDDX_PWR_SOURCE_INTERNAL) {
		/* enable vdd3 internal powersource */
		regval &= ~(1 << 13);
	}
	/* use external power source. */
	else {
		/* enable vdd3 external powersource */
		regval |= (1 << 13);
	}
	ven_writew(host, SDBAR1_GPIO_FUNC_SEL_508, regval);

	DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"cfg->vdd3_power_source: 0x%X\n", cfg->vdd3_power_source);

	/* set vdd2/3's control signal: GPIO 1/2 default value */
	if (!(shift_bit_func_enable(host))) {
		regval = ven_readw(host, SDBAR1_GPIO_FUNC_GPIOCTRL_510);
		regval &= (u16)(~(0xFFFF));
		regval |= (0x0808);
		ven_writew(host, SDBAR1_GPIO_FUNC_GPIOCTRL_510, regval);
	}
	/* set vdd2/3's GPIO power control inverter  */
	regval = ven_readw(host, SDBAR1_GPIO_FUNC_GPIOCTRL_510);
	regval &= ~(1 << 7 | 1 << 15);
	ven_writew(host, SDBAR1_GPIO_FUNC_GPIOCTRL_510, regval);

	DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"cfg_hsmux_vcme: 0x%X\n", *cfg_hsmux_vcme);
	if (cfg_hsmux_vcme->enable) {
		regval =
		    (cfg_hsmux_vcme->rc_rx_vcme << 3) |
			(cfg_hsmux_vcme->sd_tx_vcme << 2) |
		    (cfg_hsmux_vcme->sd_rx_vcme << 1) |
			(cfg_hsmux_vcme->rc_tx_vcme << 0);
		/* clear 440 bit[31:28] and set 440 bit[31:28] */
		pci_andw(host, 0x442, (u16)(~(0xF << 12)));
		pci_orw(host, 0x442, (regval << 12));
	}

	/*
	 * set host IO drive strength, PCR 0x304 bit7 is control switch.
	 * 0: set host IO drive strength by driver;
	 * 1: set host IO drive strength by BIOS
	 */
	if ((pci_readw(host, 0x304) & BIT7)) {
		for (pcr_item_index = 0;
		     pcr_item_index < host->cfg->pcr_item.cnt;
		     pcr_item_index++) {
			if ((host->cfg->pcr_item.pcr_tb[pcr_item_index].type == 0)
				&& (host->cfg->pcr_item.pcr_tb[pcr_item_index].addr == 0x304)) {
				host->cfg->pcr_item.pcr_tb[pcr_item_index].valid_flg = 0;
				DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT,
					NOT_TO_RAM,
					"disabled PCR 0x304 pcr config valid_flg\n");
			}
		}
	}

	/* set Uhs2 L0 clock request mode control in non-dormant state */
	if (host->chip_type == CHIP_SEAEAGLE) {
		pci_andl(host, 0x35C, 0xFFFFFFFC);
		DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
			"uhs2_setting.l1_requirement_source: 0x%X\n",
			host->cfg->card_item.uhs2_setting.l1_requirement_source);
		pci_orl(host, 0x35C,
			host->cfg->card_item.uhs2_setting.l1_requirement_source);
	}

	goto exit;

camera_mode:

	/* enable GPIO3 input, polling solution */
	regval = ven_readw(host, SDBAR1_WP_GPIO3_CTRL_REG_514);
	/* clear bit [4:0] */
	regval &= ~0x1f;
	regval |= (1 << 1 | 1 << 4);
	ven_writew(host, SDBAR1_WP_GPIO3_CTRL_REG_514, regval);

	/* set VDD1 controlled by GPIO 2, and default value */
	regval = ven_readw(host, SDBAR1_GPIO_FUNC_GPIOCTRL_510);
	/* clear bit [13:8] */
	regval &= ~(0x3f << 8);
	/* set bit [11], GPIO2 output, default vdd1 off */
	regval |= (1 << 11);
	ven_writew(host, SDBAR1_GPIO_FUNC_GPIOCTRL_510, regval);

	/* set VDD2 controlled by GPIO 1, and default value */
	/* select vdd2_en_pol =1 */
	pci_andl(host, 0x444, ~(1 << 17));

	/* nothing, SDPWR_EXT_SEL(CDN) is fixed to 0 */

	regval = ven_readw(host, SDBAR1_GPIO_1_2_CTRL_REG_510);
	/* clear bit [5:0] */
	regval &= ~(0x3f);
	/* GPIO4 output & input, default vdd2 off */
	regval |= (1 << 3);
	ven_writew(host, SDBAR1_GPIO_1_2_CTRL_REG_510, regval);

	goto exit;

exit:
	DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);

}

/*
 *
 * Function Name: hostven_ltr_issue
 *
 * Abstract:
 *
 *			The issue fix for Seabird PM issue 15#.
 *			The issue is the hardware LTR state machine issue.
 *
 * Input:
 *
 *			host [in]: A pointer to the host structure.
 *
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			NULL.
 *
 * Notes:
 *
 *
 *
 */
static void hostven_ltr_issue(sd_host_t *host)
{
	u32 reg_val = 0;

	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE | FEATURE_DRIVER_INIT,
		NOT_TO_RAM, "Enter %s\n", __func__);

	if ((host->chip_type == CHIP_FUJIN2) ||
	    (host->chip_type == CHIP_SEABIRD)) {
		reg_val = pci_readl(host, 0xa8);
		if (reg_val & (1 << 10)) {
			pci_writel(host, 0xa8, (reg_val & (~(1 << 10))));
			pci_writel(host, 0xa8, (reg_val | (1 << 10)));
		}
	}

	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE | FEATURE_DRIVER_INIT,
		NOT_TO_RAM, "Exit %s\n", __func__);
}

/*
 *
 * Function Name: hostven_dma_engine_issue
 *
 * Abstract:
 *
 *			DMA engine is not reset and DMA registers is reset to default value
 *			(default value is MWr) when remove SD card.
 *
 * Input:
 *
 *			host [in]: A pointer to the host structure.
 *
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			NULL.
 *
 * Notes:
 *
 * disable DMA reset
 *
 */
static void hostven_dma_engine_issue(sd_host_t *host)
{
	u32 reg_val = 0;

	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE | FEATURE_DRIVER_INIT,
		NOT_TO_RAM, "Enter %s\n", __func__);
	reg_val = pci_readl(host, 0x308);
	DbgErr("old PCR Register 0x308: 0x%x\n", reg_val);
	reg_val |= 0xC00000;
	pci_writel(host, 0x308, reg_val);
	DbgErr("new PCR Register 0x308: 0x%x\n", pci_readl(host, 0x308));
	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE | FEATURE_DRIVER_INIT,
		NOT_TO_RAM, "Exit %s\n", __func__);
}

/*
 *
 * Function Name: hostven_ms_sd30_dis
 *
 * Abstract:
 *
 *			Disable the SD 3.0 function for the Microsoft win7 driver
 *
 * Input:
 *
 *			host [in]: A pointer to the host structure.
 *
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			NULL.
 *
 * Notes:
 *
 *			Add SD 3.0 enable bit for Microsoft driver:
 *			pcr 0x3E4[22] : MS SD driver disable.
 *			0-- enable; 1-- disable. Default 0.
 *			Pcr3e4[22] :MS SD driver disable.
 *			0: enable;
 *			1: disable.
 *			Default 0.
 *
 *			Add sd3.0 enable for MS driver.
 *			Pcr f8[31]:
 *			0: sd2.0;
 *			1: sd3.0.
 *			Default 0.
 *
 *			Set pcr {3e4[22] ,f8[31]}=2'b00
 *			Use Microsoft dirver sd2.0 driver,
 *			base clock is 50M and base frequency capability is 8'h32;
 *
 *			Set pcr {3e4[22] ,f8[31]}=2'b01
 *			Use Microsoft dirver sd3.0 driver,
 *			base clock is 200M and base frequency capability is 8'hc8;
 *
 *			Set pcr 3e4[22]=1'b1
 *			Use O2 driver, base clock is set by pcr 304
 *			and base frequency capability is set by pcr328[15:8];
 *
 */
static void hostven_ms_sd30_dis(sd_host_t *host)
{
	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE | FEATURE_DRIVER_INIT,
		NOT_TO_RAM, "Enter %s\n", __func__);

	if ((host->chip_type == CHIP_SEABIRD) ||
	    (host->chip_type == CHIP_SEAEAGLE) ||
	    (host->chip_type == CHIP_SEAEAGLE2) ||
	    (host->chip_type == CHIP_GG8) ||
	    (host->chip_type == CHIP_ALBATROSS)) {
		pci_orl(host, 0x3e4, 1 << 22);
	}
	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE | FEATURE_DRIVER_INIT,
		NOT_TO_RAM, "Exit %s\n", __func__);
}

void hostven_ocb_cfg(sd_host_t *host)
{
	cfg_ocb_ctrl_t *test_ocb_ctrl = &(host->cfg->host_item.test_ocb_ctrl);

	/*
	 * if software to set the power off and clear the OCB status,
	 * need to disable hw power off function
	 */
	if (test_ocb_ctrl->sw_pwroff_en) {
		/* unlock write protect bit */
		pci_andl(host, 0xd0, 0x7fffffff);
		pci_andl(host, 0xd4, ~0x10);
		/* restore write protect bit */
		pci_orl(host, 0xd0, (1 << 31));
	} else {
		pci_andl(host, 0xd0, 0x7fffffff);
		pci_orl(host, 0xd4, 0x10);
		pci_orl(host, 0xd0, (1 << 31));
	}

	if ((host->chip_type == CHIP_FUJIN2) ||
	    (host->chip_type == CHIP_SEABIRD)
	    ) {
		/* host 0x1c0 [22][5] */
		sdhci_or32(host, SDHCI_DRIVER_CTRL_REG,
			   (SDHCI_OCB_FET_INT_DENOUNCE | SDHCI_OCB_INT_MASK));
	}

	if (test_ocb_ctrl->int_check_en == 0) {
		/* clear 0x1c0 [5] */
		sdhci_and32(host, SDHCI_DRIVER_CTRL_REG, ~SDHCI_OCB_INT_MASK);
	}

}

void hostven_switch_flow_cfg(sd_host_t *host)
{
	u32 regval = 0;

	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE | FEATURE_DRIVER_INIT,
		NOT_TO_RAM, "Enter %s\n", __func__);

	pci_andw(host, 0x444, 0xF8FF);
	switch (host->cfg->card_item.sd7_sdmode_switch_control.switch_method_ctrl) {
	case HW_DETEC_HW_SWITCH:
		pci_orw(host, 0x444, BIT8);
		break;

	case SW_POLL_SW_SWITCH:
	case SW_POLL_INTER_SW_SWITCH:
		pci_orw(host, 0x444, BIT9);
		break;

	case SW_POLL_SWCTRL_SWITCH:
	case SW_POLL_INTER_SWCRTL_SWITCH:
		pci_orw(host, 0x444, BIT10);
		if (host->cfg->card_item.sd7_sdmode_switch_control.sw_ctrl_polarit)
			pci_orw(host, 0x444, BIT15);
		else
			pci_andw(host, 0x444, (u16)(~BIT15));

		break;

	default:
		DbgErr
		    ("Error:no such value in registry sd7_sdmode_switch_control, use default value\n");
		pci_orw(host, 0x444, BIT8);
		break;
	}

	regval = pci_readl(host, 0x444);

	regval = pci_readl(host, 0x328);
	if (host->cfg->card_item.sd7_sdmode_switch_control.vdd3_control)
		regval |= (1 << 5);
	else
		regval &= ~(1 << 5);

	pci_writel(host, 0x328, regval);

	if (host->cfg->card_item.sd7_sdmode_switch_control.sd70_trail_run) {
		/* sd7.0 trail run case */
		/* set pcr 0x444[11] = 1, default 0 */
		regval = pci_readl(host, 0x444);
		regval |= (1 << 11);
		pci_writel(host, 0x444, regval);

	} else {
		/* sd7.0 cdm switch case */
		regval = pci_readl(host, 0x444);
		regval &= (~(1 << 11));
		pci_writel(host, 0x444, regval);
	}

	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE | FEATURE_DRIVER_INIT,
		NOT_TO_RAM, "Exit %s,0x444 = 0x%x\n", __func__, regval);
}

void hostven_cmd_low_cfg(sd_host_t *host)
{

	u32 regval = 0;

	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE | FEATURE_DRIVER_INIT,
		NOT_TO_RAM, "Enter %s\n", __func__);

	if (host->cfg->card_item.sd7_sdmode_switch_control.sd_cmd_low_function_en) {
		/* enable source */
		regval = pci_readl(host, 0x444);
		regval |= (1 << 13);
		pci_writel(host, 0x444, regval);

		/* enable event status interrupt */
		regval = pci_readl(host, 0x448);
		regval |= (0x3 << 29);
		regval &= ~(1 << 31);
		pci_writel(host, 0x448, regval);
	} else {
		/* disable source */
		regval = pci_readl(host, 0x444);
		regval &= ~(1 << 13);
		pci_writel(host, 0x444, regval);

		/* disable event status interrupt */
		regval = pci_readl(host, 0x448);
		regval &= ~(0x3 << 29);
		pci_writel(host, 0x448, regval);
	}

	regval = pci_readl(host, 0x444);

	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE | FEATURE_DRIVER_INIT,
		NOT_TO_RAM, "Exit %s,0x444 = 0x%x\n", __func__, regval);
}

void hostven_pinshare_cfg(sd_host_t *host)
{
	cfg_driver_item_t *driver_item = &(host->cfg->driver_item);
	u32 temp_value;
	u32 switch_method =
	    host->cfg->card_item.sd7_sdmode_switch_control.switch_method_ctrl;
	/* clear GPIOs setting to 0 */
	ven_and16(host, 0x510, 0xF8F8);

	if (!shift_bit_func_enable(host))
		ven_and16(host, 0x514, 0xFFF8);

	/*
	 * SD7.0 card remove interrupt status bit can't be cleanned by BHT driver,
	 * Add below part to clean 0x51c bit 2 when driver loading
	 */

	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE | FEATURE_DRIVER_INIT,
		NOT_TO_RAM, "Enter %s\n", __func__);

	if (switch_method == SW_POLL_SWCTRL_SWITCH
	    || switch_method == SW_POLL_INTER_SWCRTL_SWITCH) {
		temp_value = pci_readl(host, 0x51C);
		temp_value |= 0x2;
		pci_writel(host, 0x51C, temp_value);
	}

	if (host->device_id != 0x9862) {
		if (host->cfg->card_item.sd7_sdmode_switch_control.camera_mode_enable) {
			/* set PCR 0x444[14]=1 to enable camera mode */
			temp_value = pci_readl(host, 0x444);
			temp_value |= (1 << 14);
			pci_writel(host, 0x444, temp_value);
		} else {
			/* set PCR 0x444[14]=0 to disable camera mode */
			temp_value = pci_readl(host, 0x444);
			temp_value &= ~(1 << 14);
			pci_writel(host, 0x444, temp_value);
		}

		ven_and16(host, 0x50C, 0xFF78);

		/* set led polarity */
		if (driver_item->led_polarity)
			ven_or16(host, 0x50C, 0x80);

		if ((switch_method == SW_POLL_SWCTRL_SWITCH
		     || switch_method == SW_POLL_INTER_SWCRTL_SWITCH)
		    && driver_item->sw_ctl_led_gpio0 == 0) {
			ven_and16(host, 0x518, 0xFFF3);
			ven_and16(host, 0x520, 0xFFF3);

			/* set BAR1 0x50c[2:0] = 000. */
			ven_or16(host, 0x50C, 0x10);

			ven_or16(host, 0x518, 0x0c);
			ven_or16(host, 0x520, 0x0c);

		} else {
			switch (driver_item->sw_ctl_led_gpio0) {
			case 1:
				ven_or16(host, 0x50C, 0x01);

				break;
			case 2:
				ven_or16(host, 0x50C, 0x02);

				break;

			default:
				ven_or16(host, 0x50C, 0x03);
				break;
			}
		}

	}

	if (shift_bit_func_enable(host)) {
		/* gpio1 */
		/* GPIO1 input enable for GPIO and external interrupt. */
		ven_or32(host, 0x510, 0x00000010);
		/* 000h: Register-controlled GPIO and external interrupt. */
		ven_and32(host, 0x510, 0xFFFFFFF0);
		/* GPIO1 negedge/posedge interrupt enable bit */
		ven_or32(host, 0x518, 0x00000030);
		/* GPIO1 negedge/posedge interrupt signal enable bit */
		ven_or32(host, 0x520, 0x00000030);

		/* gpio2 */
		/* GPIO2 output enable for GPIO only. */
		ven_or32(host, 0x510, 0x00000800);
		/* 000h: Register-controlled GPIO. */
		ven_and32(host, 0x510, 0xFFFFF8FF);
		/* Default status: GPIO2 output low, */
		set_gpio_levels(host, 0, 0);

		/* gpio3 */
		/* GPIO3 output enable for GPIO only. */
		ven_or32(host, 0x514, 0x00000008);
		/* 010h: Register-controlled GPIO  */
		ven_or32(host, 0x514, 0x00000002);
		/* Default status: GPIO3 output high. */
		set_gpio_levels(host, 1, 1);

		temp_value = pci_readl(host, 0x51C);
		if (temp_value & 0x4) {
			temp_value |= 0x4;
			pci_writel(host, 0x51C, temp_value);
		}

	}

	switch (driver_item->led_gpio1) {
		/* use as GPIO */
	case 0:
		break;
		/* use as led */
	case 1:
		ven_or16(host, 0x510, 0x30);
		break;
	default:
		ven_or16(host, 0x510, 0x30);
		break;
	}

	switch (driver_item->led_gpio2) {
		/* use as GPIO */
	case 0:
		break;
		/* use as led */
	case 1:
		ven_or16(host, 0x510, 0x0100);
		break;
	case 2:
	default:
		ven_or16(host, 0x510, 0x0300);
		break;
	}

	switch (driver_item->wp_led_gpio3) {
		/* use as sd_wp */
	case 0:
		break;
		/* use as led */
	case 1:
		ven_or16(host, 0x514, 0x01);
		break;
		/* use as gpio */
	case 2:
		break;
		/* use as RTD3 function test */
	case 3:
		ven_or16(host, 0x514, 0x2A);
		break;
	default:
		ven_or16(host, 0x514, 0x03);
		break;
	}
}

/*
 *
 * Function Name: hostven_dll_watchdog_timer
 *
 * Abstract:
 *
 *			Set the DLL watch dog timer register value.
 *
 * Input:
 *
 *			host [in]: A pointer to the host structure.
 *
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			NULL.
 *
 * Notes:
 *
 *
 *
 *
 */
static void hostven_dll_watchdog_timer(sd_host_t *host)
{
	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE | FEATURE_DRIVER_INIT,
		NOT_TO_RAM, "Enter %s\n", __func__);
	if (host->chip_type == CHIP_FUJIN2)
		sdhci_writew(host, 0x1c8, 0x1280);
	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE | FEATURE_DRIVER_INIT,
		NOT_TO_RAM, "Exit %s\n", __func__);

}

static void hostven_socket_pow_en(sd_host_t *host)
{
	u32 reg_val = 0;

	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE | FEATURE_DRIVER_INIT,
		NOT_TO_RAM, "Enter %s\n", __func__);
	if ((host->chip_type == CHIP_SDS0) || (host->chip_type == CHIP_SDS1)
	    || (host->chip_type == CHIP_FUJIN2)) {
		reg_val = pci_readl(host, 0xec);
		reg_val |= 0x3;
		pci_writel(host, 0xec, reg_val);
	}

	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE | FEATURE_DRIVER_INIT,
		NOT_TO_RAM, "Exit %s\n", __func__);
}

static bool seabird_pcr_check(u16 addr)
{
	if (addr < 0x100) {
		if (((addr >= 0x64) && (addr < 0x6b))
		    || ((addr >= 0x74) && (addr < 0x7f)) || ((addr >= 0xd0)
							     && (addr < 0xff)))
			return FALSE;
		else
			return TRUE;
	} else if ((addr >= 0x300) && (addr < 0xfff)) {
		return FALSE;
	} else {
		return TRUE;
	}
}

static bool seaeagle_pcr_check(u16 addr)
{
	if (addr < 0x100) {
		if (((addr >= 0x64) && (addr < 0x6b))
		    || ((addr >= 0x74) && (addr < 0x7f)) || ((addr >= 0xd0)
							     && (addr < 0xff)))
			return FALSE;
		else
			return TRUE;
	} else if ((addr >= 0x300) && (addr < 0xfff)) {
		return FALSE;
	} else {
		return TRUE;
	}
}

#if (1)
static bool hostven_pcr_need_direct_access(sd_host_t *host, u16 addr)
{
	if (host->chip_type == CHIP_SEABIRD)
		return seabird_pcr_check(addr);
	else
		return seaeagle_pcr_check(addr);
}
#else
static bool hostven_pcr_need_direct_access(u16 addr)
{

	if (addr < 0x100) {
		switch (addr & 0xfffc) {
		case 0x64:
		case 0x68:
		case 0x74:
		case 0x78:
		case 0x7c:
		case 0xd0:
		case 0xd4:
		case 0xd8:
		case 0xdc:
		case 0xe0:
		case 0xe8:
		case 0xec:
		case 0xf0:
		case 0xf4:
		case 0xfc:
			return FALSE;
		default:
			return TRUE;
		}
	} else if (addr <= 0xfff && addr >= 0x300) {
		return FALSE;
	} else
		return TRUE;
}
#endif

static void hostven_load_pcr_cfg(sd_host_t *host)
{

	cfg_item_t *cfg = host->cfg;
	u32 i = 0;
	cfg_pcr_t *pcr = 0;

	for (i = 0; i < MAX_PCR_SETTING_SIZE; i++) {
		u16 val = 0;

		pcr = &cfg->pcr_item.pcr_tb[i];
		if (pcr->valid_flg == 0)
			continue;
		switch (pcr->type) {
		case 0:
			val = pcr->mask;
			PrintMsg
			    ("PCR Setting: Addr=0x%04X, Mask=0x%04X, Val==0x%04X\n",
			     pcr->addr, pcr->mask, pcr->val);
			if (TRUE ==
			    hostven_pcr_need_direct_access(host, pcr->addr)) {
				u32 reg_val = 0;
				u32 mask = pcr->mask;
				u32 val32 = pcr->val;

				reg_val = pci_readl(host, pcr->addr & 0xfffc);
				if (pcr->addr & 2) {
					mask <<= 16;
					val32 <<= 16;
				}
				reg_val &= ~mask;
				reg_val |= (val32 & mask);
				pci_cfgio_writel(host, pcr->addr & 0xfffc,
						 reg_val);
			} else {
				u16 reg_val16 = 0;
				u32 reg_val = 0;

				reg_val16 = pci_readw(host, pcr->addr);
				reg_val16 &= (~val);
				reg_val16 |= (pcr->val & pcr->mask);
				pci_writew(host, pcr->addr, reg_val16);
				reg_val = pci_readw(host, pcr->addr);

			}
			break;
		case 1:
			val = pcr->mask;
			PrintMsg
			    ("MEM Setting: Addr=0x%04X, Mask=0x%04X, Val==0x%04X\n",
			     pcr->addr, pcr->mask, pcr->val);
			sdhci_and16(host, pcr->addr, ~val);
			val = (pcr->val & pcr->mask);
			sdhci_or16(host, pcr->addr, val);
			break;
		default:
			break;
		}
	}

}

void hostven_set_tuning_phase(sd_host_t *host, u32 input_n1, u32 output_n1,
			      bool off)
{
	u32 val32;
	bht_dev_ext_t *pdx = host->pdx;
	cfg_output_tuning_item_t *cfg =
	    &pdx->cfg->feature_item.output_tuning_item;

	if ((cfg->enable_dll == 0) || (cfg->enable_dll_divider == 0))
		return;

	DbgInfo(MODULE_VEN_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s\n", __func__);

	if (off == TRUE) {
		pci_andl(host, 0x354, 0xFE0EFFFF);
		val32 = 1;
		pci_andl(host, 0x354, (~(val32 << 25)));
		pci_andl(host, 0x354, (~(val32 << 26)));
		sdhci_and32(host, 0x1b0, (~(val32 << 28)));
		sdhci_and32(host, 0x1b0, 0xC0FFFFFF);
	} else {
		host_enable_clock(host, FALSE);

		pci_orl(host, 0x354, (1 << 25));

		/* choose output tuning */
		pci_andl(host, 0x354, 0xFE0EFFFF);
		pci_orl(host, 0x354, (output_n1 << 20));
		pci_orl(host, 0x354, (1 << 16));

		/* choose input tuning */
		val32 = (sdhci_readl(host, 0x1b0) & 0xC0FFFFFF);
		val32 |= (1 << 28);
		val32 |= ((input_n1 % 16) << 24);
		val32 |= (input_n1 >> 4) << 29;
		sdhci_writel(host, 0x1b0, val32);
		pci_orl(host, 0x354, (1 << 26));
		host_enable_clock(host, TRUE);
	}
	DbgInfo(MODULE_VEN_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

bool hostven_dll_input_tuning_init(sd_host_t *host)
{
	u8 i;

	DbgInfo(MODULE_VEN_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s\n", __func__);

	host_enable_clock(host, FALSE);

	host_internal_clk_setup(host, FALSE);

	host_init_clock(host,
			host->cfg->dmdn_tbl[FREQ_DDR50_INPUT_TUNIN_START_INDEX]);

	host_internal_clk_setup(host, TRUE);
	host_enable_clock(host, TRUE);

	i = 0;
	while (!((sdhci_readl(host, 0x1cc) & 0x4000)
		 && (sdhci_readl(host, 0x1cc) & 0x800))) {
		if (i > 50)
			return FALSE;
		os_mdelay(1);
		i++;
	}

	host_enable_clock(host, FALSE);
	host_internal_clk_setup(host, FALSE);
	pci_orl(host, 0x354, 0x6010000);
	sdhci_or32(host, 0x1b0, 0x10000000);
	host_internal_clk_setup(host, TRUE);

	i = 0;
	while (!((sdhci_readl(host, 0x1cc) & 0x4000))) {
		if (i > 50)
			return FALSE;
		os_mdelay(1);
		i++;
	}

	if ((host->chip_type == CHIP_SEAEAGLE2) || (host->chip_type == CHIP_GG8)
	    || (host->chip_type == CHIP_ALBATROSS))
		host_enable_clock(host, TRUE);

	sdhci_or32(host, 0x1cc, (1 << 17));
	sdhci_or32(host, 0x1cc, (1 << 16));

	i = 0;
	while (!((sdhci_readl(host, 0x1cc) & 0x800))) {
		if (i > 50)
			return FALSE;
		os_mdelay(1);
		i++;
	}

	sdhci_and32(host, 0x1cc, 0xFFFCFFFF);

	i = 0;
	while (!((sdhci_readl(host, 0x1cc) & 0x4000)
		 && (sdhci_readl(host, 0x1cc) & 0x800))) {
		if (i > 50)
			return FALSE;
		os_mdelay(1);
		i++;
	}

	if (host->chip_type == CHIP_SEAEAGLE)
		host_enable_clock(host, TRUE);

	DbgInfo(MODULE_VEN_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return TRUE;
}

static void hostven_output_tuning_init(sd_host_t *host)
{

	DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	host->output_tuning.auto_flag = FALSE;

}

void hostven_detect_refclk_count_range_init(sd_host_t *host)
{
	u16 expected_range = 0;
	cfg_item_t *cfg = host->cfg;

	DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if ((pci_readl(host, 0x460) & BIT31)) {
		expected_range = pci_readw(host, 0x460);
		DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
			"Hardware invoked auto-adjust refclk counter range done, range min %#x , max %#x\n",
			(expected_range >> 8), (expected_range & 0xFF));
	}

	/*
	 * Enable PCIe reference clock detection timeout status and
	 * PCIe reference clock detection timeout interrupt signal
	 */
	sdhci_or32(host, 0x1E0, (BIT23 | BIT27));

	if (cfg->feature_item.auto_detect_refclk_counter_range_ctl.enable) {
		DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
			"set refclk_range_detect_cnt: 0x%X\n",
			cfg->feature_item.auto_detect_refclk_counter_range_ctl.refclk_range_detect_cnt);

		/* reference clock stable counter register set disable */
		pci_andw(host, 0x456, (u16)(~BIT15));

		/* set cycles_of_detection_period */
		pci_andw(host, 0x462, 0xFF00);
		pci_orw(host, 0x462,
			(u16) cfg->feature_item.auto_detect_refclk_counter_range_ctl.refclk_range_detect_cnt);

		/* reference clock stable counter register set enable */
		pci_orw(host, 0x456, BIT15);
		os_udelay(10);

		/* reference clock stable counter register set disable */
		pci_andw(host, 0x456, (u16)(~BIT15));

		if (cfg->feature_item.auto_detect_refclk_counter_range_ctl.req_refclkcnt_minmax_source_sel) {
			DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT,
				NOT_TO_RAM,
				"set required_refclk_count_value, min: 0x%X, max: 0x%X\n",
				cfg->feature_item.auto_detect_refclk_counter_range_ctl.req_refclkcnt_min,
				cfg->feature_item.auto_detect_refclk_counter_range_ctl.req_refclkcnt_max);

			/* select from register configure 0x460[15:0] */
			pci_orw(host, 0x462, BIT13);

			/* set required_refclk_count_value */
			pci_writew(host, 0x460, (((u16)
						  (cfg->feature_item.auto_detect_refclk_counter_range_ctl.req_refclkcnt_min)
						  << 8) |
						 cfg->feature_item.auto_detect_refclk_counter_range_ctl.req_refclkcnt_max));

			DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT,
				NOT_TO_RAM, "PCR 0x460: 0x%X\n", pci_readw(host,
									   0x460));
		}

		if (cfg->feature_item.auto_detect_refclk_counter_range_ctl.refclkcnt_range_detect_softreset) {
			/* set refclk_cnt_range_detect_soft_reset */
			pci_orw(host, 0x462, BIT14);
		}
	}

	DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

 /*
  * Below function designe please refers to GG8 architecture
  * chapter12.3: Refclk stable detection circuit
  */
void hostven_refclk_stable_detection_circuit(sd_host_t *host)
{
	cfg_item_t *cfg = host->cfg;
	u32 regval;

	DbgInfo(MODULE_VEN_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s\n", __func__);
	/*
	 * Step0: Set PCR 0xD0[31] = 0 – disable ‘write protect’ for
	 * BHT defined config space registers;
	 */
	pci_andl(host, 0xd0, ~BIT31);

	/* Step1: Set PCR 0x454[31] = 0 – disable the setting; */
	pci_andl(host, 0x454, ~BIT31);

	/* Step2: Set SD host register 1C0[1] = 1 to force L0 request; */
	sdhci_or16(host, 0x1c0, BIT1);

	/* Step3 : Set target registry with the expected parameter value; */

	/* Step3.1  L1_ENTER_EXIT_LOGIC_CTL setting */
	if (cfg->feature_item.l1_enter_exit_logic_ctl.enable) {
		/*PCR 0x444[26] */
		if (cfg->feature_item.l1_enter_exit_logic_ctl.disable_tx_command_mode)
			/* disable */
			pci_orl(host, 0x444, BIT26);
		else
			/* enable (default) */
			pci_andl(host, 0x444, ~BIT26);

		/*PCR 0x444[28], disable “PCIe Phy Reference clock active detection logic */
		if (cfg->feature_item.l1_enter_exit_logic_ctl.disable_pcie_phy_clk)
			/* disable */
			pci_orl(host, 0x444, BIT28);
		else
			/* enable (default) */
			pci_andl(host, 0x444, ~BIT28);
	}

	/* Step3.2  REFCLK_STABLE_DETECTION_COUNTER1 */
	if (cfg->feature_item.refclk_stable_detection_counter1.enable) {

		/* PCR 0x454[15:0] */
		regval = pci_readl(host, 0x454);
		regval &= 0xffff0000;
		regval |=
		    cfg->feature_item.refclk_stable_detection_counter1.required_refclk_compare_count;
		pci_writel(host, 0x454, regval);

		/*
		 * REFCLK_STABLE_DETECTION_COUNTER2
		 * required_refclk_compare_timeout_d0l10, it is controlled by PCR 0x458[31:16]
		 * The detection timeout counter in d0 L1.0.
		 * The timeout time is required_refclk_compare_timeout_d0l10 * Tclk_2m.
		 * Default: ‘h001E (15us).
		 *
		 * REFCLK_STABLE_DETECTION_COUNTER2
		 * required_refclk_compare_timeout_d0l11, it is controlled by PCR 0x458[15:0]
		 * The detection timeout counter in d0 L1.1.
		 * The timeout time is required_refclk_compare_timeout_d0l11 * Tclk_2m.
		 * Default: ‘h044C  (550us).
		 */
		regval = pci_readl(host, 0x458);
		regval &= 0x0;
		regval |=
		    (cfg->feature_item.refclk_stable_detection_counter2.required_refclk_compare_timeout_d0l10
		     << 16) |
		    (cfg->feature_item.refclk_stable_detection_counter2.required_refclk_compare_timeout_d0l11);
		pci_writel(host, 0x458, regval);

		/*
		 * REFCLK_STABLE_DETECTION_COUNTER3
		 * required_refclk_compare_timeout_d0l12, it is controlled by PCR 0x45C[31:16]
		 * The detection timeout counter in d0 L1.2.
		 * The timeout time is required_refclk_compare_timeout_d0l12 * Tclk_2m.
		 * Default: ‘h04B0  (600us).
		 *
		 * required_refclk_compare_timeout_d3l12, it is controlled by PCR 0x458C[15:0]
		 * The detection timeout counter in d3 L1.2 and
		 * “switch back from PCIe SD mode to Legacy SD mode”.
		 * The timeout time is required_refclk_compare_timeout_d3l12 * Tclk_2m.
		 * Default: ‘h24B0 (4.7ms).
		 */
		regval = pci_readl(host, 0x45c);
		regval &= 0x0;
		regval |=
		    (cfg->feature_item.refclk_stable_detection_counter3.required_refclk_compare_timeout_d0l12
		     << 16) |
		    (cfg->feature_item.refclk_stable_detection_counter3.required_refclk_compare_timeout_d3l12);
		pci_writel(host, 0x45c, regval);

		/* PCR 0x454[31] = 1 – enable the setting; */
		if (cfg->feature_item.refclk_stable_detection_counter1.chk_refclk_parameter_en)
			pci_orl(host, 0x454, BIT31);
	}

	/* Step4: Read back target registers to make sure previous accessing is OK; */
	DbgInfo(MODULE_VEN_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Check START------------------\n");
	pci_readl(host, 0x444);
	pci_readl(host, 0x454);
	pci_readl(host, 0x458);
	pci_readl(host, 0x45c);
	DbgInfo(MODULE_VEN_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Check END--------------------\n");

	/* Step5: Set SD host register 1C0[1] = 0 clear force L0 request; */
	sdhci_and16(host, 0x1c0, ~BIT1);

	DbgInfo(MODULE_VEN_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

 /* Below function designe please refers to GG8 architecture chapter12.4 */
void hostven_pcie_phy_tx_amplitude_adjustment(sd_host_t *host)
{
	cfg_item_t *cfg = host->cfg;
	u32 regval_1 =
	    cfg->feature_item.pcie_phy_amplitude_adjust.pcietx_amplitude_setting;
	u32 regval_2;
	u16 i;
	u32 regval;

	struct amplitude_configuration amplitude_configuration_arr[5] = {
		{ 0x6a, "1.0V" },
		{ 0x6f, "1.05V" },
		{ 0x75, "1.1V" },
		{ 0x7a, "1.15V" },
		{ 0x7f, "1.2V" }
	};

	DbgInfo(MODULE_VEN_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s\n", __func__);

	if (cfg->feature_item.pcie_phy_amplitude_adjust.pcietx_amplitude_chg_en) {
		/*Step of CR write */

		/*
		 * Step0: Set PCR 0xD0[31]=0 – disable ‘write protect’ for
		 * BHT defined config space registers;
		 */
		pci_andl(host, 0xd0, ~BIT31);

		/*
		 * Step1: Write CR address 16’h1002 to reg 0x78 [15:0];
		 *
		 * Step2: Write updated CR data to reg 0x78[31:16],
		 * default value is 7b’1101010;
		 */
		regval = pci_readl(host, 0x78);
		regval &= 0x0;
		regval |= (0x1002 | (regval_1 << 16));
		pci_writel(host, 0x78, regval);

		/*
		 * Step3: Write 1’b1 to reg 0x78[30],
		 * to enable override CR data that write to CR address 16’h1002;
		 */
		pci_orl(host, 0x78, BIT30);

		/* Step4: Config reg 0x7C[0] to 1’b1 (Config CR direction to “write”); */
		pci_orl(host, 0x7C, BIT0);

		/*
		 * Step5: Config reg 0x7C[16] to 1’b1 to Initiate the CR access,
		 * and the CR write will be operated automatically;
		 */
		pci_orl(host, 0x7C, BIT16);

		/*Step of CR read */

		/* Step1: Write CR address 16’h1009 to reg 0x78[15:0]; */
		regval = pci_readl(host, 0x78);
		regval &= 0xffff0000;
		regval |= 0x1009;
		pci_writel(host, 0x78, regval);

		/* Step2: Config reg 0x7C[0] to 1’b0(Config CR direction to “read”); */
		pci_andl(host, 0x7C, ~BIT0);

		/*
		 * Step3: Config reg 0x7C[16] to 1’b1 to Initiate the CR access,
		 * and the CR read will be operated automatically;
		 */
		pci_orl(host, 0x7C, BIT16);

		/*
		 * Step4: Read reg 0x78[31：16] to get the CR read data,
		 * to confirm pcs_tx_swing_full[6:0] value is updated successfully;
		 */
		regval_2 = ((pci_readl(host, 0x78) & (0x00FF0000)) >> 16);

		/*
		 * Step5: Set PCR 0xD0[31]=1 – enable ‘write protect’ for
		 * BHT defined config space registers.
		 */
		pci_orl(host, 0xD0, BIT31);

		/*
		 * Debug driver should compare the CR write and CR read data,
		 * to confirm the CR write data is correct,
		 * then driver print “PCIe PHY TX Amplitude is xxV.”.
		 */

		if (regval_1 == regval_2) {
			for (i = 0; i < 5; i++) {
				if (regval_1 ==
				    amplitude_configuration_arr
				    [i].pcietx_amplitude)
					DbgInfo(MODULE_VEN_HOST,
						FEATURE_ERROR_RECOVER,
						NOT_TO_RAM,
						"PCIe PHY TX Amplitude is %s\n",
						amplitude_configuration_arr
						[i].amplitude);
			}
		} else {
			DbgErr
			    ("Warning: write CR data is 0x%x, read CR data is 0x%x!\n",
			     regval_1, regval_2);
		}
	}

	DbgInfo(MODULE_VEN_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

void hostven_set_output_tuning_phase(sd_host_t *host, u32 value, bool off)
{
	bht_dev_ext_t *pdx = host->pdx;

	DbgInfo(MODULE_VEN_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s, Value:%x, Off:%x\n", __func__, value, off);
	if (off == TRUE) {
		/* select opclk */
		pci_andl(host, 0x354, 0xFFFEFFFF);
	} else {
		switch (host->chip_type) {
			/* dll clock is selected according to UHS work mode */
		case CHIP_GG8:
		case CHIP_ALBATROSS:
			if (pdx->card.info.sw_cur_setting.sd_access_mode ==
			    SD_FNC_AM_DDR200) {
				pci_andl(host, 0x354, 0xFFFFFF0F);
				pci_orl(host, 0x354, (value << 4));
			} else if (pdx->card.info.sw_cur_setting.sd_access_mode ==
				   SD_FNC_AM_DDR50) {
				/* Not support this mode at Bayhub Driver */
				DbgErr("DDR50 mode isn't supported !!!");
			} else if (pdx->card.info.sw_cur_setting.sd_access_mode ==
				   SD_FNC_AM_SDR104) {
				pci_andl(host, 0x354, 0xFF0FFFFF);
				pci_orl(host, 0x354, (value << 20));
			} else if (pdx->card.info.sw_cur_setting.sd_access_mode ==
				   SD_FNC_AM_SDR50) {
				pci_andl(host, 0x354, 0xFFFFFFF0);
				pci_orl(host, 0x354, value);
			}
			break;
		default:
			/* select dll clock */
			pci_andl(host, 0x354, 0xFF0EFFFF);
			pci_orl(host, 0x354, ((value << 20) | BIT16));
			break;
		}
	}
	DbgInfo(MODULE_VEN_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

bool hostven_fix_output_tuning(sd_host_t *host, byte sd_access_mode)
{
	bht_dev_ext_t *pdx = host->pdx;
	cfg_output_tuning_item_t *cfg =
	    &pdx->cfg->feature_item.output_tuning_item;

	DbgInfo(MODULE_VEN_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s, Access mode:%x\n", __func__, sd_access_mode);

	host->output_tuning.auto_flag = FALSE;
	if (cfg->enable_dll == 0)
		goto exit;

	if (host->output_tuning.auto_phase_flag == TRUE) {
		host_set_output_tuning_phase(host,
					     host->output_tuning.auto_phase);
		goto exit;
	}

	/*
	 * check whether need do output tuning or not:
	 * featre.output_tuning enable or not
	 * only SD_CARD
	 */

	if (cfg->enable_emmc_hs400 == 1) {
		host->output_tuning.auto_flag = TRUE;
		host_set_output_tuning_phase(host, cfg->fixed_value_emmc_hs400);
	}

	switch (sd_access_mode) {
	case SD_FNC_AM_DDR200:
		if (cfg->enable_ddr200 == 0) {
			host->output_tuning.auto_flag = TRUE;
			host_set_output_tuning_phase(host,
						     cfg->fixed_value_ddr200);
		}
		break;
	case SD_FNC_AM_SDR104:
		if (cfg->enable_sdr104 == 0) {
			host->output_tuning.auto_flag = TRUE;
			host_set_output_tuning_phase(host,
						     cfg->fixed_value_sdr104);
		}
		break;
	case SD_FNC_AM_SDR50:
		if (cfg->enable_sdr50 == 0) {
			host->output_tuning.auto_flag = TRUE;
			host_set_output_tuning_phase(host,
						     cfg->fixed_value_sdr50);
		}
		break;
	case SD_FNC_AM_DDR50:
		DbgErr("DDR50 isn't supported\n");
		break;
	default:
		break;
	}

exit:
	DbgInfo(MODULE_VEN_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return host->output_tuning.auto_flag;
}

u8 hostven_tuning_type_selection(sd_host_t *host, byte sd_access_mode)
{
	/* this function only SD_CARD */
	bht_dev_ext_t *pdx = host->pdx;
	cfg_output_tuning_item_t *cfg =
	    &pdx->cfg->feature_item.output_tuning_item;
	u8 tuning_type = 0;
	u8 output_clock_source = 0;
	u8 input_clock_source = 0;

	DbgInfo(MODULE_VEN_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s, Access mode:%x\n", __func__, sd_access_mode);

	/* select output clock source by mode */
	switch (sd_access_mode) {
	case SD_FNC_AM_DDR200:
		/* DLL clock is used for output */
		if (cfg->enable_ddr200 == 1)
			output_clock_source = 1;
		break;
	case SD_FNC_AM_SDR104:
		/* DLL clock is used for output */
		if (cfg->enable_sdr104 == 1)
			output_clock_source = 1;
		break;
	case SD_FNC_AM_SDR50:
		/* DLL clock is used for output */
		if (cfg->enable_sdr50 == 1)
			output_clock_source = 1;
		break;
	case SD_FNC_AM_DDR50:
		/* if (cfg->enable_ddr50 == 1) // DLL clock is used for output */
		/* output_clock_source = 1; */
		DbgErr("DDR50 isn't supported\n");
		break;
	default:
		break;
	}

/* input_clock: */

	input_clock_source = 1;

	/* opclk used for input clock, Only SDR50 use it */
	if (input_clock_source == 0) {
		/* no need input tuning, and use fix output phase */
		tuning_type = 0;
	} else {
		if (output_clock_source == 0)
			/* use fix output phase and do input tuning */
			tuning_type = 1;
		else
			/* do input tuning and auto output tuning */
			tuning_type = 2;
	}

	DbgInfo(MODULE_VEN_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Exit %s with tuning type %d\n", __func__, tuning_type);
	return tuning_type;
}

/* init host feature */
void host_vendor_feature_init(sd_host_t *host)
{
	bht_dev_ext_t *pdx = host->pdx;

	DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	os_memset(&host->feature, 0, sizeof(host->feature));

	hostven_bios_cfg(host);
	hostven_load_pcr_cfg(host);
	hostven_ltr_issue(host);
	hostven_dma_engine_issue(host);
	hostven_ms_sd30_dis(host);

	hostven_dll_watchdog_timer(host);
	hostven_socket_pow_en(host);
	hostven_output_tuning_init(host);
	hostven_drive_strength_cfg(host);

	switch (host->chip_type) {
	case CHIP_SEABIRD:
		pci_orl(host, 0xd4, BIT6);
		/* can't use timer for dump mode */
		if (host->dump_mode == FALSE)
			host->feature.hw_led_fix = 1;
		break;
	case CHIP_SEAEAGLE:
		/* can't use timer for dump mode */
		if (host->dump_mode == FALSE)
			host->feature.hw_led_fix = 1;
		/* Failsafe disable */
		pci_andl(host, 0x3E0, ~BIT6);
		/* add for fail safe delay */
		os_mdelay(3 +
			  pdx->cfg->driver_item.delay_for_failsafe_s3resume);
		break;
	case CHIP_SEAEAGLE2:
	case CHIP_GG8:
	case CHIP_ALBATROSS:
		host->feature.hw_pll_enable = 1;
		host->feature.hw_resp_chk = 1;
		host->feature.hw_autocmd = 1;
		host->feature.hw_41_supp = 1;
		host->camera_mode_card_state = CARD_DESERTED;
		pci_andl(host, 0x3E0, ~BIT6);
		/* can't use timer for dump mode */
		if (host->dump_mode == FALSE)
			host->feature.hw_led_fix = 1;

		hostven_cmd_low_cfg(host);
		hostven_switch_flow_cfg(host);
		hostven_pinshare_cfg(host);
		pcie_weakup(host->pdx, 0, TRUE);
		break;
	case CHIP_FUJIN2:
		/* Default Enable fujin2 led */
		pci_andl(host, 0xdc, ~BIT13);
		pci_orl(host, 0xd4, BIT6);
		break;
	default:
		break;
	}

	/* fail safe issue fix */

	DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

void host_force_pll_enable(sd_host_t *host, bool force)
{
	if (force)
		sdhci_or32(host, 0x1cc, BIT18);
	else
		sdhci_and32(host, 0x1cc, ~BIT18);
}

bool hostven_chk_card_present(sd_host_t *host)
{

	u32 reg_val;

	if (shift_bit_func_enable(host)) {
		reg_val = ven_readl(host, 0x510);
		if (!(reg_val & 0x00000040)) {
			DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT,
				NOT_TO_RAM, "gpio check card present\n");
			return TRUE;
		} else {
			DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT,
				NOT_TO_RAM, "gpio check card not present\n");
			return FALSE;
		}
	} else {
		reg_val = sdhci_readl(host, SDHCI_PRESENT_STATE);
		if ((reg_val & SDHCI_CARD_PRESENT_QUIRK)
		    && (reg_val != REGL_INVALID_VAL))
			return TRUE;
		else
			return FALSE;
	}

#if (0)
	bool result;
	u32 reg_val = 0;
	int debounceTimeMax = 50;

#if GLOBAL_ENABLE_BOOT
	return TRUE;
#else

	reg_val = sdhci_readl(host, SDHCI_PRESENT_STATE);
	if (reg_val == REGL_INVALID_VAL)
		return FALSE;

	if (host->chip_type == CHIP_SEABIRD || host->chip_type == CHIP_SEAEAGLE) {
		/* check if PLL is locked or not, if not, setup pll and enable pll first */
		if (!(sdhci_readl(host, 0x1cc) & BIT14)) {
			/* host_init_emmc_400k_clock(host); */
			host_internal_clk_setup(host, TRUE);
		}
	}
	/* wait for CD# debounce finished */
	while (1) {
		reg_val = sdhci_readl(host, SDHCI_PRESENT_STATE);
		if (((reg_val >> 18) & 0x01) == ((reg_val >> 16) & 0x01))
			break;
		os_mdelay(1);
		--debounceTimeMax;
		if (debounceTimeMax <= 0 && (debounceTimeMax % 1000) == 0)
			DbgErr("%s timeout\n", __func__);
	}

	result = ((reg_val & SDHCI_CARD_PRESENT) == 0) ? FALSE : TRUE;
	/* DbgErr("Card present status is %d\n", result); */
#endif
	return result;
#endif
}

/*
 *
 * Function Name: check_chip_type
 *
 * Abstract:
 *
 *			Acquire the chip type according to the vendor ID and the device ID.
 *
 * Input:
 *
 *			pdev_ext [in]: Points to the device extension.
 *			ConfigInfo [in]: configuration information for a host bus adapter (HBA).
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			If the driver found the chip type,
 *			it will return TRUE. Or it will return FALSE.
 *
 * Notes:
 *
 */
bool hostven_chip_type_check(sd_host_t *host)
{

	bool b_find = FALSE;

	DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	host->sub_version = (u16) ((pci_readl(host, 0xdc) >> 24) & 0xff);

	if ((host->vendor_id == 0x1217) && (host->device_id == 0x8420)) {

		host->chip_type = CHIP_SDS0;
		b_find = TRUE;
		goto AssignFlag;
	}
	if ((host->vendor_id == 0x1217) && (host->device_id == 0x8421)) {
		host->chip_type = CHIP_SDS1;
		b_find = TRUE;
		goto AssignFlag;
	}

	if ((host->vendor_id == 0x1217) && (host->device_id == 0x8520)) {
		if (0x12 == host->sub_version || 0x11 == host->sub_version) {
			host->chip_type = CHIP_SEAEAGLE;
			b_find = TRUE;
			goto AssignFlag;
		} else {
			host->chip_type = CHIP_FUJIN2;
			b_find = TRUE;
			goto AssignFlag;
		}
	}

	if ((host->vendor_id == 0x1217) && (host->device_id == 0x8621)) {
		host->chip_type = CHIP_SEABIRD;
		b_find = TRUE;
		goto AssignFlag;
	}

	if ((host->vendor_id == 0x1217) && (host->device_id == 0x8620)) {
		if (host->sub_version == 0x1) {
			host->chip_type = CHIP_FUJIN2;
			b_find = TRUE;
			goto AssignFlag;
		} else {
			host->chip_type = CHIP_SEABIRD;
			b_find = TRUE;
			goto AssignFlag;
		}
	}

	if (((host->vendor_id == 0x1217) && (host->device_id == 0x8720))
	    || ((host->vendor_id == 0x1217) && (host->device_id == 0x8721))
	    || ((host->vendor_id == 0x1217) && (host->device_id == 0x8722))
	    || ((host->vendor_id == 0x1217) && (host->device_id == 0x8723))) {
		host->chip_type = CHIP_SEAEAGLE2;
		b_find = TRUE;
		goto AssignFlag;

	}

	if (((host->vendor_id == 0x1217) && (host->device_id == 0x9860))
	    || ((host->vendor_id == 0x1217) && (host->device_id == 0x9861))
	    || ((host->vendor_id == 0x1217) && (host->device_id == 0x9862))
	    || ((host->vendor_id == 0x1217) && (host->device_id == 0x9863))) {
		host->chip_type = CHIP_GG8;
		b_find = TRUE;
		goto AssignFlag;

	}

	if (((host->vendor_id == 0x1217) && (host->device_id == 0x9960))
	    || ((host->vendor_id == 0x1217) && (host->device_id == 0x9961))
	    || ((host->vendor_id == 0x1217) && (host->device_id == 0x9962))
	    || ((host->vendor_id == 0x1217) && (host->device_id == 0x9963))) {
		host->chip_type = CHIP_ALBATROSS;
		b_find = TRUE;
		goto AssignFlag;

	}

	DbgErr("venid=0x%04X devid=0x%04X\n", host->vendor_id, host->device_id);

AssignFlag:

	if (b_find == TRUE) {

	} else {
		DbgErr(" Chip not found!ven_id = 0x%08x, device_id = 0x%08x\n",
		       host->vendor_id, host->device_id);
		DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
			"Exit %s, Find:%x\n", __func__, FALSE);
		return FALSE;
	}

	DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Exit %s, Find:%x\n", __func__, TRUE);
	return TRUE;

}

/*
 *
 * Function Name: hostven_rtd3_check
 *
 * Abstract:
 *
 *			Check the runtime D3 enable or not.
 *
 * Input:
 *
 *			host [in]: Points to the host structure.
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			If RTD3 is enabled the value is TRUE. Or it is FALSE.
 *
 * Notes:
 *
 */
bool hostven_rtd3_check(sd_host_t *host)
{
	bool rtd3_en = FALSE;
	cfg_psd_mode_t rtd3_setting = host->cfg->feature_item.psd_mode;

	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	if ((host->chip_type == CHIP_FUJIN2)
	    || (host->chip_type == CHIP_SEABIRD)
	    || (host->chip_type == CHIP_SEAEAGLE)
	    || (host->chip_type == CHIP_SEAEAGLE2)
	    || (host->chip_type == CHIP_GG8)
	    || (host->chip_type == CHIP_ALBATROSS)) {

		if (rtd3_setting.enable_rtd3) {
			rtd3_en = TRUE;
			pci_andl(host, 0x3e0, ~(1 << 29));

			if (host->chip_type == CHIP_FUJIN2) {

				if (!(pci_readl(host, 0x3e0) & (1 << 29))) {
					/* external enable polarity control pin */
					pci_andl(host, 0xd8, ~(1 << 9));
					/* 1.2v main LDO power control source selection */
					pci_andl(host, 0x3e0, ~(1 << 30));
					/* AOSC off support */
					pci_orl(host, 0x3f0, 1 << 31);
				}
			}
		} else {
			rtd3_en = FALSE;
			pci_orl(host, 0x3e0, 1 << 29);
			if (host->chip_type == CHIP_FUJIN2) {
				pci_andl(host, 0xd8, ~(1 << 9));
				/* 1.2v main LDO power control source selection */
				pci_orl(host, 0x3e0, (1 << 30));
				/* AOSC off support */
				pci_andl(host, 0x3f0, ~(1 << 31));

			}
		}
	}

	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE, NOT_TO_RAM,
		"Exit %s, RTD3 enable:%x\n", __func__, rtd3_en);
	return rtd3_en;

}

/*
 *
 * Function Name: hostven_d3_mode_sel
 *
 * Abstract:
 *
 *			Select the D3 work mode. D3 work mode: start-up mode(2'b11),
 *			RTD3 cold with external FET (2'b00),
 *			RTD3 cold with internal FET (2'b01), D3 silence (2'b10).
 *
 *
 *
 * Input:
 *
 *			host [in]: Points to the host structure.
 *       d3_submode [out]: the d3 silence sub mode.
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			The D3 work mode value.
 *
 * Notes:
 *
 */
u32 hostven_d3_mode_sel(sd_host_t *host, u32 *d3_submode)
{
	u32 d3_mode = 0;
	cfg_psd_mode_t rtd3_setting = host->cfg->feature_item.psd_mode;

	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	if ((host->chip_type == CHIP_SEAEAGLE2) || (host->chip_type == CHIP_GG8)
	    || (host->chip_type == CHIP_ALBATROSS)) {
		if (rtd3_setting.rtd3_ctrl_mode) {
			/* d3_mode = rtd3_setting.d3_work_mode_sel; */
			pci_andl(host, 0x3f0, ~(3 << 28));
			pci_orl(host, 0x3f0, (d3_mode & 3) << 28);
			pci_andl(host, 0x3f0, ~(1 << 26));
			pci_orl(host, 0x3f0,
				(rtd3_setting.d3silence_submode_sel << 26));
			*d3_submode = rtd3_setting.d3silence_submode_sel;

		} else {
			d3_mode = (pci_readl(host, 0x3f0) >> 28) & 3;
			*d3_submode = (pci_readl(host, 0x3f0) >> 26) & 1;
		}
	}

	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE, NOT_TO_RAM,
		"Exit %s, D3 mode:%x\n", __func__, d3_mode);
	return d3_mode;
}

/*
 *
 * Function Name: hostven_pm_mode_cfg
 *
 * Abstract:
 *
 *			host vendor pm mode configure feature
 *
 *
 *
 * Input:
 *
 *			host [in]: Points to the host structure.
 *			pm_state_t *pm [in/out]: update pm state config.
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			null
 *
 * Notes:
 *
 */

void hostven_pm_mode_cfg(sd_host_t *host, pm_state_t *pm)
{

	switch (host->chip_type) {

	case CHIP_SEAEAGLE2:
	case CHIP_GG8:
	case CHIP_ALBATROSS:
		{
#define D3_SILENCE_WORK_MODE	2
#define D3_SILENCE_SUB_MODE2_EN	1
			u32 d3_silc_sub = 0;

			pm->d3_silc_en =
				hostven_d3_mode_sel(host, &d3_silc_sub) == D3_SILENCE_WORK_MODE
				? TRUE : FALSE;
			pm->d3_silc_submode2_en =
			    D3_SILENCE_SUB_MODE2_EN ==
			    d3_silc_sub ? TRUE : FALSE;
			if (pm->d3_silc_en && pm->d3_silc_submode2_en)
				pm->rtd3_en = FALSE;
		}
		break;
	case CHIP_SEABIRD:
		{
			u32 sub_chip_id;

			sub_chip_id =
			    ((host->device_id & 0x1) << 2) +
			    ((pci_readl(host, 0x3e4) & 0xC0000000) >> 30);

			switch (sub_chip_id) {
			case 0x5:
				host->ven_cap.pm.rtd3_hot = 1;
				host->ven_cap.pm.rtd3_cold = 0;
				host->ven_cap.pm.d3_silence = 1;
				host->ven_cap.pm.l1_substate = 1;
				host->ven_cap.pm.ltr = 1;
				break;
			case 0x3:
				host->ven_cap.pm.rtd3_hot = 0;
				host->ven_cap.pm.rtd3_cold = 0;
				host->ven_cap.pm.d3_silence = 1;
				host->ven_cap.pm.l1_substate = 0;
				host->ven_cap.pm.ltr = 0;
				break;
			case 0x2:
				host->ven_cap.pm.rtd3_hot = 0;
				host->ven_cap.pm.rtd3_cold = 0;
				host->ven_cap.pm.d3_silence = 0;
				host->ven_cap.pm.l1_substate = 0;
				host->ven_cap.pm.ltr = 0;
				break;
			case 0x000:
			case 0x001:
			case 0x4:
			case 0x6:
			case 0x7:
			default:
				host->ven_cap.pm.rtd3_hot = 1;
				host->ven_cap.pm.rtd3_cold = 1;
				host->ven_cap.pm.d3_silence = 1;
				host->ven_cap.pm.l1_substate = 1;
				host->ven_cap.pm.ltr = 1;
			}
			DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE, NOT_TO_RAM,
				"subchipid is %x pm(%xh)\n", sub_chip_id,
				host->ven_cap.pm);
		}
		{

		}
		break;
	default:
		break;
	}

}

/*
 *
 * Function Name: hostven_main_power_ctrl
 *
 * Abstract:
 *
 *       Set the main power control for D3 silence sub mode 2.
 *
 *       D3Silence submode2 main power control source:
 *       1'b0: Main power will be kept on.
 *       1'b1: Main power control derives from D3Silence logic according to
 *       entry and exit conditions.
 *       Driver configures this bit when SeaEagle2 is idle and can be auto-power-off.
 *       Default value is 1'b0.
 *
 *
 *
 * Input:
 *
 *			host [in]: Points to the host structure.
 *       is_keep_on [in]: The main power is kept on or not.
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			The D3 work mode value.
 *
 * Notes:
 *
 */
void hostven_main_power_ctrl(sd_host_t *host, bool is_keep_on)
{
	bht_dev_ext_t *pdev_ext = (bht_dev_ext_t *) host->pdx;
	u16 regval;

	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE, NOT_TO_RAM,
		"Enter %s is_keep_on:%xh\n", __func__, is_keep_on);

	switch (host->chip_type) {
	case CHIP_SEAEAGLE2:
	case CHIP_GG8:
	case CHIP_ALBATROSS:
		{
			if (pdev_ext->pm_state.d3_silc_en
			    && pdev_ext->pm_state.d3_silc_submode2_en) {
				if (is_keep_on)
					pci_andl(host, 0x3f0, ~(1 << 25));
				else {
					/* open the soft L0 request */
					pci_orl(host, 0x3e4, 1 << 23);
					/* Power off the chip */
					pci_andl(host, 0x3f0, ~(1 << 25));
					pci_orl(host, 0x3f0, 1 << 25);

				}
			}

			/* clear VDD2/3 GPIO power control inverter setting */
			regval = ven_readw(host, SDBAR1_GPIO_FUNC_GPIOCTRL_510);
			regval &= ~(1 << 7 | 1 << 15);
			ven_writew(host, SDBAR1_GPIO_FUNC_GPIOCTRL_510, regval);
		}
		break;
	case CHIP_SEABIRD:
		{
			if (pdev_ext->pm_state.d3_silc_en) {
				if (is_keep_on) {
					pci_orl(host, 0x3e4, 0x40000);
					pci_orl(host, 0x3e0, 0x80000000);
					pci_orl(host, 0x3e0, 0x40000000);
					pci_orl(host, 0xD8, 0x240);

				} else {
					/* power off chip */
					pci_orl(host, 0x3e4, 0x40000);
					pci_orl(host, 0x3e0, 0x80000000);
					pci_andl(host, 0xD8, (~0x240));
					pci_andl(host, 0x3e0, (~0x40000000));
				}
			}
		}
		break;
	default:
		break;
	}
	DbgInfo(MODULE_VEN_HOST, FEATURE_PM_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/*
 * Function Name: hostven_d3_mode_sel_se2
 *
 * Abstract:
 *
 *			Select the D3 work mode. D3 work mode: start-up mode(2'b11),
 *			RTD3 cold with external FET (2'b00),
 *			RTD3 cold with internal FET (2'b01), D3 silence (2'b10).
 *
 * Input:
 *
 *			host [in]: Points to the host structure.
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			The D3 work mode value.
 *
 * Notes:
 *
 */
bool hostven_hs400_host_chk(sd_host_t *host)
{
	u32 hs400_sel_mode = 0;
	bool ted_ip_en = FALSE;

	DbgInfo(MODULE_VEN_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	hs400_sel_mode = pci_readl(host, 0x320);
	if (hs400_sel_mode & (1 << 5))
		ted_ip_en = TRUE;

	DbgInfo(MODULE_VEN_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Exit %s,TED IP enable:%x\n", __func__, ted_ip_en);
	return ted_ip_en;
}

void hostven_hw_timer_start(sd_host_t *host, u32 time_ms)
{
	DbgInfo(MODULE_VEN_HOST, FEATURE_TIMER_TRACE, NOT_TO_RAM,
		"Timer: %s time_ms=%d\n", __func__, time_ms);
	time_ms *= 2000;
	pci_orl(host, 0x3f0, (3 << 30));
	ven_or32(host, 0x518, BIT7);

	pci_andl(host, 0x414, 0xfc000000);
	pci_orl(host, 0x414, time_ms);
	pci_orl(host, 0x414, BIT26);
	pci_orl(host, 0x520, BIT7);
}

void hostven_hw_timer_stop(sd_host_t *host)
{
	DbgInfo(MODULE_VEN_HOST, FEATURE_TIMER_TRACE, NOT_TO_RAM, "Timer: %s\n",
		__func__);
	pci_andl(host, 0x414, ~(BIT26));
	ven_and32(host, 0x518, ~(BIT7));
	pci_andl(host, 0x3f0, ~(3 << 30));
	pci_andl(host, 0x520, ~(BIT7));
}

void hostven_set_pml0_requrest(sd_host_t *host, bool enable)
{
	DbgInfo(MODULE_VEN_HOST, FEATURE_TIMER_TRACE, NOT_TO_RAM, "%s %xh\n",
		__func__, enable);
	if (host->chip_type == CHIP_SEAEAGLE) {
		if (enable == TRUE)
			pci_andl(host, 0x3e0, ~(BIT19));
		else
			pci_orl(host, 0x3e0, (BIT19));
	}
}
