// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: host.c
 *
 * Abstract: Include host related common functions.
 *
 * Version: 1.00
 *
 * Author: Samuel
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 9/2/2014		Creation	Samuel
 */
#include "../include/basic.h"
#include "../include/host.h"
#include "hostreg.h"
#include "../include/hostapi.h"
#include "../include/debug.h"
#include "hostven.h"
#include "../include/util.h"
#include "../include/hostvenapi.h"
#define UHS2_VENCNT_OFFSET		0x10
#define UHS2_DAT3MD_OFFSET		0x14
#define UHS2_EXTCNT_OFFSET		0x18
#define UHS2_VENGIO_OFFSET		0x20
#define GPIO2 0
#define GPIO3 1
#define GPIO_HIGHT 1
#define GPIO_LOW 0
#define START_BIT 0
#define END_BIT 1
#define SPECIAL_PATTERN 2
#define UHS1_BIT_EN (1 << 0)
#define UHS2_BIT_EN (1 << 1)
#define SD70_BIT_EN (1 << 2)
#define VDD1_BIT_EN (1 << 4)
#define VDD2_BIT_EN (1 << 5)
static void host_uhs2_init_capability(sd_host_t *host);
static void host_pll_enable(sd_host_t *host, bool enable);
static void host_uhs2_reg_clean(sd_host_t *host);
static byte host_get_datline_state(sd_host_t *host);
/* static byte host_get_cmdline_state(sd_host_t *host); */
static u16 host_check_1_8v_signal(sd_host_t *host);
void host_enable_clock(sd_host_t *host, bool on);
static bool host_uhs2_wait_dmt(sd_host_t *host);

static void host_update_clock(sd_host_t *host, u32 basediv)
{

	/* 1. Clear Divider */
	u32 reg;

	sdhci_and32(host, SDHCI_CLOCK_CONTROL, SDHCI_DIVIDER_CLEAR);
	reg = (((basediv << 8) & 0xff00) | ((basediv & 0x300) >> 2));
	sdhci_or32(host, SDHCI_CLOCK_CONTROL, reg);
}

/*
 *
 * Function Name: host_set_highspeed
 *
 * Abstract:
 *
 *			 1. Set Host to Highspeed or clear Highspeed
 *
 * Input:
 *
 *			sd_host_t *host: Pointer to the host structure
 *           bool on: Highspeed on
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			None.
 *
 * Notes:
 *
 *           Caller: sd_switch_function_set, sd_legacy_init
 */
void host_set_highspeed(sd_host_t *host, bool on)
{
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, on=%d\n", __func__, on);

	if (on)
		sdhci_or32(host, SDHCI_HOST_CONTROL, SDHCI_CTRL_HISPD);
	else
		sdhci_and32(host, SDHCI_HOST_CONTROL, ~(SDHCI_CTRL_HISPD));
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);

}

void host_set_tuning_mode(sd_host_t *host, bool hw_mode)
{
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, hw_mode=%d\n", __func__, hw_mode);

	/* 1. Set the HW/SW tuning mode */
	if (hw_mode) {
		/* Set driver HW mode here, 0x110[4] set 1'b0 to enable HW mode */
		sdhci_and16(host, SDHCI_VEN_SPEC_CTRL, ~(SDHCI_HW_TUNING));
	} else {
		/* Set driver SW mode here, 0x110[4] set 1'b1 to enable SW mode */
		sdhci_or16(host, SDHCI_VEN_SPEC_CTRL, (SDHCI_HW_TUNING));
	}

	/* 2. Trigger tuning phase */
	sdhci_or16(host, SDHCI_HOST_CONTROL2, SDHCI_CTRL_EXEC_TUNING);

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);

}

bool host_chk_tuning_comp(sd_host_t *host, bool hwtuning)
{
	u16 regval;
	u32 delay_ms = 1;
	loop_wait_t wait;
	bool ret = FALSE;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, hwtuning=%d\n", __func__, hwtuning);

	if (hwtuning) {
		/* Check HW tuning complete */
		util_init_waitloop(host->pdx, 100, delay_ms * 1000, &wait);
		do {
			/* Check tuning complete */
			regval = sdhci_readw(host, SDHCI_HOST_CONTROL2);
			if ((regval & SDHCI_CTRL_EXEC_TUNING) == 0) {
				regval = sdhci_readw(host, SDHCI_HOST_CONTROL2);
				if ((regval & SDHCI_CTRL_TUNED_CLK)) {
					DbgInfo(MODULE_SD_HOST,
						FEATURE_CARD_INIT, NOT_TO_RAM,
						"Tuning function %d OK!\n");
					ret = TRUE;
				} else {
					DbgErr(" - Tuning failed.\n");
				}
				break;
			}
			/* Delay 1ms */
			os_mdelay(delay_ms);
		} while (!util_is_timeout(&wait));
	} else {
		regval = sdhci_readw(host, SDHCI_HOST_CONTROL2);
		if ((regval & SDHCI_CTRL_EXEC_TUNING) == 0) {
			DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
				"Tuning function %d OK!\n");
			ret = TRUE;
		}
	}
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return ret;
}

void host_enable_pll_software_reset(sd_host_t *host, bool on)
{
	loop_wait_t wait;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s on=%d\n", __func__, on);

	/* Force L0 before PLL reset:0x3E4[23] = 1 */
	pci_orl(host, 0x3e4, BIT23);
	if (on) {
		sdhci_or32(host, SDHCI_DLL_WATCH_DOG, (SDHCI_PLL_RESET));
		util_init_waitloop(host->pdx, 5000, 10, &wait);
		while (!util_is_timeout(&wait)) {
			if ((sdhci_readl(host, SDHCI_DLL_WATCH_DOG) &
			     SDHCI_PLL_UNLOCKBIT) == 0) {
				break;
			}
		}
	} else {
		sdhci_and32(host, SDHCI_DLL_WATCH_DOG, ~(SDHCI_PLL_RESET));
		util_init_waitloop(host->pdx, 5000, 10, &wait);
		while (!util_is_timeout(&wait)) {
			if (sdhci_readl(host, SDHCI_DLL_WATCH_DOG) &
			    SDHCI_PLL_UNLOCKBIT) {
				break;
			}
		}
	}
	/* Cancel force L0 before PLL reset */
	pci_andl(host, 0x3e4, ~BIT23);
	os_mdelay(10);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);

}

void host_change_clock(sd_host_t *host, u32 value)
{
	u32 dmdn, basediv;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, value=0x%x\n", __func__, value);
	basediv = value & 0x7FFF;
	dmdn = ((value & 0xFFFF0000) >> 16);

	host_enable_clock(host, FALSE);
	host_enable_pll_software_reset(host, TRUE);
	hostven_update_dmdn(host, dmdn);
	host_update_clock(host, basediv);
	host_enable_pll_software_reset(host, FALSE);
	host_enable_clock(host, TRUE);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

void host_init_clock(sd_host_t *host, u32 value)
{
	u32 dmdn, basediv;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, value=0x%x\n", __func__, value);
	basediv = value & 0x7FFF;
	dmdn = ((value & 0xFFFF0000) >> 16);

	hostven_update_dmdn(host, dmdn);
	host_update_clock(host, basediv);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

void host_init_400k_clock(sd_host_t *host)
{
	if (host->cfg == NULL || host->cfg->dmdn_tbl == NULL) {
		DbgErr("Host cfg is null\n");
		return;
	}

	host_init_clock(host, host->cfg->dmdn_tbl[FREQ_400K_START_INDEX]);
}

static void host_enable_clock_nodelay(sd_host_t *host, bool on)
{
	loop_wait_t wait;
	u32 delay_us = 1;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_OPS | FEATURE_CARD_INIT,
		NOT_TO_RAM, "Enter %s on=%d\n", __func__, on);
	if (on == FALSE) {
		util_init_waitloop(host->pdx, 5000, delay_us, &wait);
		while (!util_is_timeout(&wait)) {
			if ((sdhci_readl(host, SDHCI_PRESENT_STATE) &
			     (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT)) == 0)
				break;
			/* avoid long wait for 7.0 SD driver switch to NVMe */
			else if (host->sd_express_flag == TRUE)
				break;
			else if (sdhci_readl(host, SDHCI_PRESENT_STATE) ==
				 0xffffffff)
				break;

			os_udelay(delay_us);
		}
		sdhci_and16(host, SDHCI_CLOCK_CONTROL, ~(SDHCI_CLOCK_CARD_EN));
	} else {
		/* enable SD clock */
		sdhci_or32(host, SDHCI_CLOCK_CONTROL, (SDHCI_CLOCK_CARD_EN));
	}

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_OPS | FEATURE_CARD_INIT,
		NOT_TO_RAM, "Exit %s\n", __func__);
}

void host_enable_clock(sd_host_t *host, bool on)
{
	u16 reg = sdhci_readw(host, SDHCI_CLOCK_CONTROL);

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s on=%d\n", __func__, on);

	if (on && (reg & SDHCI_CLOCK_CARD_EN))
		goto exit;
	else if ((on == 0) && (!(reg & SDHCI_CLOCK_CARD_EN)))
		goto exit;

	host_enable_clock_nodelay(host, on);

exit:
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/*
 *
 * Function Name: host_dma_select
 *
 * Abstract:
 *
 *			 1. select HOST dma mode
 *
 * Input:
 *
 *			sd_host_t *host: Pointer to the host structure
 *           e_trans_type mode
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			None.
 *
 * Notes:
 *
 *           Caller: sd_legacy_init
 */

void host_dma_select(sd_host_t *host, e_trans_type mode)
{
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	sdhci_and32(host, SDHCI_HOST_CONTROL, ~(SDHCI_CTRL_DMA_MASK));
	switch (mode) {
	case TRANS_SDMA:
		sdhci_or32(host, SDHCI_HOST_CONTROL, SDHCI_CTRL_SDMA);
		break;
	case TRANS_ADMA2:
		sdhci_or32(host, SDHCI_HOST_CONTROL, SDHCI_CTRL_ADMA32);
		break;
	case TRANS_ADMA3:
		sdhci_or32(host, SDHCI_HOST_CONTROL, SDHCI_CTRL_ADMA64);
		break;
	default:
		sdhci_or32(host, SDHCI_HOST_CONTROL, SDHCI_CTRL_ADMA32);
		break;
	}
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);

}

void host_set_uhs_mode(sd_host_t *host, byte uhs_mode)
{
	u16 reg = 0;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, access_mode=%d\n", __func__, uhs_mode);

	reg = sdhci_readw(host, SDHCI_HOST_CONTROL2);
	reg &= ~SDHCI_CTRL_UHS_MASK;
	reg |= uhs_mode;
	sdhci_writew(host, SDHCI_HOST_CONTROL2, reg);

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);

}

/*
 *
 * Function Name: host_set_buswidth
 *
 * Abstract:
 *
 *			 1. Set BUS WIDTH(4bit or 1bit) Function
 *
 * Input:
 *
 *			sd_host_t *host: Pointer to the host structure
 *           bool bus_width_4: 4Bit or not.
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			None.
 *
 * Notes:
 *
 *           Caller: sd_legacy_init
 */
void host_set_buswidth(sd_host_t *host, e_bus_width width)
{
	u16 reg;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, width=%d\n", __func__, width);

	reg = sdhci_readw(host, SDHCI_HOST_CONTROL);
	reg &= ~(SDHCI_CTRL_4BITBUS | SDHCI_CTRL_8BITBUS);
	switch (width) {
	case BUS_WIDTH4:
		reg |= SDHCI_CTRL_4BITBUS;
		break;
	case BUS_WIDTH8:
		reg |= SDHCI_CTRL_8BITBUS;
		break;
	default:
		break;
	}

	sdhci_writew(host, SDHCI_HOST_CONTROL, reg);

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);

}

static byte host_get_datline_state(sd_host_t *host)
{
	u32 data_line_state = 0;

	data_line_state = sdhci_readl(host, SDHCI_PRESENT_STATE);
	data_line_state &= SDHCI_DATA_LVL_MASK;
	data_line_state = data_line_state >> SDHCI_DATA_LVL_SHIFT;

	return (byte) data_line_state;
}

