// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: sd.c
 *
 * Abstract: SD Legacy card initialization
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
#include "../include/cardapi.h"
#include "../include/hostapi.h"
#include "cardcommon.h"
#include "../include/debug.h"
#include "../include/cmdhandler.h"
#include "../include/host.h"
#include "../include/util.h"
#include "../include/hostvenapi.h"
#include "../host/hostven.h"
#include "card_ddr200_support.h"
#include "../include/funcapi.h"
/* 0: card not support ddr200 mode, 1: support  */
u32 sd_card_ddr200_flag;
extern bool card_output_tuning(sd_card_t *card, u64 tuning_address);
extern bool store_tuning_address_content(sd_card_t *card, u64 tuning_address);
extern bool restore_tuning_address_content(sd_card_t *card,
					   u64 tuning_address);
extern u32 generate_output_input_phase_pair(sd_card_t *card,
					    u8 input_fix_phase, u8 ddr200);

inline bool uhs1_support(sd_host_t *host)
{
	bool ret = TRUE;

	/* 2. Configuration settings to disable UHSI function */
	if (host->cfg->card_item.sd_card_mode_dis.dis_sd30_card)
		ret = FALSE;

	return ret;

}

static inline bool need_switch_sig_voltage(sd_card_t *card)
{
	bool ret = FALSE;
	card_info_t *card_info = &(card->info);

	if (card->card_type == CARD_UHS2)
		goto exit;

	/* 1. Check configuration settings
	 * BH722SE2LN-A UHS1 issue#3  Sharkbay QS ULT #6 platform, BH driver 10024,
	 * set card mode to be SDR25 or SDR12 by registry, but the card is SD2.0 mode.
	 * Change to switch voltage when s18a is true.
	 */

	if (card_info->card_s18a)
		ret = TRUE;

exit:
	return ret;
}

static inline bool card_support_cmd6(sd_card_t *card)
{
	bool ret = TRUE;
	card_info_t *card_info = &(card->info);
	/* 1. SD Memory Card - Spec. Version 1.0 and 1.01 do not support CMD6 */
	if (card_info->scr.sd_spec < SCR_SPEC_VER_1)
		ret = FALSE;

	return ret;
}

#define  SDHCI_POWER_VDD1_330	0x0E00
#define POWER_ON     TRUE
#define POWER_OFF    FALSE

bool sd_send_if_cond(sd_card_t *card, sd_command_t *sd_cmd, u32 argument)
{
	byte cmd_index = SD_CMD8;
	/* u32 argument = 0x000001AA;   *  VHS set.  2.7-3.6V Check Pattern : 0xAA */
	u32 cmdflag = CMD_FLG_R7 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_NONE;
	byte *data = NULL;
	u32 datalen = 0;
	bool ret = FALSE;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);

	if (ret) {
		/* Pattern Check */
		if ((sd_cmd->response[0] & 0xFF) != 0xAA) {
			sd_cmd->err.error_code = ERR_CODE_RESP_ERR;
			ret = FALSE;
			DbgErr("CMD8 response pattern check failed.");
		}
	}

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 *	static bool sdio_check(sd_card_t *card, sd_command_t *sd_cmd)
 *	{
 *		byte cmd_index = SD_CMD5;
 *		u32 argument = 0;
 *		u32 cmdflag = CMD_FLG_R4 | CMD_FLG_RESCHK;
 *		e_data_dir dir = DATA_DIR_NONE;
 *		byte *data = NULL;
 *		u32 datalen = 0;
 *		bool ret = FALSE;
 *
 *		DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
 *			__func__);
 *
 *		ret =
 *			card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
 *			data, datalen);
 *
 *		DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
 *			ret, __func__);
 *		return ret;
 *	}
 */

/*
 *
 * Function Name: card_init_ready
 *
 * Abstract:
 *
 *			 1. Issue ACMD41 to Get OCR
 *            2. Set the card ocr variable
 *            3. Wait for card ready.
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
 *           bool flag_f8: Command 8 is executed correctly.
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

bool card_init_ready(sd_card_t *card, sd_command_t *sd_cmd, bool flag_f8)
{
	byte cmd_index = SD_CMD41 | SD_APPCMD;
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

	/* default is 0 */
	card->uhs2_card = FALSE;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, flag_f8=%d\n", __func__, flag_f8);

	if (flag_f8)
		os_udelay(20);

	/* 3. Start the card initialization */
	/* 3.1 Set argument according to the flag F8 (command 8 executed correctly) */
	argument = (1 << fls32(host->ocr_avail));

	if (card->card_type != CARD_UHS2) {
		if (flag_f8) {
			if (uhs1_support(host)) {
				/*
				 * BH722SE2LN-A UHS1 issue#3  Sharkbay QS ULT #6 platform,
				 * BH driver 10024,
				 * set card mode to be SDR25 or SDR12 by registry,
				 * but the card is SD2.0 mode.
				 * Change to send s18R for SDR12/SDR25/SDR50/SDR104.
				 */

				/* Try to set the HCS/XPC/S18R */
				argument |= 0x51000000;
			} else {
				/* Try to set the HCS */
				argument |= 0x40000000;

				/* Set XPC */
				argument |= BIT28;
			}
		}
	} else {
		/* UHS2 case */
		/* Set HCS  bit only */
		argument |= BIT30;

		/* Set XPC */
		argument |= BIT28;
	}

	/* 3.2 Wait for card ready */
	util_init_waitloop(card->host->pdx,
			   host->cfg->timeout_item.test_card_init_timeout.value,
			   delay_us, &wait);

	do {
		ret =
		    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag,
				    dir, data, datalen);
		if (!ret)
			break;

		if (flag_f8)
			os_udelay(delay_us);

		/* Check Busy status. 0b: On initialization; 1b: Initialization Complete. */
		if ((sd_cmd->response[0] & 0x80000000) == 0) {
			ret = FALSE;
			continue;
		} else {
			/* card is ready */
			ret = TRUE;
			/* check uhs2 or not */
			if ((sd_cmd->response[0] & (1 << 29)) != 0)
				card->uhs2_card = TRUE;
			break;
		}
	} while (!util_is_timeout(&wait));

	/* 3.3 If card ready, set related software flags */
	if (ret) {
		/* 3.3.1. Set sd_virt_card OCR */
		card_info->card_ccs = (sd_cmd->response[0] & BIT30) >> 30;
		if (uhs1_support(host))
			card_info->card_s18a =
			    (sd_cmd->response[0] & BIT24) >> 24;
		else
			card_info->card_s18a = 0;

	}

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 *
 * Function Name: signal_voltage_switch
 *
 * Abstract:
 *
 *			 1.  Do Signal Voltage Switch Procedure (UHSI, CMD11)
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
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

static bool signal_voltage_switch(sd_card_t *card, sd_command_t *sd_cmd)
{
	byte cmd_index = SD_CMD11;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_NONE;
	byte *data = NULL;
	u32 datalen = 0;
	bool ret = FALSE;
	sd_host_t *host = card->host;
	card_info_t *card_info = &(card->info);

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/*
	 * 1. If S18A of ACMD41 is se to 0, do not need to switch signal voltage,
	 * Exit from this procedure
	 */
	if (card_info->card_s18a == 0) {
		ret = TRUE;
		DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Do not need to switch signal voltage.\n");

		goto EXIT;
	}

	/* 2. Issue CMD11 */
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);
	if (!ret) {
		DbgErr("Issue CMD11 failed.\n");
		goto EXIT;
	}

	ret = host_enable_sd_signal18v(host);

	/* 3.3V->1.8V OK */
	if (ret == FALSE)
		goto ERROR;
	goto EXIT;

ERROR:

	/* return to previous status */

	host_1_8v_sig_set(host, FALSE);

EXIT:
	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;

}

/*
 *
 * Function Name: sd_get_sdstatus
 *
 * Abstract:
 *
 *			 1.  Read the SD Status Register (SSR) (ACMD13)
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
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

static bool sd_get_sdstatus(sd_card_t *card, sd_command_t *sd_cmd)
{

	byte cmd_index = SD_CMD13 | SD_APPCMD;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_IN;
	u32 datalen = 64;

	bool ret = FALSE;
	card_info_t *card_info = &(card->info);

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* Issue ACMD13 */
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    &(card_info->raw_ssr[0]), datalen);

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 *
 * Function Name: sd_send_cmd35
 *
 * Abstract:
 *
 *			 1.  Read the SD Status Register (SSR) (CMD13)
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
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

