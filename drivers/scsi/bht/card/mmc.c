// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: mmc.c
 *
 * Abstract: mmc/emmc card initialization
 *
 * Version: 1.00
 *
 * Author: Amma.Li
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 9/23/2014   Creation    Amma.Li
 */
#include "../include/basic.h"
#include "../include/cardapi.h"
#include "../include/hostapi.h"
#include "../host/hostven.h"
#include "../host/hostreg.h"
#include "cardcommon.h"
#include "../include/cmdhandler.h"
#include "../include/debug.h"
#include "../include/util.h"
#define  MMC_SPEC_VERS	0x04U
/* ------------------emmc setting-------------------- */
/* ext_csd[196]: card type */
#define	MMC_CARD_TYPE_H200		  0x30U
#define	MMC_CARD_TYPE_H400		  0xC0U
#define	MMC_CARD_TYPE_HS		  0x0FU
#define	MMC_CARD_DDR_SUPP		  0x0CU
#define   MMC_CARD_TYPE_HS_DDR_12   0x8
#define   MMC_CARD_TYPE_HS_DDR_18   0x4
#define   MMC_CARD_TYPE_HS_52M      0x2
#define   MMC_CARD_TYPE_HS_26M      0x1
/* ext_csd[183]: Bus Width */
#define   MMC_EXTCSD_BUS_WIDTH     (0x00B70000)
#define   MMC_BUSW_1BIT                0
#define   MMC_BUSW_SDR_4BIT           (1 << 8)
#define   MMC_BUSW_SDR_8BIT           (2 << 8)
#define   MMC_BUSW_DDR_4BIT           (5 << 8)
#define   MMC_BUSW_DDR_8BIT           (6 << 8)
/* ext_csd[185]: HS_TIMING */
#define  MMC_EXTCSD_HS_TIMING       (0x00B90000)
#define  MMC_TIMING_BACKWARDS       0
#define  MMC_TIMING_HIGH_SPEED      (1 << 8)
#define  MMC_TIMING_HS200           (2 << 8)
#define  MMC_TIMING_HS400           (3 << 8)
#define  MMC_DRIVER_TYPE            0
/* emmc CMD6 setting */
#define   MMC_EXTCSD_WRITE           (3 << 24)
#define   MMC_EXTCSD_SET             (1 << 24)
#define   MMC_EXTCSD_CLEAN           (2 < 24)
/* emmc/mmc RCA */
#define   MMC_RCA                    (1 << 16)
/* -------------emmc setting end------------------ */
static void emmc_get_ext_csd_info(sd_card_t *card);
static bool emmc_switch_buswidth(sd_card_t *card, sd_command_t *sd_cmd);
static void emmc_set_freq(sd_card_t *card, u32 clock_freq, bool bddr50);

/* -------------emmc / mmc card CMD setting------------- */

/*
 * Function Name: emmc_card_init_ready
 *
 * Abstract:
 *           1. Issue CMD1 to Get OCR
 *           2. Set the card ocr variable
 *           3. Wait for card ready
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 * sd_command_t *sd_cmd: Pointer to sd command structure
 *
 * Output: None
 *
 * Return value: Return TRUE if card ready, else return FALSE
 *
 * Notes:
 *
 * Caller: emmc_init
 *
 */

static bool emmc_card_init_ready(sd_card_t *card, sd_command_t *sd_cmd)
{
	byte cmd_index = SD_CMD1;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R3;
	e_data_dir dir = DATA_DIR_NONE;
	byte *data = NULL;
	u32 datalen = 0;
	bool ret = FALSE;
	loop_wait_t wait;
	u32 delay_us = 20;

	sd_host_t *host = card->host;
	card_info_t *card_info = &(card->info);
	cfg_emmc_mode_t *emmc_mode = &(host->cfg->card_item.emmc_mode);

	if (emmc_mode->enable_18_vcc)
		argument |= EMMC_OCR_LOW;
	else
		argument |= EMMC_OCR_HI;

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s arg 0x%08x\n", __func__, argument);

	/* Wait for card ready */
	util_init_waitloop(card->host->pdx,
			   host->cfg->timeout_item.test_card_init_timeout.value,
			   delay_us, &wait);

	do {
		ret =
		    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag,
				    dir, data, datalen);
		if (ret == FALSE) {
			DbgErr("Issue CMD1 to get eMMC card OCR Fail.\n");
			break;
		}

		/* Check Busy status. 0b: On initialization; 1b: Initialization Complete. */
		if ((sd_cmd->response[0] & 0x80000000) == 0) {
			os_udelay(delay_us);
			continue;
		} else {
			break;
		}
	} while (!util_is_timeout(&wait));

	/* If card ready, set related software flags */
	if (ret) {
		/* check card ready or not */
		if (sd_cmd->response[0] & 0x80000000) {
			if (sd_cmd->response[0] & 0x40000000)
				/* the capability > 2GB */
				card_info->card_ccs = 1;
			else
				/* the capability < 2GB */
				card_info->card_ccs = 0;
		} else
			ret = FALSE;
	}

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 * Function Name: emmc_set_rca
 *
 * Abstract: Set a new relative address RCA for MMC/eMMC card(CMD3)
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 * sd_command_t *sd_cmd: Pointer to sd command structure
 *
 * Output: None
 *
 * Return value: Return TRUE if issue CMD3 successfully, else return FALSE
 *
 * Notes:
 *
 * Caller: emmc_init
 *
 */

