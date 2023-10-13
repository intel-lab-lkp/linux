// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: geniofunc.c
 *
 * Abstract: This source file used to implement IOctrl request
 *
 * Version: 1.00
 *
 * Author: Peter.Guo
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 8/25/2014		Creation	Peter.Guo
 */

#include "../include/basic.h"
#include "../include/cardapi.h"
#include "../include/reqapi.h"
#include "../include/tqapi.h"
#include "../include/hostapi.h"
#include "../include/cmdhandler.h"
#include "../include/funcapi.h"
#include "funcapi.h"
#include "../include/debug.h"

bool func_cprm(sd_card_t *card, request_t *req)
{
	bool result = FALSE;
	bool ret = FALSE;
	sd_host_t *host = card->host;
	card_info_t *card_info = &(card->info);
	byte cmd_index;
	u32 argument = 0;
	u32 cmd_flag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	sd_command_t sd_cmd;
	u32 *pSrbData32;

	DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
		"Enter %s\n", __func__);

	if ((card->card_type != CARD_SD) && (card->card_type != CARD_UHS2))
		goto exit;

	result = card_stop_infinite(card, TRUE, NULL);
	if (result == FALSE) {
		DbgErr("Stop Infinite failed1\n");
		goto exit;
	}

	/* If use read write, Save Current DMA mode */
	host_transfer_init(host, FALSE, TRUE);
	result = FALSE;
	switch (req->gen_req_t.arg1) {
	case CPRM_IO_GETCSD:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"CPRM_IO_GETCSD\n");
		os_memcpy(req->srb_buff, &(card_info->raw_csd[0]), 16);
		result = TRUE;
		break;
	case CPRM_IO_GETMID:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"CPRM_IO_GETMID\n");
		cmd_index = SD_CMD44 | SD_APPCMD;
		cmd_flag = CMD_FLG_R1 | CMD_FLG_RESCHK;
		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index, 0, cmd_flag,
				    DATA_DIR_IN, req->srb_buff, 8);
		if (!ret) {
			DbgErr("Issue ACMD44 Fail.\n");
			goto exit;
		}
		result = TRUE;
		break;
	case CPRM_IO_GETWP:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"CPRM_IO_GETWP\n");
		pSrbData32 = (u32 *) req->srb_buff;
		if ((sdhci_readw(host, 0x26) & 0x08) != 0)
			*pSrbData32 = 0;
		else
			*pSrbData32 = 1;
		result = TRUE;
		break;
	case CPRM_IO_GETMKB:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"CPRM_IO_GETMKB\n");
		cmd_index = SD_CMD43 | SD_APPCMD;
		cmd_flag = CMD_FLG_R1 | CMD_FLG_RESCHK;
		argument = *((u32 *) req->srb_buff);
		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index, argument,
				    cmd_flag, DATA_DIR_IN, req->srb_buff, 512);
		if (!ret) {
			DbgErr("Issue ACMD43 Fail.\n");
			goto exit;
		}
		result = TRUE;
		break;
	case CPRM_IO_SETCERRN:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"CPRM_IO_SETCERRN\n");
		cmd_index = SD_CMD45 | SD_APPCMD;
		cmd_flag = CMD_FLG_R1 | CMD_FLG_RESCHK;
		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index, 0, cmd_flag,
				    DATA_DIR_OUT, req->srb_buff, 8);
		if (!ret) {
			DbgErr("Issue ACMD45 Fail.\n");
			goto exit;
		}
		result = TRUE;
		break;
	case CPRM_IO_GETCERRN:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"CPRM_IO_GETCERRN\n");
		cmd_index = SD_CMD46 | SD_APPCMD;
		cmd_flag = CMD_FLG_R1 | CMD_FLG_RESCHK;
		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index, 0, cmd_flag,
				    DATA_DIR_IN, req->srb_buff, 8);
		if (!ret) {
			DbgErr("Issue ACMD46 Fail.\n");
			goto exit;
		}
		result = TRUE;
		break;
	case CPRM_IO_SETCERRES:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"CPRM_IO_SETCERRES\n");
		cmd_index = SD_CMD47 | SD_APPCMD;
		cmd_flag = CMD_FLG_R1 | CMD_FLG_RESCHK;
		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index, 0, cmd_flag,
				    DATA_DIR_OUT, req->srb_buff, 8);
		if (!ret) {
			DbgErr("Issue ACMD47 Fail.\n");
			goto exit;
		}
		result = TRUE;
		break;
	case CPRM_IO_GETCERRES:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"CPRM_IO_GETCERRES\n");
		cmd_index = SD_CMD48 | SD_APPCMD;
		cmd_flag = CMD_FLG_R1 | CMD_FLG_RESCHK;
		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index, 0, cmd_flag,
				    DATA_DIR_IN, req->srb_buff, 8);
		if (!ret) {
			DbgErr("Issue ACMD48 Fail.\n");
			goto exit;
		}
		result = TRUE;
		break;
	case CPRM_IO_CHANGE_SA:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"CPRM_IO_CHANGE_SA\n");
		cmd_index = SD_CMD49 | SD_APPCMD;
		cmd_flag = CMD_FLG_R1 | CMD_FLG_RESCHK;
		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index, 0, cmd_flag,
				    DATA_DIR_NONE, NULL, 0);
		if (!ret) {
			DbgErr("Issue ACMD49 Fail.\n");
			goto exit;
		}
		result = TRUE;
		break;
	case CPRM_IO_READ:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"CPRM_IO_READ\n");
		if (req->gen_req_t.data_len == 512) {
			cmd_index = SD_CMD17;
			cmd_flag =
			    CMD_FLG_ADMA_SDMA | CMD_FLG_R1 | CMD_FLG_RESCHK;
		} else {
			cmd_index = SD_CMD18;
			cmd_flag =
			    CMD_FLG_MULDATA | CMD_FLG_ADMA_SDMA | CMD_FLG_R1 |
			    CMD_FLG_RESCHK;
			cmd_set_auto_cmd_flag(card, &cmd_flag);
		}

		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index,
				    req->gen_req_t.arg2, cmd_flag, DATA_DIR_IN,
				    req->srb_buff, req->gen_req_t.data_len);
		if (!ret) {
			DbgErr("Issue CMD18 Fail.\n");
			goto exit;
		}
		result = TRUE;
		break;
	case CPRM_IO_WRITE:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"CPRM_IO_WRITE\n");
		if (req->gen_req_t.data_len == 512) {
			cmd_index = SD_CMD24;
			cmd_flag =
			    CMD_FLG_ADMA_SDMA | CMD_FLG_R1 | CMD_FLG_RESCHK;
		} else {
			cmd_index = SD_CMD25;
			cmd_flag =
			    CMD_FLG_MULDATA | CMD_FLG_ADMA_SDMA | CMD_FLG_R1 |
			    CMD_FLG_RESCHK;
			cmd_set_auto_cmd_flag(card, &cmd_flag);
		}

		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index,
				    req->gen_req_t.arg2, cmd_flag, DATA_DIR_OUT,
				    req->srb_buff, req->gen_req_t.data_len);
		if (!ret) {
			DbgErr("Issue CMD25 Fail.\n");
			goto exit;
		}
		result = TRUE;
		break;
	case CPRM_IO_SECURE_READ:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"CPRM_IO_SECURE_READ\n");
		cmd_index = SD_CMD18 | SD_APPCMD;
		cmd_flag = CMD_FLG_ADMA_SDMA | CMD_FLG_R1 | CMD_FLG_RESCHK;
		if (card->card_type == CARD_SD)
			cmd_flag |= CMD_FLG_MULDATA;
		argument = req->gen_req_t.data_len >> 9;
		argument = ((argument << 24) | req->gen_req_t.arg2);

		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index, argument,
				    cmd_flag, DATA_DIR_IN, req->srb_buff,
				    req->gen_req_t.data_len);
		if (!ret) {
			DbgErr("Issue ACMD18 Fail.\n");
			goto exit;
		}
		result = TRUE;
		break;
	case CPRM_IO_SECURE_WRITE:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"CPRM_IO_SECURE_WRITE\n");
		cmd_index = SD_CMD25 | SD_APPCMD;
		cmd_flag = CMD_FLG_ADMA_SDMA | CMD_FLG_R1 | CMD_FLG_RESCHK;
		if (card->card_type == CARD_SD)
			cmd_flag |= CMD_FLG_MULDATA;
		argument = req->gen_req_t.data_len >> 9;
		argument = ((argument << 24) | req->gen_req_t.arg2);

		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index, argument,
				    cmd_flag, DATA_DIR_OUT, req->srb_buff,
				    req->gen_req_t.data_len);
		if (!ret) {
			DbgErr("Issue CMD25 Fail.\n");
			goto exit;
		}
		result = TRUE;
		break;
	case CPRM_IO_GETSDHC:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"CPRM_IO_GETSDHC\n");
		pSrbData32 = (u32 *) req->srb_buff;
		*pSrbData32 = card_info->card_ccs;
		result = TRUE;
		break;
	default:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"IOCTL Unknown\n");
		break;

	}
	/* Resorte current DMA mode */
	host_transfer_init(host, card->inf_trans_enable, FALSE);