/*
 *	static byte host_get_cmdline_state(sd_host_t *host)
 *	{
 *		u32 regval = 0;
 *
 *		regval = sdhci_readl(host, SDHCI_PRESENT_STATE);
 *		regval &= SDHCI_CMD_LVL_MASK;
 *		regval = regval >> SDHCI_CMD_LVL_SHIFT;
 *
 *		return (byte) regval;
 *	}
 */

void host_1_8v_sig_set(sd_host_t *host, bool enable)
{
	u16 regval;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, enable=%d\n", __func__, enable);

	if (enable) {
		regval = sdhci_readw(host, SDHCI_HOST_CONTROL2);
		regval |= SDHCI_CTRL_VDD_180;
		sdhci_writew(host, SDHCI_HOST_CONTROL2, regval);
	} else {
		regval = sdhci_readw(host, SDHCI_HOST_CONTROL2);
		regval &= ~(SDHCI_CTRL_VDD_180);
		sdhci_writew(host, SDHCI_HOST_CONTROL2, regval);
	}

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

void host_sig_vol_set(sd_host_t *host, e_sig_vol sig_vol)
{
	u16 regval;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, sig_vol=%d\n", __func__, sig_vol);

	/* 0x3e[3], clear 1.8V Signal Voltage */
	regval = sdhci_readw(host, SDHCI_HOST_CONTROL2);
	regval &= ~(SDHCI_CTRL_VDD_180);
	sdhci_writew(host, SDHCI_HOST_CONTROL2, regval);

	/* 0x1cc[31], clear 1.2V Signal Voltage */
	sdhci_and32(host, SDHCI_DLL_WATCH_DOG, ~(SDHCI_CTRL_VDD2_120));

	if (sig_vol == SIG_VOL_18) {
		regval = sdhci_readw(host, SDHCI_HOST_CONTROL2);
		regval |= SDHCI_CTRL_VDD_180;
		sdhci_writew(host, SDHCI_HOST_CONTROL2, regval);
	} else if (sig_vol == SIG_VOL_12) {
		regval = sdhci_readw(host, SDHCI_HOST_CONTROL2);
		regval |= SDHCI_CTRL_VDD_180;
		sdhci_writew(host, SDHCI_HOST_CONTROL2, regval);

		sdhci_or32(host, SDHCI_DLL_WATCH_DOG, SDHCI_CTRL_VDD2_120);
	}

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

static u16 host_check_1_8v_signal(sd_host_t *host)
{
	u16 regval;

	regval = sdhci_readw(host, SDHCI_HOST_CONTROL2);
	regval &= SDHCI_CTRL_VDD_180;
	return regval;
}

/*
 *	static bool host_check_voltage_stable(sd_host_t *host)
 *	{
 *		bool ret = FALSE;
 *		u32 delay_ms = 1;
 *		loop_wait_t wait;
 *
 *		if (host->feature.hw_41_supp == 0) {
 *			ret = TRUE;
 *			goto exit;
 *		}
 *
 *		util_init_waitloop(host->pdx, 50, delay_ms * 1000, &wait);
 *		while (!util_is_timeout(&wait)) {
 *			if ((sdhci_readl(host, SDHCI_PRESENT_STATE) & (1 << 25)) != 0) {
 *				DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
 *					"check voltage stable!\n");
 *				ret = TRUE;
 *				break;
 *			}
 *			os_mdelay(delay_ms);
 *		}
 *	exit:
 *		return ret;
 *	}
 */

void host_led_ctl(sd_host_t *host, bool on)
{
	if (on) {
		host->led_on = TRUE;
		sdhci_or16(host, SDHCI_HOST_CONTROL, SDHCI_CTRL_LED);
		if (host->chip_type == CHIP_SEABIRD)
			pci_orl(host, 0xd4, 0x40);
	} else {
		host->led_on = FALSE;
		sdhci_and16(host, SDHCI_HOST_CONTROL, ~SDHCI_CTRL_LED);
		if (host->chip_type == CHIP_SEABIRD)
			pci_andl(host, 0xd4, ~0x40);
	}
}

/*
 *
 * Function Name: host_set_vdd1_power
 *
 * Abstract:
 *
 *            1. Set SD Bus Voltage Select for VDD1
 *			 2. VDD1 power on/off
 *
 * Input:
 *
 *			sd_host_t *host: Pointer to the host structure
 *           bool on_off: power On or Off.
 *           u32 vol_sel:
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			None.
 *
 * Notes:
 *
 *           Caller: sd_legacy_init
 */
void host_set_vdd1_power_nodelay(sd_host_t *host, bool on, u32 vol_sel)
{
	u16 regval;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT | FEATURE_CARD_OPS,
		NOT_TO_RAM, "Enter %s, on=%d, vol_sel=0x%x\n", __func__, on,
		vol_sel);

	regval = sdhci_readw(host, SDHCI_HOST_CONTROL);

	if (on) {
		/* Clear the SD Bus Voltage Select for VDD1 */
		regval &= ~(SDHCI_POWER_VDD1_MASK);

		/* Select VDD1 voltage */
		if (vol_sel == SDHCI_POWER_VDD1_180) {
			regval |= SDHCI_POWER_VDD1_180;
			DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
				"Set VDD1 Voltage Select to 1.8V\n");

		} else if (vol_sel == SDHCI_POWER_VDD1_300) {
			regval |= SDHCI_POWER_VDD1_300;
			DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
				"Set VDD1 Voltage Select to 3.0V\n");
		} else if (vol_sel == SDHCI_POWER_VDD1_330) {
			regval |= SDHCI_POWER_VDD1_330;
			DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
				"Set VDD1 Voltage Select to 3.3V\n");
		}
		/* Set SD Bus Power Select for VDD1 and Power ON VDD1. */
		DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Set VDD1 Power ON\n");
		sdhci_writew(host, SDHCI_HOST_CONTROL,
			     regval | SDHCI_POWER_VDD1_ON);

	} else {
		/* Power off the VDD1 */
		regval &= ~(SDHCI_POWER_VDD1_ON | SDHCI_POWER_VDD1_MASK);
		sdhci_writew(host, SDHCI_HOST_CONTROL, regval);
		DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Set VDD1 Power OFF\n");

	}

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT | FEATURE_CARD_OPS,
		NOT_TO_RAM, "Exit %s\n", __func__);
}

void host_set_vdd1_power(sd_host_t *host, bool on, u32 vol_sel)
{
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, on=%d, vol_sel=0x%x\n", __func__, on, vol_sel);

	host_set_vdd1_power_nodelay(host, on, vol_sel);
	if (on) {
		os_mdelay(host->cfg->timeout_item.power_wait_time.power_on_wait_ms);
		host_chk_ocb_occur(host);
	} else {
		os_mdelay(host->cfg->timeout_item.power_wait_time.power_off_wait_ms);
	}
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/*
 *
 * Function Name: host_set_vdd2_power
 *
 * Abstract:
 *
 *            1. Set SD Bus Voltage Select for VDD2
 *			 2. VDD2 power on/off
 *
 * Input:
 *
 *			sd_host_t *host: Pointer to the host structure
 *           bool on_off: power On or Off.
 *           u32 vol_sel:
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			None.
 *
 * Notes:
 *
 *           Caller: sd_legacy_init
 */

void host_set_vdd2_power(sd_host_t *host, bool on, u32 vol_sel)
{
	u16 regval;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, on=%d, vol_sel=0x%x\n", __func__, on, vol_sel);

	regval = sdhci_readw(host, SDHCI_HOST_CONTROL);
	if (on) {
		/* Clear the SD Bus Voltage Select for VDD2 */
		regval &= ~(SDHCI_POWER_VDD2_MASK);

		/* Select VDD2 voltage */
		if (vol_sel == SDHCI_POWER_VDD2_180) {
			regval |= SDHCI_POWER_VDD2_180;
			DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
				"Set VDD2 Voltage Select to 1.8V\n");

		} else if (vol_sel == SDHCI_POWER_VDD2_120) {
			regval |= SDHCI_POWER_VDD2_120;
			DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
				"Set VDD2 Voltage Select to 1.2V\n");

		}
		/* Set SD Bus Power Select for VDD1 and Power ON VDD1. */
		sdhci_writew(host, SDHCI_HOST_CONTROL,
			     regval | SDHCI_POWER_VDD2_ON);
		DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Set VDD2 Power ON\n");

		host_chk_ocb_occur(host);
	} else {
		/* Power off the VDD2 */
		regval &= ~(SDHCI_POWER_VDD2_ON | SDHCI_POWER_VDD2_MASK);
		sdhci_writew(host, SDHCI_HOST_CONTROL, regval);
		DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Set VDD2 Power OFF\n");
	}
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