bool emmc_set_rca(sd_card_t *card, sd_command_t *sd_cmd)
{
	bool ret = FALSE;
	byte cmd_index = SD_CMD3;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R1;
	e_data_dir dir = DATA_DIR_NONE;
	byte *data = NULL;
	u32 datalen = 0;
	card_info_t *card_info = &(card->info);

	argument = MMC_RCA;

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s arg 0x%08x\n", __func__, argument);

	/* Issue CMD3 */
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);
	if (ret) {
		/* Update the card RCA */
		card_info->rca = (argument & 0xffff0000) >> 16;
	}
	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 * Function Name: emmc_get_ext_csd
 *
 * Abstract: Read the MMC/EMMC card Ext_Csd Data
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 * sd_command_t *sd_cmd: Pointer to sd command structure
 *
 * Output: None
 *
 * Return value: Return TRUE if issue eMMC CMD8 successfully, else return FALSE
 *
 * Notes:
 *
 * Caller: emmc_init
 *
 */

static bool emmc_get_ext_csd(sd_card_t *card, sd_command_t *sd_cmd)
{
	byte cmd_index = SD_CMD8;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R1;
	e_data_dir dir = DATA_DIR_IN;
	byte data[512];
	u32 datalen = 512;

	bool ret = FALSE;
	mmc_card_info_t *mmc_info = &(card->mmc);

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* Issue eMMC CMD8 to get Ext_Csd */
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);
	if (ret) {
		/* Get the Ext_Csd */
		os_memcpy(&(mmc_info->raw_extcsd[0]), data, 512);
		emmc_get_ext_csd_info(card);
	}

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 * Function Name: emmc_send_cmd6
 *
 * Abstract:
 *           1. Issue CMD6 to switch mode to modify the Ext_Csd register
 *           2. Set the emmc card hs_timing & bus width
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 * sd_command_t *sd_cmd: Pointer to sd command structure
 * argument: [31:26] set to 0
 *           [25:24]: Access    1: set     2: clean     3: write
 *           [23:16]: Index    the ext_csd index
 *           [15:8]: value
 *           [7:3]: set to 0
 *           [2:0]: cmd set
 *
 * Output: None
 *
 * Return value: Return TRUE if issue CMD6 successfully, else return FALSE
 *
 * Notes:
 *
 * Caller: emmc_switch_buswidth
 *
 */

static bool emmc_send_cmd6(sd_card_t *card,
			   sd_command_t *sd_cmd, u32 argument)
{
	bool result = FALSE;
	byte cmd_index = SD_CMD6;
	u32 cmdflag = CMD_FLG_R1B;
	e_data_dir dir = DATA_DIR_NONE;
	byte *data = NULL;
	u32 datalen = 0;

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s arg = 0x%08x\n", __func__, argument);
	result =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);

	return result;
}

/*
 *
 * Function Name: emmc_bustest_r
 *
 * Abstract:
 *			1. MMC/eMMC card bus width test read (CMD14)
 *          2. A host reads the reversed bus testing data pattern from a Device.
 *          3. Used after CMD19 (bus width test write CMD),
 *				need to check the cmd and data transfer error
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 * sd_command_t *sd_cmd: Pointer to sd command structure
 * u8 *buf : read data pattern buffer
 * u32 data_len : read data length
 *
 * Output: None
 *
 * Return value: Return TRUE if issue CMD14 successfully, else return FALSE
 *
 * Notes:
 *
 * Caller: emmc_bus_width_test
 */
static bool emmc_bustest_r(sd_card_t *card,
			   sd_command_t *sd_cmd, u8 *buf, u32 data_len)
{
	bool result = FALSE;
	byte cmd_index = SD_CMD14;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_IN;
	byte *data = buf;
	u32 datalen = data_len;

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	result =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;
}

/*
 *
 * Function Name: emmc_bustest_w
 *
 * Abstract:
 *
 * 1. MMC/eMMC card bus width test write (CMD19)
 * 2. A host send the reversed bus testing data pattern from a Device.
 * 3. Do not need to check the cmd and data transfer error
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 * sd_command_t *sd_cmd: Pointer to sd command structure
 * u8 *buf : write data pattern buffer
 * u32 data_len : write data length
 *
 * Output:
 * None
 *
 * Return value:
 *
 * Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *
 * Caller: card_init
 */
static bool emmc_bustest_w(sd_card_t *card,
			   sd_command_t *sd_cmd, u8 *buf, u32 data_len)
{
	bool result = FALSE;
	byte cmd_index = SD_CMD19;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK | CMD_FLG_NO_TRANS;
	e_data_dir dir = DATA_DIR_OUT;
	byte *data = buf;
	u32 datalen = data_len;

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	result =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);

	return result;
}

/*
 *
 * Function Name: emmc_tuning_hw
 *
 * Abstract:
 *
 *			1.  Hardware Tuning Procedure (CMD21)
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *			sd_command_t *sd_cmd: Pointer to sd command structure
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *
 *           Caller: sd_tuning
 */

static bool emmc_tuning_hw(sd_card_t *card, sd_command_t *sd_cmd)
{
	bool result = FALSE;
	byte cmd_index = SD_CMD21;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_TUNE;
	e_data_dir dir = DATA_DIR_IN;
	byte data[64];
	sd_host_t *host = card->host;
	u32 datalen = 0x40;

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* set hardware tuning */
	host_set_tuning_mode(host, TRUE);

	if ((host->chip_type == CHIP_SDS0) ||
	    (host->chip_type == CHIP_SDS1) || (host->chip_type == CHIP_FUJIN2)
	    ) {
		/* add 200us delay before CMD19 to fix FJ2 ASIC issue 14# */
		os_udelay(200);
	}

	/* send emmc tuning CMD21 */
	result =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);
	if (result == FALSE) {
		DbgErr("eMMC card send hardware tuning CMD failed!!\n");
		goto exit;
	}

	/* check tuning success or not */
	result = host_chk_tuning_comp(host, TRUE);
	if (!result)
		DbgErr("Check eMMC tuning failed!\n");

