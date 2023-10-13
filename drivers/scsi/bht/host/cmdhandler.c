// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: cmdhandler.c
 *
 * Abstract: Include card command handler
 *
 * Version: 1.00
 *
 * Author: Peter.Guo
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 9/2/2014		Creation	Peter.guo
 */

#include "../include/basic.h"
#include "../include/debug.h"
#include "../include/hostapi.h"
#include "../include/cmdhandler.h"
#include "hostven.h"
#include "../host/handler.h"
#include "../host/hostreg.h"
#include "../include/util.h"

void irq_disable_sdcmd_int(sd_host_t *host);

#if DBG || _DEBUG
void host_dump_reg(sd_host_t *host)
{
	u16 i = 0;

	if ((g_dbg_ctrl & DBG_CTRL_DUMP_HOST) == 0)
		return;

	for (i = 0; i < 0x120; i += 4) {
		DbgErr("reg offset=0x%04X value=0x%08X\n", i,
		       sdhci_readl(host, i));

		if (sdhci_readl(host, i) == 0xffffffff)
			return;
	}
}
#else
#define host_dump_reg(x)
#endif

void host_error_int_recovery_stage1(sd_host_t *host, u16 error_int_state,
				    bool check);

bool card_is_low_capacity(sd_card_t *card)
{
	if (card->card_type == CARD_SD) {
		if (card->info.csd.csd_structure == 0)
			return TRUE;
	} else if (card->card_type == CARD_EMMC || card->card_type == CARD_MMC) {
		if (card->info.card_ccs == 0)
			return TRUE;
	}

	return FALSE;
}

/*
 * Function Name: cmd_test_fill_err
 *
 * Abstract: This Function is to test error interrupt handler
 *
 * Input:
 * sd_card_t *card,
 * u32 percent: percent of error occur rate
 * u32 fix: 0 means random generate error; other means the error reg setting
 *
 * Output: None
 *
 * Return value:
 * True: If no error generate
 *
 */

/*
 *	static bool cmd_test_fill_err(sd_card_t *card, u32 percent, u32 fix)
 *	{
 *		bool generate = FALSE;
 *		sd_host_t *host = card->host;
 *		u32 reg;
 *
 *		generate = random_percent_check(percent);
 *
 *		if (generate == FALSE)
 *			goto exit;
 *
 *		if (host->uhs2_flag) {
 *			if (fix == 0)
 *				reg = (1 << os_random_get(32));
 *			else
 *				reg = fix;
 *
 *			sdhci_writel(host, host->uhs2_cap.tst_base, reg);
 *
 *		} else {
 *			if (fix == 0)
 *				reg = (1 << os_random_get(16));
 *			else
 *				reg = fix;
 *
 *			sdhci_writew(host, SDHCI_ERROR_INTR_EVENT, (u16) reg);
 *		}
 *
 *	exit:
 *		return generate;
 *	}
 */

/*
 * Function Name: cmd_check_card_exist
 *
 * Abstract: This Function is to check whether card is present or not
 *
 * Input:
 * sd_card_t *card : The Command will send to which  Card
 * sd_command_t *sd_cmd: This parameter will contail card command info and reg info
 * for adma3 case this reg don't need conatin reg info
 *
 * Output: None
 *
 * Return value:
 * If the routine succeeds, it must return TRUE
 * otherwize reutrn FALSE
 */
static bool cmd_check_card_exist(sd_card_t *card, sd_command_t *sd_cmd)
{
	bool ret = FALSE;

	if (card->card_present == FALSE || card->card_chg) {
		sd_cmd->err.error_code = ERR_CODE_NO_CARD;
		goto exit;
	}

	ret = TRUE;

exit:
	return ret;
}

/*
 * Function Name: sdcmd_response_chk
 * Abstract: This Function is used to verify the response for sd-tran command
 *
 * Input:
 *			e_card_type type: card type
 *			sd_command_t *sd_cmd:
 *			u32 response: response value
 *
 *
 * Return value:
 *			TRUE: means ok
 *			others error
 *
 * Notes:
 *
 *        so giving the routine another name requires you to modify the build tools.
 */

static bool sdcmd_response_chk(e_card_type type, sd_command_t *sd_cmd,
			       u32 response)
{
	bool ret = TRUE;
	u32 flag = (sd_cmd->cmd_flag & CMD_FLG_RESP_MASK);

	sd_cmd->err.resp_err = 0;
	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Enter %s\n",
		__func__);

	/* don't check resopnse case */
	if (!(sd_cmd->cmd_flag & CMD_FLG_RESCHK))
		goto exit;

	switch (flag) {
	case CMD_FLG_R1:
	case CMD_FLG_R1B:

		/*
		 * Card status Check
		 * OUT_OF_RANGE
		 * ADDRESS_ERROR
		 * BLOCK_LEN_ERROR
		 * ERASE_SEQ_ERROR
		 * ERASE_PARAM
		 * WP_VIOLATION
		 * LOCK_UNLOCK_FAILED
		 * COM_CRC_ERROR
		 * ILLEGAL_COMMAND
		 * CARD_ECC_FAILED
		 * CC_ERROR
		 * ERROR
		 * UNDERRUN
		 * OVERRUN
		 * CID/CSD_OVERWRITE
		 */

		if (response & 0xFDF90000) {
			sd_cmd->err.error_code = ERR_CODE_RESP_ERR;
			sd_cmd->err.resp_err = (response & 0xFDF90000);
			ret = FALSE;
		}
		break;
	case CMD_FLG_R6:
		if (response & 0x0000E000) {
			sd_cmd->err.error_code = ERR_CODE_RESP_ERR;

			if (response & 0x00000800)
				sd_cmd->err.resp_err |= RESP_ERR_TYPE_ERROR;
			if (response & 0x00004000)
				sd_cmd->err.resp_err |=
				    RESP_ERR_TYPE_ILLEGAL_CMD;
			if (response & 0x00002000)
				sd_cmd->err.resp_err |=
				    RESP_ERR_TYPE_COM_CRC_ERROR;
			ret = FALSE;
		}
		break;

		/* Response = R5 */
	case CMD_FLG_R5:
		if ((response & 0x0000CB00) && type == CARD_SDIO) {
			sd_cmd->err.error_code = ERR_CODE_RESP_ERR;
			if (response & 0x00000800)
				sd_cmd->err.resp_err |= RESP_ERR_TYPE_ERROR;
			if (response & 0x00000100)
				sd_cmd->err.resp_err |=
				    RESP_ERR_TYPE_OUT_OF_RANGE;
			if (response & 0x00000200)
				sd_cmd->err.resp_err |= RESP_ERR_TYPE_FUNC_NUM;
			if (response & 0x00004000)
				sd_cmd->err.resp_err |=
				    RESP_ERR_TYPE_ILLEGAL_CMD;
			if (response & 0x00008000)
				sd_cmd->err.resp_err |=
				    RESP_ERR_TYPE_COM_CRC_ERROR;
			ret = FALSE;
		}
		break;
	default:
		break;
	}
exit:
	if (ret == FALSE)
		DbgErr("resp err=0x%08X,response=%x\n", sd_cmd->err.resp_err,
		       response);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Exit %s ret=%d\n",
		__func__, ret);
	return ret;
}

/*
 * Function Name: cmd_legacy_response
 * Abstract: This Function is used to get response of legacy command
 *
 * Input:
 *			void *card : pointer to card
 *			void *host_request poineter to host_cmd_req_t
 *
 *
 * Return value:
 *			0: means ok
 *			others error
 *
 * Notes:
 *
 *        so giving the routine another name requires you to modify the build tools.
 */