void host_set_vddx_power(sd_host_t *host, u8 vddx, bool on)
{
	cfg_vdd_power_source_item_t *cfg =
		&host->cfg->host_item.vdd_power_source_item;
	u16 regval;
	u32 regval32;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (host->cfg->driver_item.camera_mode_ctrl_vdd1_vdd2_cd == 1)
		goto camera_mode;
	else
		goto pc_mode;

pc_mode:

	switch (vddx) {
	case VDD1:
		{
			/*
			 * power source internal/external and polarity refer to:
			 * hosteven.c: hostven_bios_cfg()
			 */

			if (on) {

				/* set power voltage */
				if (cfg->vdd1_voltage == VDDX_PWR_VOLTAGE_3V3) {
					/* set vdd1 3.3V and power on it */
					regval =
					    sdhci_readw(host,
							SDHCI_POWER_CONTROL);
					regval |= (0xF << 0);
					sdhci_writew(host, SDHCI_POWER_CONTROL,
						     regval);
					DbgInfo(MODULE_SD_HOST,
						FEATURE_CARD_INIT, NOT_TO_RAM,
						"Set VDD1 Power On\n");

				} else {
					DbgErr("VDD1 only support 3.3V!\n");
				}

				/*
				 * delay to avoid 3.3V power switch signal level issue.
				 * no need this delay for VDD2, VDD3
				 */
				os_mdelay(host->cfg->timeout_item.power_wait_time.power_on_wait_ms);
			} else {
				/* set vdd1 power off */
				regval = sdhci_readw(host, SDHCI_POWER_CONTROL);
				regval &= ~(0xF << 0);
				sdhci_writew(host, SDHCI_POWER_CONTROL, regval);
				DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT,
					NOT_TO_RAM, "Set VDD1 Power Off\n");

				os_mdelay(host->cfg->timeout_item.power_wait_time.power_off_wait_ms);
			}

		}
		break;

	case VDD2:
		{
			/*
			 * power source internal/external and polarity refer to:
			 * hosteven.c: hostven_bios_cfg()
			 */

			if (on) {
				/* set power voltage */
				regval = sdhci_readw(host, SDHCI_POWER_CONTROL);
				if (cfg->vdd2_voltage == VDDX_PWR_VOLTAGE_1V8) {
					/* VDD2 1.8V and power on. */
					regval &= ~(0xF << 4);
					regval |= (0xB << 4);
					DbgInfo(MODULE_SD_HOST,
						FEATURE_CARD_INIT, NOT_TO_RAM,
						"Set VDD2 Power On 1.8V\n");
				} else if (cfg->vdd2_voltage ==
					   VDDX_PWR_VOLTAGE_1V2) {
					/* VDD2 1.2V and power on. */
					regval &= ~(0xF << 4);
					regval |= (0x9 << 4);
					DbgInfo(MODULE_SD_HOST,
						FEATURE_CARD_INIT, NOT_TO_RAM,
						"Set VDD2 Power On 1.2V\n");
				} else {
					DbgErr
					    ("VDD2 only support 1.8V or 1.2V!\n");
				}
				sdhci_writew(host, SDHCI_POWER_CONTROL, regval);

				/* set on-off control by gpio */
				if (cfg->vdd2_use_gpio1) {
					/* use gpio1 as on-off control */
					regval =
					    ven_readw(host,
						      SDBAR1_GPIO_FUNC_GPIOCTRL_510);

					/* active high */
					if (cfg->vdd2_onoff_polarity ==
					    VDDX_POLARITY_ACTIVE_HIGH) {
						/* enable gpio1 && output high. */
						regval |= (1 << 3) | (1 << 5);
					} else {
						/* enable gpio1 && output low. */
						regval |= (1 << 3);
						regval &= ~(1 << 5);
					}
					ven_writew(host,
						   SDBAR1_GPIO_FUNC_GPIOCTRL_510,
						   regval);

					if (host->sd_express_flag == TRUE) {
						/*
						 * VDD2 set GPIO power control inverter for
						 * SD7.0 switch to SD driver
						 * hardware auto power-off.
						 */
						regval =
						    ven_readw(host,
							      SDBAR1_GPIO_FUNC_GPIOCTRL_510);
						regval |= (1 << 7);
						ven_writew(host,
							   SDBAR1_GPIO_FUNC_GPIOCTRL_510,
							   regval);
					}

				}

			}
			/* power off */
			else {

				/* common part for all power source cases */
				regval = sdhci_readw(host, SDHCI_POWER_CONTROL);
				/* set vdd2 power off */
				regval &= ~(0xF << 4);
				sdhci_writew(host, SDHCI_POWER_CONTROL, regval);
				DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT,
					NOT_TO_RAM, "Set VDD2 Power Off\n");

				/* use gpio1 as on-off control */
				if (cfg->vdd2_use_gpio1) {
					regval =
					    ven_readw(host,
						      SDBAR1_GPIO_FUNC_GPIOCTRL_510);
					/* active high */
					if (cfg->vdd2_onoff_polarity ==
					    VDDX_POLARITY_ACTIVE_HIGH) {
						/* gpio1 output low to set power off. */
						regval &= ~(1 << 5);
					} else {
						/* gpio1 output high to set power off. */
						regval |= (1 << 5);
					}
					ven_writew(host,
						   SDBAR1_GPIO_FUNC_GPIOCTRL_510,
						   regval);

					if (host->sd_express_flag == TRUE) {
						/* VDD2 clear GPIO power control */
						regval =
						    ven_readw(host,
							      SDBAR1_GPIO_FUNC_GPIOCTRL_510);
						regval &= ~(1 << 7);
						ven_writew(host,
							   SDBAR1_GPIO_FUNC_GPIOCTRL_510,
							   regval);
					}

				}

			}

		}
		break;

	case VDD3:
		{
			/*
			 * power source internal/external and polarity refer to:
			 * hosteven.c: hostven_bios_cfg()
			 */
			if (on) {

				/* set power enable, Enable VDD3 */
				regval32 =
				    sdhci_readl(host, SDHCI_DRIVER_CTRL_REG);
				regval32 |= (1 << 30);
				sdhci_writel(host, SDHCI_DRIVER_CTRL_REG,
					     regval32);

				/* set on-off control by gpio */
				regval =
				    ven_readw(host,
					      SDBAR1_GPIO_FUNC_GPIOCTRL_510);
				/* active high */
				if (cfg->vdd3_onoff_polarity ==
				    VDDX_POLARITY_ACTIVE_HIGH) {
					/* Set GPIO2 high. */
					regval |= (1 << 13);
				} else {
					/* Set GPIO2 low. */
					regval &= ~(1 << 13);
				}
				ven_writew(host, SDBAR1_GPIO_FUNC_GPIOCTRL_510,
					   regval);

				if (host->sd_express_flag == TRUE) {
					/* VDD3 set GPIO power control inverter. */
					regval =
					    ven_readw(host,
						      SDBAR1_GPIO_FUNC_GPIOCTRL_510);
					regval |= (1 << 15);
					ven_writew(host,
						   SDBAR1_GPIO_FUNC_GPIOCTRL_510,
						   regval);
				}
			}
			/* power off */
			else {

				/* set power disable */
				regval32 =
				    sdhci_readl(host, SDHCI_DRIVER_CTRL_REG);
				/* disable VDD3 */
				regval32 &= ~(1 << 30);
				sdhci_writel(host, SDHCI_DRIVER_CTRL_REG,
					     regval32);

				/* set on-off control by gpio */
				regval =
				    ven_readw(host,
					      SDBAR1_GPIO_FUNC_GPIOCTRL_510);
				/* active high */
				if (cfg->vdd3_onoff_polarity ==
				    VDDX_POLARITY_ACTIVE_HIGH) {
					/* Set GPIO2 low. */
					regval &= ~(1 << 13);
				} else {
					/* Set GPIO2 high. */
					regval |= (1 << 13);
				}
				ven_writew(host, SDBAR1_GPIO_FUNC_GPIOCTRL_510,
					   regval);

				if (host->sd_express_flag == TRUE) {
					/* VDD3 clear GPIO power control inverter. */
					regval =
					    ven_readw(host,
						      SDBAR1_GPIO_FUNC_GPIOCTRL_510);
					regval &= ~(1 << 15);
					ven_writew(host,
						   SDBAR1_GPIO_FUNC_GPIOCTRL_510,
						   regval);
				}
			}

		}
		break;

	default:
		break;
	}
	/* pc_mode exit */
	goto exit;

camera_mode:

	switch (vddx) {
	case VDD1:
		{

			if (on) {
				ven_or16(host, SDBAR1_GPIO_FUNC_GPIOCTRL_510,
					 (1 << 13));

				/* set power voltage */
				if (cfg->vdd1_voltage == VDDX_PWR_VOLTAGE_3V3) {
					regval =
					    sdhci_readw(host,
							SDHCI_POWER_CONTROL);
					/* set vdd1 3.3V and power on it */
					regval |= (0xF << 0);
					sdhci_writew(host, SDHCI_POWER_CONTROL,
						     regval);
					DbgInfo(MODULE_SD_HOST,
						FEATURE_CARD_INIT, NOT_TO_RAM,
						"Set VDD1 Power On\n");

				} else {
					DbgErr("VDD1 only support 3.3V!\n");
				}
				/*
				 * delay to avoid 3.3V power switch signal level issue.
				 * no need this delay for VDD2, VDD3
				 */
				os_mdelay(host->cfg->timeout_item.power_wait_time.power_on_wait_ms);

			} else {
				ven_and16(host, SDBAR1_GPIO_FUNC_GPIOCTRL_510,
					  ~(1 << 13));

				regval = sdhci_readw(host, SDHCI_POWER_CONTROL);
				/* set vdd1 power off */
				regval &= ~(0xF << 0);
				sdhci_writew(host, SDHCI_POWER_CONTROL, regval);
				DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT,
					NOT_TO_RAM, "Set VDD1 Power Off\n");
				os_mdelay(host->cfg->timeout_item.power_wait_time.power_off_wait_ms);
			}

		}
		break;

	case VDD2:
		{
			if (on) {
				ven_or16(host, SDBAR1_GPIO_1_2_CTRL_REG_510,
					 (1 << 5));

				/* set power voltage */
				regval = sdhci_readw(host, SDHCI_POWER_CONTROL);
				if (cfg->vdd2_voltage == VDDX_PWR_VOLTAGE_1V8) {
					/* VDD2 1.8V and power on. */
					regval &= ~(0xF << 4);
					regval |= (0xB << 4);
					DbgInfo(MODULE_SD_HOST,
						FEATURE_CARD_INIT, NOT_TO_RAM,
						"Set VDD2 Power On 1.8V\n");
				} else if (cfg->vdd2_voltage ==
					   VDDX_PWR_VOLTAGE_1V2) {
					/* VDD2 1.2V and power on. */
					regval &= ~(0xF << 4);
					regval |= (0x9 << 4);
					DbgInfo(MODULE_SD_HOST,
						FEATURE_CARD_INIT, NOT_TO_RAM,
						"Set VDD2 Power On 1.2V\n");
				} else {
					DbgErr
					    ("VDD2 only support 1.8V or 1.2V!\n");
				}
				sdhci_writew(host, SDHCI_POWER_CONTROL, regval);

				/* use gpio1 as on-off control */
				if (cfg->vdd2_use_gpio1) {
					regval =
					    ven_readw(host,
						      SDBAR1_GPIO_FUNC_GPIOCTRL_510);
					/* active high */
					if (cfg->vdd2_onoff_polarity ==
					    VDDX_POLARITY_ACTIVE_HIGH) {
						/* enable gpio1 && output high. */
						regval |= (1 << 3) | (1 << 5);
					} else {
						/* enable gpio1 && output low. */
						regval |= (1 << 3);
						regval &= ~(1 << 5);
					}
					ven_writew(host,
						   SDBAR1_GPIO_FUNC_GPIOCTRL_510,
						   regval);

					if (host->sd_express_flag == TRUE) {
						/*
						 * VDD2 set GPIO power control inverter for
						 * SD7.0 switch to SD driver
						 * hardware auto power-off.
						 */
						regval =
						    ven_readw(host,
							      SDBAR1_GPIO_FUNC_GPIOCTRL_510);
						regval |= (1 << 7);
						ven_writew(host,
							   SDBAR1_GPIO_FUNC_GPIOCTRL_510,
							   regval);
					}

				}

			} else {
				ven_and16(host, SDBAR1_GPIO_1_2_CTRL_REG_510,
					  ~(1 << 5));

				regval = sdhci_readw(host, SDHCI_POWER_CONTROL);
				/* set vdd2 power off */
				regval &= ~(0xF << 4);
				sdhci_writew(host, SDHCI_POWER_CONTROL, regval);
				DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT,
					NOT_TO_RAM, "Set VDD2 Power Off\n");

				/* use gpio1 as on-off control */
				if (cfg->vdd2_use_gpio1) {
					regval =
					    ven_readw(host,
						      SDBAR1_GPIO_FUNC_GPIOCTRL_510);
					/* active high */
					if (cfg->vdd2_onoff_polarity ==
					    VDDX_POLARITY_ACTIVE_HIGH) {
						/* gpio1 output low to set power off. */
						regval &= ~(1 << 5);
					} else {
						/* gpio1 output high to set power off. */
						regval |= (1 << 5);
					}
					ven_writew(host,
						   SDBAR1_GPIO_FUNC_GPIOCTRL_510,
						   regval);

					if (host->sd_express_flag == TRUE) {
						/* VDD2 clear GPIO power control */
						regval =
						    ven_readw(host,
							      SDBAR1_GPIO_FUNC_GPIOCTRL_510);
						regval &= ~(1 << 7);
						ven_writew(host,
							   SDBAR1_GPIO_FUNC_GPIOCTRL_510,
							   regval);
					}
				}
			}
		}
		break;
		/* No VDD3 case for camera mode */
	default:
		break;
	}
	/* camera_mode exit */
	goto exit;

exit:
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/*
 *
 * Function Name: host_get_vdd1_state
 *
 * Abstract:
 *
 *            1. Get VDD1 power on/off state
 *
 * Input:
 *
 *			sd_host_t *host: Pointer to the host structure
 *
 * Output:
 *
 *			TRUE: VDD1 power is ON.
 *           FALSE: VDD1 power is OFF.
 *
 * Return value:
 *
 *			None.
 *
 * Notes:
 *
 *           Caller: sd_legacy_init
 */

bool host_get_vdd1_state(sd_host_t *host)
{
	u16 regval;
	u16 vdd1_voltage_sel;
	bool ret = FALSE;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	regval = sdhci_readw(host, SDHCI_HOST_CONTROL);
	vdd1_voltage_sel = regval & 0xE;

	if (shift_bit_func_enable(host))
		goto camera_mode;

	/* pc_mode: */
	if (regval & SDHCI_POWER_VDD1_ON)
		ret = TRUE;
	else
		ret = FALSE;

camera_mode:
	if ((regval & SDHCI_POWER_VDD1_ON) && (vdd1_voltage_sel != 0))
		ret = TRUE;
	else
		ret = FALSE;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"host get vdd1 state: Vdd1 is %s\n", ret ? "ON" : "OFF");

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return ret;
}