exit:

	DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return result;
}

bool func_io_reg(sd_card_t *card, request_t *req)
{
	bool result = FALSE;
	sd_host_t *host = card->host;
	u32 *pSrbData32;

	DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
		"Enter %s\n", __func__);

	switch (req->gen_req_t.arg1) {
	case IO_READ_PCI_REG:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"IO_READ_PCI_REG\n");
		pSrbData32 = (u32 *) req->srb_buff;
		*pSrbData32 = pci_readl(host, (u16) req->gen_req_t.arg2);
		result = TRUE;
		break;
	case IO_WRITE_PCI_REG:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"IO_WRITE_PCI_REG\n");
		pSrbData32 = (u32 *) req->srb_buff;
		pci_writel(host, (u16) req->gen_req_t.arg2, *(pSrbData32 + 1));
		result = TRUE;
		break;
	case IO_READ_MEM_REG:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"IO_READ_MEM_REG\n");
		pSrbData32 = (u32 *) req->srb_buff;
		*pSrbData32 = sdhci_readl(host, (u16) req->gen_req_t.arg2);
		result = TRUE;
		break;
	case IO_WRITE_MEM_REG:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"IO_WRITE_MEM_REG\n");
		pSrbData32 = (u32 *) req->srb_buff;
		sdhci_writel(host, (u16) req->gen_req_t.arg2,
			     *(pSrbData32 + 1));
		result = TRUE;
		break;
	default:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"IOCTL Unknown\n");
		break;

	}
	DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return result;
}