exit:
	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;
}

/*
 *
 * Function Name: sd_tuning_sw
 *
 * Abstract:
 *
 *			1.  Software Tuning Procedure (CMD21)
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *			sd_command_t *sd_cmd: Pointer to sd command structure
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *
 *           Caller: sd_tuning
 */
static bool emmc_tuning_sw(sd_card_t *card, sd_command_t *sd_cmd)
{
	bool result = FALSE;
	byte cmd_index = SD_CMD21;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_TUNE;
	e_data_dir dir = DATA_DIR_IN;
	byte data[64];
	u16 i = 0;
	sd_host_t *host = card->host;
	u32 datalen = 0x40;

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	for (i = 0; i < 100; i++) {
		/* set software tuning */
		host_set_tuning_mode(host, FALSE);

		if ((host->chip_type == CHIP_SDS0) ||
		    (host->chip_type == CHIP_SDS1) ||
		    (host->chip_type == CHIP_FUJIN2)
		    ) {
			/* add 200us delay before CMD19 to fix FJ2 ASIC issue 14# */
			os_udelay(200);
		}

		/* send emmc tuning CMD21 */
		result =
		    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag,
				    dir, data, datalen);
		if (result == FALSE) {
			DbgErr("eMMC card hardware tuning failed!!\n");
			goto exit;
		}

		/* check tuning success or not */
		result = host_chk_tuning_comp(host, FALSE);
		if (result)
			break;
	}

exit:
	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;
}

/*
 *
 * Function Name: emmc_tuning
 *
 * Abstract:
 *
 *			 1. Send Hw tuning or Sw tuning by the registry setting
 *             2. tuning mode: 1 = CFG_TUNING_MODE_HW     0 = CFG_TUNING_MODE_SW
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
 *
 * Output:
 *			None
 *
 * Return value:
 *
 *			Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *
 *           Caller: card_init
 */
bool emmc_tuning(sd_card_t *card, sd_command_t *sd_cmd)
{
	bool result = FALSE;

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (card->mmc.cur_hs_type != EMMC_MODE_HS200
	    && card->mmc.cur_hs_type != EMMC_MODE_HS400) {
		result = TRUE;
		goto exit;
	}

	if (TUNING_MODE)
		result = emmc_tuning_hw(card, sd_cmd);
	else
		result = emmc_tuning_sw(card, sd_cmd);

exit:
	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;
}

/*
 *
 * Function Name: emmc_bus_width_test
 *
 * Abstract:
 *			 1. MMC/eMMC card bus width test by CMD14 (read) and CMD19(write)
 *            2. The data pattern decided by the bus width (8-bit\4-bit)
 *            3. Do not need to check the CMD19 any cmd and data transfer error
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
 *           u32 data_len : data pattern length
 *
 * Output:
 *			None
 *
 * Return value:
 *
 *			Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *
 *           Caller: card_init
 */
static bool emmc_bus_width_test(sd_card_t *card,
				sd_command_t *sd_cmd, u32 data_len)
{
	bool result = FALSE;
	u8 buf[8] = { 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa };
	u8 patten[8] = { 0 };

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s datlen %d\n", __func__, data_len);

	if (card->card_present == FALSE)
		goto exit;

	if (data_len == 4) {
		os_memset(buf, 0, 8);
		os_memset(buf, 0x5a, 4);

	}

	/* send CMD19 (eMMC bus write) */
	result = emmc_bustest_w(card, sd_cmd, buf, data_len);

	/* delay NCR clock */
	os_udelay(20);

	/* send CMD14 (eMMC bus read) */
	result = emmc_bustest_r(card, sd_cmd, patten, data_len);
	if (!result) {
		DbgErr("eMMC card CMD14 Receive bus width data Failed.\n");
	} else {
		result = FALSE;

		/* check patten */
		if (data_len == 8) {
			if ((patten[0] == 0xaa) || (patten[1] == 0x55)) {
				result = TRUE;
				DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT,
					NOT_TO_RAM,
					"8-bit bus width test OK!!\n");
			}
		} else if (data_len == 4) {
			if (patten[0] == 0xa5) {
				result = TRUE;
				DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT,
					NOT_TO_RAM,
					"4-bit bus width test OK!!\n");
			}
		}
	}

exit:
	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;
}

/*
 *
 * Function Name: emmc_get_ext_csd_info
 *
 * Abstract:
 *
 *			 1. Get the Ext_Csd structure
 *				(card_type, power class for 52M 26M 1.8V 3.3V voltage)
 *				from the Ext_Csd raw
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *
 * Output:
 *			None
 *
 * Return value:
 *
 *			None
 *
 * Notes:
 *
 *           Caller: emmc_get_ext_csd
 */
static void emmc_get_ext_csd_info(sd_card_t *card)
{
	mmc_card_info_t *mmc_info = &(card->mmc);
	extcsd_t *ext_csd = &(mmc_info->ext_csd);
	u8 *raw_ext_csd = &(mmc_info->raw_extcsd[0]);

	ext_csd->card_type = *(raw_ext_csd + 196);
	ext_csd->driver_strength_type = *(raw_ext_csd + 197);
	ext_csd->pwr_cl_52_195 = *(raw_ext_csd + 200);
	ext_csd->pwr_cl_26_195 = *(raw_ext_csd + 201);
	ext_csd->pwr_cl_52_360 = *(raw_ext_csd + 202);
	ext_csd->pwr_cl_26_360 = *(raw_ext_csd + 203);
	ext_csd->pwr_cl_ddr_52_195 = *(raw_ext_csd + 238);
	ext_csd->pwr_cl_ddr_52_360 = *(raw_ext_csd + 239);
	ext_csd->sec_cnt =
	    (*(raw_ext_csd + 215) << 24) + (*(raw_ext_csd + 214) << 16) +
	    (*(raw_ext_csd + 213) << 8) + *(raw_ext_csd + 212);
}