void host_reset(sd_host_t *host, u32 resetmode)
{
	u32 delay_us = 1;
	loop_wait_t wait;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, resetmode=0x%x\n", __func__, resetmode);

	sdhci_or32(host, SDHCI_CLOCK_CONTROL, resetmode);
	util_init_waitloop(host->pdx, RESET_FOR_ALL_ABRT_TM, delay_us, &wait);
	while (!util_is_timeout(&wait)) {
		if ((sdhci_readl(host, SDHCI_CLOCK_CONTROL) & resetmode) == 0)
			break;
		else if (sdhci_readl(host, SDHCI_CLOCK_CONTROL) == 0xffffffff)
			break;

		os_udelay(delay_us);
	}

	/* SetSlowLTRRequire(host); */

	/*
	 * host reset will clear host registers, include host power register 0x29
	 * need to clear gpio registers if vdd is external power source.
	 */
	host_set_vddx_power(host, VDD2, POWER_OFF);
	/* host_set_vddx_power(host, VDD3, POWER_OFF); */
	os_mdelay(50);

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

void host_cmddat_line_reset(sd_host_t *host)
{
	host_reset(host, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
}

void host_int_sig_update(sd_host_t *host, u32 int_val)
{
	if (host->dump_mode == FALSE && host->poll_mode == FALSE)
		sdhci_writel(host, SDHCI_SIGNAL_ENABLE, int_val);
	else
		sdhci_writel(host, SDHCI_SIGNAL_ENABLE, 0);
}

void host_uhs2_err_sig_update(sd_host_t *host, u32 int_val)
{
	if (host->dump_mode == FALSE && host->poll_mode == FALSE)
		sdhci_writel(host, SDHCI_UHS2_ERRINT_SIG_EN, int_val);
	else
		sdhci_writel(host, SDHCI_UHS2_ERRINT_SIG_EN, 0);
}

static void host_int_sig_en(sd_host_t *host, u32 int_val)
{

	u32 reg = sdhci_readl(host, SDHCI_SIGNAL_ENABLE);

	host_int_sig_update(host, reg | int_val);
}

void host_int_sig_dis(sd_host_t *host, u32 int_val)
{
	u32 reg = sdhci_readl(host, SDHCI_SIGNAL_ENABLE);

	host_int_sig_update(host, reg & (~int_val));
}

void host_int_clr_status(sd_host_t *host)
{
	sdhci_writel(host, SDHCI_INT_STATUS, 0xffffffff);
}

void host_int_dis_sig_all(sd_host_t *host, bool all)
{
	/* Disable all interrupts except card insert/remove */
	host_int_sig_dis(host, SDHCI_INT_ALL_MASK);
	host_int_sig_en(host, SDHCI_INT_CARD_INSERT | SDHCI_INT_CARD_REMOVE);

}

void host_internal_clk_setup(sd_host_t *host, bool on)
{
	u32 timeout = 10000;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, on=%d\n", __func__, on);

	if (on) {
		/* 1. Set Internal Clock Enable */
		sdhci_or32(host, SDHCI_CLOCK_CONTROL, SDHCI_CLOCK_INT_EN);
		while (timeout) {
			if (sdhci_readl(host, SDHCI_CLOCK_CONTROL) &
			    SDHCI_CLOCK_INT_STABLE)
				goto exit;
			else if (sdhci_readl(host, SDHCI_CLOCK_CONTROL) ==
				 0xffffffff)
				goto exit;

			os_udelay(1);
			timeout--;
		}

	} else {
		sdhci_and32(host, SDHCI_CLOCK_CONTROL, ~(SDHCI_CLOCK_INT_EN));
	}

exit:
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/* this function called to stop host power */
void host_uninit(sd_host_t *host, bool disable_all_int)
{
	if (host_check_lost(host))
		return;

	host_poweroff(host, CARD_NONE);

	host_int_dis_sig_all(host, disable_all_int);
}

void host_poweroff(sd_host_t *host, e_card_type type)
{
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, type=%d\n", __func__, type);

	hostven_set_tuning_phase(host, 0, 0, TRUE);
	host_enable_clock(host, FALSE);

	if (shift_bit_func_enable(host))
		set_pattern_value(host, 0x00);

	host_set_vddx_power(host, VDD2, POWER_OFF);

	if (host_get_vdd1_state(host))
		host_set_vddx_power(host, VDD1, POWER_OFF);

	if (type == CARD_UHS2 || host->uhs2_flag) {
		host_uhs2_reg_clean(host);
	} else if (type == CARD_SD) {
		host_set_highspeed(host, FALSE);
		host_set_uhs_mode(host, 0);
	} else if (type == CARD_NONE || type == CARD_ERROR) {
		host_uhs2_reg_clean(host);
		host_set_highspeed(host, FALSE);
		host_set_uhs_mode(host, 0);
	} else {
		/* MMC and EMMC case */
		host_set_highspeed(host, FALSE);
		host_set_uhs_mode(host, 0);
	}

	host_pll_enable(host, FALSE);
	host_internal_clk_setup(host, FALSE);
	host_led_ctl(host, FALSE);

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

void host_init_capbility(sd_host_t *host)
{
	u32 regval;

	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	host->ocr_avail = 0;

	/* 1. Init ocr host supported */
	regval = sdhci_readl(host, SDHCI_CAPABILITIES);
	if (regval & SDHCI_CAN_VDD_330) {
		host->ocr_avail |= BIT19 | BIT20;
		host->mmc_ocr_avail |= (0x1ff << 15);
	}
	if (regval & SDHCI_CAN_VDD_300)
		host->ocr_avail |= BIT17 | BIT18;
	if (regval & SDHCI_CAN_VDD_180) {
		host->ocr_avail |= BIT7;
		host->mmc_ocr_avail |= BIT7;

	}

	if (regval & SDHCI_CAN_DO_SDMA)
		host->sdma_supp = 1;
	if (regval & SDHCI_CAN_DO_ADMA2)
		host->adma2_supp = 1;
	if (regval & SDHCI_CAN_64BIT_V3)
		host->bit64_v3_supp = 1;
	if (regval & SDHCI_CAN_64BIT_V4)
		host->bit64_v4_supp = 1;
	if (regval & SDHCI_CAN_DO_HISPD)
		host->hs_supp = 1;
	if (regval & SDHCI_CAN_DO_8BIT)
		host->bus_8bit_supp = 1;

	host->max_block_len = (u8) ((regval & SDHCI_MAX_BLOCK_MASK) >> 16);
	host->max_block_len = (2 << (9 + host->max_block_len));

	regval = sdhci_readl(host, SDHCI_CAPABILITIES_1);
	if (regval & SDHCI_CAP1_ADMA3_SUPP)
		host->adma3_supp = 1;
	if (regval & SDHCI_CAN_VDD2_18V)
		host->vdd2_18v_supp = 1;
	if (regval & SDHCI_CAN_VDD2_12V)
		host->vdd2_12v_supp = 1;

	regval = sdhci_readl(host, SDHCI_MAX_CURRENT);
	host->max_18vdd1_current =
	    4 *
	    ((regval & SDHCI_MAX_CURRENT_180_MASK) >>
	     SDHCI_MAX_CURRENT_180_SHIFT);
	host->max_33vdd1_current =
	    4 *
	    ((regval & SDHCI_MAX_CURRENT_330_MASK) >>
	     SDHCI_MAX_CURRENT_330_SHIFT);
	host->max_30vdd1_current =
	    4 *
	    ((regval & SDHCI_MAX_CURRENT_300_MASK) >>
	     SDHCI_MAX_CURRENT_300_SHIFT);

	regval = sdhci_readl(host, SDHCI_MAX_CURRENT_2);
	host->max_vdd2_current =
	    4 *
	    ((regval & SDHCI_MAX_CUR_VDD2_180_MASK) >>
	     SDHCI_MAX_CUR_VDD2_180_SHIFT);

	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"ocr_avail=%d\n", host->ocr_avail);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"mmc_ocr_avail=%d\n", host->mmc_ocr_avail);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"sdma_supp=%d\n", host->sdma_supp);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"adma2_supp=%d\n", host->adma2_supp);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"bit64_v3_supp=%d\n", host->bit64_v3_supp);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"bit64_v4_supp=%d\n", host->bit64_v4_supp);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM, "hs_supp=%d\n",
		host->hs_supp);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"bus_8bit_supp=%d\n", host->bus_8bit_supp);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"max_block_len=%d\n", host->max_block_len);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"adma3_supp=%d\n", host->adma3_supp);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"vdd2_18v_supp=%d\n", host->vdd2_18v_supp);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"vdd2_12v_supp=%d\n", host->vdd2_12v_supp);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"max_18vdd1_current=%d\n", host->max_18vdd1_current);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"max_33vdd1_current=%d\n", host->max_33vdd1_current);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"max_30vdd1_current=%d\n", host->max_30vdd1_current);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"max_vdd2_current=%d\n", host->max_vdd2_current);

	host_uhs2_init_capability(host);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);

}

static u16 host_get_boundary_value(u32 nkb)
{
	switch (nkb) {
	case 4:
		return 0;
	case 8:
		return 1;
	case 16:
		return 2;
	case 32:
		return 3;
	case 64:
		return 4;
	case 128:
		return 5;
	case 256:
		return 6;
	case 512:
		return 7;
	default:
		DbgErr("Error Sdma boundary size\n");
		break;
	}

	return 0;
}

/*
 * Function Name: host_init_internal
 *
 * Abstract: This Function is used to init host setting registers and variables
 *
 * Input:
 *			sd_host_t *host,
 *
 */

static void host_init_internal(sd_host_t *host)
{
	u16 w;
	u32 dma_mode = 0;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	dma_mode = host->cfg->host_item.test_dma_mode_setting.dma_mode;
	host->uhs2_flag = FALSE;

	/* 2. Interrupt enable */

	host_int_clr_status(host);
	host_int_dis_sig_all(host, FALSE);
	sdhci_writel(host, SDHCI_INT_ENABLE, 0xffffffff);

	hostven_ocb_cfg(host);
	hostven_set_output_tuning_phase(host, 0, TRUE);
	hostven_set_tuning_phase(host, 0, 0, TRUE);
	hostven_detect_refclk_count_range_init(host);
	hostven_refclk_stable_detection_circuit(host);
	hostven_pcie_phy_tx_amplitude_adjustment(host);

	if (host->cfg->host_item.test_dma_mode_setting.dma_mode != 0xF) {
		/* 3. After host reset , reset related software variable */

		/* default is 4kb, set according to cfg */
		host->sdma_boundary_val = 0x00;
		host->bit64_enable =
		    (byte) host->cfg->host_item.test_dma_mode_setting.enable_dma_64bit_address;
		if (host->bit64_v3_supp == 0 && host->bit64_v4_supp == 0)
			host->bit64_enable = 0;

		w = sdhci_readw(host, SDHCI_HOST_CONTROL2);

		if (host->cfg->host_item.test_dma_mode_setting.enable_dma_26bit_len
		    || host->cfg->host_item.test_dma_mode_setting.enable_dma_32bit_blkcount
		    || (host->adma3_supp
			&& (2 == dma_mode || 4 == dma_mode || 5 == dma_mode
			    || 6 == dma_mode))
		    || host->bit64_enable)
			host->sd_host4_enable = 1;

		if (host->cfg->host_item.test_dma_mode_setting.enable_dma_26bit_len)
			w |= SDHCI_CTRL_ADMA2_26BIT_EN;

		if (host->bit64_enable)
			w |= SDHCI_CTRL_64BIT_EN;

		if (host->sd_host4_enable) {
			w |= SDHCI_CTRL_VER4_EN;
			sdhci_writew(host, SDHCI_HOST_CONTROL2, w);
		}
	} else {
		host->sd_host4_enable = 0;
		host->bit64_enable = 0;
	}

	host->led_on = FALSE;

	host->sdma_boundary_kb = host->cfg->host_item.test_sdma_boundary.value;
	host->sdma_boundary_val =
	    host_get_boundary_value(host->sdma_boundary_kb);

	/* 4. set timeout */
	w = sdhci_readw(host, SDHCI_TIMEOUT_CONTROL);
	w &= SDHCI_DAT_TIMEOUT_MASK;
	w |= SDHCI_DAT_TIMEOUT_VAL;
	sdhci_writew(host, SDHCI_TIMEOUT_CONTROL, w);

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "uhs2_flag=%d\n",
		host->uhs2_flag);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"sdma_boundary_val=%d\n", host->sdma_boundary_val);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"bit64_enable=%d\n", host->bit64_enable);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"sd_host4_enable=%d\n", host->sd_host4_enable);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "led_on=%d\n",
		host->led_on);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"sdma_boundary_kb=%d\n", host->sdma_boundary_kb);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"sdma_boundary_val=%d\n", host->sdma_boundary_val);

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