u32 cmd_legacy_response(void *pcard, void *host_request)
{
	u32 ret = INTR_CB_OK;
	sd_card_t *card = pcard;
	sd_host_t *host = card->host;
	host_cmd_req_t *req = host_request;
	sd_command_t *sd_cmd = req->private;
	byte *buff = NULL;
	u32 val;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Enter %s\n",
		__func__);

	if (req->trans_type == TRANS_ADMA3 || req->inf_mode != INF_NONE)
		goto exit;

	if (sd_cmd->cmd_flag & CMD_FLG_R2) {
		val = sdhci_readl(host, SDHCI_RESPONSE);
		buff = (byte *) sd_cmd->response;
		buff[14] = (byte) (val & 0x000000ff);
		buff[13] = (byte) ((val & 0x0000ff00) >> 8);
		buff[12] = (byte) ((val & 0x00ff0000) >> 16);
		buff[11] = (byte) ((val & 0xff000000) >> 24);

		val = sdhci_readl(host, SDHCI_RESPONSE + 4);
		buff[10] = (byte) (val & 0x000000ff);
		buff[9] = (byte) ((val & 0x0000ff00) >> 8);
		buff[8] = (byte) ((val & 0x00ff0000) >> 16);
		buff[7] = (byte) ((val & 0xff000000) >> 24);

		val = sdhci_readl(host, SDHCI_RESPONSE + 8);
		buff[6] = (byte) (val & 0x000000ff);
		buff[5] = (byte) ((val & 0x0000ff00) >> 8);
		buff[4] = (byte) ((val & 0x00ff0000) >> 16);
		buff[3] = (byte) ((val & 0xff000000) >> 24);

		val = sdhci_readl(host, SDHCI_RESPONSE + 12);
		buff[2] = (byte) (val & 0x000000ff);
		buff[1] = (byte) ((val & 0x0000ff00) >> 8);
		buff[0] = (byte) ((val & 0x00ff0000) >> 16);
	} else {
		sd_cmd->response[0] = sdhci_readl(host, SDHCI_RESPONSE);
		if (sdcmd_response_chk
		    (card->card_type, sd_cmd, sd_cmd->response[0]) == FALSE)
			ret = INTR_CB_ERR;
	}

	if (sd_cmd->cmd_flag & CMD_FLG_DDR200_WORK_AROUND
	    && sd_cmd->data->dir == DATA_DIR_OUT) {
		DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, NOT_TO_RAM,
			"update output phase for write case\n");
		/* Disable SD clock */
		sdhci_and32(host, SDHCI_CLOCK_CONTROL, ~(SDHCI_CLOCK_CARD_EN));

		/* update output phase */
		pci_andl(host, 0x354, 0xFFFFFF0F);
		pci_orl(host, 0x354, (host->cur_output_phase << 4));

		/* update input phase */
		sdhci_and32(card->host, SDHCI_DLL_PHASE_CFG, ~0x1F000000);
		sdhci_or32(card->host, SDHCI_DLL_PHASE_CFG,
			   (BIT28) |
			   (card->output_input_phase_pair
			    [host->cur_output_phase]
			    << 24));

		/* Enable SD clock */
		sdhci_or32(host, SDHCI_CLOCK_CONTROL, (SDHCI_CLOCK_CARD_EN));

		/* Continue transfer */
		sdhci_or32(host, SDHCI_DRIVER_CTRL_REG,
			   SDHCI_DRIVER_CTRL_ADMA2_START_INF);
		/* sd_cmd->gg8_ddr200_workaround = 0; */
	}

exit:
	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0,
		"Exit %s ret=0x%08X\n", __func__, ret);
	return ret;
}

/*
 * Function Name: cmd_uhs2_response
 * Abstract: This Function is used to get response of UHS2 command
 *
 * Input:
 *			void *card : pointer to card
 *			void *host_request poineter to host_cmd_req_t
 *
 *
 * Return value:
 *			0: means ok
 *			others error
 *
 * Notes:
 *
 *        so giving the routine another name requires you to modify the build tools.
 */
u32 cmd_uhs2_response(void *pcard, void *host_request)
{
	u32 ret = INTR_CB_OK;
	sd_card_t *card = pcard;
	sd_host_t *host = card->host;
	host_cmd_req_t *req = host_request;
	sd_command_t *sd_cmd = req->private;
	u32 resp0 = 0;
	int i;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Enter %s\n",
		__func__);

	if (req->trans_type == TRANS_ADMA3 || req->inf_mode != INF_NONE)
		goto exit;

	if (sd_cmd->cmd_flag & CMD_FLG_RESCHK) {
		resp0 = sdhci_readl(host, SDHCI_UHS2_RESPONSE);
		if (resp0 & UHS2_RESP_NACK)
			sd_cmd->uhs2_nack = 1;
	}

	if (sd_cmd->sd_cmd) {
		if (sd_cmd->cmd_flag & CMD_FLG_R2) {
			for (i = 0; i < 4; i++)
				sd_cmd->response[i] =
				    sdhci_readl(host,
						SDHCI_UHS2_RESPONSE4 + i * 4);

		} else if (sd_cmd->cmd_index == SD_CMD12) {
			sd_cmd->response[0] =
			    swapu32(sdhci_readl(host, SDHCI_UHS2_CMD12_RES));
			if (sdcmd_response_chk
			    (card->card_type, sd_cmd,
			     sd_cmd->response[0]) == FALSE)
				ret = INTR_CB_ERR;
			sdhci_writel(host, SDHCI_ADMA_ADDRESS, 0);
		} else {
			sd_cmd->response[0] =
			    swapu32(sdhci_readl(host, SDHCI_UHS2_RESPONSE4));
			if (sdcmd_response_chk
			    (card->card_type, sd_cmd,
			     sd_cmd->response[0]) == FALSE)
				ret = INTR_CB_ERR;
		}
	} else {
		if (UHS2_GET_NATIVE_IOADDR(sd_cmd->uhs2_header) ==
		    UHS2_IOADDR_ABORT)
			sd_cmd->response[0] =
			    sdhci_readl(host, SDHCI_RESPONSE + 4);
		else {
			for (i = 0; i < 4; i++)
				sd_cmd->response[i] =
				    sdhci_readl(host,
						SDHCI_UHS2_RESPONSE4 + i * 4);
		}
	}

exit:
	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0,
		"Exit %s return=0x%08X\n", __func__, ret);
	return ret;

}

/*
 * Function Name: cmd_piobuff_ready
 * Abstract: This Function is used to handle pio data buffer ready
 *
 * Input:
 *			void *card : pointer to card
 *			void *host_request poineter to host_cmd_req_t
 *
 *
 * Return value:
 *			0: means ok
 *			others error
 *
 * Notes:
 *
 *        so giving the routine another name requires you to modify the build tools.
 */