/*
 *
 * Function Name: emmc_switch_hs400
 *
 * Abstract:
 *			 1. eMMC card switch HS400 mode
 *
 * Input:
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *
 * Output:
 *			None
 *
 * Return value:
 *			Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *           Caller: emmc_init_stage2
 */
static bool emmc_switch_hs400(sd_card_t *card, sd_command_t *sd_cmd)
{
	bool result = FALSE;
	u32 argument = 0;
	sd_host_t *host = card->host;

	mmc_card_info_t *mmc_info = &(card->mmc);

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

#if (0)
	/* 1. Check card support hs400 */
	if ((mmc_info->ext_csd.card_type & MMC_CARD_TYPE_H400) == 0) {
		DbgErr("eMMC card don't support HS400 mode!!\n");
		goto exit;
	}

	/* 2. check card support 8-bit bus width */
	if ((mmc_info->cur_buswidth != EMMC_8Bit_BUSWIDTH) ||
	    (host->bus_8bit_supp == FALSE)
	    ) {
		DbgErr("The card or host don't support 8bit bus width!!\n");
		goto exit;
	}
#endif

	/* 3. clear UHSI mode select */
	host_set_uhs_mode(host, 0);

	/* 4.set card mode to DDR50 mode (eMMC: CMD6) */
	argument =
	    (MMC_EXTCSD_WRITE | MMC_EXTCSD_HS_TIMING | MMC_TIMING_HIGH_SPEED |
	     (mmc_info->drv_strength << 12));
	result = emmc_send_cmd6(card, sd_cmd, argument);
	if (!result) {
		DbgErr("Switch DDR50 mode Failed.\n");
		goto exit;
	}

	/* 5. change clock to 50M Hz for DDR50 mode */
	emmc_set_freq(card, SD_CLK_50M, TRUE);

	/* 6.change to 8-bit DDR mode (eMMC: CMD6) */
	argument =
	    (MMC_EXTCSD_WRITE | MMC_EXTCSD_BUS_WIDTH | MMC_BUSW_DDR_8BIT);
	result = emmc_send_cmd6(card, sd_cmd, argument);
	if (!result) {
		DbgErr("Change to 8-bit DDR mode Failed.\n");
		goto exit;
	}

	/* 7.switch to hs400 */
	argument =
	    (MMC_EXTCSD_WRITE | MMC_EXTCSD_HS_TIMING | MMC_TIMING_HS400 |
	     (mmc_info->drv_strength << 12));
	result = emmc_send_cmd6(card, sd_cmd, argument);
	if (!result) {
		DbgErr("Switch hs400 mode Failed.\n");
		goto exit;
	}

	/* 8.if switch hs400 ok, set PCI Register */
	host_emmc_hs400_set(host, TRUE);

	/* 9.change SDCLK frequency to 200M Hz */
	emmc_set_freq(card, SD_CLK_BASE, FALSE);

exit:
	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;
}

/*
 *
 * Function Name: emmc_switch_hs200
 *
 * Abstract:
 *			 1. eMMC card switch HS200 mode
 *
 * Input:
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *
 * Output:
 *			None
 *
 * Return value:
 *			Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *           Caller: emmc_init_stage2
 */
static bool emmc_switch_hs200(sd_card_t *card, sd_command_t *sd_cmd)
{
	bool result = FALSE;
	u32 argument = 0;
	u32 card_status = 0;
	sd_host_t *host = card->host;
	mmc_card_info_t *mmc_info = &(card->mmc);
	cfg_emmc_mode_t *emmc_mode = &(host->cfg->card_item.emmc_mode);

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* 1. switch signal Data Rate mode bus width (4-bit or 8-bit: eMMC CMD6) */
	mmc_info->cur_hs_type = EMMC_MODE_HS200;
	result = emmc_switch_buswidth(card, sd_cmd);
	if (!result) {
		DbgErr("Set signal Data Rate 8/4-bit Bus Width Failed.\n");
		goto exit;
	}

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Card support driver type %x\n",
		mmc_info->ext_csd.driver_strength_type);
	if ((2 << (emmc_mode->drv_strength)) &
	    (mmc_info->ext_csd.driver_strength_type)
	    )
		mmc_info->drv_strength = (byte) emmc_mode->drv_strength;
	else
		mmc_info->drv_strength = 0;
	/* 2.switch card support driver type & hs200 mode (eMMC: CMD6) */
	argument =
	    (MMC_EXTCSD_WRITE | MMC_EXTCSD_HS_TIMING | MMC_TIMING_HS200 |
	     (mmc_info->drv_strength << 12));
	result = emmc_send_cmd6(card, sd_cmd, argument);
	if (!result) {
		DbgErr("Switch HS200 mode Failed.\n");
		goto exit;
	}

	/* 3.check card status (Issue CMD13) */
	result = card_get_card_status(card, sd_cmd, &card_status);
	if ((result == FALSE) || (card_status & 0x80)
	    ) {
		DbgErr("Card Status failed.\n");
		goto exit;
	}

	/* 4.change clock to 200M Hz */
	emmc_set_freq(card, SD_CLK_BASE, FALSE);

	/* 5.switch mode (hs200 == SDR104) */
	host_set_uhs_mode(host, SDHCI_CTRL_UHS_HS200);

	if (((host->chip_type < CHIP_SEAEAGLE2) || (host->chip_type == CHIP_GG8)
	     || (host->chip_type == CHIP_ALBATROSS))
	    && (mmc_info->cur_buswidth == EMMC_8Bit_BUSWIDTH)) {
		host_set_buswidth(host, BUS_WIDTH4);
	}

	result = emmc_tuning(card, sd_cmd);
	if (!result)
		goto exit;

	if (mmc_info->cur_buswidth == EMMC_8Bit_BUSWIDTH)
		host_set_buswidth(host, BUS_WIDTH8);

	/* 7.if tuning ok, set PCI & Host Register */
	host_emmc_hs400_set(host, FALSE);

