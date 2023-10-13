// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: cardcommon.c
 *
 * Abstract: define card related common functions
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
#include "../include/hostapi.h"
#include "../include/cmdhandler.h"
#include "../include/debug.h"
#include "../include/util.h"
#include "../include/tqapi.h"
#include "../include/transhapi.h"
#include "cardcommon.h"

bool card_need_get_info(sd_card_t *card)
{
	if ((card->quick_init) && (card->initialized_once))
		return FALSE;
	else
		return TRUE;
}

bool card_send_command12(sd_card_t *card, sd_command_t *sd_cmd)
{
	bool ret = FALSE;
	u32 status = 0;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_OPS, NOT_TO_RAM, "Enter %s\n",
		__func__);

	ret = card_send_sdcmd(card, sd_cmd, SD_CMD12, 0,
			      CMD_FLG_R1B | CMD_FLG_RESCHK, DATA_DIR_NONE, NULL,
			      0);

	if (ret == FALSE) {
		if ((sd_cmd->err.resp_err & RESP_ERR_TYPE_OUT_OF_RANGE) ==
		    RESP_ERR_TYPE_OUT_OF_RANGE) {
			ret = card_get_card_status(card, sd_cmd, &status);
		}
	}

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_OPS, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

bool card_send_sdcmd_timeout(sd_card_t *card,
			     sd_command_t *sd_cmd,
			     byte cmd_index,
			     u32 argument,
			     u32 cmdflag,
			     e_data_dir dir,
			     byte *data, u32 datalen, u32 timeout)
{

	sd_data_t sd_data;
	bool ret;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARDCMD_TRACE, NOT_TO_RAM,
		"Enter %s cmd=0x%02X arg=0x%08X\n", __func__, cmd_index,
		argument);

	/* Avoid  recursion call */
	if (card->has_built_inf && cmd_index != SD_CMD12) {
		ret = card_send_command12(card, sd_cmd);
		if (ret == FALSE)
			goto exit;
	}

	os_memset(sd_cmd, 0, sizeof(sd_command_t));

	sd_cmd->cmd_flag = cmdflag;
	sd_cmd->cmd_index = cmd_index;
	sd_cmd->argument = argument;
	sd_cmd->sd_cmd = 1;
	sd_cmd->timeout = timeout;

	if (dir == DATA_DIR_NONE)
		sd_cmd->data = NULL;
	else {
		os_memset(&sd_data, 0, sizeof(sd_data_t));
		sd_cmd->data = &sd_data;
		sd_data.dir = dir;
		sd_data.data_mng.driver_buff = data;
		sd_data.data_mng.total_bytess = datalen;
		if (cmdflag & CMD_FLG_ADMA_SDMA) {
			ret =
			    build_dma_ctx(card->host->pdx, &sd_data, cmdflag,
					  dir, data, datalen, 0, 0);
			if (ret == FALSE) {
				DbgErr("build adma io error\n");
				ret = FALSE;
				goto exit;
			}
		}

		if (sd_cmd->cmd_flag & CMD_FLG_DDR200_WORK_AROUND)
			sd_cmd->gg8_ddr200_workaround = 1;
	}

	ret = cmd_generate_reg(card, sd_cmd);
	if (ret == FALSE)
		goto exit;

	ret = cmd_execute_sync(card, sd_cmd, NULL);

exit:
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARDCMD_TRACE, NOT_TO_RAM,
		"Exit(%d) %s\n", ret, __func__);
	return ret;

}

bool card_send_sdcmd_dma_timeout(sd_card_t *card,
				 sd_command_t *sd_cmd,
				 sd_data_t *sd_data,
				 byte cmd_index,
				 u32 argument,
				 u32 cmdflag,
				 e_data_dir dir,
				 byte *data, u32 datalen, u32 timeout)
{
	bool ret = FALSE;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARDCMD_TRACE, NOT_TO_RAM,
		"Enter %s cmd=0x%02X arg=0x%08X\n", __func__, cmd_index,
		argument);

	/* Avoid recursion call */
	if (card->has_built_inf && cmd_index != SD_CMD12) {
		ret = card_send_command12(card, sd_cmd);
		if (ret == FALSE)
			goto exit;
	}

	os_memset(sd_cmd, 0, sizeof(sd_command_t));

	sd_cmd->cmd_flag = cmdflag;
	sd_cmd->cmd_index = cmd_index;
	sd_cmd->argument = argument;
	sd_cmd->sd_cmd = 1;
	sd_cmd->timeout = timeout;

	if (dir == DATA_DIR_NONE)
		sd_cmd->data = NULL;
	else {
		sd_cmd->data = sd_data;
		sd_data->dir = dir;
		/* sd_data->data_mng.driver_buff = data; */
		sd_data->data_mng.total_bytess = datalen;
	}

	ret = cmd_generate_reg(card, sd_cmd);
	if (ret == FALSE)
		goto exit;

	ret = cmd_execute_sync(card, sd_cmd, NULL);