u32 cmd_piobuff_ready(void *pcard, void *host_request)
{
	u32 ret = INTR_CB_OK;
	sd_card_t *card = pcard;
	sd_host_t *host = card->host;
	host_cmd_req_t *req = host_request;
	sd_command_t *sd_cmd = req->private;
	sd_data_t *data = sd_cmd->data;
	u32 i;
	u32 trans_len = 0;
	u32 *buffer = NULL;
	u32 left = 0;

	DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, 0, "Enter %s\n", __func__);
	if (data == NULL) {
		ret = INTR_CB_ERR;
		goto exit;
	}

	buffer = (u32 *) data->data_mng.driver_buff;
	if (buffer == NULL) {
		ret = INTR_CB_ERR;
		goto exit;
	}

	/* get transfer start position and */
	buffer += data->data_mng.offset / 4;
	left = data->data_mng.total_bytess - data->data_mng.offset;
	DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, 0,
		"pio dir=%d left=%d offset=%d\n", data->dir, left,
		data->data_mng.offset);

	/* Caculete the data transfer length for this time */
	trans_len = data->block_size;
	if (sd_cmd->uhs2_cmd) {
		u32 n_fcu = card->uhs2_info.uhs2_setting.n_fcu;

		if (n_fcu == 0)
			n_fcu = 256;
		trans_len = os_min(left, n_fcu * trans_len);
	} else {
		trans_len = os_min(left, trans_len);
	}

	trans_len /= 4;
	if (sd_cmd->cmd_index == SD_CMD17) {
		ven_or16(host, 0x510, 0x2000);
		os_udelay(1);
		ven_and16(host, 0x510, ~0x2000);
	}

	/* transfer data */
	for (i = 0; i < trans_len; i++) {
		if (data->dir == DATA_DIR_IN)
			buffer[i] = sdhci_readl(host, SDHCI_BUFFER);
		else
			sdhci_writel(host, SDHCI_BUFFER, buffer[i]);

		data->data_mng.offset += 4;
		left -= 4;
	}

	if (left > 0)
		ret = INTR_CB_NOEND;
exit:
	DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, 0, "Exit %s return=0x%08X\n",
		__func__, ret);
	return ret;
}

/*
 * Function Name: uhs2_sdcmd_generate
 * Abstract: This Function is used to generate UHS2 SDCmd registers
 *
 * Input:
 *			sd_card_t *card : The Command will send to which  Card
 *			sd_command_t *sd_cmd: This parameter will contail card command info
 * Output:
 *			sd_command_t *sd_cmd to store register values
 *
 * Return value:
 *
 *			If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *			otherwize reutrn FALSE
 * Notes:
 *
 *        so giving the routine another name requires you to modify the build tools.
 */
static bool uhs2_sdcmd_generate(sd_card_t *pcard, sd_command_t *sd_cmd)
{
	sd_data_t *data = sd_cmd->data;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Enter %s\n",
		__func__);

	/* Step1: Generate header */
	sd_cmd->hw_resp_chk = 0;
	sd_cmd->uhs2_header = 0;
	sd_cmd->payload_cnt = 1;
	sd_cmd->trans_reg_cnt = 1;
	sd_cmd->uhs2_set_pld = 1;
	sd_cmd->uhs2_header |= pcard->uhs2_info.dev_id;
	if (data != NULL)
		sd_cmd->uhs2_header |= UHS2_CMD_HEADER_DCMD;

	if (sd_cmd->app_cmd)
		sd_cmd->uhs2_header |= UHS2_CMD_HEADER_APPCMD;

	sd_cmd->uhs2_header |= ((sd_cmd->cmd_index & 0x3f) << 24);
	sd_cmd->trans_reg[0].payload[2] = 0;
	if (sd_cmd->muldat_cmd) {
		if (!(sd_cmd->cmd_flag & CMD_FLG_INF) && (data != NULL)) {
			sd_cmd->uhs2_header |= UHS2_CMD_TMODE_LM;
			sd_cmd->trans_reg[0].payload[2] =
			    swapu32(data->block_cnt);
			/* for muldata command we need geneate paylaod2     */
			sd_cmd->payload_cnt++;
		}

		if (pcard->uhs2_info.uhs2_setting.half_supp
		    && (pcard->degrade_uhs2_half == 0)
		    && (pcard->thermal_uhs2_half_dis == 0))
			sd_cmd->uhs2_header |= UHS2_CMD_TMODE_DM;
	}

	/* step2: paylaod */
	sd_cmd->trans_reg[0].payload[0] = sd_cmd->uhs2_header;
	sd_cmd->trans_reg[0].payload[1] = swapu32(sd_cmd->argument);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0,
		"uhs2_sdcmd:header%08X Arg%08X\n", sd_cmd->uhs2_header,
		sd_cmd->argument);

	/* step3: generate transfer mode reg */
	sd_cmd->trans_reg[0].trans_mode = 0;

	if (data) {
		sd_cmd->trans_reg[0].block_size = data->block_size;

		if (sd_cmd->uhs2_header & UHS2_CMD_TMODE_DM)
			sd_cmd->trans_reg[0].trans_mode |=
			    SDHCI_UHS2_TRAN_HALF_DUPLEX;
		if (data->dir == DATA_DIR_OUT)
			sd_cmd->trans_reg[0].trans_mode |=
			    SDHCI_UHS2_TRAN_WRITE;

		if (sd_cmd->cmd_flag & CMD_FLG_DMA)
			sd_cmd->trans_reg[0].trans_mode |=
			    SDHCI_UHS2_TRAN_DMA_EN;

		if (!(sd_cmd->cmd_flag & CMD_FLG_INF)) {
			sd_cmd->trans_reg[0].trans_mode |=
			    SDHCI_UHS2_TRAN_EBSY_WAIT | SDHCI_UHS2_TRAN_BLK_EN;
			sd_cmd->trans_reg[0].block_cnt = data->block_cnt;
		} else
			sd_cmd->trans_reg[0].block_cnt = 0;

		sd_cmd->trans_reg[0].trans_mode |= SDHCI_UHS2_CMD_DATA_PRESENT;

		if (pcard->host->feature.hw_resp_chk
		    && (sd_cmd->cmd_flag & CMD_FLG_RESCHK)
		    && (sd_cmd->cmd_flag & (CMD_FLG_R5 | CMD_FLG_R1))
		    && (!(sd_cmd->cmd_flag & CMD_FLG_INF_CON))) {
			sd_cmd->hw_resp_chk = 1;
			if (sd_cmd->cmd_flag & CMD_FLG_R5)
				sd_cmd->trans_reg[0].trans_mode |=
				    SDHCI_UHS2_RESP_TYPE_R5;
			sd_cmd->trans_reg[0].trans_mode |=
			    SDHCI_UHS2_RESP_CHK | SDHCI_UHS2_RESP_INTR_DIS;
		}
	} else {
		if (sd_cmd->cmd_flag & CMD_FLG_R1B)
			sd_cmd->trans_reg[0].trans_mode |=
			    SDHCI_UHS2_TRAN_EBSY_WAIT;

		if (sd_cmd->cmd_index == SD_CMD12)
			sd_cmd->trans_reg[0].trans_mode |=
			    SDHCI_UHS2_CMD_TYPE_CMD12;
	}
	sd_cmd->trans_reg[0].trans_mode |= ((sd_cmd->payload_cnt + 1) << 26);

	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0,
		"uhs2_sdcmd:trans_mode=%08X\n",
		sd_cmd->trans_reg[0].trans_mode);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Exit %s\n",
		__func__);
	return TRUE;

}

/*
 * Function Name: uhs2_native_generate
 * Abstract: This Function is used to generate legacy SDCmd registers
 *
 * Input:
 *			sd_card_t *card : The Command will send to which  Card
 *			sd_command_t *sd_cmd: This parameter will contail card command info with
 *				uhs2_head and payload_cnt set
 * Output:
 *			sd_command_t *sd_cmd to store register values
 *
 * Return value:
 *
 *			If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *			otherwize reutrn FALSE
 * Notes:
 *
 *        so giving the routine another name requires you to modify the build tools.
 */