bool erase_rw_blk_end_set(sd_card_t *card, sd_command_t *sd_cmd, u32 sec_addr)
{
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_NONE;
	byte *data = NULL;
	u32 datalen = 0;

	bool ret = FALSE;

	DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
		"Enter %s\n", __func__);

	ret =
	    card_send_sdcmd(card, sd_cmd, SD_CMD33, sec_addr, cmdflag, dir,
			    data, datalen);
	if (!ret)
		DbgErr("erase rw blk end set error\n");

	return ret;
}

bool erase_rw_blk_start_set(sd_card_t *card, sd_command_t *sd_cmd,
			    u32 sec_addr)
{
	u32 cmdflag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_NONE;
	byte *data = NULL;
	u32 datalen = 0;

	bool ret = FALSE;

	DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
		"Enter %s\n", __func__);

	ret =
	    card_send_sdcmd(card, sd_cmd, SD_CMD32, sec_addr, cmdflag, dir,
			    data, datalen);
	if (!ret)
		DbgErr("erase rw blk start set error\n");

	return ret;
}

bool func_erase(sd_card_t *card, sd_command_t *sd_cmd)
{
	u32 cmdflag = CMD_FLG_R1B | CMD_FLG_RESCHK;
	e_data_dir dir = DATA_DIR_NONE;
	byte *data = NULL;
	u32 datalen = 0;
	byte cmd_index = SD_CMD38;
	bool ret = FALSE;
	u32 argument = 0x0;

	DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
		"Enter %s\n", __func__);

	ret =
	    card_send_sdcmd(card, sd_cmd, cmd_index, argument, cmdflag, dir,
			    data, datalen);
	if (!ret)
		DbgErr("erase error\n");

	DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return ret;
}