static bool sd_send_cmd35(sd_card_t *card)
{

	byte cmd_index = SD_CMD35;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_OUT;
	sd_command_t sd_cmd;
	byte buffer[512] = {
		0x10, 0x03,
		0x27, 0x86, 0x45, 0xA5, 0x19, 0x40, 0xF2, 0x25,
		0x20, 0x47, 0xDF, 0x94, 0xB8, 0x16, 0x13, 0x00,
		0x11, 0xF2, 0x1B, 0x4F, 0x23, 0x08, 0x2B, 0x33,
		0x21, 0x8C, 0x16, 0x52, 0x6A, 0x1D, 0x89, 0xE3,
		0x14, 0x09, 0x53, 0x84, 0x09, 0x47, 0x59, 0x50,
		0x57, 0xBB, 0x71, 0x3C, 0x47, 0xA7, 0x2A, 0x46,
		0xE2, 0xAF, 0x43, 0x10, 0x44, 0x05, 0x53, 0x7A,
		0x79, 0xC3, 0x29, 0xF3, 0x83, 0x45, 0x22, 0x1B,
	};
	u32 datalen = 512;
	bool ret = FALSE;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* Issue CMD35 */
	ret =
	    card_send_sdcmd(card, &sd_cmd, cmd_index, argument, cmdflag, dir,
			    buffer, datalen);

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 *
 * Function Name: sd_send_cmd34
 *
 * Abstract:
 *
 *			 1.  Read the SD Status Register (SSR) (CMD13)
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
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

static bool sd_send_cmd34(sd_card_t *card)
{

	byte cmd_index = SD_CMD34;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_IN;
	sd_command_t sd_cmd;
	byte buffer[512] = { 0x00, 0x00, };
	u32 datalen = 512;
	bool ret = FALSE;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* Issue CMD34 */
	ret =
	    card_send_sdcmd(card, &sd_cmd, cmd_index, argument, cmdflag, dir,
			    buffer, datalen);

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 *
 * Function Name: sd_set_bus_width
 *
 * Abstract:
 *
 *			 1.  Set Bus Width to 4bit (ACMD6)
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
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

static bool sd_set_bus_width(sd_card_t *card, sd_command_t *sd_cmd)
{

	byte cmd_index = SD_CMD6 | SD_APPCMD;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_NONE;
	byte *data = NULL;
	u32 datalen = 0;
	bool ret = FALSE;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	argument = BUS_WIDTH_4BIT;

	/* Issue ACMD6 */
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 *
 * Function Name: sd_get_cid
 *
 * Abstract:
 *
 *			1.  Addressed card sends its card identification data (CID)
 *			on the CMD line (CMD10)
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
 *			Caller: card_init
 */

static bool sd_get_cid(sd_card_t *card, sd_command_t *sd_cmd)
{

	byte cmd_index = SD_CMD10;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R2;
	e_data_dir dir = DATA_DIR_NONE;
	byte *data = NULL;
	u32 datalen = 0;
	bool ret = FALSE;
	card_info_t *card_info = &(card->info);

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	argument = card_info->rca << 16;

	/* Issue CMD10 */
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);
	if (ret) {
		/* Set the card CID info */
		os_memcpy(&(card_info->raw_cid[0]), &(sd_cmd->response[0]), 16);
		/* Parse the CID info */
		card_info->cid.manfid = card_info->raw_cid[0];
		card_info->cid.oemid =
		    card_info->raw_cid[1] | (card_info->raw_cid[2] << 8);
		os_memcpy(card_info->cid.prod_name, &(card_info->raw_cid[3]),
			  5);
		card_info->cid.prv = card_info->raw_cid[8];
		card_info->cid.serial =
		    card_info->raw_cid[9] | (card_info->raw_cid[10] << 8) |
		    (card_info->raw_cid[11] << 16) | (card_info->raw_cid[12] << 24);
	}

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 *
 * Function Name: sd_get_scr
 *
 * Abstract:
 *
 *			 1.  Read the SD Configuration Register (SCR) (ACMD51)
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
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

static bool sd_get_scr(sd_card_t *card, sd_command_t *sd_cmd)
{

	byte cmd_index = SD_CMD51 | SD_APPCMD;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_IN;
	u32 datalen = 8;
	bool ret = FALSE;
	card_info_t *card_info = &(card->info);

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* Issue ACMD51 */
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    &(card_info->raw_scr[0]), datalen);
	if (ret) {
		/* Parse the CSD info */
		card_info->scr.sd_spec = card_info->raw_scr[0] & 0xF;
		card_info->scr.cmd_support = card_info->raw_scr[3] & 0xF;
		card_info->scr.sd_spec3 = card_info->raw_scr[2] & 0x80;
		card_info->scr.sd_specx =
		    (card_info->raw_scr[2] & 0x3) << 2 |
			(card_info->raw_scr[3] & 0xc0) >> 6;
		card_info->scr.reserved_B0 = card_info->raw_scr[7] & 0xFF;
		card_info->scr.reserved_B1 = card_info->raw_scr[6] & 0xFF;
	}
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 *
 * Function Name: sd_get_scr
 *
 * Abstract:
 *
 *			 1.  Read the SD Configuration Register (SCR) (ACMD51)
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
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

/*
 *	static bool sd_clear_card_detect(sd_card_t *card)
 *	{
 *
 *		sd_command_t sd_cmd;
 *		byte cmd_index = SD_CMD42 | SD_APPCMD;
 *		u32 argument = 0;
 *		u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
 *		e_data_dir dir = DATA_DIR_NONE;
 *		bool ret = FALSE;
 *
 *		DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
 *			__func__);
 *
 *		ret =
 *			card_send_sdcmd(card, &sd_cmd, cmd_index, argument, cmdflag, dir,
 *			NULL, 0);
 *		DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
 *			ret, __func__);
 *		return ret;
 *	}
 */

/*
 *
 * Function Name: sd_switch_function_set_am
 *
 * Abstract:
 *
 *			 1.  Set SD switch function status (Access Mode) (CMD6)
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
 *           byte access_mode: access mode which want be set.
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
 *           Caller: sd_switch_function_set
 */

bool sd_switch_function_set_am(sd_card_t *card,
			       sd_command_t *sd_cmd, byte access_mode)
{

	byte cmd_index = SD_CMD6;
	u32 argument = SD_FNC_SW | SD_FNC_G1_INFL;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_IN;
	byte data[64];
	u32 datalen = 64;
	bool ret = FALSE;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, access_mode=0x%x\n", __func__, access_mode);

	if (access_mode == SD_FNC_AM_DDR200)
		argument = 0x80FFFFEF;
	else
		argument |= access_mode;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Set AM to %x, argument=%08X\n", access_mode, argument);

	/* Issue CMD6 */
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 *
 * Function Name: sd_switch_function_set_ds
 *
 * Abstract:
 *
 *			 1.  Set SD switch function status (Driver Strength) (CMD6)
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
 *           byte driver_strength: driver strength which want be set.
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
 *           Caller: sd_switch_function_set
 */

bool sd_switch_function_set_ds(sd_card_t *card,
			       sd_command_t *sd_cmd, byte driver_strength)
{

	byte cmd_index = SD_CMD6;
	u32 argument = SD_FNC_SW | SD_FNC_G3_INFL;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_IN;
	byte data[64];
	u32 datalen = 64;
	bool ret = FALSE;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, driver_strength=0x%x\n", __func__,
		driver_strength);

	argument |= driver_strength << 8;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Set Driver Strength to %x, argument=%08X\n", driver_strength,
		argument);

	/* Issue CMD6 */
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

static byte card_get_max_am_cap(sd_card_t *card)
{
	byte max_am_cap = 0;

	if (sd_ddr_support(card))
		max_am_cap = SD_FNC_AM_DDR200;
	else if (card->info.sw_func_cap.sd_access_mode & (1 << SD_FNC_AM_SDR104))
		max_am_cap = SD_FNC_AM_SDR104;
	else if (card->info.sw_func_cap.sd_access_mode & (1 << SD_FNC_AM_SDR50))
		max_am_cap = SD_FNC_AM_SDR50;
	else if (card->info.sw_func_cap.sd_access_mode & (1 << SD_FNC_AM_DDR50))
		max_am_cap = SD_FNC_AM_DDR50;
	else if (card->info.sw_func_cap.sd_access_mode & (1 << SD_FNC_AM_HS))
		max_am_cap = SD_FNC_AM_HS;
	else if (card->info.sw_func_cap.sd_access_mode & (1 << SD_FNC_AM_DS))
		max_am_cap = SD_FNC_AM_DS;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "(%x) %s\n",
		max_am_cap, __func__);
	return max_am_cap;
}

static byte card_get_min_am(byte am1, byte am2)
{
	byte am = 0;

	switch (am1) {
	case SD_FNC_AM_DDR200:
		am = am2;
		break;
	case SD_FNC_AM_SDR104:
		if (am2 == SD_FNC_AM_DDR200)
			am = am1;
		else
			am = am2;
		break;
	case SD_FNC_AM_SDR50:
		if (am2 == SD_FNC_AM_SDR104 || am2 == SD_FNC_AM_DDR200)
			am = am1;
		else
			am = am2;
		break;
	case SD_FNC_AM_DDR50:
		if (am2 == SD_FNC_AM_SDR50 || am2 == SD_FNC_AM_SDR104
		    || am2 == SD_FNC_AM_DDR200) {
			am = am1;
		} else {
			am = am2;
		}
		break;
	case SD_FNC_AM_HS:
		if (am2 == SD_FNC_AM_DS)
			am = am2;
		else
			am = am1;
		break;
	case SD_FNC_AM_DS:
		am = am1;
		break;
	default:
		break;
	}
	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "(%x) %s\n", am,
		__func__);
	return am;
}

static byte card_power_limit_to_index(u8 pmlimit)
{
	byte i = 0;
	byte power_limit[5] = {
		SD_FNC_PL_072W,
		SD_FNC_PL_144W,
		SD_FNC_PL_180W,
		SD_FNC_PL_216W,
		SD_FNC_PL_288W
	};

	for (i = 0; i < 5; i++)
		if (power_limit[i] == pmlimit)
			return i;

	return 0;
}

bool sd_switch_power_limit(sd_card_t *card, sd_command_t *sd_cmd, bool *bchg)
{
	bool ret = FALSE;
	card_info_t *card_info = &(card->info);
	bool high_to_low = TRUE;
	byte start;

	byte power_limit[5] = {
		SD_FNC_PL_072W,
		SD_FNC_PL_144W,
		SD_FNC_PL_180W,
		SD_FNC_PL_216W,
		SD_FNC_PL_288W
	};
	int i;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, *bchg= %d\n", __func__, *bchg);

	/*
	 * 3.1 Get Max Power Limit, default: 2.88W
	 * 3.2 check the Max power limit that card support
	 * Check order: 2.88W -> 2.16W -> 1.8W -> 1.44W -> 0,72W
	 */

	start =
	    card_power_limit_to_index(card->sw_target_setting.sd_power_limit);
	if (start > 4) {
		DbgErr("Power Limit settings invalid!");
		start = 4;
	}

	/* start  = i; */
	if (card->thermal_enable)
		if (card->thermal_heat == 0)
			high_to_low = FALSE;

	if (high_to_low) {
		for (i = start; i >= 0; i--) {
			if (card_info->sw_cur_setting.sd_power_limit ==
			    power_limit[i] && *bchg == FALSE) {
				ret = TRUE;
				*bchg = FALSE;
				break;
			}
			/* Check card support or not */
			if (card_info->sw_func_cap.sd_power_limit &
				(1 << (power_limit[i]))) {
				ret =
				    sd_switch_function_set_pl(card, sd_cmd,
							      power_limit[i]);
				if (!ret) {
					DbgErr
					    ("Set Power Limit to %X Failed.\n",
					     power_limit[i]);
					goto exit;
				}
				/* Update the current settings */
				card_info->sw_cur_setting.sd_power_limit =
				    power_limit[i];
				DbgInfo(MODULE_ALL_CARD,
					FEATURE_FUNC_THERMAL |
					FEATURE_CARD_INIT, NOT_TO_RAM,
					"Powerlimit1 index=%d\n", i);
				*bchg = TRUE;
				break;
			}
		}
	} else {
		for (i = 0; i <= start; i++) {
			if (card_info->sw_cur_setting.sd_power_limit ==
			    power_limit[i] && *bchg == FALSE) {
				ret = TRUE;
				*bchg = FALSE;
				break;
			}
			/* Check card support or not */
			if (card_info->sw_func_cap.sd_power_limit &
				(1 << (power_limit[i]))) {
				ret =
				    sd_switch_function_set_pl(card, sd_cmd,
							      power_limit[i]);
				if (!ret) {
					DbgErr
					    ("Set Power Limit to %X Failed.\n",
					     power_limit[i]);
					goto exit;
				}
				/* Update the current settings */
				card_info->sw_cur_setting.sd_power_limit =
				    power_limit[i];
				DbgInfo(MODULE_ALL_CARD,
					FEATURE_FUNC_THERMAL |
					FEATURE_CARD_INIT, NOT_TO_RAM,
					"Powerlimit2 index=%d\n", i);
				*bchg = TRUE;
				break;
			}
		}
	}
exit:
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

bool sd_switch_access_mode(sd_card_t *card, sd_command_t *sd_cmd, bool *bchg)
{
	bool ret = FALSE;
	card_info_t *card_info = &(card->info);
	byte i;
	u32 clock_freq;
	u32 regval;

	sd_host_t *host = card->host;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, *bchg= %d\n", __func__, *bchg);

	/* 4.1 Get Max Access Mode of UHSI */
	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"card->sw_target_setting.sd_access_mode %d\n",
		card->sw_target_setting.sd_access_mode);
	card->sw_target_setting.sd_access_mode =
	    card_get_min_am(card->sw_target_setting.sd_access_mode,
			    (byte) card_get_max_am_cap(card));
	i = card->sw_target_setting.sd_access_mode;

	while (i >= 0) {
		/* Check card support or not */
		if (i == SD_FNC_AM_DDR200) {
			ret = sd_switch_function_set_am(card, sd_cmd, i);
			if (!ret) {
				DbgErr("Set Access Mode to %X Failed.\n", i);
				i = SD_FNC_AM_SDR104;
				continue;
			}

			/* Update the current settings */
			card_info->sw_cur_setting.sd_access_mode = i;
			DbgInfo(MODULE_ALL_CARD,
				FEATURE_FUNC_THERMAL | FEATURE_CARD_INIT,
				NOT_TO_RAM, "Access Mode=%d\n", i);
			*bchg = TRUE;
			break;
		}

		if (card_info->sw_func_cap.sd_access_mode & (1 << i)) {
			if (card_info->sw_cur_setting.sd_access_mode == i
			    && *bchg == FALSE) {
				ret = TRUE;
				*bchg = FALSE;
				goto exit;
			}
			ret = sd_switch_function_set_am(card, sd_cmd, i);
			if (!ret) {
				DbgErr("Set Access Mode to %X Failed.\n", i);
				goto exit;
			}
			/* Update the current settings */
			card_info->sw_cur_setting.sd_access_mode = i;
			DbgInfo(MODULE_ALL_CARD,
				FEATURE_FUNC_THERMAL | FEATURE_CARD_INIT,
				NOT_TO_RAM, "Access Mode=%d\n", i);
			*bchg = TRUE;
			break;
		}

		/* If DDR50 is the target access mode,
		 * then the next access mode should be High Speed (1),
		 * instead of SDR104 (3)
		 */
		if (i == SD_FNC_AM_DDR50) {
			i = SD_FNC_AM_HS;
			continue;
		}

		i--;
	}

	/* 5 Set timing accrodingly */
	switch (card_info->sw_cur_setting.sd_access_mode) {
	case SD_FNC_AM_DDR200:
		{
			if (card->ddr225_card_flag)
				clock_freq = SD_CLK_225M;
			else
				clock_freq = SD_CLK_200M;

			break;
		}
	case SD_FNC_AM_SDR104:
		{
			clock_freq = SD_CLK_200M;
			break;
		}
	case SD_FNC_AM_SDR50:
		{
			clock_freq = SD_CLK_100M;
			break;
		}
	case SD_FNC_AM_SDR25:
		{
			clock_freq = SD_CLK_50M;
			break;
		}
	case SD_FNC_AM_SDR12:
		{
			clock_freq = SD_CLK_25M;
			break;
		}
	case SD_FNC_AM_DDR50:
		{
			clock_freq = SD_CLK_50M;
			break;
		}
	default:
		{
			clock_freq = SD_CLK_25M;
			break;
		}
	}

	if (card_info->sw_cur_setting.sd_access_mode == SD_FNC_AM_DDR50
	    || card_info->sw_cur_setting.sd_access_mode == SD_FNC_AM_DDR200) {
		card_legacy_change_clock(card, clock_freq, TRUE);
	} else {
		card_legacy_change_clock(card, clock_freq, FALSE);
	}

	/*
	 * if the switch is successful,set host mode to DDR200
	 * host 0x110[17]=1
	 */
	if (card_info->sw_cur_setting.sd_access_mode == SD_FNC_AM_DDR200) {
		regval = sdhci_readl(host, 0x110);
		regval |= (1 << 17);
		sdhci_writel(host, 0x110, regval);
	}

	host_set_uhs_mode(host, card_info->sw_cur_setting.sd_access_mode);

exit:
	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;

}

/*
 *
 * Function Name: sd_switch_function_set
 *
 * Abstract:
 *
 *			 1.  Set SD switch function status
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
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

bool sd_switch_function_set(sd_card_t *card, sd_command_t *sd_cmd)
{

	bool ret = FALSE;
	sd_host_t *host = card->host;
	card_info_t *card_info = &(card->info);
	bool bchg = TRUE;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* 1. Check the function card supported */

	/*
	 * 2. Set Driver Strength
	 * As default, driver do not set driver strength
	 * Only set it when configure driver_strength enabled
	 */

	if (host->cfg->card_item.test_driver_strength_sel.enable_set) {
		byte driver_strength = card->sw_target_setting.sd_drv_type;
		/* Do function switch when card support this function */
		if ((1 << driver_strength) &
		    (card_info->sw_func_cap.sd_drv_type)) {
			ret =
			    sd_switch_function_set_ds(card, sd_cmd,
						      driver_strength);
			if (!ret) {
				DbgErr("Set driver strength to %X Failed.\n",
				       driver_strength);
				goto exit;
			}
			/* Update the current settings */
			card_info->sw_cur_setting.sd_drv_type = driver_strength;
		}
	} else if ((card_info->cid.manfid == 0x1b) && sd_ddr_support(card)) {
		/* Driver strength select 3h: Type D */
		byte driver_strength = 0x3;

		PrintMsg("Samsung DDR200 card need switch DS to type D\n");

		ret = sd_switch_function_set_ds(card, sd_cmd, driver_strength);
		if (!ret) {
			DbgErr("Set driver strength to %X Failed.\n",
			       driver_strength);
			goto exit;
		}
		/* Update the current settings */
		card_info->sw_cur_setting.sd_drv_type = driver_strength;
	} else {
		/* do nothing */
	}

	/* 3. Set Power Limit. */
	/* Init case we always do pm setting */
	bchg = TRUE;
	ret = sd_switch_power_limit(card, sd_cmd, &bchg);
	if (!ret) {
		DbgErr("Set Power Limit Failed.\n");
		goto exit;
	}

	/* 4. Set Access mode */
	/* Init case we always do am setting */
	bchg = TRUE;
	ret = sd_switch_access_mode(card, sd_cmd, &bchg);
	if (!ret) {
		DbgErr("Set Access Mode Failed.\n");
		goto exit;
	}
exit:

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 *
 * Function Name: sd_tuning_hw
 *
 * Abstract:
 *
 *			 1.  Hardware Tuning Procedure (CMD19)
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
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

bool sd_tuning_hw(sd_card_t *card, sd_command_t *sd_cmd, u32 timeout)
{

	byte cmd_index = SD_CMD19;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_TUNE;
	e_data_dir dir = DATA_DIR_IN;
	byte data[64];
	u32 datalen = 64;

	bool ret = FALSE;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* 1. Set driver HW mode here */
	host_set_tuning_mode(card->host, TRUE);
	/* add 200us delay before CMD19 to fix FJ2 ASIC issue 14 */
	if (card->host->chip_type == CHIP_FUJIN2)
		os_udelay(200);

	/* 2. Tuning now */
	ret =
	    card_send_sdcmd_timeout(card, sd_cmd, cmd_index, argument, cmdflag,
				    dir, data, datalen, timeout);
	if (!ret)
		DbgErr(" - SendCommand19 failed during tuning!\n");
	else
		ret = host_chk_tuning_comp(card->host, TRUE);

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 *
 * Function Name: sd_tuning_sw
 *
 * Abstract:
 *
 *			 1.  Software Tuning Procedure (CMD19)
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
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

bool sd_tuning_sw(sd_card_t *card, sd_command_t *sd_cmd)
{
	byte cmd_index = SD_CMD19;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_TUNE;
	e_data_dir dir = DATA_DIR_IN;
	byte data[64];
	u32 datalen = 64;
	u16 i;

	bool ret = FALSE;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* Try 100 SW tuning */
	for (i = 0; i < 100; i++) {
		/* 1. Set driver SW mode here */
		host_set_tuning_mode(card->host, FALSE);
		/* add 200us delay before CMD19 to fix FJ2 ASIC issue 14 */
		if (card->host->chip_type == CHIP_FUJIN2)
			os_udelay(200);

		/* 2. Tuning now */
		ret =
		    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag,
				    dir, data, datalen);
		if (!ret) {
			DbgErr("SendCommand19 failed during tuning!\n");
			break;
		}

		ret = host_chk_tuning_comp(card->host, TRUE);
		if (ret)
			break;

	}
	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 *
 * Function Name: sd_tuning
 *
 * Abstract:
 *
 *			 1.  Tuning Procedure (CMD19)
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
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

bool sd_tuning(sd_card_t *card, sd_command_t *sd_cmd, u32 timeout)
{
	bool ret = FALSE;
	card_info_t *card_info = &(card->info);

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (card_info->sw_cur_setting.sd_access_mode < SD_FNC_AM_SDR50) {
		/* Only do tuning procedure for DDR50, SDR104, SDR50,DDR200 */
		DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"No need to do tuning for access mode %d\n",
			card_info->sw_cur_setting.sd_access_mode);
		ret = TRUE;
	} else {
		if (TUNING_MODE) {
			/* HW tuning */
			ret = sd_tuning_hw(card, sd_cmd, timeout);
		} else {
			ret = sd_tuning_sw(card, sd_cmd);
		}

	}
	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 *
 * Function Name: sd_switch_function_check
 *
 * Abstract:
 *
 *			 1.  Get SD switch function status (CMD6)
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
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

bool sd_switch_function_check(sd_card_t *card, sd_command_t *sd_cmd)
{

	byte cmd_index = SD_CMD6;
	u32 argument = SD_FNC_NOINFL;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_IN;
	byte data[64];
	u32 datalen = 64;
	bool ret = FALSE;
	card_info_t *card_info = &(card->info);

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* Issue CMD6 */
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);
	if (ret) {
		/* Set the card swich function status info */
		card_info->sw_func_cap.sd_access_mode = data[13];
		card_info->sw_func_cap.sd_command_system = data[10];
		card_info->sw_func_cap.sd_drv_type = data[9];
		card_info->sw_func_cap.sd_power_limit = data[7];
		DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Card Sup AM:%X\n",
			card_info->sw_func_cap.sd_access_mode);
		DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Card Sup CS:%X\n",
			card_info->sw_func_cap.sd_command_system);
		DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Card Sup DS:%X\n", card_info->sw_func_cap.sd_drv_type);
		DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Card Sup PL:%X\n",
			card_info->sw_func_cap.sd_power_limit);
	}
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 *
 * Function Name: sd_switch_function_set_pl
 *
 * Abstract:
 *
 *			 1.  Set SD switch function status (Power Limit) (CMD6)
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
 *           byte driver_strength: driver strength which want be set.
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
 *           Caller: sd_switch_function_set
 */