void host_init(sd_host_t *host)
{
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* Host soft reset for all registers! */
	if (shift_bit_func_enable(host))
		set_pattern_value(host, 0x00);

	host_reset(host, SDHCI_RESET_ALL);

	/*
	 * if (host->chip_type == CHIP_SEABIRD)
	 * host_pll_enable(host, TRUE);
	 */
	host_init_internal(host);

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/*
 * Function Name: host_uhs2_reset
 * Abstract: This Function is used to do host uhs2 full reset
 *
 * Input:
 *			sd_host_t *host,
 *			bool fullreset: True means do UHS2 Host Full Reset;
 *			False means do host SD_Tran Reset
 *
 *			FullReset will clear all host setting except card power
 *			SD-Tran Reset only clear transfer buffer and interrupt
 *
 */
void host_uhs2_reset(sd_host_t *host, bool fullreset)
{

	u16 reg;
	u32 delay_us = 1;
	loop_wait_t wait;

	u16 set = (fullreset) ? SDHCI_UHS2_FULL_RESET : SDHCI_UHS2_SDTRAN_RESET;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, fullreset=%d\n", __func__, fullreset);

	/* For Full Reset case is called by uhs2_full_reset only we need to wait for dmt bit set */
	if (set == SDHCI_UHS2_FULL_RESET) {
		if (host_uhs2_wait_dmt(host) == FALSE)
			DbgErr("Wait Host Dm set before fullreset failed\n");
	}

	sdhci_or16(host, SDHCI_UHS2_SOFT_RST, set);
	util_init_waitloop(host->pdx, RESET_FOR_ALL_ABRT_TM, delay_us, &wait);
	while (!util_is_timeout(&wait)) {
		reg = sdhci_readw(host, SDHCI_UHS2_SOFT_RST);
		if (!(reg & set))
			break;
		else if (reg == 0xffffffff)
			break;

		os_udelay(delay_us);
	}

	if (fullreset) {
		host_pll_enable(host, FALSE);
		host_init_internal(host);
	} else {
		/* Enable Host interrupt again, Enable UHS2 Err Status */
		host_int_clr_status(host);
		host_int_dis_sig_all(host, FALSE);
		sdhci_writel(host, SDHCI_INT_ENABLE, 0xffffffff);
		sdhci_writel(host, SDHCI_UHS2_ERRINT_STS_EN, 0xFFFFFFFF);
	}

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

bool host_wr_protect_pin(sd_host_t *host)
{
	u32 regval;
	bool ret = TRUE;

	regval = sdhci_readl(host, SDHCI_PRESENT_STATE);
	if (regval & SDHCI_WRITE_PROTECT)
		ret = FALSE;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s,regval:%x,ret:%x\n", __func__, regval, ret);
	return ret;
}

static void host_emmc_power_supply(sd_host_t *host,
				   cfg_emmc_mode_t *emmc_mode, u32 *power_vdd)
{
	u32 power_vdd1, power_vdd2;

	power_vdd1 = power_vdd2 = 0;
	power_vdd1 |= SDHCI_POWER_VDD1_330;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* SE2 chip support two combination mode */
	if ((host->chip_type != CHIP_SEAEAGLE2) && (host->chip_type != CHIP_GG8)
	    && (host->chip_type != CHIP_ALBATROSS)) {
		emmc_mode->enable_18_vcc = 0;
		*power_vdd = power_vdd1;
		*(power_vdd + 1) = power_vdd2;
		goto exit;
	}

	/* 3.3v Vcc + 3.3v Vccq mode: */
	if (((emmc_mode->enable_18_vccq) == (emmc_mode->enable_12_vccq)) ||
	    ((host->vdd2_12v_supp == FALSE) && (host->vdd2_18v_supp == FALSE))
	    ) {
		emmc_mode->enable_18_vccq = emmc_mode->enable_12_vccq = 0;
		*power_vdd = power_vdd1;
		*(power_vdd + 1) = power_vdd2;
		goto exit;
	}

	/* 1.8v Vcc */
	if (emmc_mode->enable_18_vcc)
		power_vdd1 = SDHCI_POWER_VDD1_180;
	else
		power_vdd1 = SDHCI_POWER_VDD1_330;

	/* 1.2v Vccq */
	if ((host->vdd2_12v_supp) && (emmc_mode->enable_12_vccq)
	    ) {
		power_vdd2 = SDHCI_POWER_VDD2_120;
	}
	/* 1.8v Vccq */
	else if ((host->vdd2_18v_supp) && (emmc_mode->enable_18_vccq)
	    ) {
		power_vdd2 = SDHCI_POWER_VDD2_180;
	}

	*power_vdd = power_vdd1;
	*(power_vdd + 1) = power_vdd2;

exit:
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

bool host_emmc_init(sd_host_t *host, cfg_emmc_mode_t *emmc_mode)
{
	bool ret = FALSE;
	u32 power_vdd[2] = { 0 };
	u32 enable_1_8v_sig;
	u32 enable_1_2v_sig;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	host_emmc_power_supply(host, emmc_mode, power_vdd);

	host_internal_clk_setup(host, TRUE);

	/* 1. clear VDD1 */
	if (host_get_vdd1_state(host) == TRUE)
		host_set_vddx_power(host, VDD1, POWER_OFF);

	/* 2. set bus power VDD1 + VDD2 */
	if (power_vdd[1])
		host_set_vddx_power(host, VDD2, POWER_ON);

	host_set_vddx_power(host, VDD1, POWER_ON);

	/* 4. set 1.8V/1.2V IO voltage */
	enable_1_8v_sig = emmc_mode->enable_18_vccq;
	enable_1_2v_sig = emmc_mode->enable_12_vccq;
	if (enable_1_8v_sig)
		host_sig_vol_set(host, SIG_VOL_18);
	else if (enable_1_2v_sig)
		host_sig_vol_set(host, SIG_VOL_12);
	else
		host_sig_vol_set(host, SIG_VOL_33);

	/* start clock to 400KHz */
	host_enable_clock(host, TRUE);
	/* 6. clear bus width (4bit & 8bit) */
	host_set_buswidth(host, BUS_WIDTH1);
	/* 7. clear ddr mode */
	host_emmc_ddr_set(host, FALSE);

	ret = TRUE;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

void host_emmc_hs400_set(sd_host_t *host, bool b_hs400)
{
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, b_hs400=%d\n", __func__, b_hs400);

	if (b_hs400) {
		if (hostven_hs400_host_chk(host) == TRUE) {
			/* 0x110[17] set to 1 */
			sdhci_or32(host, SDHCI_VEN_SPEC_CTRL,
				   SDHCI_ENABLE_HS400);
		} else {
			/* set host to HS400 mode */
			host_set_uhs_mode(host, SDHCI_CTRL_UHS_HS400);
		}
	} else {
		/* hs200 set 0x110[17] to 0 */
		sdhci_and32(host, SDHCI_VEN_SPEC_CTRL, ~(SDHCI_ENABLE_HS400));
	}

	/* 0x28[2] set to 1 */
	host_set_highspeed(host, TRUE);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

void host_emmc_ddr_set(sd_host_t *host, bool b_ddr)
{
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, b_ddr=%d\n", __func__, b_ddr);

	if (b_ddr) {
		if ((host->chip_type == CHIP_SDS0)
		    || (host->chip_type == CHIP_SDS1)
		    || (host->chip_type == CHIP_FUJIN2)
		    || (host->chip_type == CHIP_SEABIRD)
		    || (host->chip_type == CHIP_SEAEAGLE)
		    ) {
			/* For SDS|SB|FJ2|SE chip:Set eMMC DDR mode 0x110[5] = 1'b1 */
			sdhci_or32(host, SDHCI_VEN_SPEC_CTRL,
				   SDHCI_EMMC_HS_DDR);
		}
	} else {
		/* clear eMMC DDR mode: set 0x110[15] to 0 */
		sdhci_and32(host, SDHCI_VEN_SPEC_CTRL, ~(SDHCI_EMMC_HS_DDR));
	}

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/*
 * Function Name: host_pll_enable
 * Abstract: This Function is used to do uhs2 phy init
 *
 *
 * Input:
 *			sd_host_t *host,
 */
static void host_pll_enable(sd_host_t *host, bool enable)
{

	if (!host->feature.hw_pll_enable)
		return;

	if (host->sd_host4_enable == FALSE && host->uhs2_flag == FALSE)
		return;

	if (enable) {
		u32 timeout = 5000;

		DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Host Pll Enable\n");
		sdhci_or16(host, SDHCI_CLOCK_CONTROL, SDHCI_HOST_PLL_EN);
		while (timeout) {
			if (sdhci_readl(host, SDHCI_CLOCK_CONTROL) &
			    SDHCI_CLOCK_INT_STABLE)
				break;
			else if (sdhci_readl(host, SDHCI_CLOCK_CONTROL) ==
				 0xffffffff)
				break;

			os_udelay(1);
			timeout--;
		}
		DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Real Enable pll 0x%04X\n", sdhci_readw(host,
								SDHCI_CLOCK_CONTROL));
	} else
		sdhci_and16(host, SDHCI_CLOCK_CONTROL, ~SDHCI_HOST_PLL_EN);
}

void host_uhs2_init(sd_host_t *host, u32 clk_value, bool bfullreset)
{
	u16 reg;
	u32 reg32;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, clk_value=0x%x\n", __func__, clk_value);

	host->uhs2_flag = TRUE;
	reg = sdhci_readw(host, SDHCI_HOST_CONTROL2);

	/* Enable UHS2 function */
	reg |= SDHCI_CTRL_UHS2IF_EN | SDHCI_CTRL_VER4_EN;
	sdhci_writew(host, SDHCI_HOST_CONTROL2, reg);
	reg = sdhci_readw(host, SDHCI_HOST_CONTROL2);
	reg |= SDHCI_CTRL_UHS2;
	sdhci_writew(host, SDHCI_HOST_CONTROL2, reg);
	if (bfullreset == 0) {
		host_init_clock(host, clk_value);
		host_internal_clk_setup(host, TRUE);
	}

	/* Set UHS2 Timeout */
	reg = sdhci_readw(host, SDHCI_UHS2_TIMER_CTRL);
	reg |= 0xFF;
	sdhci_writew(host, SDHCI_UHS2_TIMER_CTRL, reg);

	/* Enable UHS2 Err Status */
	sdhci_writel(host, SDHCI_UHS2_ERRINT_STS_EN, 0xFFFFFFFF);

	/* Set  Scrambling according to vender cfg */
	reg32 = sdhci_readl(host, host->uhs2_cap.vnd_base + UHS2_EXTCNT_OFFSET);
	if (host->cfg->card_item.test_uhs2_setting2.disable_scramb_mode == 0)
		reg32 |= BIT0;
	else
		reg32 &= ~BIT0;
	sdhci_writel(host, host->uhs2_cap.vnd_base + UHS2_EXTCNT_OFFSET, reg32);

	if (shift_bit_func_enable(host))
		set_pattern_value(host, 0x00);

	host_set_vddx_power(host, VDD1, POWER_OFF);
	os_mdelay(36);
	host_set_vddx_power(host, VDD1, POWER_ON);

	if (shift_bit_func_enable(host))
		set_pattern_value(host, 0x30);

	host_set_vddx_power(host, VDD1, POWER_ON);
	host_set_vddx_power(host, VDD2, POWER_ON);

	if (host_get_vdd1_state(host) == FALSE)
		host_set_vddx_power(host, VDD1, POWER_ON);

	os_mdelay(36);

	host_pll_enable(host, TRUE);

	host_enable_clock(host, TRUE);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

static void host_uhs2_reg_clean(sd_host_t *host)
{
	u16 reg;

	reg = SDHCI_CTRL_UHS2IF_EN;
	if (host->sd_host4_enable == 0)
		reg |= SDHCI_CTRL_VER4_EN;
	sdhci_and16(host, SDHCI_HOST_CONTROL2, ~reg);
	sdhci_writel(host, SDHCI_UHS2_ERRINT_STS_EN, 0);
	sdhci_and16(host, SDHCI_UHS2_TIMER_CTRL, ~0xFF);
	host_set_uhs_mode(host, 0);
	host->uhs2_flag = FALSE;
}

/*
 * Function Name: host_uhs2_clear
 * Abstract: This Function is used to check clear uhs2 related register
 *
 * Input:
 *			sd_host_t *host,
 *			bool breset: do host softreset for all to clear uhs2 status or not
 *
 */
void host_uhs2_clear(sd_host_t *host, bool breset)
{

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, breset=%d\n", __func__, breset);

	/* clear uhs2 related reg */
	if (breset) {
		host_enable_clock(host, FALSE);
		if (shift_bit_func_enable(host))
			set_pattern_value(host, 0x10);

		host_set_vddx_power(host, VDD2, POWER_OFF);
		if (host_get_vdd1_state(host))
			host_set_vddx_power(host, VDD1, POWER_OFF);

		host_init(host);
	} else {
		host_enable_clock(host, FALSE);

		if (shift_bit_func_enable(host))
			set_pattern_value(host, 0x10);

		host_set_vddx_power(host, VDD2, POWER_OFF);
		host_uhs2_reg_clean(host);
		host_pll_enable(host, FALSE);
		host_internal_clk_setup(host, FALSE);
		host_init_internal(host);
	}
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/*
 * Function Name: host_uhs2_phychk
 * Abstract: This Function is used to check uhs2 phy init ok or not
 *
 * Input:
 *			sd_host_t *host,
 *			bool fromslp: whether call this in uhs2 resume  context
 * Output:
 *			bool *stbl : STBL check is ok or not
 *
 * Return value:
 *			phy init ok or not
 */
bool host_uhs2_phychk(sd_host_t *host, bool fromslp, bool *stbl)
{
	u32 timeout = 0;
	u32 reg;
	bool result = FALSE;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, *stbl=%d\n", __func__, *stbl);

	*stbl = FALSE;

	if (fromslp)
		timeout = 6000;
	timeout = 250;

	do {
		reg = sdhci_readl(host, SDHCI_PRESENT_STATE);
		if (reg == 0xFFFFFFFF) {
			DbgErr("Present All FF in STBL check\n");
			goto exit;
		}

		if (!(reg & SDHCI_CARD_PRESENT))
			goto exit;

		if (reg & SDHCI_UHS2_IF_DETECT)
			break;

		timeout -= 1;
		os_udelay(1);

	} while (timeout > 0);

	if (!timeout) {
		DbgWarn(MODULE_SD_HOST, NOT_TO_RAM, "uhs2 STBL timeout\n");
		goto exit;
	}

	*stbl = TRUE;
	timeout = 1500 * 100;

	do {
		reg = sdhci_readl(host, SDHCI_PRESENT_STATE);
		if (!(reg & SDHCI_CARD_PRESENT))
			goto exit;

		if (reg & SDHCI_UHS2_LANE_SYNC)
			break;

		reg = sdhci_readl(host, SDHCI_UHS2_ERRINT_STS);
		if (reg & SDHCI_UHS2_INT_TO_DEADLOCK) {
			DbgErr("UHS2 Wait for Sync deadlock occur\n");
			goto exit;
		}

		timeout -= 10;
		os_udelay(10);
	} while (timeout > 0);

	if (timeout <= 0) {
		DbgErr("UHS2 Wait for Sync timeout\n");
		goto exit;
	}

	result = TRUE;
exit:
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return result;
}

/*
 * Function Name: host_uhs2_cfg_set
 * Abstract: This Function is used to update uhs2 host setting registers
 *
 * Input:
 *			sd_host_t *host,
 *			uhs2_info_t *setting: The setting values
 *			bool stage2; State 2 is update for RangeB and lanes,
 *			      it is called after resume from dmt in uhs2 cfg flow
 *
 */
void host_uhs2_cfg_set(sd_host_t *host, uhs2_info_t *setting, bool stage2)
{
	u32 reg;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, stage2=%d\n", __func__, stage2);

	if (stage2) {
		/* update lanes and function */
		reg =
		    sdhci_readl(host,
				host->uhs2_cap.set_base +
				SDHCI_UHS2_IDX_GENERAL);
		reg &= ~(SDHCI_UHS2_LANE_FUNC_MASK);
		reg |= (setting->lanes << SDHCI_UHS2_LANE_FUNC_SHIFT);
		sdhci_writel(host,
			     host->uhs2_cap.set_base + SDHCI_UHS2_IDX_GENERAL,
			     reg);

		/* update speed range */
		reg =
		    sdhci_readl(host,
				host->uhs2_cap.set_base +
				SDHCI_UHS2_IDX_PHYSICAL);
		reg &= ~(SDHCI_UHS2_SPEED_MASK);
		reg |= (setting->speed_range << SDHCI_UHS2_SPEED_SHIFT);
		sdhci_writel(host,
			     host->uhs2_cap.set_base + SDHCI_UHS2_IDX_PHYSICAL,
			     reg);
		goto exit;
	}

	/* General Setting reg */
	reg =
	    sdhci_readl(host, host->uhs2_cap.set_base + SDHCI_UHS2_IDX_GENERAL);
	reg &= ~(SDHCI_UHS2_LANE_FUNC_MASK | SDHCI_UHS2_POWER_MODE_MASK);
	reg |= setting->pwr_mode;
	sdhci_writel(host, host->uhs2_cap.set_base + SDHCI_UHS2_IDX_GENERAL,
		     reg);

	/* Phy setting reg */
	reg =
	    sdhci_readl(host,
			host->uhs2_cap.set_base + SDHCI_UHS2_IDX_PHYSICAL);
	reg &=
	    ~(SDHCI_UHS2_SPEED_MASK | SDHCI_UHS2_HIBERNATE_MASK |
	      SDHCI_UHS2_LSS_DIR_MASK | SDHCI_UHS2_LSS_SYN_MASK);
	reg |= (setting->n_lss_dir << SDHCI_UHS2_LSS_DIR_SHIFT);
	reg |= (setting->n_lss_syn << SDHCI_UHS2_LSS_SYN_SHIFT);
	reg |= (setting->hibernate << SDHCI_UHS2_HIBERNATE_SHIFT);
	sdhci_writel(host, host->uhs2_cap.set_base + SDHCI_UHS2_IDX_PHYSICAL,
		     reg);

	/* Link and Tran regs */
	reg =
	    sdhci_readl(host, host->uhs2_cap.set_base + SDHCI_UHS2_IDX_LNKTRH);
	reg &= ~(SDHCI_UHS2_DATE_GAP_MASK);
	reg |= (setting->n_data_gap);
	sdhci_writel(host, host->uhs2_cap.set_base + SDHCI_UHS2_IDX_LNKTRH,
		     reg);

	reg =
	    sdhci_readl(host, host->uhs2_cap.set_base + SDHCI_UHS2_IDX_LNKTRL);
	reg &= ~(SDHCI_UHS2_RETRY_CNT_MASK | SDHCI_UHS2_NFCU_MASK);
	reg |= (setting->retry_cnt << SDHCI_UHS2_RETRY_CNT_SHIFT);
	reg |= (setting->n_fcu << SDHCI_UHS2_NFCU_SHIFT);
	sdhci_writel(host, host->uhs2_cap.set_base + SDHCI_UHS2_IDX_LNKTRL,
		     reg);

exit:
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return;

}

/*
 * Function Name: host_uhs2_init_capability
 * Abstract: This Function is used to get host uhs2 related capbility
 *
 * Input:
 *			sd_host_t *host,
 *
 */
static void host_uhs2_init_capability(sd_host_t *host)
{
	u32 reg;

	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/*
	 * below code is used for chip lost detection test
	 * sdhci_readl_test(host, SDHCI_CAPABILITIES_1);
	 * sdhci_readw_test(host, SDHCI_CAPABILITIES_1);
	 */

	reg = sdhci_readl(host, SDHCI_CAPABILITIES_1);
	if (!(reg & SDHCI_CAP1_UHS2_SUPP)) {
		DbgWarn(MODULE_SD_HOST, TO_RAM, "host don't support uhs2\n");
		goto no_uhs2;
	}

	host->uhs2_cap.vdd2_ocr = ((reg & SDHCI_CAP1_UHS2_VDD2_MASK) >>
				   SDHCI_CAP1_UHS2_VDD2_SHIFT);

	reg = sdhci_readl(host, SDHCI_MAX_CURRENT_2);
	host->uhs2_cap.vdd2_18_maxpower = ((reg & SDHCI_MAX_CUR_VDD2_180_MASK)
					   * SDHCI_MAX_CURRENT_MULTIPLIER);

	/* Get uhs2 host reallocate register base */
	host->uhs2_cap.set_base = (sdhci_readw(host, SDHCI_UHS2_SETTING_BASE) &
				   SDHCI_MAX_LOCATABLE_REG);
	host->uhs2_cap.cap_base =
	    (sdhci_readw(host, SDHCI_UHS2_CAPABILITY_BASE) &
	     SDHCI_MAX_LOCATABLE_REG);
	host->uhs2_cap.tst_base =
	    (sdhci_readw(host, SDHCI_UHS2_TEST_BASE) & SDHCI_MAX_LOCATABLE_REG);
	host->uhs2_cap.vnd_base =
	    (sdhci_readw(host, SDCHI_UHS2_VENDOR_BASE) &
	     SDHCI_MAX_LOCATABLE_REG);

	/* Get Host caps from General Capability reg */
	reg =
	    sdhci_readl(host, host->uhs2_cap.cap_base + SDHCI_UHS2_IDX_GENERAL);
	host->uhs2_cap.max_devices =
	    ((reg & SDHCI_UHS2_DEVICE_NUM_MASK) >> SDHCI_UHS2_DEVICE_NUM_SHIFT);

	if (!(reg & SDHCI_UHS2_BUS_TOP_MASK))
		host->uhs2_cap.max_devices = 1;
	else if (host->uhs2_cap.max_devices == 0) {
		DbgErr("Host support zero uhs2 devices\n");
		goto no_uhs2;
	}

	host->uhs2_cap.dap = (reg & SDHCI_UHS2_DAP_MASK);
	host->uhs2_cap.gap = ((reg & SDHCI_UHS2_GAP_MASK) >>
			      SDHCI_UHS2_GAP_SHIFT);
	host->uhs2_cap.num_of_lane = ((reg & SDHCI_UHS2_LANE_MASK) >>
				      SDHCI_UHS2_LANE_SHIFT);

	/* Get Host caps from Phy Capability reg */
	reg =
	    sdhci_readl(host,
			host->uhs2_cap.cap_base + SDHCI_UHS2_IDX_PHYSICAL);
	host->uhs2_cap.n_lss_dir =
	    ((reg & SDHCI_UHS2_LSS_DIR_MASK) >> SDHCI_UHS2_LSS_DIR_SHIFT);
	host->uhs2_cap.n_lss_syn =
	    ((reg & SDHCI_UHS2_LSS_SYN_MASK) >> SDHCI_UHS2_LSS_SYN_SHIFT);
	host->uhs2_cap.speed_range =
	    ((reg & SDHCI_UHS2_SPEED_MASK) >> SDHCI_UHS2_SPEED_SHIFT);

	/* Get Host caps from LinkTran Capability reg */
	reg =
	    sdhci_readl(host, host->uhs2_cap.cap_base + SDHCI_UHS2_IDX_LNKTRL);
	host->uhs2_cap.n_fcu =
	    ((reg & SDHCI_UHS2_NFCU_MASK) >> SDHCI_UHS2_NFCU_SHIFT);
	host->uhs2_cap.max_blk_len =
	    ((reg & SDHCI_UHS2_MAX_BLK_MASK) >> SDHCI_UHS2_MAX_BLK_SHIFT);

	reg =
	    sdhci_readl(host, host->uhs2_cap.cap_base + SDHCI_UHS2_IDX_LNKTRH);
	host->uhs2_cap.n_data_gap = (reg & SDHCI_UHS2_DATE_GAP_MASK);
	host->uhs2_cap.retry_cnt = 3;

	/* Set host support uhs2 */
	host->uhs2_supp = 1;
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host uhs2_flag=%d\n", host->uhs2_flag);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host uhs2_flag=%d\n", host->uhs2_cap.vdd2_ocr);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host vdd2_18_maxpower=%d\n", host->uhs2_cap.vdd2_18_maxpower);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host set_base=%d\n", host->uhs2_cap.set_base);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host cap_base=%d\n", host->uhs2_cap.cap_base);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host tst_base=%d\n", host->uhs2_cap.tst_base);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host vnd_base=%d\n", host->uhs2_cap.vnd_base);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host max_devices=%d\n", host->uhs2_cap.max_devices);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host dap=%d\n", host->uhs2_cap.dap);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host gap=%d\n", host->uhs2_cap.gap);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host num_of_lane=%d\n", host->uhs2_cap.num_of_lane);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host n_lss_dir=%d\n", host->uhs2_cap.n_lss_dir);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host n_lss_syn=%d\n", host->uhs2_cap.n_lss_syn);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host speed_range=%d\n", host->uhs2_cap.speed_range);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host n_fcu=%d\n", host->uhs2_cap.n_fcu);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host max_blk_len=%d\n", host->uhs2_cap.max_blk_len);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host n_data_gap=%d\n", host->uhs2_cap.n_data_gap);
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Host retry_cnt=%d\n", host->uhs2_cap.retry_cnt);

	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return;