bool func_nsm(sd_card_t *card, request_t *req, bht_dev_ext_t *pdx)
{
	bool result = FALSE;
	bool ret = FALSE;
	sd_host_t *host = card->host;
	card_info_t *card_info = &(card->info);
	byte cmd_index;
	u32 cmd_flag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	sd_command_t sd_cmd;
	u8 *p8;

	DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
		"Enter %s\n", __func__);

	if ((card->card_type != CARD_SD) && (card->card_type != CARD_UHS2))
		goto exit;

	result = card_stop_infinite(card, TRUE, NULL);
	if (result == FALSE) {
		DbgErr("Stop Infinite failed1\n");
		goto exit;
	}

	/* If use read write, Save Current DMA mode */
	host_transfer_init(host, FALSE, TRUE);
	result = FALSE;
	switch (req->gen_req_t.arg1) {

	case IO_NSM_CMD48:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"IO_NSM_CMD48\n");
		cmd_index = SD_CMD48;
		cmd_flag = CMD_FLG_R1 | CMD_FLG_RESCHK;
		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index,
				    req->gen_req_t.arg2, cmd_flag, DATA_DIR_IN,
				    req->srb_buff, req->gen_req_t.data_len);
		if (!ret) {
			DbgErr("IO_NSM_CMD48 Fail.\n");
			goto exit;
		}
		result = TRUE;
		break;
	case IO_NSM_CMD49:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"IO_NSM_CMD49\n");
		cmd_index = SD_CMD49;
		cmd_flag = CMD_FLG_R1 | CMD_FLG_RESCHK;
		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index,
				    req->gen_req_t.arg2, cmd_flag, DATA_DIR_OUT,
				    req->srb_buff, req->gen_req_t.data_len);
		if (!ret) {
			DbgErr("IO_NSM_CMD49 Fail.\n");
			goto exit;
		}
		result = TRUE;
		break;
	case IO_NSM_CMD58:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"IO_NSM_CMD58\n");
		cmd_index = SD_CMD58;
		cmd_flag =
		    CMD_FLG_AUTO12 | CMD_FLG_MULDATA | CMD_FLG_R1 |
		    CMD_FLG_RESCHK;
		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index,
				    req->gen_req_t.arg2, cmd_flag, DATA_DIR_IN,
				    req->srb_buff, req->gen_req_t.data_len);
		if (!ret) {
			DbgErr("IO_NSM_CMD58 Fail.\n");
			goto exit;
		}
		result = TRUE;
		break;
	case IO_NSM_CMD59:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"IO_NSM_CMD59\n");
		cmd_index = SD_CMD59;
		cmd_flag =
		    CMD_FLG_AUTO12 | CMD_FLG_MULDATA | CMD_FLG_R1 |
		    CMD_FLG_RESCHK;
		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index,
				    req->gen_req_t.arg2, cmd_flag, DATA_DIR_OUT,
				    req->srb_buff, req->gen_req_t.data_len);
		if (!ret) {
			DbgErr("IO_NSM_CMD59 Fail.\n");
			goto exit;
		}
		result = TRUE;
		break;
	case IO_NSM_CMD42:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"IO_NSM_CMD42\n");

		result = TRUE;
		cmd_index = SD_CMD42;
		cmd_flag = CMD_FLG_R1 | CMD_FLG_RESCHK;
		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index,
				    req->gen_req_t.arg2, cmd_flag, DATA_DIR_OUT,
				    req->srb_buff, req->gen_req_t.data_len);
		if (!ret) {
			DbgErr("IO_NSM_CMD42 Fail.\n");
			goto exit;
		}
		p8 = (u8 *) (req->srb_buff);
		/* If Unlock command */
		if (((*p8) & 0x4) == 0) {
			card->locked = FALSE;
			result = card_init_stage2(card);

			if (result == TRUE && pdx->scsi.last_present == 0) {

				DbgInfo(MODULE_MAIN_THR, FEATURE_THREAD_TRACE,
					NOT_TO_RAM,
					"Exec Bus Change for Unlock ok\n");
				/* callback execute successfully */
				if (thread_exec_high_prio_job
				    (pdx, os_bus_change, pdx))
					pdx->scsi.last_present = 1;
			}
		}
		break;
	case IO_NSM_CMD9:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"IO_NSM_CMD9\n");
		os_memcpy(req->srb_buff, &(card_info->raw_csd[0]), 16);
		result = TRUE;