bool sd_switch_function_set_pl(sd_card_t *card,
			       sd_command_t *sd_cmd, byte power_limit)
{

	byte cmd_index = SD_CMD6;
	u32 argument = SD_FNC_SW | SD_FNC_G4_INFL;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_IN;
	byte data[64];
	u32 datalen = 64;
	bool ret = FALSE;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, power_limit=%d\n", __func__, power_limit);

	argument |= (power_limit) << 12;
	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Set Power Limit to %x, argument=%08X\n", power_limit,
		argument);

	/* Issue CMD6 */
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 *
 * Function Name: sd_switch_function_set_pl
 *
 * Abstract:
 *
 *			 1.  Set SD switch function status (Power Limit) (CMD6)
 *
 * Input:
 *
 *			sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *           sd_command_t *sd_cmd: Pointer to sd command structure
 *           byte driver_strength: driver strength which want be set.
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
 *           Caller: sd_switch_function_set
 */

bool sd_lightning_mode_sw(sd_card_t *card, sd_command_t *sd_cmd)
{

	byte cmd_index = SD_CMD6;
	u32 argument = SD_FNC_SW | SD_FNC_G4_INFL;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_IN;
	byte data[64];
	u32 datalen = 64;
	u32 card_status = 0;
	bool ret = FALSE;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* 1. Check if lightning mode is enabled or not */
	/* Host was set to do not support lightning mode */
	goto EXIT;

	if (card->info.cid.manfid != MID_SANDISK) {
		/* Card is not SanDisk Card */
		goto EXIT;
	}

	/* 2. Set card to vendor specific mode */
	{
		cmd_index = SD_CMD6;
		argument = SD_FNC_SW | SD_FNC_G2_VEN;
		cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
		dir = DATA_DIR_IN;
		datalen = 64;
		/* Issue CMD6 */
		ret =
		    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag,
				    dir, data, datalen);
		if (ret == FALSE) {
			DbgErr(("Set Vendor Specific mode failed!\n"));

			goto EXIT;
		}
	}

	/* 3. Send CMD13 */
	ret = card_get_card_status(card, sd_cmd, &card_status);
	if (ret == FALSE) {
		DbgErr(("Send Status error(CMD13)\n"));

		goto EXIT;
	}

	/* 4. Send CMD35 */
	ret = sd_send_cmd35(card);
	if (ret == FALSE) {
		DbgErr(("Send Status error(CMD13)\n"));

		goto EXIT;
	}

	/* Send CMD34 */
	ret = sd_send_cmd34(card);
	if (ret == FALSE) {
		DbgErr(("Send Status error(CMD13)\n"));

		goto EXIT;
	}

	/* Delay */
	os_mdelay(1);