exit:
	if (result == FALSE)
		mmc_info->cur_hs_type = 0;
	else
		emmc_mode->enable_ddr_mode = 0;

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;
}

/*
 *
 * Function Name: emmc_switch_hs
 *
 * Abstract:
 *			 1. eMMC card switch High Speed mode
 *
 * Input:
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *
 * Output:
 *			None
 *
 * Return value:
 *			Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *           Caller: emmc_init_stage2
 */
static bool emmc_switch_hs(sd_card_t *card, sd_command_t *sd_cmd)
{
	bool result = FALSE;
	u32 argument = 0;
	u32 clk = 0;
	bool bddr50 = FALSE;
	sd_host_t *host = card->host;
	u32 b_dis_hs = host->cfg->card_item.emmc_mode.dis_hs;
	u8 device_type = card->mmc.ext_csd.card_type;

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/*
	 * if registry set disable hs mode or host don't support high speed,
	 * check card type, and set the clk
	 */
	if (b_dis_hs || (host->hs_supp == 0)) {
		DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"User disable high speed, select 25M timing!!\n");
		if (device_type & MMC_CARD_TYPE_HS_26M) {
			DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"select 25M timing!!\n");
			clk = SD_CLK_25M;
		}
	} else {
		if (device_type &
		    (MMC_CARD_TYPE_HS_52M | MMC_CARD_TYPE_HS_DDR_12 |
		     MMC_CARD_TYPE_HS_DDR_18)) {
			DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"select 52M timing!!\n");
			clk = SD_CLK_50M;
			if ((host->cfg->card_item.emmc_mode.enable_ddr_mode) &&
			    (device_type & MMC_CARD_DDR_SUPP)
			    ) {
				bddr50 = TRUE;
			}
		} else if (device_type & MMC_CARD_TYPE_HS_26M) {
			DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"select 25M timing!!\n");
			clk = SD_CLK_25M;
		}
	}

	/* dump power size */
	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"PMclass- 52M, DDR, 3.3v: 0x%xh 1.8v: 0x%xh\n",
		card->mmc.ext_csd.pwr_cl_ddr_52_360,
		card->mmc.ext_csd.pwr_cl_ddr_52_195);
	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"PMclass- 26M, SDR, 3.3v: 0x%xh 1.8v: 0x%xh\n",
		card->mmc.ext_csd.pwr_cl_26_360,
		card->mmc.ext_csd.pwr_cl_26_195);
	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"PMclass- 52M, SDR, 1.8v: 0x%xh 3.3v: 0x%xh\n",
		card->mmc.ext_csd.pwr_cl_52_195,
		card->mmc.ext_csd.pwr_cl_52_360);

	/* if host & card all support high speed, then set the hs_timing */
	if (clk) {
		argument =
		    (MMC_EXTCSD_WRITE | MMC_EXTCSD_HS_TIMING |
		     MMC_TIMING_HIGH_SPEED);
		result = emmc_send_cmd6(card, sd_cmd, argument);
		if (!result) {
			DbgErr("Switch High Speed mode Failed.\n");
			goto exit;
		}
	}

	/* change clock */
	if (clk)
		emmc_set_freq(card, clk, bddr50);

	/* if clock > 50M hz, set 0x28[2]: high speed enable */
	if (clk == SD_CLK_50M)
		host_set_highspeed(host, TRUE);

exit:
	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;
}

/*
 *
 * Function Name: emmc_ddr_mode_set
 *
 * Abstract:
 *			 1. set eMMC card DDR50 mode
 *
 * Input:
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *
 * Output:
 *			None
 *
 * Return value:
 *			Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *           Caller: emmc_init_stage2
 */
static void emmc_ddr_mode_set(sd_card_t *card)
{
	sd_host_t *host = card->host;
	u32 b_dis_hs = host->cfg->card_item.emmc_mode.dis_hs;
	u8 device_type = card->mmc.ext_csd.card_type;
	u32 enable_ddr = host->cfg->card_item.emmc_mode.enable_ddr_mode;

	if ((b_dis_hs == FALSE) &&
	    (enable_ddr) && (device_type & MMC_CARD_DDR_SUPP)
	    ) {
		DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Set host DDR50 mode!!\n");
		host_set_uhs_mode(host, SDHCI_CTRL_UHS_DDR50);
		host_emmc_ddr_set(host, TRUE);
	} else {
		host->cfg->card_item.emmc_mode.enable_ddr_mode = 0;
		host_emmc_ddr_set(host, FALSE);
	}

}

/*
 *
 * Function Name: emmc_switch_buswidth
 *
 * Abstract:
 *			 1. set eMMC card bus width
 *
 * Input:
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *
 * Output:
 *			None
 *
 * Return value:
 *			Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *           Caller: emmc_init_stage2
 */