#if (0)
		result = TRUE;
		cmd_index = SD_CMD9;
		cmd_flag = CMD_FLG_R2 | CMD_FLG_RESCHK;
		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index,
				    card_info->rca << 16, cmd_flag,
				    DATA_DIR_NONE, NULL, 0);
		if (!ret) {
			DbgErr("IO_NSM_CMD9 Fail.\n");
			goto exit;
		} else {
			/* Set the card CSD info */
			os_memcpy(req->srb_buff, &(sd_cmd->response[0]), 16);
		}
#endif
		break;
	case IO_NSM_CMD10:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"IO_NSM_CMD10\n");
		os_memcpy(req->srb_buff, &(card_info->raw_cid[0]), 16);

		result = TRUE;
#if (0)
		cmd_index = SD_CMD10;
		cmd_flag = CMD_FLG_R2 | CMD_FLG_RESCHK;
		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index,
				    card_info->rca << 16, cmd_flag,
				    DATA_DIR_NONE, NULL, 0);
		if (!ret) {
			DbgErr("IO_NSM_CMD10 Fail.\n");
			goto exit;
		} else {
			/* Set the card CID info */
			os_memcpy(req->srb_buff, &(sd_cmd->response[0]), 16);
		}
#endif
		break;

	case IO_NSM_ACMD51:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"IO_NSM_ACMD51\n");

		result = TRUE;
		cmd_index = SD_CMD51 | SD_APPCMD;
		cmd_flag = CMD_FLG_R1 | CMD_FLG_RESCHK;
		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index,
				    card_info->rca << 16, cmd_flag, DATA_DIR_IN,
				    req->srb_buff, 8);
		if (!ret) {
			DbgErr("IO_NSM_ACMD51 Fail.\n");
			goto exit;
		}
		break;

	case IO_NSM_ACMD13:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"IO_NSM_ACMD13\n");

		result = TRUE;
		cmd_index = SD_CMD13 | SD_APPCMD;
		cmd_flag = CMD_FLG_R1 | CMD_FLG_RESCHK;
		ret =
		    card_send_sdcmd(card, &sd_cmd, cmd_index, 0, cmd_flag,
				    DATA_DIR_IN, req->srb_buff, 64);
		if (!ret) {
			DbgErr("IO_NSM_ACMD13 Fail.\n");
			goto exit;
		}
		break;

	default:
		DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
			"IOCTL Unknown\n");
		break;

	}
	/* Resorte current DMA mode */
	host_transfer_init(host, card->inf_trans_enable, FALSE);
exit:

	DbgInfo(MODULE_MAIN_GENIO, FEATURE_IOCTL_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return result;
}