static bool uhs2_native_generate(sd_card_t *pcard, sd_command_t *sd_cmd)
{
	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Enter %s\n",
		__func__);

	sd_cmd->trans_reg_cnt = 1;
	sd_cmd->hw_resp_chk = 0;
	sd_cmd->trans_reg[0].block_cnt = 0;
	sd_cmd->trans_reg[0].trans_mode = 0;
	sd_cmd->trans_reg[0].payload[0] = sd_cmd->uhs2_header;
	if (sd_cmd->uhs2_set_pld == 0)
		sd_cmd->payload_cnt = 0;
	sd_cmd->trans_reg[0].trans_mode |= ((sd_cmd->payload_cnt + 1) << 26);
	if (UHS2_GET_NATIVE_IOADDR(sd_cmd->uhs2_header) == UHS2_IOADDR_ABORT)
		sd_cmd->trans_reg[0].trans_mode |= SDHCI_UHS2_CMD_TYPE_ABORT;
	else if (UHS2_GET_NATIVE_IOADDR(sd_cmd->uhs2_header) ==
		 UHS2_IOADDR_GODMT)
		sd_cmd->trans_reg[0].trans_mode |=
		    SDHCI_UHS2_CMD_TYPE_GODORMANT;
	else if (UHS2_GET_NATIVE_IOADDR(sd_cmd->uhs2_header) ==
		 UHS2_IOADDR_FULLRESET)
		sd_cmd->trans_reg[0].trans_mode |=
		    SDHCI_UHS2_CMD_TYPE_GODORMANT;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0,
		"uhs2_native:trans_mode=%08X\n",
		sd_cmd->trans_reg[0].trans_mode);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Exit %s\n",
		__func__);
	return TRUE;
}

/*
 * Function Name: uhs2_sdcmd_generate
 * Abstract: This Function is used to generate legacy SDCmd registers
 *
 * Input:
 *			sd_card_t *pcard : The Command will send to which  Card
 *			sd_command_t *sd_cmd: This parameter will contail card command info
 * Output:
 *			sd_command_t *sd_cmd to store register values
 *
 * Return value:
 *
 *			If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *			otherwize reutrn FALSE
 * Notes:
 *
 *        so giving the routine another name requires you to modify the build tools.
 */
static bool legacy_sdcmd_generate(sd_card_t *pcard, sd_command_t *sd_cmd)
{
	int i = 0;
	sd_host_t *host = pcard->host;
	sd_data_t *data = sd_cmd->data;
	u32 flgs = sd_cmd->cmd_flag;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Enter %s\n",
		__func__);

	sd_cmd->trans_reg_cnt = 1;
	sd_cmd->hw_resp_chk = 0;

	/* step 1 generate cmd55 registers */
	if (sd_cmd->app_cmd) {
		sd_cmd->trans_reg_cnt++;

		sd_cmd->trans_reg[i].block_cnt = 0;
		sd_cmd->trans_reg[i].block_size = 0;
		sd_cmd->trans_reg[i].payload[0] = (pcard->info.rca << 16);
		sd_cmd->trans_reg[i].trans_mode = (SD_CMD55 << 24);
		sd_cmd->trans_reg[i].trans_mode |= SDHCI_RSP_TYPE_R1;
		i++;
	}

	/* step2 generate transmode reg */
	sd_cmd->trans_reg[i].trans_mode = ((sd_cmd->cmd_index & 0x3f) << 24);
	sd_cmd->trans_reg[i].payload[0] = sd_cmd->argument;
	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0,
		"legacycmd:cmdidx%08X Arg%08X\n", sd_cmd->cmd_index,
		sd_cmd->argument);

	if (data) {
		/* generate block_size and block_cnt and argument register */
		sd_cmd->trans_reg[i].block_size = data->block_size;
		if (!(flgs & CMD_FLG_INF))
			sd_cmd->trans_reg[i].block_cnt = data->block_cnt;
		else
			sd_cmd->trans_reg[i].block_cnt = 0;

		sd_cmd->trans_reg[i].trans_mode |= SDCHI_CMD_DATA_PRESENT;

		if (data->dir == DATA_DIR_IN)
			sd_cmd->trans_reg[i].trans_mode |= SDHCI_TRNS_READ;

		if (sd_cmd->cmd_flag & CMD_FLG_DMA)
			sd_cmd->trans_reg[i].trans_mode |= SDHCI_TRNS_DMA;

		if (sd_cmd->muldat_cmd) {
			sd_cmd->trans_reg[i].trans_mode |= SDHCI_TRNS_MULTI;

			if (!(flgs & CMD_FLG_INF)) {
				sd_cmd->trans_reg[i].trans_mode |=
				    SDHCI_TRNS_BLK_CNT_EN;
				if (sd_cmd->cmd_flag & (CMD_FLG_AUTO12 |
							CMD_FLG_AUTO23)) {
					if (host->feature.hw_autocmd)
						sd_cmd->trans_reg[i].trans_mode |=
						    SDHCI_TRNS_AUTO_CMD12 |
						    SDHCI_TRNS_AUTO_CMD23;
					else if (sd_cmd->cmd_flag &
						 CMD_FLG_AUTO12)
						sd_cmd->trans_reg[i].trans_mode |=
						    SDHCI_TRNS_AUTO_CMD12;
					else
						sd_cmd->trans_reg[i].trans_mode |=
						    SDHCI_TRNS_AUTO_CMD23;
				}
			}
		}

		if (host->feature.hw_resp_chk
		    && (sd_cmd->cmd_flag & CMD_FLG_RESCHK)
		    && (0 == (sd_cmd->cmd_flag & CMD_FLG_DDR200_WORK_AROUND))
		    && (sd_cmd->cmd_flag & (CMD_FLG_R5 | CMD_FLG_R1))
		    && (!(flgs & CMD_FLG_INF_CON))) {
			sd_cmd->hw_resp_chk = 1;
			if (sd_cmd->cmd_flag & CMD_FLG_R5)
				sd_cmd->trans_reg[i].trans_mode |=
				    SDHCI_TRNS_RESP_R5;
			sd_cmd->trans_reg[i].trans_mode |=
			    SDHCI_TRNS_RESP_CHK | SDHCI_TRNS_RESP_INTR_DIS;
		}
	} else {
		if (sd_cmd->cmd_index == SD_CMD12)
			sd_cmd->trans_reg[i].trans_mode |=
			    SDHCI_CMD_TYPE_12_OR_52;

	}

	/* generate respone related register */
	if ((flgs & CMD_FLG_R1) || (flgs & CMD_FLG_R5) || (flgs & CMD_FLG_R6)
	    || (flgs & CMD_FLG_R7)) {
		sd_cmd->trans_reg[i].trans_mode |= SDHCI_RSP_TYPE_R1;
	} else if (flgs & CMD_FLG_R2) {
		sd_cmd->trans_reg[i].trans_mode |= SDHCI_RSP_TYPE_R2;
	} else if ((flgs & CMD_FLG_R3) || (flgs & CMD_FLG_R4)) {
		sd_cmd->trans_reg[i].trans_mode |= SDHCI_RSP_TYPE_R3;
	} else if (flgs & CMD_FLG_R1B) {
		sd_cmd->trans_reg[i].trans_mode |= SDHCI_RSP_TYPE_R1B;
	} else {
		sd_cmd->trans_reg[i].trans_mode |= SDHCI_RSP_NONE;
	}
	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0,
		"legacy_sdcmd:trans_mode=%08X\n",
		sd_cmd->trans_reg[0].trans_mode);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Exit %s\n",
		__func__);

	return TRUE;
}

/*
 * Function Name: cmd_generate_reg
 * Abstract: This Function is used to generate host register according to Card command
 *
 * Input:
 *			sd_card_t *card : The Command will send to which  Card
 *			sd_command_t *sd_cmd: This parameter will contail card command info
 * Output:
 *			sd_command_t *sd_cmd to store register values
 *
 * Return value:
 *
 *			If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *			otherwize reutrn FALSE
 * Notes:
 *
 *        so giving the routine another name requires you to modify the build tools.
 */