no_uhs2:
	host->uhs2_supp = 0;
	DbgInfo(MODULE_SD_HOST, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

static bool host_uhs2_wait_dmt(sd_host_t *host)
{
	u32 reg = 0;
	u32 delay_us = 1;
	loop_wait_t wait;

	util_init_waitloop(host->pdx, 1500, delay_us, &wait);
	while (!util_is_timeout(&wait)) {
		reg = sdhci_readl(host, SDHCI_PRESENT_STATE);
		if (!(reg & SDHCI_CARD_PRESENT))
			return FALSE;

		if (reg == 0xffffffff) {
			DbgErr("chip lost when go dmt\n");
			return FALSE;
		}

		if (reg & SDHCI_UHS2_DMT_STATUS)
			break;

		os_udelay(delay_us);
	}

	if (!(reg & SDHCI_UHS2_DMT_STATUS)) {
		DbgErr("wait host dmt timeout.\n");
		return FALSE;
	}

	return TRUE;
}

/*
 * Function Name: host_uhs2_go_dmt
 * Abstract: This Function is used to set uhs2 card to dormant status
 *
 *
 * Input:
 *			sd_host_t *host,
 *			bool hbr: enter hibernate status or not
 */
bool host_uhs2_go_dmt(sd_host_t *host, bool hbr)
{
	bool result = TRUE;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s hbr=%d\n", __func__, hbr);

	result = host_uhs2_wait_dmt(host);
	if (result == FALSE) {
		DbgErr("Uhs2 Go Dormant wait for Host Dmt failed\n");
		goto exit;
	}

	if (DMT_DELAY_BEF_STOP_CLK_US)
		os_udelay(DMT_DELAY_BEF_STOP_CLK_US);

	host_enable_clock_nodelay(host, FALSE);

	if (DMT_DEALY_AFT_STOP_CLK_US)
		os_udelay(DMT_DEALY_AFT_STOP_CLK_US);

	host_pll_enable(host, FALSE);
	if (host->cfg->card_item.test_uhs2_setting2.enable_internal_clk_dormant
		== 0) {
		host_internal_clk_setup(host, FALSE);
		if (DMT_DELAY_AFT_ST_REFCLK_US)
			os_udelay(DMT_DELAY_AFT_ST_REFCLK_US);
	}

	if (hbr) {
		/* host_set_vdd1_power_nodelay(host, FALSE, 0); */
		host_set_vddx_power(host, VDD1, POWER_OFF);
		if (DMT_DELAY_AFT_PWROFF_MS)
			os_mdelay(DMT_DELAY_AFT_PWROFF_MS);

	}
exit:
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return result;
}

/*
 * Function Name: host_uhs2_resume_dmt
 * Abstract: This Function is used to resume from dormant
 *
 *
 * Input:
 *			sd_host_t *host,
 *			bool hbr: resume hibernate status or not
 */
bool host_uhs2_resume_dmt(sd_host_t *host, bool hbr)
{
	bool stbl;
	bool result = FALSE;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT | FEATURE_CARD_OPS,
		NOT_TO_RAM, "Enter %s hbr=%d\n", __func__, hbr);

	if (host->cfg->card_item.test_uhs2_setting2.enable_internal_clk_dormant
		== 0) {
		host_internal_clk_setup(host, TRUE);
	}

	if (hbr) {
		host_set_vddx_power(host, VDD1, POWER_ON);
		if (RESUME_POWER_ON_DELAY_MS)
			os_mdelay(RESUME_POWER_ON_DELAY_MS);
		host_chk_ocb_occur(host);
	}

	host_pll_enable(host, TRUE);
	host_enable_clock_nodelay(host, TRUE);

	result = host_uhs2_phychk(host, TRUE, &stbl);
	if (stbl == FALSE)
		DbgErr("STBL failed for wakeup");

	if (RESUME_DALAY_US)
		os_udelay(RESUME_DALAY_US);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return result;
}