static bool emmc_switch_buswidth(sd_card_t *card, sd_command_t *sd_cmd)
{
	bool result = FALSE;
	u32 argument = 0;
	u32 data_len = 0;
	sd_host_t *host = card->host;
	mmc_card_info_t *mmc_info = &(card->mmc);
	u32 dis_8_bit = host->cfg->card_item.emmc_mode.dis_8bit_bus_width;
	u32 dis_4_bit = host->cfg->card_item.emmc_mode.dis_4bit_bus_width;
	u32 enable_ddr = host->cfg->card_item.emmc_mode.enable_ddr_mode;

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (card->card_present == FALSE)
		goto exit;

	/* check host & user support 8-bit bus width */
	if ((dis_8_bit == 0) && (host->bus_8bit_supp)
	    ) {
		mmc_info->cur_buswidth = EMMC_8Bit_BUSWIDTH;
		data_len = 8;

		/* host set 8-bit bus width */
		host_set_buswidth(host, BUS_WIDTH8);

		/* delay 100us */
		os_udelay(100);

		result = emmc_bus_width_test(card, sd_cmd, data_len);
		if (result == TRUE) {
			if (enable_ddr &&
			    (card->mmc.ext_csd.card_type & MMC_CARD_DDR_SUPP) &&
			    (mmc_info->cur_hs_type != EMMC_MODE_HS200)
			    ) {
				argument =
				    (MMC_EXTCSD_WRITE | MMC_EXTCSD_BUS_WIDTH |
				     MMC_BUSW_DDR_8BIT);
			} else {
				argument =
				    (MMC_EXTCSD_WRITE | MMC_EXTCSD_BUS_WIDTH |
				     MMC_BUSW_SDR_8BIT);
			}
			result = emmc_send_cmd6(card, sd_cmd, argument);
			if (!result)
				DbgErr("Set 8-bit bus width Failed.\n");
			else
				goto exit;
		}
		DbgErr("Set 8-bit bus width Failed.\n");

	}

	/* check user support 4-bit bus width */
	if (dis_4_bit == FALSE) {
		mmc_info->cur_buswidth = EMMC_4Bit_BUSWIDTH;
		/* host set 4-bit bus width */
		host_set_buswidth(host, BUS_WIDTH4);
		data_len = 4;

		/* Test 4-bit patten, set block size to 4 bytes */
		result = emmc_bus_width_test(card, sd_cmd, data_len);
		if (!result)
			goto exit;

		if (enable_ddr &&
		    (card->mmc.ext_csd.card_type & MMC_CARD_DDR_SUPP) &&
		    (mmc_info->cur_hs_type != EMMC_MODE_HS200)
		    ) {
			argument =
			    (MMC_EXTCSD_WRITE | MMC_EXTCSD_BUS_WIDTH |
			     MMC_BUSW_DDR_4BIT);
		} else {
			argument =
			    (MMC_EXTCSD_WRITE | MMC_EXTCSD_BUS_WIDTH |
			     MMC_BUSW_SDR_4BIT);
		}

		result = emmc_send_cmd6(card, sd_cmd, argument);
		if (!result)
			DbgErr("Set 4-bit bus width Failed.\n");
		else
			goto exit;

		DbgErr("Set 4-bit bus width Failed.\n");

	}

	/* default 1-bit bus width */
	mmc_info->cur_buswidth = EMMC_1Bit_BUSWIDTH;
	host_set_buswidth(host, BUS_WIDTH1);

exit:
	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;
}

/*
 *
 * Function Name: emmc_set_trans_clk
 *
 * Abstract:
 *			 1. set MMC/eMMC card(Not support HS) transfer clock
 *
 * Input:
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *
 * Output:
 *			None
 *
 * Return value:
 *			Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *           Caller: emmc_init_stage2
 */
static bool emmc_set_trans_clk(sd_card_t *card)
{
	bool result = FALSE;
	u32 freq_unit = 0;
	u32 multip_factor = 0;
	u32 trans_clk;
	card_info_t *card_info = &(card->info);

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* get the frequency unit    0: 1KHz     1: 10KHz     2: 100KHz       3: 1000KHZ */
	switch ((card_info->csd.tran_speed) & 0x7) {
	case 0:
		freq_unit = 1;
		break;
	case 1:
		freq_unit = 10;
		break;
	case 2:
		freq_unit = 100;
		break;
	case 3:
		freq_unit = 1000;
		break;
	default:
		return result;
	}

	switch (((card_info->csd.tran_speed) & 0x78) >> 3) {
	case 1:
		multip_factor = 100;
		break;
	case 2:
		multip_factor = 120;
		break;
	case 3:
		multip_factor = 130;
		break;
	case 4:
		multip_factor = 150;
		break;
	case 5:
		multip_factor = 200;
		break;
	case 6:
		multip_factor = 260;
		break;
	case 7:
		multip_factor = 300;
		break;
	case 8:
		multip_factor = 350;
		break;
	case 9:
		multip_factor = 400;
		break;
	case 10:
		multip_factor = 450;
		break;
	case 11:
		multip_factor = 520;
		break;
	case 12:
		multip_factor = 550;
		break;
	case 13:
		multip_factor = 600;
		break;
	case 14:
		multip_factor = 700;
		break;
	case 15:
		multip_factor = 800;
		break;
	default:
		goto exit;
	}

	trans_clk = freq_unit * multip_factor;
	if (trans_clk >= SD_CLK_BASE) {
		/* 200M Hz */
		trans_clk = SD_CLK_BASE;
	}

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"MMC support trans clk=%d\n", trans_clk);
	/* change transfer clock */
	emmc_set_freq(card, trans_clk, FALSE);
	result = TRUE;