exit:
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARDCMD_TRACE, NOT_TO_RAM,
		"Exit(%d) %s\n", ret, __func__);
	return ret;

}

/*
 * Function Name: card_send_sdcmd
 *
 * Abstract: Issue command
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 * sd_command_t *sd_cmd: Pointer to the sd command structure. the caller need to check it's status.
 * byte cmd_index: Command Index
 * u32 argument: Command argument
 * u32 cmdflag: Command flags, like response tpye, DMA or PIO
 * e_data_dir dir: data direction: NONE/IN/OUT
 * byte *data: Pointer to the data buffer for data command
 * u32 datalen: Data length for transfer.
 *
 * Output: None
 *
 * Return value: Return TRUE if command successfully, else return FALSE.
 *
 * Notes:
 *
 * Caller:
 *
 */

bool card_send_sdcmd(sd_card_t *card,
		     sd_command_t *sd_cmd,
		     byte cmd_index,
		     u32 argument,
		     u32 cmdflag, e_data_dir dir, byte *data, u32 datalen)
{
	return card_send_sdcmd_timeout(card, sd_cmd, cmd_index, argument,
				       cmdflag, dir, data, datalen, 0);
}

bool card_wr_protect(sd_card_t *card)
{
	bool ret = FALSE;
	card_info_t *card_info = &(card->info);

	if (card_info->csd.temp_protect || card_info->csd.parm_protect)
		ret = TRUE;

	return ret;
}

bool card_reset_card(sd_card_t *card, sd_command_t *sd_cmd)
{
	byte cmd_index = (byte) (SD_CMD0);
	u32 argument = 0;
	u32 cmdflag = 0;
	e_data_dir dir = DATA_DIR_NONE;
	byte *data = NULL;
	u32 datalen = 0;
	bool ret = FALSE;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

bool card_all_send_cid(sd_card_t *card, sd_command_t *sd_cmd)
{

	byte cmd_index = SD_CMD2;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R2;
	e_data_dir dir = DATA_DIR_NONE;
	byte *data = NULL;
	u32 datalen = 0;
	card_info_t *card_info = &(card->info);
	bool ret = FALSE;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* Issue CMD2 */
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);
	if (ret) {
		os_memcpy(&(card_info->raw_cid[0]), &(sd_cmd->response[0]), 16);
		card_info->cid.manfid = card_info->raw_cid[0];
		card_info->cid.oemid =
		    card_info->raw_cid[1] | (card_info->raw_cid[2] << 8);
		os_memcpy(card_info->cid.prod_name, &(card_info->raw_cid[3]), 5);
		card_info->cid.prv = card_info->raw_cid[8];
		card_info->cid.serial =
		    card_info->raw_cid[9] |
			(card_info->raw_cid[10] << 8) |
		    (card_info->raw_cid[11] << 16) |
			(card_info->raw_cid[12] << 24);
		card_info->cid.reserved = card_info->raw_cid[13] >> 4;
	}
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 * Function Name: card_get_rca
 *
 * Abstract: Ask the card to publish a new relative address RCA (CMD3)
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 * sd_command_t *sd_cmd: Pointer to sd command structure
 *
 * Output: None
 *
 * Return value: Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *
 * Caller: sd_card_identify
 *
 */

bool card_get_rca(sd_card_t *card, sd_command_t *sd_cmd)
{

	byte cmd_index = SD_CMD3;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R6 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_NONE;
	byte *data = NULL;
	u32 datalen = 0;

	bool ret = FALSE;

	card_info_t *card_info = &(card->info);

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* Issue CMD3 */
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);
	if (ret) {
		/* Update the card RCA */
		card_info->rca = (sd_cmd->response[0] & 0xFFFF0000) >> 16;
	}
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 * Function Name: card_select_card
 *
 * Abstract: Select card (CMD7)
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 * sd_command_t *sd_cmd: Pointer to sd command structure
 *
 * Output: None
 *
 * Return value: Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *
 * Caller: sd_card_select
 *
 */

bool card_select_card(sd_card_t *card, sd_command_t *sd_cmd)
{

	byte cmd_index = SD_CMD7;
	u32 argument = 0;
	u32 cmdflag = 0;
	e_data_dir dir = DATA_DIR_NONE;
	byte *data = NULL;
	u32 datalen = 0;

	bool ret = FALSE;
	card_info_t *card_info = &(card->info);

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (card->card_type == CARD_UHS2)
		cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	else
		cmdflag = CMD_FLG_R1B | CMD_FLG_RESCHK;
	argument = (card_info->rca) << 16;

	/* Issue CMD7 */
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);
	if (ret == TRUE) {
		/* Get Lock/Unlock status, CMD7 Response [25].
		 * Check bit 25 of CMD7 response.
		 */
		if (sd_cmd->response[0] & BIT25)
			card->locked = TRUE;
		else
			card->locked = FALSE;
	}
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Exit(%d) %s locked=%d\n", ret, __func__, card->locked);
	return ret;
}