EXIT:
	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 *
 * Function Name: sd_card_identify
 *
 * Abstract:
 *
 *			 1. Issue reset command (CMD0)
 *            2. Issue send IF condition command (CMD8)
 *            3. SDIO swithch function supportted?
 *            4. Wait for card ready (ACMD41)
 *            5. Signal voltage switch prucedure (CMD11)
 *            6. Get card CID(CMD2)
 *            7. Get card relative address (CMD3)
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
 *           Caller: sd_legacy_init, ush2_card_init
 */

bool sd_card_identify(sd_card_t *card)
{
	bool result = FALSE;
	bool flag_f8 = FALSE;
	sd_command_t sd_cmd;
	u32 argument = 0x000001AA;

	card->uhs2_card = FALSE;
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	os_memset(&sd_cmd, 0, sizeof(sd_command_t));

	if (card->card_type != CARD_UHS2)
		os_udelay(200);

	/* 1. Issue reset command (CMD0) */

	result = card_reset_card(card, &sd_cmd);
	if (!result) {
		/* Go Idle State command failed. exit directly. */
		DbgErr("Reset Card (CMD0) Failed.\n");
		goto exit;
	}

	/* 2. Issue send IF condition command (CMD8) */
	result = sd_send_if_cond(card, &sd_cmd, argument);
	if (!result) {

		/* 2.1 Error response */
		if (sd_cmd.err.error_code == ERR_CODE_RESP_ERR ||
		    sd_cmd.err.error_code == ERR_CODE_NO_CARD) {
			DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"CMD8 Response Error or no card.\n");
			goto exit;
		}

		/* 2.2 No Response  (Standard Capacity Card) */
		{
			DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"CMD8 No Responser.\n");

			/* 2.2.1 Set flag F8 = 0 */
			flag_f8 = FALSE;
#if (0)
			card->card_cap_type = CARD_SDSC_V1;
#endif
			/* 2.2.2 Reset card (CMD0) again. */
			result = card_reset_card(card, &sd_cmd);
			if (!result) {
				/* Go Idle State command failed. exit directly. */
				DbgErr("Reset Card Again (CMD0) Failed.\n");
				goto exit;
			}
		}
	} else {
		/* 2.3 Good Response  (High Capacity card) */
		/* 2.3.1 Set flag F8 = 1 */
		DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"CMD8 Good Responser (High Capacity Card).\n");

		flag_f8 = TRUE;
	}

	/* 3. SDIO swithch function supportted? */
