// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: thermal.c
 *
 * Abstract: This File is used to handle thread event
 *
 * Version: 1.00
 *
 * Author: Peter.Guo
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 11/05/2014		Creation	Peter.Guo
 */

#include "../include/basic.h"
#include "../include/card.h"
#include "../include/function.h"
#include "../include/cardapi.h"
#include "cardcommon.h"
#include "../include/hostapi.h"
#include "../include/util.h"
#include "../include/debug.h"

/*
 * Function Name: thermal_gpio_sensor
 * Abstract: This Function is used to do get sensor result for thermal control
 *
 * Input:
 *			sd_host_t *host
 *
 * Return value:
 *			NORMAL
 *			COOL
 *			HOT
 *
 * Notes:
 *			run in thread context
 */

static e_thermal_val thermal_gpio_sensor(sd_host_t *host)
{
	e_thermal_val result = THERMAL_NORMAL;
	u32 value = 0;

	DbgInfo(MODULE_THERMAL, FEATURE_FUNC_THERMAL, NOT_TO_RAM, "Enter %s\n",
		__func__);

	switch (host->chip_type) {
	case CHIP_SEAEAGLE:
		ven_and32(host, 0x22c, ~0x7);
		ven_or32(host, 0x22c, 0x13);
		value = ven_readl(host, 0x22c);
		value = (value & 0x40) >> 6;
		break;
	case CHIP_SEAEAGLE2:
	case CHIP_GG8:
	case CHIP_ALBATROSS:
		ven_and32(host, 0x50c, ~0x7);
		ven_or32(host, 0x50c, 0x13);
		value = ven_readl(host, 0x50c);
		value = (value & 0x40) >> 6;
		break;
	default:
		value = pci_readl(host, 0xD4);
		value = (value & 0x80) >> 7;
		break;
	}

	result = (e_thermal_val) value;

	DbgInfo(MODULE_THERMAL, FEATURE_FUNC_THERMAL, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, value);
	return result;

}

/*
 * Function Name: thermal_i2c_sensor
 * Abstract: This Function is used to do get sensor result for thermal control
 *
 * Input:
 *			sd_host_t *host
 *
 * Return value:
 *			NORMAL
 *			COOL
 *			HOT
 *
 * Notes:
 *			run in thread context
 */