exit:
	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;
}

static u64 get_emmc_sec_count(sd_card_t *card)
{
	u64 capability = 0;

	card_info_t *card_info = &(card->info);
	mmc_card_info_t *mmc_info = &(card->mmc);

	/* <2G capability / SD_BLOCK_LEN */
	if (card_info->card_ccs == 0)
		capability = card->sec_count;
	else {
		/* > 2G sector count */
		capability = mmc_info->ext_csd.sec_cnt;
	}

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"MMC * EMMC sector count = 0x%x!!\n", capability);
	return capability;
}

/*
 *
 * Function Name: set_emmc_block_len
 *
 * Abstract:
 *			 1. set MMC/eMMC card block length
 *
 * Input:
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *            bool b_hs400: if DDR mode, can't set CMD16 to set block length
 *
 * Output:
 *			None
 *
 * Return value:
 *			Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *           Caller: emmc_init_stage2
 */
static bool set_emmc_block_len(sd_card_t *card, bool b_hs400)
{
	bool result = FALSE;
	u32 block_len = 0;
	card_info_t *card_info = &(card->info);
	sd_host_t *host = card->host;
	cfg_emmc_mode_t *cfg_emmc_mode = &(host->cfg->card_item.emmc_mode);
	sd_command_t sd_cmd;

	os_memset(&sd_cmd, 0, sizeof(sd_command_t));

	block_len = (2 << (card_info->csd.read_bl_len));

	if (block_len > host->max_block_len) {
		DbgWarn(MODULE_MMC_CARD, NOT_TO_RAM,
			"Device block length > HW Init max block length!!\n");
	}

	if ((cfg_emmc_mode->enable_ddr_mode) || b_hs400) {
		/* todo: if DDR mode, */
		result = TRUE;
	} else {
		result = card_set_block_len(card, &sd_cmd, SD_BLOCK_LEN);
	}

	return result;
}

/*
 *
 * Function Name: emmc_init_stage2
 *
 * Abstract:
 *			 1. emmc card initialize main function.
 *            2. Fill virtual card structure, like cid, csd, ext_csd etc.
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *
 *           Caller: card_init_stage2
 */
bool emmc_init_stage2(sd_card_t *card)
{
	bool result = FALSE;
	sd_host_t *host = card->host;
	mmc_card_info_t *mmc_info = &card->mmc;
	cfg_emmc_mode_t *cfg_emmc_mode = &(host->cfg->card_item.emmc_mode);
	sd_command_t sd_cmd;

	os_memset(&sd_cmd, 0, sizeof(sd_command_t));
	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* set card work clock to 25M */
	if (mmc_info->ext_csd.card_type) {
		emmc_set_freq(card, SD_CLK_25M, FALSE);
		DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Set eMMC Card Host Clock to 25MHz.\n");
	}

	/* check emmc card HS200 mode */
	if ((mmc_info->ext_csd.card_type & MMC_CARD_TYPE_H200) &&
	    (cfg_emmc_mode->enable_force_hs != 1)
	    ) {
		result = emmc_switch_hs200(card, &sd_cmd);
		if (result == FALSE) {
			DbgErr("Switch hs200 mode failed!!\n");
			goto exit;
		}

		if ((cfg_emmc_mode->enable_force_hs200) ||
		    (mmc_info->cur_buswidth != EMMC_8Bit_BUSWIDTH) ||
		    (host->bus_8bit_supp == FALSE)
		    )
			goto exit;

		/* check emmc card HS400 mode */
		if ((host->chip_type == CHIP_SEAEAGLE2
		     || cfg_emmc_mode->enable_force_hs400
		     || host->chip_type == CHIP_GG8
		     || host->chip_type == CHIP_ALBATROSS)
		    && (mmc_info->ext_csd.card_type & MMC_CARD_TYPE_H400)
		    ) {
			/* choose the eMMC hs400 mode */
			result = emmc_switch_hs400(card, &sd_cmd);
			if (result == FALSE)
				DbgErr("Switch hs400 mode failed!!\n");
			else
				mmc_info->cur_hs_type = EMMC_MODE_HS400;
		}

		goto exit;
	}

	/* check emmc card HS mode */
	if ((mmc_info->ext_csd.card_type) & MMC_CARD_TYPE_HS) {
		result = emmc_switch_hs(card, &sd_cmd);
		if (result == FALSE)
			DbgErr("Switch hs mode failed!!\n");

	} else {
		/* set transfer clock */
		result = emmc_set_trans_clk(card);
		if (!result) {
			DbgErr("Set Basic transfer clock failed.\n");
			goto exit;
		}
	}

	/* set MMC/eMMC card bus width & DDR mode */
	if (emmc_switch_buswidth(card, &sd_cmd) == TRUE) {
		/* DDR mode not support 1-bit */
		emmc_ddr_mode_set(card);
	}

exit:
	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;
}

/*
 *
 * Function Name: emmc_init
 *
 * Abstract:
 *			 1. emmc card initialize main function.
 *            2. Fill virtual card structure, like cid, csd, ext_csd etc.
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *
 *           Caller: card_init
 */