bool cmd_generate_reg(sd_card_t *card, sd_command_t *sd_cmd)
{
	bool result = FALSE;
	sd_data_t *data = sd_cmd->data;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Enter %s\n",
		__func__);
	X_ASSERT(card != NULL);

	if ((sd_cmd->cmd_flag & CMD_FLG_DMA) == 0)
		host_chk_ocb_occur(card->host);

	if (cmd_check_card_exist(card, sd_cmd) == FALSE) {
		DbgErr("Card not exist in %s\n", __func__);
		goto exit;
	}

	/* Step1 Generate sd_data according to cmd_flag */

	if (data == NULL)
		goto step2;
	if (data->dir == DATA_DIR_NONE)
		goto exit;
	if (data->data_mng.total_bytess > SD_BLOCK_LEN) {
		data->block_size = SD_BLOCK_LEN;
		data->block_cnt = data->data_mng.total_bytess / SD_BLOCK_LEN;
	} else {
		data->block_size = data->data_mng.total_bytess;
		data->block_cnt = 1;
	}
	DbgInfo(MODULE_SD_HOST, FEATURE_RW_TRACE, 0,
		"block_size=%d block_cnt=%d\n", data->block_size,
		data->block_cnt);

	/* Step 2, prepare handle sd_cmd to generate uhs2_cmd and cmd_index */
step2:
	if (card->host->uhs2_flag)
		sd_cmd->uhs2_cmd = 1;
	else
		sd_cmd->uhs2_cmd = 0;

	if (sd_cmd->cmd_flag & CMD_FLG_MULDATA)
		sd_cmd->muldat_cmd = 1;

	if (sd_cmd->cmd_index & SD_APPCMD) {
		sd_cmd->app_cmd = 1;
		/* Legacy AppCmd case */
		if (sd_cmd->uhs2_cmd == 0)
			sd_cmd->trans_reg_cnt = 2;
	}

	/* Infinite transfer case don't have register setting */
	if (sd_cmd->cmd_flag & CMD_FLG_INF_CON) {
		sd_cmd->trans_reg_cnt = 0;
		result = TRUE;
		goto exit;
	}

	/*
	 * Step3 Generate register for 3 case.
	 *  (1) UHS2 SD cmd
	 *  (2) UHS2 Native Cmd
	 *  (3) Legacy SD cmd
	 */

	if (sd_cmd->uhs2_cmd && sd_cmd->sd_cmd)
		result = uhs2_sdcmd_generate(card, sd_cmd);
	else if (sd_cmd->uhs2_cmd)
		result = uhs2_native_generate(card, sd_cmd);
	else
		result = legacy_sdcmd_generate(card, sd_cmd);

exit:
	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Exit %s ret=%d\n",
		__func__, result);
	return result;
}

/*
 * Function Name: wait_fifo_empty
 * Abstract: This Function is used to wait host fifo empty for write case
 *
 * Input:
 *			sd_host_t *host : the host
 * Output:
 *
 * Return value: None
 *
 * Notes:
 *			this function only use for infinite mode
 */
static void wait_fifo_empty(sd_host_t *host)
{
	int i = 0;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Enter %s\n",
		__func__);

#define MAX_FIFO_TIMEOUT 400000

	for (i = 0; i < MAX_FIFO_TIMEOUT; i++) {
		if (sdhci_readl(host, SDHCI_DRIVER_CTRL_REG) &
		    SDHCI_DRIVER_CTRL_FIFO_EMPTY) {
			/* 5us */
			os_udelay(5);
		} else {
			break;
		}
	}

	if (i == MAX_FIFO_TIMEOUT)
		DbgErr("Wait FiFo empty failed\n");

	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Exit %s\n",
		__func__);
}

/*
 * Function Name: cmd_final_execute
 * Abstract: This Function is used to Send SD command to host and wait
 *
 * Input:
 *			sd_card_t *card : The Command will send to which  Card
 *			host_cmd_req_t  *req; Caller need to allocate mem for this
 *			host_trans_reg_t *reg register value to be set
 *			sd_command_t *sd_cmd: This parameter will
 *			contail card command info and reg info
 *			for adma3 case this reg don't need conatin reg info
 *         bool bsync:	Last command execute sync or async
 * Output:
 *			Whether
 *
 * Return value:
 *
 *			If the routine succeeds, it must return TRUE
 *			otherwize reutrn FALSE
 *
 */
static bool cmd_final_execute(sd_card_t *card, sd_command_t *sd_cmd,
			      host_cmd_req_t *req, host_trans_reg_t *reg)
{
	byte buhs2 = sd_cmd->uhs2_cmd;
	int i = 0;
	sd_host_t *host = card->host;
	sd_data_t *data = sd_cmd->data;
	cfg_item_t *cfg = host->cfg;
	bool result = FALSE;
	u32 timeout;
	bool autocmd23 = FALSE;

	if ((reg->trans_mode & SDHCI_TRNS_AUTO_CMD23)
	    && (sd_cmd->cmd_flag & CMD_FLG_AUTO23))
		autocmd23 = TRUE;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Enter %s\n",
		__func__);

	/* step1 get timeout from configuration */
	if (data) {
		if (data->dir == DATA_DIR_IN) {
			if (sd_cmd->cmd_flag & CMD_FLG_TUNE)
				timeout = (u32) (TUNING_TIMEOUT & 0x0000ffff);
			else
				timeout =
				    (u32) cfg->timeout_item.test_read_data_timeout.value;
		} else
			timeout =
			    (u32) cfg->timeout_item.test_write_data_timeout.value;
	} else {
		if (sd_cmd->sd_cmd == 0)
			timeout = UHS2_NATIVE_DATA_TIMEOUT;
		/* timeout = (u32)cfg->timeout_item.uhs2_native_data_timeout.value; */
		else if (sd_cmd->cmd_flag & CMD_FLG_R1B)
			timeout =
			    (u32) cfg->timeout_item.test_r1b_data_timeout.value;
		else if (sd_cmd->cmd_flag & CMD_FLG_TUNE)
			timeout = (u32) (TUNING_TIMEOUT & 0x0000ffff);
		else
			timeout =
			    (u32) cfg->timeout_item.test_non_data_timeout.value;
	}

	if (sd_cmd->timeout != 0)
		timeout = sd_cmd->timeout;

	/* adma3 don't have argument */
	if (req->trans_type == TRANS_ADMA3)
		goto next;

	/* step 2  Set argument register and block register, infinite transfer don't set */
	if (req->inf_mode != INF_CONTINUE) {
		if (buhs2) {
			for (i = 0; i <= sd_cmd->payload_cnt; i++) {
				sdhci_writel(host, SDHCI_UHS2_CMD_PKG(i * 4),
					     reg->payload[i]);
			}
		} else {
			sdhci_writel(host, SDHCI_ARGUMENT, reg->payload[0]);
		}

		if (sd_cmd->data) {
			if (buhs2) {
				sdhci_writel(host, SDHCI_UHS2_BLOCK_SIZE,
					     reg->block_size |
					     (host->sdma_boundary_val << 12));
				sdhci_writel(host, SDHCI_UHS2_BLOCK_COUNT,
					     reg->block_cnt);
			} else {
				if (host->sd_host4_enable) {
					sdhci_writew(host, SDHCI_BLOCK_SIZE,
						     (host->sdma_boundary_val <<
						      12) | reg->block_size);
					sdhci_writel(host, SDHCI_ARGUMENT2,
						     reg->block_cnt);
				} else {
					sdhci_writel(host, SDHCI_BLOCK_SIZE,
						     (reg->block_cnt << 16) |
						     (host->sdma_boundary_val <<
						      12)
						     | reg->block_size);
				}
			}

			/* Host need to set dma mode at init stage */
		}
	}

	/* step3 set software structure ready and enable signale intr */