static e_thermal_val thermal_i2c_sensor(sd_host_t *host)
{
	u32 temp_val = 0, count = 0;
	u32 upper_limit = 0, lower_limit = 0;
	e_thermal_val result = THERMAL_NORMAL;

	DbgInfo(MODULE_THERMAL, FEATURE_FUNC_THERMAL, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* Enable I2C I/F */
	ven_and32(host, 0x228, ~0x7);
	ven_or32(host, 0x228, 0x1);
	ven_and32(host, 0x230, ~0x7);
	ven_or32(host, 0x230, 0x1);
	os_mdelay(1);

	/* Reset I2C function */
	ven_writel(host, 0x220, 0x80000000);

	os_mdelay(1);

	/* Set FLTR and One short bit */
	ven_writel(host, 0x220, 0x440000);
	ven_writel(host, 0x220, 0x20009401);

	while ((ven_readl(host, 0x220) & 0x20000000)) {
		/* Timeout 5ms */
		if (count == 5) {
			DbgErr(" - Wait I2C write operation timeout!\n");
			break;
		}
		/* else if (device_status == DEVICE_STATUS_CHIPLOST) */
		else if (ven_readl(host, 0x220) == 0xffffffff) {
			DbgErr("break loop because chip lost!\n");
			break;
		}

		os_mdelay(1);
		count += 1;
	}

	/* Read the temperature value */
	ven_writel(host, 0x220, 0x50009400);

	while ((ven_readl(host, 0x220) & 0x10000000)) {
		/* Timeout 5ms */
		if (count == 5) {
			DbgErr(" - Wait I2C read operation timeout!\n");
			break;
		}
		/* else if (device_status == DEVICE_STATUS_CHIPLOST) */
		else if (ven_readl(host, 0x220) == 0xffffffff) {
			DbgErr("break loop because chip lost!!\n");
			break;
		}
		os_mdelay(1);
		count += 1;
	}

	temp_val = (ven_readl(host, 0x224) & 0xffff) >> 6;
	/* upper_limit = (tmp_high & 0xffff0000) >> 16; */
	/* lower_limit = tmp_low & 0xffff; */

	if ((temp_val & 0x200) && (upper_limit & 0x8000)) {
		if ((temp_val & 0x1ff) < ((upper_limit & 0x1ff) << 2)) {
			result = THERMAL_HOT;
			goto exit;
		}
	}

	if ((0 == (temp_val & 0x200)) && (upper_limit & 0x8000)) {
		result = THERMAL_HOT;
		goto exit;
	}

	if ((0 == (temp_val & 0x200)) && (0 == (upper_limit & 0x8000))) {
		if ((temp_val & 0x1ff) > ((upper_limit & 0x1ff) << 2)) {
			result = THERMAL_HOT;
			goto exit;
		}
	}

	if ((temp_val & 0x200) && (lower_limit & 0x8000)) {
		if ((temp_val & 0x1ff) > ((lower_limit & 0x1ff) << 2)) {
			result = THERMAL_COOL;
			goto exit;
		}
	}

	if ((temp_val & 0x200) && (0 == (lower_limit & 0x8000))) {
		result = THERMAL_COOL;
		goto exit;
	}

	if ((0 == (temp_val & 0x200)) && (0 == (lower_limit & 0x8000))) {
		if ((temp_val & 0x1ff) < ((lower_limit & 0x1ff) << 2)) {
			result = THERMAL_COOL;
			goto exit;
		}
	}

exit:
	DbgInfo(MODULE_THERMAL, FEATURE_FUNC_THERMAL, NOT_TO_RAM,
		"Exit %s result=%d\n", __func__, result);
	return result;

}

/*
 * Function Name: func_thermal_control
 * Abstract: This Function is used to do thremal control
 *			  This function should be called before send card Read Write
 *
 * Input:
 *			sd_card_t *card
 *
 * Return value:
 *			TRUE: means ok
 *			others means error occur, caller need do error recovery
 *
 * Notes:
 *			run in thread context
 */

bool func_thermal_control(sd_card_t *card)
{
	sd_host_t *host = card->host;
	bht_dev_ext_t *pdx = host->pdx;
	bool result = TRUE;
	e_thermal_val thermal = THERMAL_NORMAL;

	DbgInfo(MODULE_THERMAL, FEATURE_FUNC_THERMAL, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* If Thermal Control is disabled then do nothing */
	if (pdx->thermal.enable == 0)
		goto exit;

	/* If Thermal timeout is not occur then do nothing */
	if (pdx->thermal.enable_timer_chk == 1 && pdx->thermal.timeout == 0)
		goto exit;

	pdx->thermal.timeout = 0;
	pdx->thermal.last_check_ms = os_get_cur_tick();

	/* If card not working do nothing */
	if (card->state != CARD_STATE_WORKING || card->card_present == FALSE)
		goto exit;

	if (card->initialized_once == FALSE)
		goto exit;

	/* Currently only uhs2 and SD support thermal control */
	switch (card->card_type) {
	case CARD_SD:
	case CARD_UHS2:
		break;
	default:
		goto exit;
	}

	if (pdx->thermal.use_i2c)
		thermal = thermal_i2c_sensor(host);
	else
		thermal = thermal_gpio_sensor(host);

	DbgInfo(MODULE_THERMAL, FEATURE_FUNC_THERMAL, NOT_TO_RAM,
		"Start do Thermal control sensor=%d\n", thermal);

	if (thermal == THERMAL_NORMAL) {
		/* nothing to do for no thermal change */
		goto exit;
	} else if (thermal == THERMAL_COOL) {
		/* NEED change to higher mode */
		pdx->card.thermal_enable = 1;
		pdx->card.thermal_heat = 1;
		result = card_thermal_control(card);
	} else {
		/* change to lower mode */
		pdx->card.thermal_enable = 1;
		pdx->card.thermal_heat = 0;
		result = card_thermal_control(card);
	}

	pdx->card.thermal_enable = 0;

exit:
	DbgInfo(MODULE_THERMAL, FEATURE_FUNC_THERMAL, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, result);
	return result;
}

/*
 * Function Name: func_thermal_control
 * Abstract: This Function is used to update thermal control time
 *			  This function should be called before send card Read Write
 *
 * Input:
 *			sd_card_t *card
 *
 * Return value:
 *			TRUE: means ok
 *			others means error occur, caller need do error recovery
 *
 * Notes:
 */

void func_thermal_update_time(bht_dev_ext_t *pdx)
{
	/* If time check for thermal contorl is not enable do nothing */
	if (pdx->thermal.enable == 0 || pdx->thermal.enable_timer_chk == 0)
		return;

	if (pdx->card.state != CARD_STATE_WORKING
	    || pdx->card.card_present == FALSE)
		return;

	if (pdx->card.initialized_once == FALSE)
		return;

	if (pdx->thermal.timeout == 0) {
		pdx->thermal.timeout =
		    (os_get_cur_tick() >
		     (pdx->thermal.last_check_ms +
		      pdx->thermal.check_period_ms)) ? 1 : 0;
	}

}

void thermal_init(bht_dev_ext_t *pdx)
{
	pdx->thermal.enable = 0;
	if (pdx->thermal.enable == 0)
		return;
	pdx->thermal.use_i2c = 0;
	pdx->thermal.last_check_ms = os_get_cur_tick();
	pdx->thermal.enable_timer_chk = 0;
	pdx->thermal.check_period_ms = 0;
	DbgInfo(MODULE_THERMAL, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"thermal enable=%d i2c=%d timechk=%dms chkperiod=%dms\n",
		pdx->thermal.enable, pdx->thermal.use_i2c,
		pdx->thermal.enable_timer_chk, pdx->thermal.check_period_ms);

}

void thermal_uninit(bht_dev_ext_t *pdx)
{
	if (pdx->thermal.enable == 0)
		return;
	pdx->thermal.last_check_ms = os_get_cur_tick();
}