#if (0)
	/* RTU_OK */
	if ((card->card_type != CARD_SD) && (card->card_type != CARD_UHS2)) {
		DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"SDIO swithch function supportted.\n");

		/* 3.1 issue CMD5 to check card is SDIO card or not */
		result = sdio_check(card, &sd_cmd);
		if (result == TRUE) {
			/* 3.1.1 set card type to SDIO_CARD */
			card->card_type = CARD_SDIO;
			DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"SDIO card.\n");
			/* 3.1.3 Return failed */
			result = FALSE;
			goto exit;
		}
	}
#endif

	/* 4. Wait for card ready (ACMD41) */
	result = card_init_ready(card, &sd_cmd, flag_f8);
	if (!result) {
		/*
		 * 4.1 Return failed for following cases:
		 * - OCR check fail,
		 * - or command timeout,
		 * - or command55 fail,
		 * - or ACMD41 response error
		 */
		DbgErr("Wait for card ready (ACMD41) Failed.\n");
		goto exit;
	}

	/* Try to init as SD Legacy card */
	if (card->card_type != CARD_UHS2)
		card->card_type = CARD_SD;

	/* 5. Signal voltage switch prucedure (CMD11) */
	if (need_switch_sig_voltage(card)) {
		DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Need to do signal voltage switch.\n");

		result = signal_voltage_switch(card, &sd_cmd);
		if (!result) {
			/*
			 * 5.1 If signal voltage switch failed,
			 * need return failed for power cycle.
			 */
			DbgErr("Signal voltage switch failed.\n");
			goto exit;
		}
	}

	/* 6. Get card CID(CMD2) */
	result = card_all_send_cid(card, &sd_cmd);
	if (!result) {
		/* 6.1 If failed, need return failed for power cycle. */
		DbgErr("Get card CID(CMD2) failed.\n");
		goto exit;
	}

	/* 7. Get card relative address (CMD3) */
	result = card_get_rca(card, &sd_cmd);
	if (!result) {
		/* 7.1 If failed, need return failed for power cycle. */
		DbgErr("Get card relative address (CMD3) failed.\n");
		goto exit;
	}

exit:
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;
}

/*
 *
 * Function Name: sd_card_select
 *
 * Abstract:
 *
 *           1. Get CID (CMD10)
 *           2. Get CSD (CMD9)
 *           3. Select the card (CMD7)
 *           4. Get Lock/Unlock status, CMD7 Response [25].
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
 *           Caller: sd_legacy_init, ush2_card_init
 */

bool sd_card_select(sd_card_t *card)
{
	bool result = FALSE;
	sd_command_t sd_cmd;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	os_memset(&sd_cmd, 0, sizeof(sd_command_t));

	if (card_need_get_info(card)) {
		/* 1.1 Get CID (CMD10) */
		result = sd_get_cid(card, &sd_cmd);
		if (!result) {
			DbgErr("Get CID (CMD10) failed.\n");
			goto exit;
		}

		/* 1.2 Get CSD (CMD9) */
		result = card_get_csd(card, &sd_cmd);
		if (!result) {
			DbgErr("Get CSD (CMD9) failed.\n");
			goto exit;
		}
	} else {
		DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Already get card info, skip getting CID,CSD.\n");
	}

	/* 2. Select the card (CMD7) */
	result = card_select_card(card, &sd_cmd);
	if (!result) {
		/* 2.1 If failed, need return failed for power cycle. */
		DbgErr("Select card (CMD7) failed.\n");
		goto exit;
	}

exit:
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;

}

bool sd_init_get_info(sd_card_t *card)
{
	bool result = TRUE;
	sd_command_t sd_cmd;
	sd_host_t *host = card->host;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	os_memset(&sd_cmd, 0, sizeof(sd_command_t));

	/* 11. Set bus width */
	/* uhs2 card don't need this flow */
	if (card->card_type == CARD_SD) {
		/* 11.1 Set card bus width (ACMD6) */
		result = sd_set_bus_width(card, &sd_cmd);
		if (!result) {
			/* 11.1 If failed, need return failed for power cycle. */
			DbgErr("Set card bus width (ACMD6) failed.\n");
			goto exit;
		}

		/* 11.2 Set Host bus width */
		host_set_buswidth(host, BUS_WIDTH4);

		/* 12. Set block length (CMD16) */
		result = card_set_block_len(card, &sd_cmd, SD_BLOCK_LEN);
		if (!result) {
			/* 12.1 If failed, need return failed for power cycle. */
			DbgErr("Set block length (CMD16) failed.\n");
			goto exit;
		}
	}

	/* 13. Get card related info, like CID,CSD, SCR, SD_Status */
	if (card_need_get_info(card)) {
		/* 13.3 Get SCR (ACMD51) */
		result = sd_get_scr(card, &sd_cmd);
		if (!result) {
			DbgErr("Get SCR (ACMD51) failed.\n");
			goto exit;
		}
		/* 13.4 Get SD Status (ACMD13) */
		result = sd_get_sdstatus(card, &sd_cmd);
		if (!result) {
			DbgErr("Get SD Status (ACMD13) failed.\n");
			goto exit;
		}
	} else {
		DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Already get card info, skip getting SCR, SD_Status.\n");
	}

exit:
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;
}