next:
	host->cmd_req = req;
	req->private = sd_cmd;
	req->card = card;
	host_led_ctl(host, TRUE);
	os_init_completion(host->pdx, &req->done);

	/* Clear Status Register */
	sdhci_writel(host, SDHCI_INT_STATUS,
		     ~SDHCI_INT_INSERT_REMOVE_CARD_BITS);
	if (sd_cmd->uhs2_cmd)
		sdhci_writel(host, SDHCI_UHS2_ERRINT_STS, 0xFFFFFFFF);

	/* step4 update sys addr */
	if (data) {
		phy_addr_t sys_addr = data->data_mng.sys_addr;

		switch (req->trans_type) {
		case TRANS_SDMA:
			if (buhs2 || host->sd_host4_enable) {
				sdhci_writel(host, SDHCI_ADMA_ADDRESS,
					     os_get_phy_addr32l(sys_addr));
				if (host->bit64_enable)
					sdhci_writel(host, SDHCI_ADMA_ADDRESSH,
						     os_get_phy_addr32h
						     (sys_addr));
			} else {
				/* This case only support 32bit */
				if (autocmd23) {
					DbgErr
					    ("SDMA without SD4 enable can't use AutoCmd23\n");
					sd_cmd->err.error_code =
					    ERR_CODE_SOFTARE_ARG;
					goto exit;
				}
				sdhci_writel(host, SDHCI_DMA_ADDRESS,
					     os_get_phy_addr32l(sys_addr));
			}

			break;
		case TRANS_ADMA2:
		case TRANS_ADMA2_SDMA_LIKE:
			if (req->inf_mode == INF_CONTINUE) {
				sdhci_or32(host, SDHCI_DRIVER_CTRL_REG,
					   SDHCI_DRIVER_CTRL_ADMA2_START_INF);
			} else {
				sdhci_writel(host, SDHCI_ADMA_ADDRESS,
					     os_get_phy_addr32l(sys_addr));
				if (host->bit64_enable)
					sdhci_writel(host, SDHCI_ADMA_ADDRESSH,
						     os_get_phy_addr32h
						     (sys_addr));
			}
			break;
		case TRANS_ADMA3:
			sdhci_writel(host, SDHCI_ADMA3_ADDRESS,
				     os_get_phy_addr32l(sys_addr));
			if (host->bit64_enable)
				sdhci_writel(host, SDHCI_ADMA3_ADDRESSH,
					     os_get_phy_addr32h(sys_addr));
			break;
		default:
			break;
		}

		if (autocmd23 && req->trans_type != TRANS_ADMA3)
			sdhci_writel(host, SDHCI_DMA_ADDRESS, data->block_cnt);
	}

	/* cmd12 to stop infinite case */
	if (sd_cmd->cmd_index == SD_CMD12 && card->has_built_inf) {
		if (card->last_dir == DATA_DIR_OUT)
			wait_fifo_empty(host);
	}

	/* step5 set transfer mode */
	if (req->inf_mode != INF_CONTINUE && req->trans_type != TRANS_ADMA3) {
		if (buhs2)
			sdhci_writel(host, SDHCI_UHS2_TRAN_MODE,
				     reg->trans_mode);
		else {
			/* GPIO2 Trigger for GG8 chip DDR200 write operation: timing issue */
			if (host->chip_type == CHIP_GG8
			    && card->info.sw_cur_setting.sd_access_mode ==
			    SD_FNC_AM_DDR200) {
				if (card->state == CARD_STATE_WORKING
				    || sd_cmd->cmd_flag &
				    CMD_FLG_DDR200_WORK_AROUND
				    || sd_cmd->cmd_index == SD_CMD12) {

					if ((pci_readl(host, 0x354) & 0xF0) !=
					    (host->cfg->feature_item.output_tuning_item.fixed_value_sdr104
					     << 4)) {
						/* Disable SD clock */
						sdhci_and32(host,
							    SDHCI_CLOCK_CONTROL,
							    ~
							    (SDHCI_CLOCK_CARD_EN));
						/* update output phase */
						pci_andl(host, 0x354,
							 0xFFFFFF0F);
						pci_orl(host, 0x354,
							(host->cfg->feature_item.output_tuning_item.fixed_value_sdr104
							 << 4));

						/* update input phase */
						sdhci_and32(host,
							    SDHCI_DLL_PHASE_CFG,
							    ~0x1F000000);
						sdhci_or32(host,
							   SDHCI_DLL_PHASE_CFG,
							   (BIT28) |
							   (card->output_input_phase_pair
							    [host->cfg->feature_item.output_tuning_item.fixed_value_sdr104]
							    << 24));

						/* Enable SD clock */
						sdhci_or32(host,
							   SDHCI_CLOCK_CONTROL,
							   (SDHCI_CLOCK_CARD_EN));
					}
				}

			}
			sdhci_writel(host, SDHCI_TRANSFER_MODE,
				     reg->trans_mode);
		}
	}

	/* step6 enable intr        */
	host_int_sig_update(host,
			    SDHCI_INT_INSERT_REMOVE_CARD_BITS |
			    SDHCI_INT_ERROR_MASK | req->int_flag_wait);
	if (buhs2)
		host_uhs2_err_sig_update(host, req->int_flag_uhs2_err);

	/* step7 update card command related info   */
	if (req->inf_mode == INF_BUILT)
		card->has_built_inf = 1;
	if (req->inf_mode != INF_NONE && data) {
		card->last_dir = data->dir;
		if (card_is_low_capacity(card))
			card->last_sect =
			    (sd_cmd->argument / SD_BLOCK_LEN) + data->block_cnt;
		else
			card->last_sect = sd_cmd->argument + data->block_cnt;
	} else if (sd_cmd->cmd_index == SD_CMD12)
		card->has_built_inf = 0;

	/* step8 wait transfer done */
	if (host->dump_mode == FALSE && host->poll_mode == FALSE) {
#if (GBL_ASYNC_PERFEATCH_IO)
		if (req->issue_post_cb)
			req->issue_post_cb(host->pdx);
#endif
		result = os_wait_for_completion(host->pdx, &req->done, timeout);
	} else {
		result = irq_poll_cmd_done(host->pdx, &req->done, timeout);
	}

	/* timeout */
	if (result == FALSE) {
		DbgErr("wait cmd software timeout\n");
		host_dump_reg(host);
		irq_disable_sdcmd_int(host);
		sd_cmd->err.error_code = ERR_CODE_TIMEOUT;
		host_error_int_recovery_stage1(host, SDHCI_INT_DATCMD_ERR_MASK,
					       TRUE);
		goto exit;
	} else {
		if (sd_cmd->err.error_code != 0) {
			result = FALSE;
			host_dump_reg(host);
			host_error_int_recovery_stage1(host,
						       sd_cmd->err.legacy_err_reg,
						       TRUE);
		} else {
			if (sd_cmd->cmd_index == SD_CMD12) {
				if (sd_cmd->uhs2_cmd == 0)
					host_cmddat_line_reset(host);
				/*
				 * Add 120us delay after send CMD12 to stop infinite transfer to
				 * fix "FJ2 Customer platform issue #2 ,
				 * Gloria VAUAO LA-9591P platform ,win8x64,
				 * SD driver O2FJ2w7 1.2.2.1011,
				 * emmc card NTFS file can't format".
				 */

				os_udelay(120);
			}
			result = TRUE;
		}
	}