/*
 * Function Name: card_deselect_card
 *
 * Abstract: De-Select card (CMD7)
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 * sd_command_t *sd_cmd: Pointer to sd command structure
 *
 * Output: None
 *
 * Return value: Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *
 * Caller: sd_read_csd
 *
 */

bool card_deselect_card(sd_card_t *card, sd_command_t *sd_cmd)
{

	byte cmd_index = SD_CMD7;
	u32 argument = 0;
	u32 cmdflag = 0;
	e_data_dir dir = DATA_DIR_NONE;
	byte *data = NULL;
	u32 datalen = 0;

	bool ret = FALSE;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	cmdflag = 0;
	argument = 0;

	/* Issue CMD7 */
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Exit(%d) %s locked=%d\n", ret, __func__, card->locked);
	return ret;
}

/*
 * Function Name: card_set_csd_info
 *
 * Abstract: Acquired CSD Data, to be stored into Struct of CSD and Card.
 *			Save some contents of CSD Register into Struct of CSD,
 *			and generate(calcurate) necessary Data to save into Struct of Card.
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 * unsigned char  *csdbuff: CSD Buffer Pointer
 * csd_t *csd_info: CSD information Pointer
 *
 * Output: None
 *
 * Return value: None
 *
 * Notes:
 *
 * Caller: card_get_csd
 *
 */