bool sd_init_stage2(sd_card_t *card)
{
	bool result = FALSE;
	sd_host_t *host = card->host;
	cfg_item_t *cfg_item = card->host->cfg;
	card_info_t *card_info = &(card->info);
	sd_command_t sd_cmd;
	u8 tuning_type = 0;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	os_memset(&sd_cmd, 0, sizeof(sd_command_t));

	if (host->chip_type != CHIP_GG8) {
		/* 1. Set SD Host Clock to 25MHz */
		card_legacy_change_clock(card, SD_CLK_25M, FALSE);
		DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Set SD Host Clock to 25MHz.\n");
	} else
		os_mdelay(15);

	result = sd_init_get_info(card);
	if (!result) {
		DbgErr("SD Card get info failed\n");
		goto exit;
	}
#if (0)
	/* Turns Off the Pull-up resistor of the SD Card */
	result = sd_clear_card_detect(card);
	if (!result) {
		DbgErr
		    ("Turns Off the Pull-up resistor of the SD Card failed\n");
		goto exit;
	}
#endif

	if (card->restore_tuning_content_fail) {
		result =
		    restore_tuning_address_content(card,
						   card->sec_count -
						   TUNING_ADDRESS_OFFSET);
		if (!result) {
			DbgErr("restore_tuning_address_content failed\n");
			card->restore_tuning_content_fail = 1;
			goto exit;
		}
	}

	/* 2. Need to clear High Speed Enable */
	host_set_highspeed(host, FALSE);

	/* 3. Swich function check/set */
	if (card_info->scr.sd_spec < SCR_SPEC_VER_1) {
		result = TRUE;
		card->sw_target_setting.sd_access_mode = SD_FNC_AM_DS;
		if (host->chip_type == CHIP_GG8) {
			/* 1. Set SD Host Clock to 25MHz */
			card_legacy_change_clock(card, SD_CLK_25M, FALSE);
			DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"Set SD Host Clock to 25MHz.\n");
		}
		goto exit;
	} else if (!(card_info->card_s18a)) {
		card->sw_target_setting.sd_access_mode =
		    os_min(card->sw_target_setting.sd_access_mode,
			   SD_FNC_AM_HS);
		if (card_need_get_info(card)) {
			/* 3.1. Check if card support Hight Speed. */
			result = sd_switch_function_check(card, &sd_cmd);
			if (!result) {
				DbgErr("Swich function check (CMD6) failed.\n");
				goto exit;
			}
		} else {
			DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"Card function check skipped.\n");
		}

		DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Card support High Speed.\n");

		if ((card->info.sw_func_cap.sd_access_mode & (1 << SD_FNC_AM_HS))
		    && (card->sw_target_setting.sd_access_mode >= SD_FNC_AM_HS)) {
			result =
			    sd_switch_function_set_am(card, &sd_cmd,
						      SD_FNC_AM_HS);
			if (!result) {
				DbgErr
				    ("Set Access Mode to High Speed Failed.\n");
				goto exit;
			}
			DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"Switch to High Speed OK.\n");
			if (host->chip_type == CHIP_GG8) {
				/* 1. Set SD Host Clock to 25MHz */
				card_legacy_change_clock(card, SD_CLK_25M,
							 FALSE);
				DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT,
					NOT_TO_RAM,
					"Set SD Host Clock to 25MHz.\n");
			}

			/* 3.2. Update the current settings */
			card_info->sw_cur_setting.sd_access_mode = SD_FNC_AM_HS;
			/* 3.3. Need to set High Speed Enable */
			host_set_highspeed(host, TRUE);

			/* 4. Check Lighting card support */
			result = sd_lightning_mode_sw(card, &sd_cmd);
			if (result == TRUE) {
				DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT,
					NOT_TO_RAM,
					"Card support Lighting mode, change clock to 75MHz.\n");
				/* 4.1. Change the clock to 75MHz */
				card_legacy_change_clock(card, SD_CLK_75M,
							 FALSE);
			} else {
				DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT,
					NOT_TO_RAM, "Change clock to 50MHz.\n");
				/* 4.2. Change the clock to 50MHz */
				card_legacy_change_clock(card, SD_CLK_50M,
							 FALSE);
			}
			result = TRUE;
		} else {
			/*
			 * Degrade access mode to Default Speed case.
			 * Need to switch access mode to Default Speed
			 * as card default AM is High Speed.
			 */
			result =
			    sd_switch_function_set_am(card, &sd_cmd,
						      SD_FNC_AM_DS);
			if (!result) {
				DbgErr
				    ("Set Access Mode to Default Speed Failed.\n");
				goto exit;
			}
			DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"Switch to Default Speed OK.\n");

		}

	} else {

		if (card_need_get_info(card)) {

			/* 5.1 Swich function check first to get card function capabilities */
			result = sd_switch_function_check(card, &sd_cmd);
			if (!result) {
				DbgErr("Swich function check (CMD6) failed.\n");
				goto exit;
			}
		} else {
			DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"Card function check skipped.\n");
		}

		if ((card_get_max_am_cap(card) >= SD_FNC_AM_SDR50)
		    && (cfg_item->card_item.test_max_access_mode.value >= 0x2))
			result =
			    store_tuning_address_content(card,
							 card->sec_count -
							 TUNING_ADDRESS_OFFSET);
		else
			/* SD2.0 card shall not store tuning  */
			result = FALSE;

		if (!result) {
			DbgErr("store_tuning_address_contento failed\n");
			/* goto exit; */
		}

		/*
		 * 5.2 Swich function check set.
		 * - Driver Strength,
		 * - Access Mode,
		 * - Power Limit
		 * - Change clock freq
		 */

		result = sd_switch_function_set(card, &sd_cmd);
		if (!result) {
			DbgErr("Swich function set (CMD6) failed.\n");
			goto exit;
		}

		tuning_type =
		    hostven_tuning_type_selection(host,
						  card_info->sw_cur_setting.sd_access_mode);

		/* 5.3 Tuning Procedure (for DDR200, SDR104 and SDR50 Only) */
		if (card->info.sw_cur_setting.sd_access_mode == SD_FNC_AM_SDR104
		    || card->info.sw_cur_setting.sd_access_mode ==
		    SD_FNC_AM_SDR50
		    || card->info.sw_cur_setting.sd_access_mode ==
		    SD_FNC_AM_DDR200) {
			switch (tuning_type) {
			case 0:
				hostven_fix_output_tuning(host,
							  card_info->sw_cur_setting.sd_access_mode);
				break;
			case 1:
				hostven_fix_output_tuning(host,
							  card_info->sw_cur_setting.sd_access_mode);
				result = sd_tuning(card, &sd_cmd, 0);
				if (!result) {
					DbgErr("Tuning (CMD19) failed.\n");
					goto exit;
				}
				break;
			case 2:
				result =
				    card_output_tuning(card,
						       card->sec_count -
						       TUNING_ADDRESS_OFFSET);
				if (!result) {
					DbgErr("card_output_tuning failed.\n");
					card->restore_tuning_content_fail = 1;
					goto exit;
				}

				if (card->read_signal_block_flag) {
					result =
					    restore_tuning_address_content(card,
									   card->sec_count
									   -
									   TUNING_ADDRESS_OFFSET);
					if (!result) {
						DbgErr
						    ("restore_tuning_address_content failed\n");
						card->restore_tuning_content_fail
						    = 1;
						goto exit;
					}
				} else {
					erase_rw_blk_start_set(card, &sd_cmd,
							       (u32)
							       (card->sec_count)
							       -
							       TUNING_ADDRESS_OFFSET);
					erase_rw_blk_end_set(card, &sd_cmd,
							     ((u32)
							      (card->sec_count)
							      -
							      TUNING_ADDRESS_OFFSET)
							     + 1);
					func_erase(card, &sd_cmd);
				}

				break;
			default:
				break;
			}

			DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"Tuning Procedure Done. Access Mode=%d.\n",
				card_info->sw_cur_setting.sd_access_mode);
		}

	}
exit:
	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;
}

/*
 *
 * Function Name: sd_legacy_init
 *
 * Abstract:
 *
 *			 1. sd legacy card (uhs1, legacy) initialize main function.
 *            2. Fill virtual card structure, like cid, csd, etc.
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

bool sd_legacy_init(sd_card_t *card)
{
	bool result = FALSE;
	sd_host_t *host = card->host;

	card->uhs2_card = FALSE;
	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (shift_bit_func_enable(host))
		set_pattern_value(host, 0x10);

	host_sd_init(host);

	/* SD Card Identification */
	result = sd_card_identify(card);
	if (!result) {
		DbgErr("SD Card Identification Stage failed.\n");
		goto error_exit;
	}

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Card Identification Stage OK.\n");

	/* SD Card Select and lock/unlock check */
	result = sd_card_select(card);
	if (!result) {
		DbgErr("SD Card Select failed.\n");
		goto error_exit;
	}

	if (card->locked == TRUE) {
		DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"Card is Locked.\n");
		goto next;
	}

	result = card_init_stage2(card);
	if (!result) {
		DbgErr("SD init stage 2 failed.\n");
		goto error_exit;
	}
	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Card Init Stage 2 OK.\n");

next:

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(OK) %s\n",
		__func__);
	return result;

error_exit:
	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Exit(FAIL) %s\n", __func__);
	return result;
}

static byte sd_get_lower_am(sd_card_t *card, byte access_mode)
{
	cfg_item_t *cfg = card->host->cfg;
	byte lower_am = 0;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT | FEATURE_ERROR_RECOVER,
		NOT_TO_RAM, "Enter %s, access_mode=%d\n", __func__,
		access_mode);

	/* Degrade access mode according to current access mode. */
	switch (access_mode) {
	case SD_FNC_AM_DDR200:
		lower_am =
		    card_get_min_am((byte) cfg->card_item.test_max_access_mode.value,
				    (byte) card_get_max_am_cap(card));
		break;
	case SD_FNC_AM_SDR104:
		/* Degrade to SDR50 directly */
		lower_am = SD_FNC_AM_SDR50;
		break;
	case SD_FNC_AM_SDR50:
		if ((cfg->card_item.test_max_access_mode.value == SD_FNC_AM_DDR50)
		    && (card->info.sw_func_cap.sd_access_mode &
			(1 << SD_FNC_AM_DDR50))) {
			/*
			 * Max Access mode is DDR50 and card support DD50,
			 * then can be degrade to DDR50
			 */
			lower_am = SD_FNC_AM_DDR50;
		} else {
			/* Degrade to High Speed */
			lower_am = SD_FNC_AM_HS;
		}
		break;
	case SD_FNC_AM_DDR50:
		/* Degrade to High Speed directly */
		lower_am = SD_FNC_AM_HS;
		break;
	case SD_FNC_AM_HS:
		/* Degrade to Default Speed directly */
		lower_am = SD_FNC_AM_DS;
		break;
	default:
		lower_am = SD_FNC_AM_DS;
		break;
	}
	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT | FEATURE_ERROR_RECOVER,
		NOT_TO_RAM, "Exit(%d) %s\n", lower_am, __func__);
	return lower_am;
}