exit:
	host->cmd_req = NULL;
	if (result == FALSE) {
		if (buhs2)
			DbgErr
			    ("UHS2cmd inf_info=%d cmd=0x%08X transmode=0x%08X err=%08X\n",
			     req->inf_mode, reg->payload[0], reg->trans_mode,
			     sd_cmd->err.error_code);
		else
			DbgErr
			    ("Legacycmd inf_info=%d transmode=0x%08X err=%08X, err_reg=%08X\n",
			     req->inf_mode, reg->trans_mode,
			     sd_cmd->err.error_code,
			     sd_cmd->err.legacy_err_reg);
	}

	if (host->feature.hw_led_fix == 0)
		host_led_ctl(host, FALSE);
	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Exit %s result=%d\n",
		__func__, result);
	return result;
}

/*
 * Function Name: cmd_execute_sync_inner
 * Abstract: This Function is used to generate host register according to Card command
 *
 * Input:
 *			sd_card_t *card : The Command will send to which  Card
 *			host_cmd_req_t  *req; Caller need to allocate mem for this
 *			sd_command_t *sd_cmd: This parameter will
 *			contail card command info and reg info.
 *			for adma3 case this reg don't need conatin reg info
 *         req_callback func_done : call back function to end Srb if necessary
 *         bool bsync:	Last command execute sync or async
 * Output:
 *			Whether
 *
 * Return value:
 *
 *			If the routine succeeds, it must return TRUE
 *			otherwize reutrn FALSE
 *
 */
static bool cmd_execute_sync_inner(sd_card_t *card, host_cmd_req_t *req,
				   sd_command_t *sd_cmd,
				   req_callback func_done,
				   issue_post_callback post_cb)
{
	int i = 0;
	bool res = FALSE;
	sd_data_t *data = sd_cmd->data;
	u32 timeout = 1000;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Enter %s\n",
		__func__);
	if (cmd_check_card_exist(card, sd_cmd) == FALSE) {
		DbgErr("Card not exist in exec_inner\n");
		goto end;
	}

	if (sd_cmd->uhs2_cmd == 0 && !(sd_cmd->cmd_flag & CMD_FLG_INF_CON)) {
		while (timeout) {
			if (cmd_dat_line_chk(card, sd_cmd))
				break;
			else {
				os_udelay(10);
				timeout--;
			}
		}
		if (timeout <= 0) {
			DbgErr("Check CMD/DAT line inhabit failed\n");
			goto end;
		}

	}

	/* Handle Legacy SD Cmd 55 first */
	if (sd_cmd->app_cmd && sd_cmd->uhs2_cmd == 0) {
		sd_command_t cmd;

		os_memset(&cmd, 0, sizeof(sd_command_t));
		os_memset(req, 0, sizeof(host_cmd_req_t));

		req->cb_response = cmd_legacy_response;
		req->issue_post_cb = NULL;
		req->inf_mode = INF_NONE;
		req->card_type = card->card_type;
		req->trans_type = TRANS_NONDATA;
		req->int_flag_err = SDHCI_INT_ERR_NON_DATA;
		req->int_flag_wait = SDHCI_INT_CMD_COMP;
		cmd.cmd_index = SD_CMD55;
		cmd.data = NULL;
		cmd.cmd_flag = CMD_FLG_R1;
		cmd.uhs2_cmd = sd_cmd->uhs2_cmd;
		res = cmd_final_execute(card, &cmd, req, &sd_cmd->trans_reg[0]);

		if (res == FALSE) {
			memcpy(&sd_cmd->err, &cmd.err, sizeof(cmd_err_t));
			sd_cmd->err.app_stage = 1;
			goto end;
		}
		i++;
	}

	/* generate host_cmd_req_t */
	os_memset(req, 0, sizeof(host_cmd_req_t));
	req->card_type = card->card_type;
	req->cb_req_complete = func_done;
	req->issue_post_cb = post_cb;

	if (sd_cmd->cmd_flag & CMD_FLG_INF_BUILD)
		req->inf_mode = INF_BUILT;
	else if (sd_cmd->cmd_flag & CMD_FLG_INF_CON)
		req->inf_mode = INF_CONTINUE;
	else
		req->inf_mode = INF_NONE;

	if (sd_cmd->cmd_flag & CMD_FLG_ADMA2)
		req->trans_type = TRANS_ADMA2;
	else if (sd_cmd->cmd_flag & CMD_FLG_ADMA_SDMA)
		req->trans_type = TRANS_ADMA2_SDMA_LIKE;
	else if (sd_cmd->cmd_flag & CMD_FLG_ADMA3)
		req->trans_type = TRANS_ADMA3;
	else if (sd_cmd->cmd_flag & CMD_FLG_SDMA)
		req->trans_type = TRANS_SDMA;
	else if (sd_cmd->data)
		req->trans_type = TRANS_PIO;
	else
		req->trans_type = TRANS_NONDATA;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0,
		"cmd_exec_inner:cmdflag=%08X transtype=%d infmode=%d\n",
		sd_cmd->cmd_flag, req->trans_type, req->inf_mode);

	req->cb_response =
	    sd_cmd->uhs2_cmd ? cmd_uhs2_response : cmd_legacy_response;

	/* generate intr flag according to  transfer type */
	switch (req->trans_type) {
	case TRANS_PIO:
		if (sd_cmd->cmd_flag & CMD_FLG_TUNE) {
			req->int_flag_wait = SDHCI_INT_DATA_AVAIL;
			req->int_flag_err = SDHCI_INT_ERR_TUNING_CMD;
		} else {
			req->int_flag_err = SDHCI_INT_ERR_DATA_CMD;
			req->int_flag_wait =
			    SDHCI_INT_CMD_COMP | SDHCI_INT_TRANSFER_COMP;
			req->int_flag_wait |=
			    (data->dir ==
			     DATA_DIR_IN) ? SDHCI_INT_DATA_AVAIL :
			    SDHCI_INT_SPACE_AVAIL;
			req->cb_buffer_ready = cmd_piobuff_ready;
		}
		break;
	case TRANS_SDMA:
		req->int_flag_err = SDHCI_INT_ERR_DATA_CMD;
		req->int_flag_wait = SDHCI_INT_SDMA_BITS;
		req->cb_boundary = cmd_sdma_boundary;
		req->cb_trans_complete = cmd_sdma_trans_done;
		break;
	case TRANS_ADMA2:
	case TRANS_ADMA2_SDMA_LIKE:
		req->int_flag_err = SDHCI_INT_ERR_ADMA_CMD;
		req->int_flag_wait = SDHCI_INT_ADMA_BITS;
		if (req->inf_mode != INF_NONE) {
			req->int_flag_wait |= SDHCI_INT_DMA_END;
			req->cb_boundary = cmd_adma2_inf_boundary;
		} else {
			if (req->trans_type == TRANS_ADMA2_SDMA_LIKE)
				req->cb_trans_complete =
				    cmd_adma2_sdma_like_trans_done;
		}
		break;
	case TRANS_ADMA3:
		req->int_flag_wait = SDHCI_INT_TRANSFER_COMP;
		req->int_flag_err = SDHCI_INT_ERR_ADMA_CMD;
		req->cb_trans_complete = cmd_adma3_trans_done;
		break;

	default:
		{
			req->int_flag_err = SDHCI_INT_ERR_NON_DATA;
			req->int_flag_wait = SDHCI_INT_CMD_COMP;
			if (sd_cmd->cmd_flag & CMD_FLG_R1B)
				req->int_flag_wait |= SDHCI_INT_TRANSFER_COMP;
		}
		break;
	}

	if (req->inf_mode != INF_NONE) {
		/* Infinte case don't have transfer complete and command complete */
		req->int_flag_wait &= ~(SDHCI_INT_TRANSFER_COMP);
		if (req->inf_mode == INF_CONTINUE)
			req->int_flag_wait &= ~SDHCI_INT_CMD_COMP;
	}
	if (sd_cmd->cmd_flag & CMD_FLG_NO_TRANS)
		req->int_flag_wait &= ~SDHCI_INT_TRANSFER_COMP;

	/* If hardware response check enabled */
	if (sd_cmd->hw_resp_chk) {
		req->int_flag_wait &= ~(SDHCI_INT_CMD_COMP);
		if (sd_cmd->uhs2_cmd == 0)
			req->int_flag_err |= SDHCI_INT_RESP_ERROR;
	}

	if (sd_cmd->uhs2_cmd)
		req->int_flag_uhs2_err = SDHCI_UHS2_INT_ERR_ALL;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0,
		"cmd_exec_inner:flag_wait=%08X err_wait=%08X uhs2_errwait=%08X\n",
		req->int_flag_wait, req->int_flag_err, req->int_flag_uhs2_err);

	res = cmd_final_execute(card, sd_cmd, req, &sd_cmd->trans_reg[i]);