static void card_set_csd_info(sd_card_t *card, unsigned char *csdbuff,
csd_t *csd_info)
{
	u32 blocknr, mult, block_len, dummy1, dummy2;
	byte i;
	u32 value, unit;
	byte taac_value, taac_unit;
	u64 tmpsize;

	card_info_t *card_info = &(card->info);

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	value = unit = taac_value = taac_unit = 0;
	mult = block_len = 1;
	blocknr = dummy1 = dummy2 = 0;

	/* Store the CSD information to Struct of CSD */

	/* Get "CSD Structure" */
	csd_info->csd_structure = ((csdbuff[0] & 0xC0) >> 6);

	/* Get MMC "Spec_Vers" ( = System Specification version ) */
	csd_info->mmc_spec_vers = ((csdbuff[0] & 0x3C) >> 2);

	/* Get "TRAN SPEED" */
	csd_info->tran_speed = csdbuff[3];

	/* Get "TAAC" */
	csd_info->taac = csdbuff[1];

	/* Get "NSAC" */
	csd_info->nsac = csdbuff[2];

	/* Get "read_bl_len" */
	csd_info->read_bl_len = (csdbuff[5] & 0x0F);

	/* Get "PERM_WRITE_PROTECT" */
	csd_info->parm_protect = (csdbuff[14] & 0x20) >> 5;

	/* Get "TMP_WRITE_PROTECT" */
	csd_info->temp_protect = (csdbuff[14] & 0x10) >> 4;

	/* Get "c_size" */
	csd_info->c_size = 0;

	if (card->card_type == CARD_SD || card->card_type == CARD_UHS2) {
		if (csd_info->csd_structure == 0) {
			/* CSD Version 1.0 (Standard Capacity) */
			dummy1 = (csdbuff[6] & 0x03);
			dummy1 = (dummy1 << 10);
			dummy2 = csdbuff[7];
			dummy2 = (dummy2 << 2);
			dummy2 = (dummy1 | dummy2);
			csd_info->c_size =
			    (dummy2 | ((csdbuff[8] & 0xC0) >> 6));
		} else {
			/* CSD Version 2.0 (High Capacity and Extended Capacity) */
			dummy1 = (csdbuff[7] & 0x3F);
			dummy1 = (dummy1 << 16);
			dummy2 = csdbuff[8];
			dummy2 = (dummy2 << 8);
			dummy2 = (dummy1 | dummy2);
			csd_info->c_size = (dummy2 | (csdbuff[9] & 0xFF));
		}
	} else if ((card->card_type == CARD_MMC) ||
		   (card->card_type == CARD_EMMC)
	    ) {
		if (card_info->card_ccs == 0) {
			/* (Standard Capacity) */
			dummy1 = (csdbuff[6] & 0x03);
			dummy1 = (dummy1 << 10);
			dummy2 = csdbuff[7];
			dummy2 = (dummy2 << 2);
			dummy2 = (dummy1 | dummy2);
			csd_info->c_size =
			    (dummy2 | ((csdbuff[8] & 0xC0) >> 6));
		} else {
			/* (High Capacity and Extended Capacity) */
			dummy1 = (csdbuff[7] & 0x3F);
			dummy1 = (dummy1 << 16);
			dummy2 = csdbuff[8];
			dummy2 = (dummy2 << 8);
			dummy2 = (dummy1 | dummy2);
			csd_info->c_size = (dummy2 | (csdbuff[9] & 0xFF));
		}

	}

	/* Get "sect_size" */
	if ((card->card_type == CARD_MMC) || (card->card_type == CARD_EMMC)
	    ) {
		/* MMC */
		csd_info->sector_size = ((csdbuff[10] & 0x7C) >> 2);
	} else {
		/* SD Memory Card */
		csd_info->sector_size = (((csdbuff[10] & 0x3f) << 1) |
					 ((csdbuff[11] & 0x80) >> 7));
	}

	/* Get "c_size_mult" */
	if (card->card_type == CARD_SD || card->card_type == CARD_UHS2) {
		if (csd_info->csd_structure == 0) {
			/* CSD Version 1.0 (Standard Capacity) */
			csd_info->c_size_mult = (((csdbuff[9] & 0x03) << 1) |
						 ((csdbuff[10] & 0x80) >> 7));
		} else {
			/* CSD Version 2.0 (High Capacity and Extended Capacity) */
			/* not exist */
			;
		}
	} else if ((card->card_type == CARD_MMC) ||
		   (card->card_type == CARD_EMMC)
	    ) {

		if (card_info->card_ccs == 0) {
			/* CSD Version 1.0 (Standard Capacity) */
			csd_info->c_size_mult = (((csdbuff[9] & 0x03) << 1) |
						 ((csdbuff[10] & 0x80) >> 7));
		} else {
			/* CSD Version 2.0 (High Capacity and Extended Capacity) */
			/* not exist */
			;
		}
	}

	/*
	 * Acquired CSD Data, to be stored into Struct of CSD and Card
	 * Save some contents of CSD Register into Struct of CSD, and
	 * generate(calcurate) necessary Data to save into Struct of Card
	 */

	/* Calcuration of Total Sector count & Card Size */
	if (card->card_type == CARD_SD || card->card_type == CARD_UHS2) {
		if (csd_info->csd_structure == 0) {
			/* CSD Version 1.0 (Standard Capacity) */
			for (i = 0; i < (csd_info->c_size_mult + 2); i++)
				mult = mult * 2;
			for (i = 0; i < csd_info->read_bl_len; i++)
				block_len = block_len * 2;
			blocknr = (csd_info->c_size + 1) * mult;
			/* Card Size (Byte) */

			card->sec_count = ((u64) (blocknr) * (u64) (block_len));
		} else {
			/* CSD Version 2.0 (High Capacity and Extended Capacity) */

			/* Card Size (Byte) */

			/* (c_size + 1) * 512K */
			tmpsize = ((u64) csd_info->c_size) + 1;
			card->sec_count = tmpsize * 524288;
		}
	} else if ((card->card_type == CARD_MMC) ||
		   (card->card_type == CARD_EMMC)
	    ) {
		if (card_info->card_ccs == 0) {
			/* CSD Version 1.0 (Standard Capacity) */
			for (i = 0; i < (csd_info->c_size_mult + 2); i++)
				mult = mult * 2;
			for (i = 0; i < csd_info->read_bl_len; i++)
				block_len = block_len * 2;
			blocknr = (csd_info->c_size + 1) * mult;

			/* Card Size (Byte) */

			card->sec_count = ((u64) (blocknr) * (u64) (block_len));
		} else {
			/* sector size will calculate at MMC_Set_CSDEXT() */
			;
		}
	}

	/* Total sector count of Card */
	card->sec_count = ((card->sec_count) / (SD_BLOCK_LEN));
	/* SD_INFO_PRINTF("Card_Info.sect_num = %x\n", Card_Info.sect_num); */
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Exit %s CardSectors: %d, %dGB\n", __func__,
		card->sec_count, card->sec_count / 2 / 1024 / 1024);

}