/*
 * Function Name: host_uhs2_resume_dmt
 * Abstract: This Function is used for sd legacy host init operation
 *
 * Input:
 *			sd_host_t *host,
 */
void host_sd_init(sd_host_t *host)
{

	host_init_400k_clock(host);
	host_internal_clk_setup(host, TRUE);
	/* 1. Power on card */
	if (host_get_vdd1_state(host) == FALSE) {
		os_mdelay(10);
		host_set_vddx_power(host, VDD1, POWER_ON);

	}

	if (shift_bit_func_enable(host))
		set_pattern_value(host, 0x11);

	host_enable_clock(host, TRUE);

}

bool host_enable_sd_signal18v(sd_host_t *host)
{
	bool result = FALSE;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* 2.1. Stop SD CLK after SD transfer. */

	/*   If it is not stopping, it shall be stopped */
	host_enable_clock(host, FALSE);
	/*
	 * 1'. Wait 1ms for clock off. Issue #12074:
	 * [UHS1] SD_CLK voltage is wrong during Signal voltage switch sequence.
	 * Samuel 2011-09-06
	 */
	os_mdelay(1);

	/* 2. Check DAT[3:0]line signal =0000b or not in the Present State (0x24:D23-D20) */
	if (host_get_datline_state(host) != 0) {
		DbgErr("Check DAT[3:0]line signal =0000b Failed.\n");
		goto exit;
	}

	/* 3. Set Host Control2 (0x3e:D03) .8V Signaling Enable=1b */
	host_1_8v_sig_set(host, TRUE);

	/* 4. Minimum Wait 5ms */
	/* os_mdelay(10); */

	/* 5. Check Host Control2 (0x3e:D03) 1.8V Signaling Enable=1b or not */
	if (host_check_1_8v_signal(host) == 0x0) {
		DbgErr
		    ("Check Host Control2 (0x3e:D03) 1.8V Signaling Enable=1b Failed.\n");
		goto exit;
	}
#if (0)
	/* voltage_switch_method_choose 1:hardware control 0:sofaware control */
	if (host->cfg->host_item.test_voltage_switch_method_choose.voltage_switch_method) {
		/* hardware control */
		if (host_check_voltage_stable(host) == FALSE)
			DbgErr("Check voltage stable Failed.\n");
	} else {
		/* software control */
		DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Software Method to Switch Voltage, Delay 10ms\n");
		os_mdelay(10);
	}

#else
	/* software control */
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Software Method to Switch Voltage, Delay 10ms\n");
	os_mdelay(10);

#endif

	/*
	 * Add voltage stable check for voltage switch failed issue
	 * BH722SE2LN-A UHS1 issue#6  GIGABYTE D3V #6 platform, BH driver 10024,
	 * the Host regulator voltage switch de-bounce time setting did not
	 * meets design, so some SD3.0 card would switch to 1.8V fail.
	 */

	/* 6. Clock On */
	host_enable_clock(host, TRUE);
	/*  Signal Voltage Switch procedure need 1ms wait */
	os_mdelay(1);

	/* 9. Check DAT[3:0]line signal =1111b or not in the Present State (0x24:D23-D20) */
	if (host_get_datline_state(host) != 0xF) {
		DbgErr("Check DAT[3:0]line signal =1111b Failed.\n");
		goto exit;
	}

	result = TRUE;
exit:
	if (result == FALSE)
		DbgErr("host set sd 18v signal failed\n");

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;
}

void host_enable_cmd23(sd_host_t *host, bool enable)
{
	if (host->feature.hw_autocmd) {
		if (enable == 0)
			sdhci_and16(host, SDHCI_HOST_CONTROL2,
				    ~SDHCI_CTRL_CMD23_EN);
		else
			sdhci_or16(host, SDHCI_HOST_CONTROL2,
				   SDHCI_CTRL_CMD23_EN);
	}
}

void host_transfer_init(sd_host_t *host, bool enable_infinite, bool force_adma)
{
	u32 dma_mode = 0;

	DbgInfo(MODULE_SD_HOST,
		FEATURE_IOCTL_TRACE | FEATURE_CARD_INIT | FEATURE_ERROR_RECOVER,
		NOT_TO_RAM, "Enter %s, enable_infinite=%d, force_adma=%d\n",
		__func__, enable_infinite, force_adma);

	if (force_adma == TRUE) {
		/* todo according to registery */
		host_dma_select(host, TRANS_ADMA2);
		hostven_transfer_init(host, FALSE);
	} else {
		dma_mode = host->cfg->host_item.test_dma_mode_setting.dma_mode;
		switch (dma_mode) {
		case CFG_TRANS_MODE_SDMA:
			host_dma_select(host, TRANS_SDMA);
			break;
		case CFG_TRANS_MODE_ADMA2:
		case CFG_TRANS_MODE_ADMA2_SDMA_LIKE:
			host_dma_select(host, TRANS_ADMA2);
			break;
		case CFG_TRANS_MODE_ADMA3:
		case CFG_TRANS_MODE_ADMA3_SDMA_LIKE:
			host_dma_select(host, TRANS_ADMA3);
			break;
		case CFG_TRANS_MODE_ADMA_MIX:
		case CFG_TRANS_MODE_ADMA_MIX_SDMA_LIKE:
			if ((host->chip_type == CHIP_SEAEAGLE2)
			    || (host->chip_type == CHIP_GG8)
			    || (host->chip_type == CHIP_ALBATROSS))
				/* ADMA2 or ADMA3 if SD4.0 */
				host_dma_select(host, TRANS_ADMA3);
			else
				/* ADMA2 or ADMA3 if SD4.0 */
				host_dma_select(host, TRANS_ADMA2);
			break;
		default:
			DbgErr("%s dma mode %d no define\n", __func__,
			       dma_mode);
			host_dma_select(host, TRANS_ADMA2);
			break;
		}
		hostven_transfer_init(host, enable_infinite);
	}
	DbgInfo(MODULE_SD_HOST,
		FEATURE_IOCTL_TRACE | FEATURE_CARD_INIT | FEATURE_ERROR_RECOVER,
		NOT_TO_RAM, "Exit %s\n", __func__);
}