end:
	DbgInfo(MODULE_SD_HOST, FEATURE_CARDCMD_TRACE, 0, "Exit %s ret=%d\n",
		__func__, res);
	return res;

}

/*
 * Function Name: cmd_execute_sync
 * Abstract: This Function is used to issue sd command and wati sync
 *
 * Input:
 *			sd_card_t *card : The Command will send to which  Card
 *			sd_command_t *sd_cmd: This parameter will
 *			contail card command info and reg info
 *			for adma3 case this reg don't need conatin reg info
 *			req_callback func_done : call back function to end Srb if necessary
 * Output:
 *			Whether
 *
 * Return value:
 *
 *			If the routine succeeds, it must return TRUE
 *			otherwize reutrn FALSE
 *
 */
bool cmd_execute_sync(sd_card_t *card, sd_command_t *sd_cmd,
		      req_callback func_done)
{
	return cmd_execute_sync_inner(card, &card->cmd_req, sd_cmd, func_done,
				      NULL);
}

/*
 * Function Name: cmd_execute_sync2
 * Abstract: This Function is used to issue sd command and wait sync2
 * Input:
 *			sd_card_t *card : The Command will send to which  Card
 *			sd_command_t *sd_cmd: This parameter will
 *			contail card command info and reg info
 *			for adma3 case this reg don't need conatin reg info
 *			host_cmd_req_t  *req: Caller need to allocate mem for this pointer
 *			req_callback func_done : call back function to end Srb if necessary
 * Output:
 *			Whether
 *
 * Return value:
 *
 *			If the routine succeeds, it must return TRUE
 *			otherwize reutrn FALSE
 *
 */
bool cmd_execute_sync2(sd_card_t *card, sd_command_t *sd_cmd,
		       host_cmd_req_t *req, req_callback func_done)
{
	return cmd_execute_sync_inner(card, req, sd_cmd, func_done, NULL);
}

/*
 * Function Name: cmd_execute_async3
 * Abstract: This Function is used to issue sd command and
 *			assign a callback immediately follow issue CMD
 * Input:
 *			sd_card_t *card : The Command will send to which  Card
 *			sd_command_t *sd_cmd: This parameter will
 *			contail card command info and reg info
 *			for adma3 case this reg don't need conatin reg info
 *			host_cmd_req_t  *req: Caller need to allocate mem for this pointer
 *			req_callback func_done : call back function to end Srb if necessary
 *			issue_post_callback post_cb: call back function for async operator
 * Output:
 *			Whether
 *
 * Return value:
 *
 *			If the routine succeeds, it must return TRUE
 *			otherwize reutrn FALSE
 *
 */
bool cmd_execute_sync3(sd_card_t *card, sd_command_t *sd_cmd,
		       host_cmd_req_t *req, req_callback func_done,
		       issue_post_callback post_cb)
{
#if (GBL_ASYNC_PERFEATCH_IO)
	return cmd_execute_sync_inner(card, req, sd_cmd, func_done, post_cb);
#else
	return cmd_execute_sync_inner(card, req, sd_cmd, func_done, NULL);
#endif
}

/*
 * Function Name: cmd_dat_line_chk
 * Abstract: This Function is to check whether card is present or not
 *
 * Input:
 *			sd_card_t *card: The Command will send to which  Card
 *			sd_command_t *sd_cmd: This parameter will
 *			contail card command info and reg info
 *			for adma3 case this reg don't need conatin reg info
 * Output:
 *
 * Return value:
 *
 *			If the routine succeeds, it must return TRUE
 *			otherwize reutrn FALSE
 *
 */
bool cmd_dat_line_chk(sd_card_t *card, sd_command_t *sd_cmd)
{
	u32 reg = sdhci_readl(card->host, SDHCI_PRESENT_STATE);

	if (reg & SDHCI_CMD_INHIBIT)
		return FALSE;
	if (sd_cmd->data && (reg & SDHCI_DATA_INHIBIT))
		return FALSE;
	return TRUE;
}

/*
 * Function Name: cmd_can_use_inf
 * Abstract: This Function is to check whether next transfer use infinite or not
 *
 * Input:
 *			sd_card_t *card,
 *			u32 sec_addr: the start address want to transfer
 *			u32 sec_cnt: the sector count want to transfer
 *
 * Output:
 *
 * Return value:
 *
 *			If the routine succeeds, it must return TRUE
 *			otherwize reutrn FALSE
 *
 */
u32 cmd_can_use_inf(sd_card_t *card, e_data_dir dir, u32 sec_addr, u32 sec_cnt)
{
	u32 n_fcu = 1;
	u32 flg = 0;
	bool buhs2 = card->card_type == CARD_UHS2 ? TRUE : FALSE;

	if (card->inf_trans_enable == 0)
		goto exit;

	if (buhs2) {
		n_fcu = card->uhs2_info.uhs2_setting.n_fcu;
		if (n_fcu == 0)
			n_fcu = 256;
		if ((sec_cnt % n_fcu) != 0)
			goto exit;
	}

	if (sec_addr != card->last_sect || dir != card->last_dir) {
		flg = CMD_FLG_INF_BUILD;
		goto exit;
	}

	if (card->has_built_inf)
		flg = CMD_FLG_INF_CON;
	else
		flg = CMD_FLG_INF_BUILD;

exit:
	return flg;
}

void cmd_set_auto_cmd_flag(sd_card_t *card, u32 *cmd_flag)
{
	if (card->card_type != CARD_UHS2 && (*cmd_flag & CMD_FLG_MULDATA)) {
		/* sd card if support cmd23 */
		if ((card->card_type == CARD_SD
		     && card->info.scr.cmd_support & 0x2)) {
			*cmd_flag |= CMD_FLG_AUTO23;
		} else
			*cmd_flag |= CMD_FLG_AUTO12;
	}

}

/*
 * Function Name: cmd_is_adma_error
 * Abstract: This Function is to test error interrupt handler
 *
 * Input:
 * sd_command_t *sd_cmd
 *
 * Output:
 *
 * Return value:
 *
 * True: Adma error occur
 *
 */
bool cmd_is_adma_error(sd_command_t *sd_cmd)
{
	if (sd_cmd == NULL)
		return FALSE;

	if ((sd_cmd->err.legacy_err_reg & SDHCI_INT_ADMA_ERROR) ||
	    (sd_cmd->uhs2_cmd
	     && (sd_cmd->err.uhs2_err_reg & SDHCI_UHS2_INT_ADMA)))
		return TRUE;
	return FALSE;
}