static byte sd_get_higher_am(sd_card_t *card, byte access_mode)
{
	cfg_item_t *cfg = card->host->cfg;
	byte higher_am = 0;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT | FEATURE_ERROR_RECOVER,
		NOT_TO_RAM, "Enter %s, access_mode=%d\n", __func__,
		access_mode);

	/* get higher access mode. */
	switch (access_mode) {
	case SD_FNC_AM_DDR200:
		higher_am = SD_FNC_AM_DDR200;
		break;
	case SD_FNC_AM_SDR104:
		/* Keep SDR104 */
		if (sd_ddr_support(card)
		    && (cfg->card_item.test_max_access_mode.value ==
			SD_FNC_AM_SDR104))
			higher_am = SD_FNC_AM_DDR200;
		else
			higher_am = SD_FNC_AM_SDR104;

		break;
	case SD_FNC_AM_SDR50:
		if ((cfg->card_item.test_max_access_mode.value ==
		     SD_FNC_AM_SDR104)
		    && card->info.sw_func_cap.sd_access_mode &
			(1 << SD_FNC_AM_SDR104)) {
			/* SDR104 */
			higher_am = SD_FNC_AM_SDR104;
		} else {
			/* SDR50 */
			higher_am = SD_FNC_AM_SDR50;
		}
		break;
	case SD_FNC_AM_DDR50:
		/* Degrade to High Speed directly */
		if ((cfg->card_item.test_max_access_mode.value ==
		     SD_FNC_AM_SDR50)
		    && card->info.sw_func_cap.sd_access_mode &
			(1 << SD_FNC_AM_SDR50)) {
			/* SDR50 is higher level access mode of DDR50 */
			higher_am = SD_FNC_AM_SDR50;
		} else {
			/* No change */
			higher_am = SD_FNC_AM_DDR50;
		}
		break;
	case SD_FNC_AM_HS:
		/* Degrade to Default Speed directly */
		if ((cfg->card_item.test_max_access_mode.value ==
		     SD_FNC_AM_DDR50)
		    && card->info.sw_func_cap.sd_access_mode &
			(1 << SD_FNC_AM_DDR50)) {
			/* DDR50 supported, then it is higher level access mode of High Speed. */
			higher_am = SD_FNC_AM_DDR50;
		} else if ((cfg->card_item.test_max_access_mode.value ==
			 SD_FNC_AM_SDR50)
			&& card->info.sw_func_cap.sd_access_mode &
			(1 << SD_FNC_AM_SDR50)) {
			/*
			 * DDR50 do not supported,
			 * then SDR50 is the higher level access mode of High Speed.
			 */
			higher_am = SD_FNC_AM_SDR50;
		} else {
			/* No change */
			higher_am = SD_FNC_AM_HS;
		}
		break;
	case SD_FNC_AM_DS:
		if ((cfg->card_item.test_max_access_mode.value == SD_FNC_AM_HS)
		    && card->info.sw_func_cap.sd_access_mode &
			(1 << SD_FNC_AM_HS)) {
			/* High Speed is the higher level access mode of Default Speed. */
			higher_am = SD_FNC_AM_HS;
		} else {
			/* No change */
			higher_am = SD_FNC_AM_DS;
		}
		break;
	default:
		break;
	}

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT | FEATURE_ERROR_RECOVER,
		NOT_TO_RAM, "Exit(%d) %s\n", higher_am, __func__);
	return higher_am;
}

/*
 * Function Name: sd_degrade_policy
 *
 * Abstract: This Function is used set sd degrade flag
 *
 * Input:
 * sd_card_t *card : The Command will send to which  Card

 * Return value:
 */
void sd_degrade_policy(sd_card_t *card)
{
	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (card->info.sw_cur_setting.sd_access_mode == SD_FNC_AM_SDR50 ||
	    card->info.sw_cur_setting.sd_access_mode == SD_FNC_AM_SDR104 ||
	    card->info.sw_cur_setting.sd_access_mode == SD_FNC_AM_DDR200) {
		/* If Access Mode >= SDR50, then try to degrade freq first */
		if (card->degrade_freq_level < CARD_DEGRADE_FREQ_TIMES) {
			/* Degrade freq less than 3 times, continue to degrade freq */
			card->degrade_freq_level++;
		} else {
			/* As degrade mode is needed, clear the degrade freq level. */
			card->degrade_freq_level = 0;

			/* Degrade freq larger than 3 times, then degrade accessmode */
			card->sw_target_setting.sd_access_mode =
			    sd_get_lower_am(card,
					    card->sw_target_setting.sd_access_mode);
			/* If Degrade to Default Speed already. Mark as degrade final */
			if (card->sw_target_setting.sd_access_mode ==
			    SD_FNC_AM_DS) {
				card->degrade_final = 1;
			}
		}
	} else {
		/* As degrade mode is needed, clear the degrade freq level. */
		card->degrade_freq_level = 0;

		/* If Access Mode < SDR50, then degrade access mode directly */
		card->sw_target_setting.sd_access_mode =
		    sd_get_lower_am(card,
				    card->sw_target_setting.sd_access_mode);
		/* If Degrade to Default Speed already. Mark as degrade final */
		if (card->sw_target_setting.sd_access_mode == SD_FNC_AM_DS)
			card->degrade_final = 1;

	}

	DbgErr("Legacy SD degrade target=%d freq_level=%d final=%d\n",
	       card->sw_target_setting.sd_access_mode, card->degrade_freq_level,
	       card->degrade_final);
	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/*
 * Function Name: uhs2_thermal_control
 * Abstract: This Function is used to do card thremal control, only for SD and UHS2 card
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

bool sd_thermal_control(sd_card_t *card)
{
	bool bheat = (bool)card->thermal_heat;
	sd_command_t sd_cmd;
	bool result = TRUE;
	bool change_am = FALSE;
	byte am = 0;
	bool bchg = FALSE;

	DbgInfo(MODULE_SD_CARD, FEATURE_FUNC_THERMAL, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* 2.0 Card don't do thermal control */
	if (card->info.card_s18a == 0
	    || card->host->cfg->card_item.sd_card_mode_dis.dis_sd30_card)
		goto exit;

	if (bheat) {
		am = sd_get_higher_am(card,
				      card->info.sw_cur_setting.sd_access_mode);
	} else {
		am = sd_get_lower_am(card,
				     card->info.sw_cur_setting.sd_access_mode);
	}

	/* If one access mode need tuning and another don't need we can't change */
	if (am == card->info.sw_cur_setting.sd_access_mode) {
		change_am = FALSE;
	} else if (am == SD_FNC_AM_SDR50 || am == SD_FNC_AM_SDR104
		   || am == SD_FNC_AM_DDR200) {
		if (card->info.sw_cur_setting.sd_access_mode == SD_FNC_AM_SDR50
		    || card->info.sw_cur_setting.sd_access_mode ==
		    SD_FNC_AM_DDR200
		    || card->info.sw_cur_setting.sd_access_mode ==
		    SD_FNC_AM_SDR104)
			change_am = TRUE;
		else if (am != SD_FNC_AM_SDR50 && am != SD_FNC_AM_SDR104
			 && am != SD_FNC_AM_DDR200)
			if (card->info.sw_cur_setting.sd_access_mode !=
			    SD_FNC_AM_SDR50
			    && card->info.sw_cur_setting.sd_access_mode !=
			    SD_FNC_AM_SDR104
			    && card->info.sw_cur_setting.sd_access_mode !=
			    SD_FNC_AM_DDR200)
				change_am = TRUE;
	}
/* next: */
	if (change_am) {
		card->thermal_access_mode = am;
		DbgInfo(MODULE_SD_CARD, FEATURE_FUNC_THERMAL, NOT_TO_RAM,
			"thermal switch  am = %d\n", am);
		result = card_stop_infinite(card, TRUE, NULL);
		if (result == FALSE) {
			DbgErr("uhs2 Thermal Stop Infinite failed1\n");
			goto exit;
		}

		result = sd_switch_access_mode(card, &sd_cmd, &bchg);
		if (result == FALSE)
			goto exit;

		if (card->info.sw_cur_setting.sd_access_mode == SD_FNC_AM_SDR104
		    || card->info.sw_cur_setting.sd_access_mode ==
		    SD_FNC_AM_SDR50
		    || card->info.sw_cur_setting.sd_access_mode ==
		    SD_FNC_AM_DDR200) {

			if (bchg)
				result = sd_tuning(card, &sd_cmd, 0);
			if (!result) {
				DbgErr("Tuning failed for thermal control.\n");
				goto exit;
			}
		}
	}

	if (bchg == FALSE) {
		DbgInfo(MODULE_SD_CARD, FEATURE_FUNC_THERMAL, NOT_TO_RAM,
			"thermal switch pm heatup = %d\n", card->thermal_heat);
		result = card_stop_infinite(card, TRUE, NULL);
		if (result == FALSE) {
			DbgErr("uhs2 Thermal Stop Infinite failed1\n");
			goto exit;
		}
		result = sd_switch_power_limit(card, &sd_cmd, &bchg);
	}

	if (result == TRUE && bchg)
		host_cmddat_line_reset(card->host);

exit:
	DbgInfo(MODULE_SD_CARD, FEATURE_FUNC_THERMAL, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return result;

}

static u8 sd_adjust_tuning(sd_card_t *card, u32 input_n1, u32 output_n1)
{
	u8 result = TRUE;
	sd_command_t sd_cmd;
	sd_host_t *host = card->host;

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Enter %s\n",
		__func__);

	hostven_set_tuning_phase(host, input_n1, output_n1, FALSE);

	if (card->info.sw_cur_setting.sd_access_mode == SD_FNC_AM_SDR104 ||
	    card->info.sw_cur_setting.sd_access_mode == SD_FNC_AM_SDR50 ||
	    card->info.sw_cur_setting.sd_access_mode == SD_FNC_AM_DDR200) {

		result = sd_tuning(card, &sd_cmd, 150);
		if (result == FALSE) {
			DbgErr("sd adjust tuning: sd_tuning fail\n");
			result = FALSE;
			goto exit;
		}
	}

exit:

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return result;
}