void host_error_int_recovery_stage1(sd_host_t *host, u16 error_int_state,
				    bool check)
{
	/*
	 * Follow SD Host Spec V4.10 Section 3.10.1 Error Interrupt Recovery flow (Page 178)
	 *
	 * (3). Set Software Reset for CMD Line to 1 in the Software
	 *      Reset register for software reset the CMD line.
	 * (4). Check Software Reset For CMD Line in the Software
	 *      Reset register. If Software Reset For CMD Line is 0, go to (5)
	 *      If it is 1, go to step (4)
	 * (5) Check bits D06-04 in the Error Interrupt Status register.
	 *     If one of these bits (D06-04) is set to 1, goto (6).
	 *     If none are set to 1 (all are 0), goto step (8)
	 * (6) Set Software Reset for DAT Line in the Software Reset
	 *     register for software reset for DAT line.
	 * (7) Check Software Reset For DAT Line in the Software Reset register.
	 *     If Software Reset For DAT Line is 0, go to (8)
	 *     If it is 1, goto (7)
	 */

	DbgInfo(MODULE_SD_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s, error_int_state=%d, check=%d\n", __func__,
		error_int_state, check);

	/* If chip lost, do nothing. */
	if (host_check_lost(host))
		return;

	host_chk_ocb_occur(host);

	/* uhs2 case don't need this flow */
	if (host->uhs2_flag)
		return;

	if (check) {
		if (error_int_state & SDHCI_INT_CMD_ERROR_MASK)
			host_reset(host, SDHCI_RESET_CMD);
		if (error_int_state & SDHCI_INT_DAT_ERROR_MASK)
			host_reset(host, SDHCI_RESET_DATA);
	} else {
		if (error_int_state & SDHCI_INT_CMD_ERROR_MASK)
			sdhci_or32(host, SDHCI_CLOCK_CONTROL, SDHCI_RESET_CMD);
		if (error_int_state & SDHCI_INT_DAT_ERROR_MASK)
			sdhci_or32(host, SDHCI_CLOCK_CONTROL, SDHCI_RESET_DATA);
	}

	DbgInfo(MODULE_SD_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

bool host_error_int_recovery_stage2(sd_host_t *host, u16 error_int_state)
{
	bool ret = FALSE;
	u32 delay_us = 1;
	loop_wait_t wait;

	DbgInfo(MODULE_SD_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s, error_int_state=%d\n", __func__,
		error_int_state);
	/*
	 * 11 Check Command Inhibit (DAT) and Command Inhibit(CMD) in the Present State register.
	 * Repeat this step until both Command Inhibit (DAT) and Command Inhibit(CMD) are set to 0
	 */

	/* Command Inhibit (DAT) and (CMD) Check */
	sdhci_or32(host, SDHCI_CLOCK_CONTROL, SDHCI_RESET_CMD);
	util_init_waitloop(host->pdx, RESET_FOR_ALL_ABRT_TM, delay_us, &wait);
	while (!util_is_timeout(&wait)) {
		if ((sdhci_readl(host, SDHCI_PRESENT_STATE) &
		     (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT)) == 0) {
			goto next;
		} else if (sdhci_readl(host, SDHCI_PRESENT_STATE) == 0xffffffff) {
			break;
		}
		os_udelay(delay_us);
	}
	/* Command Inhibit (DAT) and Command Inhibit(CMD) timeout, treat as Non-recoverable Error */
	goto exit;

next:
	/*
	 * 12  Check bits D03-00 in the Error Interrupt Status register for Aboot Command.
	 * If one of these bits is set to 1, goto 16. if none of these bits are set to 1, go to 13
	 */
	if (error_int_state & SDHCI_INT_CMD_ERROR_MASK)
		goto exit;

	/*
	 * 13 Check Data Timeout Error in the Error interrupt Status register.
	 * If this bit is set to 1, go to step 16, If it is 0, goto 14
	 */
	if (error_int_state & SDHCI_INT_DATA_TIMEOUT)
		goto exit;

	/* 14 Wait for more than 40us */
	os_udelay(40);

	/*
	 * 15 By monitoring the DAT[3:0] Line Signal Level in the Present State register,
	 * judge whether the level of DAT line is low or not.
	 */
	if (host_get_datline_state(host) != 0xF)
		ret = FALSE;
	else
		ret = TRUE;

exit:
	DbgInfo(MODULE_SD_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Exit(%d) %s\n", ret, __func__);
	return ret;
}

bool host_check_lost(sd_host_t *host)
{
	u32 reg = sdhci_readl(host, SDHCI_PRESENT_STATE);

	if (reg == 0xffffffff)
		return TRUE;
	else
		return FALSE;
}

void host_set_output_tuning_phase(sd_host_t *host, u32 phase)
{

	DbgInfo(MODULE_SD_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s, phase=0x%x\n", __func__, phase);

	host_enable_clock(host, FALSE);

	hostven_set_output_tuning_phase(host, phase, FALSE);

	host_enable_clock(host, TRUE);

	DbgInfo(MODULE_SD_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/*
 * only used for camera mode, polling in thread daemon.
 * for PC mode, sdhci_irq do card in/dessert check by INTR
 */
extern void remove_card_handle(bht_dev_ext_t *pdx);
extern void insert_card_handle(bht_dev_ext_t *pdx);
void host_check_card_insert_desert(sd_host_t *host)
{
	u16 regval;

	if (host->cfg->driver_item.camera_mode_ctrl_vdd1_vdd2_cd == 1) {
		regval = ven_readw(host, SDBAR1_WP_GPIO3_CTRL_REG_514);
		if ((regval & (1 << 6))
		    && (host->camera_mode_card_state == CARD_INSERTED)) {
			DbgInfo(MODULE_SD_HOST,
				FEATURE_CARD_INIT | FEATURE_INTR_TRACE, TO_RAM,
				"CARD_DESERTED %s\n", __func__);
			host->camera_mode_card_state = CARD_DESERTED;
			remove_card_handle(host->pdx);
		} else if (!(regval & (1 << 6))
			   && (host->camera_mode_card_state == CARD_DESERTED)) {
			DbgInfo(MODULE_SD_HOST,
				FEATURE_CARD_INIT | FEATURE_INTR_TRACE, TO_RAM,
				"CARD_DESERTED %s\n", __func__);
			host->camera_mode_card_state = CARD_INSERTED;
			insert_card_handle(host->pdx);
		} else {
			/* nothing */
		}
	}
}

void set_gpio_levels(sd_host_t *host, bool gpio_num, bool signal_level)
{
	u8 gpio_setting_case;
	u32 regval;

	gpio_setting_case = ((gpio_num << 1) | signal_level);

	switch (gpio_setting_case) {
	case 0:
		/* GPIO2 Low */
		DbgInfo(MODULE_SD_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			"Set GPIO2 Low\n");
		regval = ven_readl(host, 0x510);
		regval &= ~(1 << 13);
		ven_writel(host, 0x510, regval);

		break;

	case 1:
		/* GPIO2 High */
		DbgInfo(MODULE_SD_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			"Set GPIO2 High\n");
		regval = ven_readl(host, 0x510);
		regval |= (1 << 13);
		ven_writel(host, 0x510, regval);

		break;

	case 2:
		/* GPIO3 Low */
		DbgInfo(MODULE_SD_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			"Set GPIO3 Low\n");
		regval = ven_readl(host, 0x514);
		regval &= ~(1 << 5);
		ven_writel(host, 0x514, regval);

		break;

	case 3:
		/* GPIO3 High */
		DbgInfo(MODULE_SD_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			"Set GPIO3 High\n");
		regval = ven_readl(host, 0x514);

		if (regval == 0xffffffff)
			DbgInfo(MODULE_SD_HOST, FEATURE_ERROR_RECOVER,
				NOT_TO_RAM, "Chip lost when Set GPIO3 High\n");
		else {
			regval |= (1 << 5);
			ven_writel(host, 0x514, regval);
		}
		break;
	}

}

void shif_byte_pattern_bit_set(sd_host_t *host, bool bit_en, u8 pattern_case)
{
	switch (pattern_case) {
	case SPECIAL_PATTERN:
		if (bit_en) {
			set_gpio_levels(host, GPIO2, GPIO_LOW);
			set_gpio_levels(host, GPIO3, GPIO_HIGHT);
			set_gpio_levels(host, GPIO2, GPIO_HIGHT);
			set_gpio_levels(host, GPIO2, GPIO_LOW);
		} else {
			set_gpio_levels(host, GPIO2, GPIO_LOW);
			set_gpio_levels(host, GPIO3, GPIO_LOW);
			set_gpio_levels(host, GPIO2, GPIO_HIGHT);
			set_gpio_levels(host, GPIO2, GPIO_LOW);
		}
		break;

	case START_BIT:
		set_gpio_levels(host, GPIO2, GPIO_HIGHT);
		set_gpio_levels(host, GPIO3, GPIO_LOW);
		set_gpio_levels(host, GPIO2, GPIO_LOW);
		break;

	case END_BIT:
		set_gpio_levels(host, GPIO2, GPIO_HIGHT);
		set_gpio_levels(host, GPIO3, GPIO_LOW);
		set_gpio_levels(host, GPIO3, GPIO_HIGHT);
		break;
	}
}

void set_pattern_value(sd_host_t *host, u8 value)
{
	int i;

	DbgInfo(MODULE_SD_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s with 0x%x\n", __func__, value);

	shif_byte_pattern_bit_set(host, 0, START_BIT);

	for (i = 5; i >= 0; i--) {
		PrintMsg("#i = %d\n", i);
		if (value & (1 << i))
			shif_byte_pattern_bit_set(host, 1, SPECIAL_PATTERN);
		else
			shif_byte_pattern_bit_set(host, 0, SPECIAL_PATTERN);
	}

	shif_byte_pattern_bit_set(host, 0, END_BIT);
}

void power_control_with_card_type(sd_host_t *host, u8 vddx, bool power_en)
{
	u8 value = 0x0;
	bht_dev_ext_t *pdx = host->pdx;
	sd_card_t *card = &pdx->card;

	DbgInfo(MODULE_SD_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s Card type-%d, VDD-%d, Power-%d\n", __func__,
		card->card_type, vddx, power_en);

	if (card->card_type == CARD_SD) {
		if (power_en && vddx == VDD1)
			value = (UHS1_BIT_EN | VDD1_BIT_EN);
		else if (!power_en && vddx == VDD1)
			value = (UHS1_BIT_EN);
	} else if (card->card_type == CARD_UHS2) {
		if (power_en && vddx == VDD1)
			value = (UHS2_BIT_EN | VDD1_BIT_EN);
		else if (power_en && vddx == VDD2)
			value = (UHS2_BIT_EN | VDD2_BIT_EN);
		else if (!power_en)
			value = (UHS2_BIT_EN);
	} else if (card->card_type == CARD_SD70) {
		if (power_en && vddx == VDD1)
			value = (SD70_BIT_EN | VDD1_BIT_EN);
		else if (power_en && vddx == VDD2)
			value = (SD70_BIT_EN | VDD2_BIT_EN);
		else if (!power_en)
			value = (SD70_BIT_EN);
	} else {
		if (power_en && vddx == VDD1)
			value = (VDD1_BIT_EN);
		if (power_en && vddx == VDD2)
			value = (VDD2_BIT_EN);
		else if (!power_en)
			value = 0x00;
	}

	set_pattern_value(host, value);

	DbgInfo(MODULE_SD_HOST, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

bool shift_bit_func_enable(sd_host_t *host)
{
	if (host->cfg != NULL) {
		if ((host->cfg->card_item.sd7_sdmode_switch_control.shift_byte_en)
		    && (host->chip_type == CHIP_GG8))
			return TRUE;
		else
			return FALSE;
	} else
		return FALSE;
}