/*
 * Function Name: card_get_csd
 *
 * Abstract: Addressed card sends its card-specific data (CSD) on the CMD line (CMD9)
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 * sd_command_t *sd_cmd: Pointer to sd command structure
 *
 * Output: None
 *
 * Return value: Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *
 * Caller: sd_read_csd
 *
 */

bool card_get_csd(sd_card_t *card, sd_command_t *sd_cmd)
{

	byte cmd_index = SD_CMD9;
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

	/* Issue CMD9 */
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);
	if (ret) {
		/* Set the card CSD info */
		os_memcpy(&(card_info->raw_csd[0]), &(sd_cmd->response[0]), 16);
		/* Parse the CSD info */
		card_set_csd_info(card, card_info->raw_csd, &(card_info->csd));
	}
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 * Function Name: card_get_card_status
 *
 * Abstract: Read the SD Status Register (SSR) (ACMD13)
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 * sd_command_t *sd_cmd: Pointer to sd command structure
 *
 * Output: None
 *
 * Return value: Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *
 * Caller: card_check_rw_ready
 *
 */

bool card_get_card_status(sd_card_t *card,
			  sd_command_t *sd_cmd, u32 *card_status)
{

	byte cmd_index = SD_CMD13;
	u32 argument = 0;
	u32 cmdflag = CMD_FLG_R1;
	e_data_dir dir = DATA_DIR_NONE;
	byte *data = NULL;
	u32 datalen = 0;

	bool ret = FALSE;
	card_info_t *card_info = &(card->info);

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_OPS, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* Issue CMD13 */
	argument = (card_info->rca << 16);
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);
	if (ret) {
		/* Send the card status */
		os_memcpy(card_status, &(sd_cmd->response[0]), 4);
	}

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_OPS, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

bool card_check_rw_ready(sd_card_t *card, sd_command_t *sd_cmd,
			 int timeout_ms)
{
	bool result = FALSE;
	u32 card_status = 0;
	loop_wait_t wait;
	u32 delay_us = 10;

	util_init_waitloop(card->host->pdx, timeout_ms, delay_us, &wait);

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_OPS, NOT_TO_RAM,
		"Enter %s, timeout_ms=%d\n", __func__, timeout_ms);

	do {
		result = card_get_card_status(card, sd_cmd, &card_status);
		if (result == FALSE)
			goto exit;

		os_udelay(delay_us);
	} while (((card_status & 0x900) != 0x900) && (!util_is_timeout(&wait)));

	if ((card_status & 0x900) != 0x900)
		result = FALSE;

exit:
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_OPS, NOT_TO_RAM, "Exit(%d) %s\n",
		result, __func__);
	return result;
}

/*
 * Function Name: card_set_block_len
 *
 * Abstract: Set the block length for all following block commands (ACMD6, block length = 5126)
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 * sd_command_t *sd_cmd: Pointer to sd command structure
 * u32 arg: SD_BLOCK_LEN
 *
 * Output: None
 *
 * Return value: Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *
 * Caller: sd_init_get_info
 *
 */

bool card_set_block_len(sd_card_t *card, sd_command_t *sd_cmd, u32 arg)
{
	byte cmd_index = SD_CMD16;
	u32 argument = arg;
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_NONE;
	byte *data = NULL;
	u32 datalen = 0;

	bool ret = FALSE;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, arg=0x%x\n", __func__, arg);

	/* Issue CMD16 */
	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);
	if (!ret)
		DbgErr("Set Block Length(CMD6) %d Error!!", argument);

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