static void sd_calc_max_passrange(u8 *pdata, u32 *ret, u32 *sum)
{
	u32 window_pass_number[22], window_start_adr[22],
	    window_pass_number_max;
	int ii, jj, first_0, dll_i_mod, dll_i, dll_mod;

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Enter %s\n",
		__func__);

	for (ii = 0; ii < 22; ii++) {
		window_pass_number[ii] = 0;
		window_start_adr[ii] = 0;
	}

	first_0 = 0;
	window_pass_number_max = 0;
	for (dll_i = 0; dll_i < 22; dll_i++) {
		if (pdata[dll_i] == 0) {
			first_0 = dll_i;
			break;
		}
	}
	jj = 0;
	for (dll_i = 0; dll_i < 22; dll_i++) {
		dll_i_mod = (first_0 + dll_i) % 22;
		DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			"DLL phase [%x] result %d.\n", dll_i_mod,
			pdata[dll_i_mod]);
		if (pdata[dll_i_mod] != 0)
			window_pass_number[jj]++;
		else {
			if (window_pass_number[jj] > 0)
				jj++;

		}
		if ((window_pass_number[jj] == 1) && (jj > 0)) {
			DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER,
				NOT_TO_RAM, "Error! there are %d DLL window\n",
				(jj + 1));
		}
		if (window_pass_number[jj] == 1)
			window_start_adr[jj] = dll_i_mod;
	}

	for (ii = 0; ii < 22; ii++) {
		if (window_pass_number_max < window_pass_number[ii]) {
			window_pass_number_max = window_pass_number[ii];
			jj = ii;
		}
	}
	if (window_pass_number_max == 0)
		DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			"DLL test result: All DLL test FAIL\n");
	else {
		DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			"DLL test result: Total  %d DLL test PASS\n",
			window_pass_number_max);
		window_pass_number_max = window_pass_number_max >> 1;
		dll_mod = window_start_adr[jj] + window_pass_number_max;
		dll_mod = dll_mod % 22;

		DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			"select DLL phase Number %d\n", dll_mod);
	}
	*ret = dll_mod;
	*sum = window_pass_number_max;
	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/* The caller check */
u8 testbuf_write[512];
u8 testbuf_read[512];

bool sd_dll_divider(sd_card_t *card, sd_command_t *pcmd)
{

	u32 ii, jj, pattern_i;
	bool ret = FALSE, result = FALSE, datcmp;

	u32 window_pass_sum, dll_i, input_n1, output_n1, input_n, output_n,
	    DLL_input_Phase = 0, DLL_output_Phase = 0;
	sd_command_t sd_cmd;
	byte test_patern[6] = { 0x55, 0xaa, 0x00, 0xff, 0xf0, 0x0f };
	u32 cmdflag;
	sd_host_t *host = card->host;
	u8 phasecheck[22][22], phasepass[22];

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Enter %s\n",
		__func__);

	host->output_tuning.start_block = pcmd->argument;

	/* If use read write, Save Current DMA mode */
	host_transfer_init(host, FALSE, TRUE);
	cmdflag = CMD_FLG_RESCHK | CMD_FLG_R1 | CMD_FLG_ADMA_SDMA;
	jj = 0;
	host_cmddat_line_reset(host);
	for (dll_i = 0; dll_i < 512; dll_i++)
		testbuf_write[dll_i] = test_patern[dll_i % 6];
	if (card_check_rw_ready(card, &sd_cmd, 600) != TRUE) {
		DbgErr
		    ("Error when sd dll divider,  card_check_rw_ready fail\n");
		result = FALSE;
		goto exit;
	}

	if (hostven_dll_input_tuning_init(host) == FALSE) {
		DbgErr
		    ("Error when sd dll divider,  hostven_dll_input_tuning_init  fail\n");
		result = FALSE;
		goto exit;
	}

	for (output_n1 = 0; output_n1 < 22; output_n1++)
		for (input_n1 = 0; input_n1 < 22; input_n1++) {

			DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER,
				NOT_TO_RAM,
				" - DLL input tuning  Test %d , %d\n", input_n1,
				output_n1);
			phasecheck[output_n1][input_n1] = 0;
			phasepass[output_n1] = 0;
			host_cmddat_line_reset(host);

			if (sd_adjust_tuning(card, input_n1, output_n1) ==
			    FALSE) {
				DbgErr
				    (" -adjust Output Tuning phase FAILED!!!\n");

				continue;
			}
			for (pattern_i = 0; pattern_i < 1; pattern_i++) {
				for (ii = 0; ii < (1 * 512); ii++)
					(*(testbuf_read + ii)) = 0x96;

				ret =
				    card_send_sdcmd_timeout(card, &sd_cmd,
							    SD_CMD24,
							    host->output_tuning.start_block,
							    (cmdflag),
							    DATA_DIR_OUT,
							    testbuf_write, 512,
							    50);
				if (ret == FALSE)
					break;
				ret =
				    card_send_sdcmd_timeout(card, &sd_cmd,
							    SD_CMD17,
							    host->output_tuning.start_block,
							    (cmdflag),
							    DATA_DIR_IN,
							    testbuf_read, 512,
							    50);
				if (ret == FALSE) {
					DbgErr
					    ("Read data FAILED when output_tuning\n");

					break;
				}

				datcmp = TRUE;
				for (ii = 0; ii < (1 * 512); ii++) {
					if (*(testbuf_write + ii) !=
					    *(testbuf_read + ii)) {
						datcmp = FALSE;
						phasecheck[output_n1][input_n1]
						    = 0;
						break;
					}
				}
				if (datcmp == FALSE) {
					DbgInfo(MODULE_SD_CARD,
						FEATURE_ERROR_RECOVER,
						NOT_TO_RAM,
						"Compare data FAILED at index %d!!!\n",
						ii);
				} else {
					DbgInfo(MODULE_SD_CARD,
						FEATURE_ERROR_RECOVER,
						NOT_TO_RAM,
						"Compare data OK.\n");
					phasecheck[output_n1][input_n1] = 1;
				}
			}
		}

	for (output_n = 0; output_n < 22; output_n++) {
		DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			" ## The output tuning %d:  ", output_n);
		for (input_n = 0; input_n < 22; input_n++)
			DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER,
				NOT_TO_RAM, "  %d:   %d   ", input_n,
				phasecheck[output_n][input_n]);
	}

	for (output_n = 0; output_n < 22; output_n++) {
		for (input_n = 0; input_n < 22; input_n++)
			phasepass[output_n] += phasecheck[output_n][input_n];
	}

	/* check for the max pass range */
	sd_calc_max_passrange(phasepass, &DLL_output_Phase, &window_pass_sum);
	sd_calc_max_passrange(phasecheck[DLL_output_Phase], &DLL_input_Phase,
			      &window_pass_sum);
	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"The best input tuning phase is %d\n", DLL_input_Phase);
	/* Get the optimized clock phase to do read/write test */
	hostven_set_tuning_phase(host, DLL_input_Phase, DLL_output_Phase,
				 FALSE);
	result = TRUE;

exit:
	/* Resorte current DMA mode */
	host_transfer_init(host, card->inf_trans_enable, FALSE);
	if (result == FALSE)
		hostven_set_tuning_phase(host, 0, 0, TRUE);
	host_cmddat_line_reset(host);
	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return result;
}

/*
 *
 * Function Name: sd_read_csd
 *
 * Abstract:
 *
 *			 1.  De-select the card and send CMD9, and then select the card.
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
 *			Caller:
 */
bool sd_read_csd(sd_card_t *card, sd_command_t *sd_cmd, byte *data)
{
	card_info_t *card_info = &(card->info);
	bool ret = FALSE, ret1 = FALSE, ret2 = FALSE;

	ret = card_deselect_card(card, sd_cmd);
	if (!ret)
		goto exit_select_card;

	ret1 = card_get_csd(card, sd_cmd);

exit_select_card:
	ret2 = card_select_card(card, sd_cmd);

	if ((ret == FALSE) || (ret1 == FALSE) || (ret2 == FALSE))
		return FALSE;
	else {
		os_memcpy(data, &(card_info->raw_csd[0]), 0x10);
		return TRUE;
	}
}

/*
 *
 * Function Name: sd_program_csd
 *
 * Abstract:
 *
 *			 1.  Program CSD by CMD27.
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
 *			Caller:
 */
bool sd_program_csd(sd_card_t *card, sd_command_t *sd_cmd, byte *data)
{

	byte cmd_index = SD_CMD27;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_OUT;
	u32 datalen = 0x10;
	sd_host_t *host = card->host;
	bool ret = FALSE;

	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);
	if (ret == FALSE)
		DbgErr("CMD27 failed\n");

	host_cmddat_line_reset(host);

	DbgInfo(MODULE_ALL_CARD, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, ret);
	return ret;

}