bool emmc_init(sd_card_t *card, bool bemmc)
{
	bool result = FALSE;
	sd_host_t *host = card->host;
	card_info_t *card_info = &(card->info);
	sd_command_t sd_cmd;
	cfg_emmc_mode_t *cfg_emmc_mode = &(host->cfg->card_item.emmc_mode);
	mmc_card_info_t *mmc_info = &(card->mmc);

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s bemmc = %d\n", __func__, bemmc);
	os_memset(&sd_cmd, 0, sizeof(sd_command_t));
	os_memset(mmc_info, 0, sizeof(mmc_card_info_t));

	/* 1. emmc host init */
	result = host_emmc_init(host, cfg_emmc_mode);
	if (result == FALSE) {
		DbgErr("Emmc Host Init Failed.\n");
		goto exit;
	}

	/* 2. Issue reset command (CMD0) */
	result = card_reset_card(card, &sd_cmd);
	result = card_reset_card(card, &sd_cmd);
	if (!result) {
		/* Go Idle State command failed. exit directly. */
		DbgErr("Reset Card (CMD0) Failed.\n");
		goto exit;
	}

	/* 2. Wait for card ready (CMD01) */
	result = emmc_card_init_ready(card, &sd_cmd);
	if (!result) {
		DbgErr("Wait for card ready CMD1 Failed.\n");
		goto exit;
	}

	if (bemmc)
		card->card_type = CARD_EMMC;
	else
		card->card_type = CARD_MMC;

	/* Get card CID(CMD2) */
	result = card_all_send_cid(card, &sd_cmd);
	if (!result) {
		DbgErr("Get card CID(CMD2) failed.\n");
		goto exit;
	}

	/* 4. Set card relative address (CMD3) */
	result = emmc_set_rca(card, &sd_cmd);
	if (!result) {
		DbgErr
		    ("MMC/eMMC card set card relative address (CMD3) failed.\n");
		goto exit;
	}

	/* 5. Get CSD (CMD9) */
	result = card_get_csd(card, &sd_cmd);
	if (!result) {
		DbgErr("Get CSD (CMD9) failed.\n");
		goto exit;
	}

	/* 6. Select the card (CMD7) */
	result = card_select_card(card, &sd_cmd);
	if (!result) {
		DbgErr("Select card (CMD7) failed.\n");
		goto exit;
	}

	/* 7. Check card lock */
	if (card->locked == TRUE) {
		DbgErr("Card is locked!!\n");
		goto exit;
	}

	/* 8. Check card SPEC version */
	if (card_info->csd.mmc_spec_vers < MMC_SPEC_VERS) {
		DbgWarn(MODULE_MMC_CARD, NOT_TO_RAM,
			"Spec version < 4: it's old MMC Device!!\n");
		/* set transfer clock */
		result = emmc_set_trans_clk(card);
		if (!result) {
			DbgErr("Set Basic transfer clock failed.\n");
			goto exit;
		}
		goto exit;
	}

	/* 9. Get Ext_Csd */
	result = emmc_get_ext_csd(card, &sd_cmd);
	if (!result) {
		DbgErr("Get mmc Ext_Csd failed.\n");
		goto exit;
	}

	/* 10. Switch Card mode */
	result = card_init_stage2(card);

exit:
	if (result) {
		/* max LBA */
		card->sec_count = get_emmc_sec_count(card);
		result =
		    set_emmc_block_len(card,
				       (mmc_info->cur_hs_type ==
					EMMC_MODE_HS400) ? TRUE : FALSE);
		if (!result)
			DbgErr("Set block length failed.\n");
	}

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;
}

/*
 * Function Name: mmc_degrade_policy
 * Abstract: This Function is used set sd degrade flag
 *
 * Input:
 * sd_card_t *card : The Command will send to which  Card
 *
 * Return value:
 *
 */
void mmc_degrade_policy(sd_card_t *card)
{

	/* check if at hs200 or hs400 then can degrade freq */
	/* else set degrade_all flag */

	cfg_emmc_mode_t *cfg_emmc_mode =
	    &(card->host->cfg->card_item.emmc_mode);

	if (cfg_emmc_mode->enable_force_hs400
	    || cfg_emmc_mode->enable_force_hs200
	    || (card->mmc.ext_csd.card_type & MMC_CARD_TYPE_H400)
	    || (card->mmc.ext_csd.card_type & MMC_CARD_TYPE_H200)) {
		if (card->degrade_freq_level < CARD_DEGRADE_FREQ_TIMES)
			card->degrade_freq_level++;
		else
			card->degrade_final = 1;
	} else
		card->degrade_final = 1;

	DbgErr("EMMC degrade final=%d freq_level=%d\n", card->degrade_final,
	       card->degrade_freq_level);

}

static void emmc_set_freq(sd_card_t *card, u32 clock_freq, bool bddr50)
{
	sd_host_t *host = card->host;
	u32 value = 0;
	u16 index = card->degrade_freq_level;

	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, clk_freq_khz=%dkhz, ddr50_mode=%d\n", __func__,
		clock_freq, bddr50);
	switch (clock_freq) {
	case SD_CLK_ID_400K:
		value = host->cfg->dmdn_tbl[FREQ_EMMC_400K_START_INDEX];
		break;
	case SD_CLK_50M:
		if (bddr50)
			value =
			    host->cfg->dmdn_tbl[FREQ_EMMC_DDR50M_START_INDEX];
		else
			value = host->cfg->dmdn_tbl[FREQ_EMMC_50M_START_INDEX];
		break;
	case SD_CLK_200M:
		value = host->cfg->dmdn_tbl[FREQ_EMMC_200M_START_INDEX + index];
		break;
	default:
		value = host->cfg->dmdn_tbl[FREQ_EMMC_25M_START_INDEX];
		break;
	}
	host_change_clock(host, value);
	DbgInfo(MODULE_MMC_CARD, FEATURE_CARD_INIT, TO_RAM,
		"Enter %s, clk_freq_khz=%dkhz, ddr50_mode=%d\n", __func__,
		clock_freq, bddr50);
}