/*
 * Function Name: card_get_legacy_freq
 *
 * Abstract:
 *           1. Set the specific clock frequency
 *           2. Set DM/DN and Clock Divider
 *
 * Input:
 *
 * sd_card_t *card: Pointer to the card structure
 * u32 clk_freq_khz: clock frequency to be set (KHz)
 * bool ddr_mode: if it is DDR50 mode (100MHz same as SDR50), need to check max frequency for DDR50
 *
 * Output: DMDN Values
 *
 * Return value: BIT[31:16]:dmdn BIT[14:0] basediv
 *
 * Notes:
 *
 * Caller: card_legacy_change_clock
 *
 */

static u32 card_get_legacy_freq(sd_card_t *card, u32 clk_freq_khz,
				bool ddr_mode)
{
	u32 value = 0;
	u16 index = 0;
	sd_host_t *host = card->host;
	u16 freq_level = card->degrade_freq_level;
	/* cfg_max_freq_item_t  * freq = &(host->cfg->host_item.max_freq_item); */

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Enter %s, Clock frequency %d KHz, ddr50_mode=%\n",
		__func__, clk_freq_khz, ddr_mode);
	if (host->cfg == NULL || host->cfg->dmdn_tbl == NULL) {
		DbgErr("host cfg is null\n");
		return 0;
	}

	/* Set DM/DN according to the clock frequency */
	switch (clk_freq_khz) {
	case SD_CLK_ID_400K:
		value = host->cfg->dmdn_tbl[FREQ_400K_START_INDEX];
		break;
	case SD_CLK_50M:
		if (ddr_mode)
			value = host->cfg->dmdn_tbl[FREQ_DDR50M_START_INDEX];
		else
			value = host->cfg->dmdn_tbl[FREQ_50M_START_INDEX];
		break;

	case SD_CLK_100M:
		index = (u16) FREQ_100M_START_INDEX + freq_level;
		if (index > (u16) FREQ_100M_DEGRE_INDEX)
			index = (u16) FREQ_100M_DEGRE_INDEX;
		value = host->cfg->dmdn_tbl[index];
		break;

	case SD_CLK_200M:
		if (ddr_mode) {
			index = (u16) FREQ_DDR200M_START_INDEX + freq_level;
			if (index > (u16) FREQ_DDR200M_DEGRE_INDEX)
				index = (u16) FREQ_DDR200M_DEGRE_INDEX;
			value = host->cfg->dmdn_tbl[index];
		} else {
			index = (u16) FREQ_200M_START_INDEX + freq_level;
			if (index > (u16) FREQ_200M_DEGRE_INDEX)
				index = (u16) FREQ_200M_DEGRE_INDEX;
			value = host->cfg->dmdn_tbl[index];
		}
		break;
	case SD_CLK_225M:
		index = (u16) FREQ_DDR225M_START_INDEX + freq_level;
		if (index > (u16) FREQ_DDR225M_DEGRE_INDEX)
			index = (u16) FREQ_DDR225M_DEGRE_INDEX;
		value = host->cfg->dmdn_tbl[index];
		break;

	case SD_CLK_75M:
		value = host->cfg->dmdn_tbl[FREQ_75M_START_INDEX];
		break;

	default:
		value = host->cfg->dmdn_tbl[FREQ_25M_START_INDEX];
		break;
	}

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, TO_RAM,
		"%s Exit  Clock=%d (KHz): value=0x%08X\n", __func__,
		clk_freq_khz, value);

	return value;

}

/*
 * Function Name: card_legacy_change_clock
 *
 * Abstract:
 *           1. Stop clock
 *           2. Set the clock frequency (DM/DN, clk divider)
 *           3. Start the clock
 *
 * Input:
 *
 * sd_card_t *card: Pointer to the card structure
 * u32 clk_freq_khz: clock frequency to be set (KHz)
 * bool ddr_mode: if it is DDR200/DDR50 mode (100MHz same as SDR50),
 *					need to check max frequency for DDR200/DDR50
 *
 * Output: None
 *
 * Return value: None
 *
 * Notes:
 *
 * Caller: sd_init_stage2
 *
 */

void card_legacy_change_clock(sd_card_t *card, u32 clk_freq_khz, bool ddr_mode)
{
	u32 value;
	sd_host_t *host = card->host;

	value = card_get_legacy_freq(card, clk_freq_khz, ddr_mode);
	host_change_clock(host, value);

}
