// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: uhs2.c
 *
 * Abstract: SD UHS2 card initialization
 *
 * Version: 1.00
 *
 * Author: peter.guo
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 10/10/2014		Creation	Peter.Guo
 */

#include "../include/basic.h"
#include "../include/cmdhandler.h"
#include "../include/cardapi.h"
#include "../include/hostapi.h"
#include "cardcommon.h"
#include "../include/util.h"
#include "../include/debug.h"

#define UHS2_DEVINIT_CF			0x00000800
#define UHS2_DEVINIT_GAP		0x0000000F
#define UHS2_ENUM_PLD			0x00000000
#define UHS2_GODRM_HBNEN		0x00000080
#define UHS2_ENMR_IDF			0x000000F0
#define UHS2_ENMR_IDL			0x0000000F

#define	UHS2_LANES_2L_HD	0x00
#define	UHS2_LANES_2D1UFD	0x02
#define	UHS2_LANES_1D2UFD	0x03
#define	UHS2_LANES_2D2UFD	0x04

#define UHS2_UNRECOVER_ERROR	(BIT0 | BIT2 | BIT7)

static inline bool uhs2_is_uncoverable(sd_command_t *sd_cmd)
{
	if ((sd_cmd->err.error_code == ERR_CODE_TIMEOUT) ||
	    (sd_cmd->err.uhs2_err_reg & UHS2_UNRECOVER_ERROR))
		return TRUE;
	else
		return FALSE;
}

/*
 * Function Name: uhs2_access_reg
 *
 * Abstract: This Function is used to send uhs2 ccmd
 *
 * Input:
 *	sd_card_t *card : The Command will send to which  Card
 *	sd_command_t *sd_cmd: This parameter will contail card command info
 *	byte ioaddr: ioaddr for uhs2 ccmd
 *	bool broadcast: use broadcast or not
 *	bool rwcmd: Set RW flag in uhs2 header or not
 *	byte payload_num: payload count
 *
 * Input & Output:
 *			u32 *payload: contain the register want to setting,
 *			and store return  regs value
 *
 * Return value:
 *	If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *			otherwize reutrn FALSE
 *
 * Notes:
 *  so giving the routine another name requires you to modify the build tools.
 */

static bool uhs2_native_ccmd_internal(sd_card_t *card, sd_command_t *sd_cmd,
				      u16 ioaddr, bool broadcast, bool rwcmd,
				      byte payload_num, u32 *payload)
{
	bool result = FALSE;
	u32 headarg = UHS2_CMD_HEADER_NP;
	int i;

	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARDCMD_TRACE, NOT_TO_RAM,
		"Enter %s ioaddr=0x%04X\n", __func__, ioaddr);

	/* step1 prepare header for uhs2 ccmd       */
	headarg |= UHS2_NATIVE_CCMD_IOADDR(ioaddr);

	if (broadcast == FALSE)
		headarg |= UHS2_HEADER_DID(card->uhs2_info.dev_id);

	if (rwcmd)
		headarg |= UHS2_NATIVE_HEADER_RW;

	switch (payload_num) {
	case 0:
		break;
	case 1:
		headarg |= UHS2_NATIVE_CCMD_PLEN4;
		break;
	case 2:
		headarg |= UHS2_NATIVE_CCMD_PLEN8;
		break;
	case 4:
		headarg |= UHS2_NATIVE_CCMD_PLEN16;
		break;
	default:
		DbgErr("uhs2 ccmd payload number is wrong\n");
		goto exit;
	}

	sd_cmd->uhs2_header = headarg;
	if (rwcmd)
		sd_cmd->uhs2_set_pld = 1;

	/* step 2 set payload       */
	sd_cmd->payload_cnt = payload_num;
	for (i = 1; i <= payload_num; i++)
		sd_cmd->trans_reg[0].payload[i] = payload[i - 1];

	result = cmd_generate_reg(card, sd_cmd);
	if (result == FALSE)
		goto exit;

	result = cmd_execute_sync(card, sd_cmd, NULL);
exit:
	if (result == FALSE)
		DbgErr("UHS2 Native cmd failed ioaddr=0x%02X errcode=0x%08X\n",
		       UHS2_GET_NATIVE_IOADDR(sd_cmd->uhs2_header),
		       sd_cmd->err.error_code);
	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARDCMD_TRACE, NOT_TO_RAM,
		"Exit %s result=%d\n", __func__, result);
	return result;
}

bool uhs2_native_ccmd(sd_card_t *card, sd_command_t *sd_cmd,
		      u16 ioaddr, bool broadcast, bool rwcmd, byte payload_num,
		      u32 *payload)
{
	os_memset(sd_cmd, 0, sizeof(sd_command_t));
	return uhs2_native_ccmd_internal(card, sd_cmd, ioaddr, broadcast, rwcmd,
					 payload_num, payload);
}

/*
 * Function Name: uhs2_access_reg
 *
 * Abstract: This Function is used to read or inquiry or set uhs2 card registers
 *
 * Input:
 *	sd_card_t *card : The Command will send to which  Card
 *			sd_command_t *sd_cmd: This parameter will contail card command info
 *			byte ioaddr: reg addr
 *	bool broadcast: use broadcast or not
 *	bool setcfg: set reg or read reg
 *	byte payload_num: reg count
 *
 * Input & Output:
 *	u32 *payload: contain the register want to setting, and store return  regs value
 *
 * Return value:
 *	If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *	otherwize reutrn FALSE
 *
 * Notes:
 *  so giving the routine another name requires you to modify the build tools.
 */
static bool uhs2_access_reg(sd_card_t *card, sd_command_t *sd_cmd,
			    u16 ioaddr, bool broadcast, bool setcfg,
			    byte payload_num, u32 *payload)
{
	u32 i;
	bool result = FALSE;

	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARDCMD_TRACE, NOT_TO_RAM,
		"Enter %s ioaddr=0x%04X\n", __func__, ioaddr);
	os_memset(sd_cmd, 0, sizeof(sd_command_t));
	if (payload_num > 4) {
		DbgErr("payload_num is large than 4\n");
		goto exit;
	}

	/* set reg case and broadcast inquiry need input value */
	if (setcfg || broadcast) {
		for (i = 0; i < payload_num; i++)
			payload[i] = swapu32(payload[i]);
		sd_cmd->uhs2_set_pld = 1;
	} else {
		for (i = 0; i < payload_num; i++)
			payload[i] = 0;
	}

	result =
	    uhs2_native_ccmd_internal(card, sd_cmd, ioaddr, broadcast, setcfg,
				      payload_num, payload);
	if (result == FALSE) {
		DbgErr
		    ("uhs2 access reg failed ioaddr=0x%02X broadcast=%d setcfg=%d\n",
		     ioaddr, broadcast, setcfg);
		goto exit;
	}

	/* set reg case don't need get register value */
	if (setcfg == 0) {
		for (i = 0; i < payload_num; i++)
			payload[i] = swapu32(sd_cmd->response[i]);
	}

exit:
	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARDCMD_TRACE, NOT_TO_RAM,
		"Exit %s result=%d\n", __func__, result);
	return result;
}

/*
 * Function Name: uhs2_send_fullreset
 *
 * Abstract: This Function is used init Send UHS2 full reset ccmd
 *
 * Input:
 *	sd_card_t *card : The Command will send to which  Card
 *	sd_command_t *sd_cmd,
 *
 * Return value:
 *	If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *			otherwize reutrn FALSE
 */
bool uhs2_send_fullreset(sd_card_t *card, sd_command_t *sd_cmd)
{
	bool result = FALSE;
	u32 payload = 0;

	DbgInfo(MODULE_UHS2_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s\n", __func__);

	result =
	    uhs2_native_ccmd(card, sd_cmd, UHS2_IOADDR_FULLRESET, TRUE, TRUE, 0,
			     &payload);

	if (result == FALSE)
		DbgErr("uhs2 fullreset failed\n");

	DbgInfo(MODULE_UHS2_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Exit %s\n", __func__);
	return result;
}

/*
 * Function Name: uhs2_trans_abort
 *
 * Abstract: This Function is used init Send UHS2 transfer abort command
 *
 * Input:
 *	sd_card_t *card : The Command will send to which  Card
 *	sd_command_t *sd_cmd,
 *
 * Return value:
 *	If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *	otherwize reutrn FALSE
 */
bool uhs2_trans_abort(sd_card_t *card, sd_command_t *sd_cmd)
{
	bool result = FALSE;
	u32 payload = 0;

	DbgInfo(MODULE_UHS2_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s\n", __func__);

	result =
	    uhs2_native_ccmd(card, sd_cmd, UHS2_IOADDR_ABORT, FALSE, TRUE, 0,
			     &payload);

	if (result == FALSE)
		DbgErr("uhs2 trans_abort failed\n");

	DbgInfo(MODULE_UHS2_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, result);
	return result;
}

/*
 * Function Name: uhs2_full_reset_card
 *
 * Abstract: This Function is used to send full reset command, if failed do host reset for all
 *
 * Input:
 *	sd_card_t *card : The Command will send to which  Card
 *
 * Return value:
 *	If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *	otherwize reutrn FALSE
 */

bool uhs2_full_reset_card(sd_card_t *card)
{
	sd_command_t sd_cmd;
	bool result = FALSE;

	DbgInfo(MODULE_UHS2_CARD, FEATURE_ERROR_RECOVER | FEATURE_CARD_OPS,
		NOT_TO_RAM, "Enter %s\n", __func__);

	result = uhs2_send_fullreset(card, &sd_cmd);
	DbgInfo(MODULE_UHS2_CARD, FEATURE_ERROR_RECOVER | FEATURE_CARD_OPS,
		NOT_TO_RAM, "uhs2 full rest ret=%d\n", result);
	if (result) {
		os_udelay(200);
		host_uhs2_reset(card->host, TRUE);
	} else {
		/* failed do reset for all */
		host_uhs2_clear(card->host, TRUE);
	}
	DbgInfo(MODULE_UHS2_CARD, FEATURE_ERROR_RECOVER | FEATURE_CARD_OPS,
		NOT_TO_RAM, "Exit %s ret=%d\n", __func__, result);

	return result;
}

/*
 * Function Name: uhs2_send_devinit
 *
 * Abstract: This Function is used init send uhs2 dev_enum ccmd and get card deviceid
 *
 * Input:
 *			sd_card_t *card : The Command will send to which  Card
 *			sd_command_t *sd_cmd,
 *
 * Return value:
 *	If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *	otherwize reutrn FALSE
 */

bool uhs2_dev_enumeration(sd_card_t *card, sd_command_t *sd_cmd)
{
	bool result = FALSE;
	u32 payload = 0;
	u8 firstid, lastid;
	u32 resp;
	u8 devcnt = 0;

	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	firstid = lastid = 0;
	payload = UHS2_ENUM_PLD;

	result =
	    uhs2_native_ccmd(card, sd_cmd, UHS2_IOADDR_ENUM, TRUE, TRUE, 1,
			     &payload);
	if (result == FALSE)
		goto exit;

	resp = sd_cmd->response[0];
	firstid = (u8) ((resp & UHS2_ENMR_IDF) >> 4);
	lastid = (u8) (resp & UHS2_ENMR_IDL);

	if (firstid > lastid)
		devcnt = (lastid + 0x10) - firstid;
	else
		devcnt = lastid - firstid + 1;

	card->uhs2_info.dev_id = firstid;

	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, TO_RAM,
		"firstid=0x%02X lastid=0x%02X\n", firstid, lastid);

exit:
	if (result == FALSE)
		DbgErr("uhs2 enumeration failed\n");
	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, result);
	return result;

}

/*
 * Function Name: uhs2_send_devinit
 *
 * Abstract: This Function is used init send uhs2 dev_init cmd
 *
 * Input:
 *			sd_card_t *card : The Command will send to which  Card
 *			sd_command_t *sd_cmd,
 *			u8 gap,
 *			u8 dap,
 *
 * Iput & Output:
 *			u8 *gd,
 *			u8 *cf
 *
 * Return value:
 *	If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *	otherwize reutrn FALSE
 */

static bool uhs2_send_devinit(sd_card_t *card, sd_command_t *sd_cmd, u8 *gd,
			      u8 gap, u8 dap, u8 *cf)
{
	bool result = FALSE;
	u32 payload;
	u32 resp;

	payload =
	    UHS2_DEVINIT_CF | (((*gd) & 0xf) << 4) | (gap & 0xf) | ((dap & 0xf)
								    << 12);
	result =
	    uhs2_native_ccmd(card, sd_cmd, UHS2_IOADDR_DEVINIT, TRUE, TRUE, 1,
			     &payload);
	if (result == FALSE)
		goto exit;

	resp = sd_cmd->response[0];
	if (resp & UHS2_DEVINIT_CF)
		*cf = 1;
	else
		*cf = 0;

	if (gap == (resp & UHS2_DEVINIT_GAP))
		(*gd)++;

exit:
	return result;
}

/*
 * Function Name: uhs2_devinit_flow
 *
 * Abstract: This Function is used do device_init flow
 *
 * Input:
 *	sd_card_t *card : The Command will send to which  Card
 *			sd_command_t *sd_cmd,
 *	sd_host_t *host
 *
 * Return value:
 *	If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *	otherwize reutrn FALSE
 */
static bool uhs2_devinit_flow(sd_card_t *card, sd_command_t *sd_cmd,
			      sd_host_t *host)
{
	bool result = FALSE;
	u8 gd, dap, gap, cf;

	/* max 1200ms delay */
	u32 timeout = 1200;
	loop_wait_t wait;
	u32 delay_us = 20;

	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	cf = 0;
	gd = 0;
	dap = host->uhs2_cap.dap;
	gap = host->uhs2_cap.gap;
	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"devinit dap=0x%02X gap=0x%02X\n", dap, gap);

	util_init_waitloop(card->host->pdx, timeout, delay_us, &wait);

	do {
		result = uhs2_send_devinit(card, sd_cmd, &gd, gap, dap, &cf);
		os_udelay(delay_us);

		if (result == FALSE) {
			DbgErr("Device Init cmd error\n");
			goto exit;
		}
	} while ((util_is_timeout(&wait) == FALSE) && (cf == 0));

	if (cf == 0)
		result = FALSE;

exit:
	if (result == FALSE)
		DbgErr("host:%p devinit failed cf=%d\n", host, cf);
	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return result;

}

/*
 * Function Name: uhs2_send_devinit
 *
 * Abstract: This Function is used init send uhs2 dev_enum ccmd and get card deviceid
 *
 * Input:
 *	sd_card_t *card : The Command will send to which  Card
 *	sd_command_t *sd_cmd,
 *
 * Return value:
 *			If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *	otherwize reutrn FALSE
 */

static bool uhs2_card_get_caps(sd_card_t *card, sd_command_t *sd_cmd,
			       sd_host_t *host)
{
	u32 payload[2];
	bool result = FALSE;

	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/*
	 * Get  phy capbality for both host and card support
	 * 1. Hibernate;  2. Lss_Dir; 3. Lss_Syn
	 */
	os_memset(payload, 0, sizeof(payload));
	result =
	    uhs2_access_reg(card, sd_cmd, UHS2_IOADDR_PHY_CAPL, FALSE, FALSE, 2,
			    payload);
	if (result == FALSE) {
		DbgErr("Inquiry card phy cap failed\n");
		goto exit;
	}

	card->uhs2_info.uhs2_cap.hibernate = (payload[0] & BIT15) ? 1 : 0;
	card->uhs2_info.uhs2_cap.n_lss_dir = ((payload[1] & 0xF0) >> 4);
	card->uhs2_info.uhs2_cap.n_lss_syn = ((payload[1] & 0x0F));
	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"card hbr=%d lssdir=%d lsssyn=%d\n",
		card->uhs2_info.uhs2_cap.hibernate,
		card->uhs2_info.uhs2_cap.n_lss_dir,
		card->uhs2_info.uhs2_cap.n_lss_syn);

	/*
	 * Get  Link/Tran capbality for both host and card support
	 * 1. nfcu;  2. datagap; 3. max block length
	 */
	os_memset(payload, 0, sizeof(payload));
	result =
	    uhs2_access_reg(card, sd_cmd, UHS2_IOADDR_LINKT_CAPL, FALSE, FALSE,
			    2, payload);
	if (result == FALSE) {
		DbgErr("Inquiry card link cap failed\n");
		goto exit;
	}

	card->uhs2_info.uhs2_cap.n_fcu = ((payload[0] & 0xFF00) >> 8);
	card->uhs2_info.uhs2_cap.n_data_gap = ((payload[1] & 0x00FF));
	card->uhs2_info.uhs2_cap.max_blk_len =
	    ((payload[0] & 0xFFF00000) >> 20);
	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"card nfcu=%d datagap=%d blklen=%d\n",
		card->uhs2_info.uhs2_cap.n_fcu,
		card->uhs2_info.uhs2_cap.n_data_gap,
		card->uhs2_info.uhs2_cap.max_blk_len);

	/* Below capabliies only decide host */
	card->uhs2_info.uhs2_cap.speed_range = host->uhs2_cap.speed_range;
	/* default we use Fast Mode */

	/* card support low power mode */
	card->uhs2_info.uhs2_cap.pwr_mode = 1;
	card->uhs2_info.uhs2_cap.retry_cnt = host->uhs2_cap.retry_cnt;

	os_memset(payload, 0, sizeof(payload));
	result =
	    uhs2_access_reg(card, sd_cmd, UHS2_IOADDR_GEN_CAPL, FALSE, FALSE, 1,
			    payload);
	if (result == FALSE) {
		DbgErr("Inquiry card gen cap failed\n");
		goto exit;
	}

	card->uhs2_info.uhs2_cap.half_supp = (payload[0] & BIT8) ? TRUE : FALSE;
	card->uhs2_info.uhs2_cap.lanes = ((payload[0] & 0x0E00) >> 8);
	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"card halfsupp=%d lanes=%d\n",
		card->uhs2_info.uhs2_cap.half_supp,
		card->uhs2_info.uhs2_cap.lanes);

exit:
	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Exit %s result=%d\n", __func__, result);
	return result;

}

static byte uhs2_get_large_lss(u32 val1, u32 val2)
{
	u32 v1, v2;
	u32 v = 0;

	v1 = val1;
	v2 = val2;

	if (v1 == 0)
		v1 = 16;
	if (v2 == 0)
		v2 = 16;

	v = os_max(v1, v2);
	if (v == 16)
		v = 0;

	return (byte) v;
}

/*
 * Function Name: uhs2_get_card_setting_host
 *
 * Abstract: This Function is used generate uhs2 card setting by card caps and host caps
 *
 * Input:
 *	sd_card_t *card :
 *			sd_host_t *host
 *
 */
static void uhs2_get_card_setting_host(sd_card_t *card, sd_host_t *host)
{
	u16 nfcu1, nfcu2;
	byte lanes;

	card->uhs2_info.uhs2_setting.hibernate =
	    card->uhs2_info.uhs2_cap.hibernate;
	card->uhs2_info.uhs2_setting.n_lss_dir =
	    uhs2_get_large_lss(card->uhs2_info.uhs2_cap.n_lss_dir,
			       host->uhs2_cap.n_lss_dir);
	card->uhs2_info.uhs2_setting.n_lss_syn =
	    uhs2_get_large_lss(card->uhs2_info.uhs2_cap.n_lss_syn,
			       host->uhs2_cap.n_lss_syn);

	card->uhs2_info.uhs2_setting.n_data_gap =
	    os_max(card->uhs2_info.uhs2_cap.n_data_gap,
		   host->uhs2_cap.n_data_gap);
	card->uhs2_info.uhs2_setting.max_blk_len = 0x200;

	nfcu1 = card->uhs2_info.uhs2_cap.n_fcu;
	nfcu2 = host->uhs2_cap.n_fcu;
	if (nfcu1 == 0)
		nfcu1 = 256;
	if (nfcu2 == 0)
		nfcu2 = 256;
	card->uhs2_info.uhs2_setting.n_fcu =
	    (nfcu1 >
	     nfcu2) ? host->uhs2_cap.n_fcu : card->uhs2_info.uhs2_cap.n_fcu;

	card->uhs2_info.uhs2_setting.speed_range =
	    card->uhs2_info.uhs2_cap.speed_range;
	card->uhs2_info.uhs2_setting.pwr_mode = 0;
	card->uhs2_info.uhs2_setting.retry_cnt = host->uhs2_cap.retry_cnt;

	card->uhs2_info.uhs2_setting.half_supp =
	    os_min(card->uhs2_info.uhs2_cap.half_supp,
		   (host->uhs2_cap.num_of_lane & 0x1));
	card->uhs2_info.uhs2_setting.lanes =
	    card->uhs2_info.uhs2_cap.lanes & host->uhs2_cap.num_of_lane;

	lanes = card->uhs2_info.uhs2_setting.lanes;

	if (lanes & UHS2_LANES_2D2UFD)
		lanes = UHS2_LANES_2D2UFD;
	else if (lanes & UHS2_LANES_1D2UFD)
		lanes = UHS2_LANES_1D2UFD;
	else if (lanes & UHS2_LANES_2D1UFD)
		lanes = UHS2_LANES_2D1UFD;
	else
		lanes = UHS2_LANES_2L_HD;
	card->uhs2_info.uhs2_setting.lanes = lanes;
}

/*
 * Function Name: uhs2_get_card_setting_host
 *
 * Abstract: This Function is used generate uhs2 card setting by vendor setting
 *
 * Input:
 *			sd_card_t *card :
 *	sd_host_t *host
 *
 */
static void uhs2_update_card_setting_vendor(sd_card_t *card, sd_host_t *host)
{
	u16 nfcu1, nfcu2;
	cfg_uhs2_setting_t *cfg = &host->cfg->card_item.uhs2_setting;

	card->uhs2_info.uhs2_setting.n_lss_dir =
	    uhs2_get_large_lss(card->uhs2_info.uhs2_setting.n_lss_dir,
			       cfg->min_lss_dir);
	card->uhs2_info.uhs2_setting.n_lss_syn =
	    uhs2_get_large_lss(card->uhs2_info.uhs2_setting.n_lss_syn,
			       cfg->min_lss_syn);

	card->uhs2_info.uhs2_setting.n_data_gap =
	    os_max(card->uhs2_info.uhs2_setting.n_data_gap,
		   cfg->min_data_gap_sel);

	nfcu1 = (u16) card->uhs2_info.uhs2_setting.n_fcu;
	nfcu2 = (u16) cfg->max_nfcn_sel;
	if (nfcu1 == 0)
		nfcu1 = 256;
	if (nfcu2 == 0)
		nfcu2 = 256;
	card->uhs2_info.uhs2_setting.n_fcu =
	    (nfcu1 >
	     nfcu2) ? cfg->max_nfcn_sel : card->uhs2_info.uhs2_setting.n_fcu;

	card->uhs2_info.uhs2_setting.speed_range =
	    os_min(card->uhs2_info.uhs2_setting.speed_range,
		   cfg->max_speed_range_sel);
	card->uhs2_info.uhs2_setting.pwr_mode = (byte) cfg->fast_low_pwr_sel;

	card->uhs2_info.uhs2_setting.half_supp =
	    os_min(card->uhs2_info.uhs2_cap.half_supp, cfg->half_full_sel);

}

/*
 * Function Name: uhs2_update_card_setting_degrade
 *
 * Abstract: This Function is used generate uhs2 card setting by degrade info
 *
 * Input:
 *			sd_card_t *card :
 *			sd_host_t *host
 *
 */
static void uhs2_update_card_setting_degrade(sd_card_t *card)
{
	if (card->degrade_uhs2_range)
		card->uhs2_info.uhs2_setting.speed_range = 0;
}

/*
 * Function Name: uhs2_update_card_setting_thermal
 *
 * Abstract: This Function is used generate uhs2 card setting by degrade info
 *
 * Input:
 *			sd_card_t *card :
 *			sd_host_t *host
 *
 */
static void uhs2_update_card_setting_thermal(sd_card_t *card)
{

	if (card->thermal_enable == 0)
		return;
}

/*
 * Function Name: uhs2_cfg_set_card
 *
 * Abstract: This Function is used to set card's configuration
 *
 * Input:
 *	sd_card_t *card : The Command will send to which  Card
 *			sd_command_t *sd_cmd,
 *
 * Return value:
 *			If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *			otherwize reutrn FALSE
 */
static bool uhs2_cfg_set_card(sd_card_t *card, sd_command_t *sd_cmd,
			      sd_host_t *host)
{
	u32 payload[2];
	bool result = FALSE;

	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* Set device Phy Setting register */
	os_memset(payload, 0, sizeof(payload));
	payload[0] = (card->uhs2_info.uhs2_setting.speed_range << 6);
	payload[1] = (card->uhs2_info.uhs2_setting.n_lss_syn) |
	    (card->uhs2_info.uhs2_setting.n_lss_dir << 4);
	result =
	    uhs2_access_reg(card, sd_cmd, UHS2_IOADDR_PHY_SETL, FALSE, TRUE, 2,
			    payload);
	if (result == FALSE) {
		DbgErr("set uhs2 cfg phy setting failed ret\n");
		goto exit;
	}

	/* Set device Link and trans registers */
	os_memset(payload, 0, sizeof(payload));
	payload[0] = (card->uhs2_info.uhs2_setting.n_fcu << 8) |
	    (card->uhs2_info.uhs2_setting.retry_cnt << 16) |
	    (card->uhs2_info.uhs2_setting.max_blk_len << 20);
	payload[1] = (card->uhs2_info.uhs2_setting.n_data_gap);
	result =
	    uhs2_access_reg(card, sd_cmd, UHS2_IOADDR_LINKT_SETL, FALSE, TRUE,
			    2, payload);
	if (result == FALSE) {
		DbgErr("set uhs2 cfg linktran setting failed\n");
		goto exit;
	}

	/* Set device general setting registers */
	os_memset(payload, 0, sizeof(payload));
	payload[0] = (card->uhs2_info.uhs2_setting.pwr_mode) |
	    (card->uhs2_info.uhs2_setting.lanes << 8);
	result =
	    uhs2_access_reg(card, sd_cmd, UHS2_IOADDR_GEN_SETL, FALSE, TRUE, 1,
			    payload);
	if (result == FALSE) {
		DbgErr("set uhs2 cfg gen setting failed ret\n");
		goto exit;
	}

	/* Set device to active status */
	os_memset(payload, 0, sizeof(payload));
	/* Set config complete      */
	payload[0] = BIT31;
	result =
	    uhs2_access_reg(card, sd_cmd, UHS2_IOADDR_GEN_SETH, FALSE, TRUE, 1,
			    payload);
	if (result == FALSE) {
		DbgErr("set uhs2 cfg set active failed\n");
		return result;
	}

exit:
	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, result);
	return result;
}

/*
 * Function Name: uhs2_enter_dmt
 *
 * Abstract: This Function is used to
 *
 * Input:
 *	sd_card_t *card : The Command will send to which  Card
 *
 * Return value:
 *	If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *			otherwize reutrn FALSE
 */
bool uhs2_enter_dmt(sd_card_t *card, sd_command_t *sd_cmd, sd_host_t *host,
		    bool hbr)
{
	bool result = FALSE;
	u32 payload = hbr ? UHS2_GODRM_HBNEN : 0;
	byte retry_cnt = 2;

	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT | FEATURE_CARD_OPS,
		NOT_TO_RAM, "Enter %s\n", __func__);
retry:
	retry_cnt--;
	result =
	    uhs2_native_ccmd(card, sd_cmd, UHS2_IOADDR_GODMT, FALSE, TRUE, 1,
			     &payload);
	if (result == FALSE) {
		if (retry_cnt > 0) {
			if (uhs2_is_uncoverable(sd_cmd))
				goto exit;

			result = uhs2_trans_abort(card, sd_cmd);
			if (result == FALSE)
				goto exit;
			goto retry;
		}
		goto exit;
	}
	result = host_uhs2_go_dmt(host, hbr);

exit:
	if (result == FALSE)
		DbgErr("UHS2 go dmt failed hbr=%d\n", hbr);
	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT | FEATURE_CARD_OPS,
		NOT_TO_RAM, "Exit %s ret=%d\n", __func__, result);
	return result;
}

/*
 * Function Name: uhs2_resume_dmt
 * Abstract: This Function is used to
 *
 * Input:
 *	sd_card_t *card : The Command will send to which  Card
 *
 * Return value:
 *			If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *	otherwize reutrn FALSE
 */
bool uhs2_resume_dmt(sd_card_t *card, sd_command_t *sd_cmd, sd_host_t *host,
		     bool hbr)
{
	return host_uhs2_resume_dmt(host, hbr);
}

/*
 * Function Name: uhs2_card_configuration
 *
 * Abstract: This Function is used to
 *
 * Input:
 *	sd_card_t *card : The Command will send to which  Card
 *
 * Return value:
 *	If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *	otherwize reutrn FALSE
 */
bool uhs2_card_configuration(sd_card_t *card, sd_command_t *sd_cmd,
			     sd_host_t *host)
{
	bool result = FALSE;
	uhs2_info_t info;

	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	result = uhs2_cfg_set_card(card, sd_cmd, host);
	if (result == FALSE)
		goto exit;

	os_memcpy(&info, &card->uhs2_info.uhs2_setting, sizeof(uhs2_info_t));
	info.speed_range = 0;
	info.lanes = 0;

	host_uhs2_cfg_set(host, &info, FALSE);

	if (card->uhs2_info.uhs2_setting.lanes == 0
	    && card->uhs2_info.uhs2_setting.speed_range == 0)
		goto exit;

	result = uhs2_enter_dmt(card, sd_cmd, host, FALSE);
	if (result == FALSE)
		goto exit;

	info.speed_range = card->uhs2_info.uhs2_setting.speed_range;
	info.lanes = card->uhs2_info.uhs2_setting.lanes;
	host_uhs2_cfg_set(host, &info, TRUE);

	result = uhs2_resume_dmt(card, sd_cmd, host, FALSE);
	if (result == FALSE)
		goto exit;

exit:
	if (result == FALSE)
		DbgErr("UHS2 card configuration failed\n");
	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, result);
	return result;
}

bool uhs2_init_stage2(sd_card_t *card)
{

	bool result = FALSE;
	sd_command_t sd_cmd;

	/* Init stage always do pm setting */
	bool bchg = TRUE;

	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	os_memset(&sd_cmd, 0, sizeof(sd_command_t));

	result = sd_init_get_info(card);
	if (result == FALSE) {
		DbgErr("SD Card get info failed\n");
		goto exit;
	}

	if (card_need_get_info(card)) {
		result = sd_switch_function_check(card, &sd_cmd);
		if (!result) {
			DbgErr("uhs2 swich function check failed.\n");
			goto exit;
		}
	}

	/* 14. Swich function check/set */

	{
		/*
		 * 14.2 Swich function check set.
		 * - Driver Strength,
		 * - Access Mode,
		 * - Power Limit
		 * - Change clock freq
		 */
		result = sd_switch_power_limit(card, &sd_cmd, &bchg);
		if (result == FALSE) {
			DbgErr("uhs2 switch power limit failed\n");
			goto exit;
		}

	}
exit:
	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, result);
	return result;
}

/*
 * Function Name: uhs2_card_init
 *
 * Abstract: This Function is used init uhs2 card
 *
 * Input:
 *	sd_card_t *card : The Command will send to which  Card
 *
 * Return value:
 *	If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *			otherwize reutrn FALSE
 */
bool uhs2_card_init(sd_card_t *card)
{
	bool result = FALSE;
	sd_command_t sd_cmd;
	sd_host_t *host = card->host;

	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* Try to init as UHS2 card */
	card->card_type = CARD_UHS2;
	result = uhs2_devinit_flow(card, &sd_cmd, host);
	if (result == FALSE)
		goto exit;

	/* do enumeration */
	result = uhs2_dev_enumeration(card, &sd_cmd);
	if (result == FALSE)
		goto exit;

	if (card_need_get_info(card)) {
		/* Get card capabilities */
		result = uhs2_card_get_caps(card, &sd_cmd, host);
		if (result == FALSE)
			goto exit;

	}

	uhs2_get_card_setting_host(card, host);
	uhs2_update_card_setting_vendor(card, host);
	uhs2_update_card_setting_degrade(card);
	uhs2_update_card_setting_thermal(card);

	result = uhs2_card_configuration(card, &sd_cmd, host);
	if (result == FALSE)
		goto exit;

	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, TO_RAM,
		"uhs2 setting n_fcu=%d lss_dir=%d lss_syn=%d datagap=%d\n",
		card->uhs2_info.uhs2_setting.n_fcu,
		card->uhs2_info.uhs2_setting.n_lss_dir,
		card->uhs2_info.uhs2_setting.n_lss_syn,
		card->uhs2_info.uhs2_setting.n_data_gap);
	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, TO_RAM,
		"uhs2 Setting range=%d half=%d lpm=%d\n",
		card->uhs2_info.uhs2_setting.speed_range,
		card->uhs2_info.uhs2_setting.half_supp,
		card->uhs2_info.uhs2_setting.pwr_mode);

	result = sd_card_identify(card);
	if (result == FALSE)
		goto exit;

	result = sd_card_select(card);
	if (result == FALSE)
		goto exit;

	if (card->locked == TRUE) {
		DbgWarn(MODULE_UHS2_CARD, NOT_TO_RAM, "uhs2 card is locked\n");
		goto exit;
	}

	result = card_init_stage2(card);
	if (result == FALSE) {
		DbgErr("SD init stage 2 failed.\n");
		goto exit;
	}

exit:
	if (result == FALSE)
		DbgErr("UHS2 card init failed\n");
	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, result);
	return result;
}

/*
 * Function Name: uhs2_degrade_policy
 *
 * Abstract: This Function is used set uhs2 degrade flag
 *
 * Input:
 *	sd_card_t *card : The Command will send to which  Card
 *  sd_command_t *sd_cmd
 *
 * Return value:
 *  If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *			otherwize reutrn FALSE
 */
void uhs2_degrade_policy(sd_card_t *card, sd_command_t *sd_cmd)
{
	if (card->degrade_freq_level < CARD_DEGRADE_FREQ_TIMES) {
		card->degrade_freq_level++;
		goto exit;
	}

	if (sd_cmd != NULL && card->uhs2_info.uhs2_setting.half_supp
	    && (card->degrade_uhs2_half == 0)) {
		card->degrade_uhs2_half = 1;
		goto exit;
	}

	if (card->degrade_uhs2_range == 0
	    && card->uhs2_info.uhs2_setting.speed_range) {
		card->degrade_uhs2_range = 1;
		goto exit;
	}

	card->degrade_uhs2_legacy = 1;
	card->quick_init = 0;
	card->card_type = CARD_NONE;
	card->degrade_freq_level = 0;

exit:
	DbgErr("UHS2 degrade range=%d freq_level=%d half=%d legacy=%d\n",
	       card->degrade_uhs2_range, card->degrade_freq_level,
	       card->degrade_uhs2_half, card->degrade_uhs2_legacy);
}

static bool uhs2_read_status_reg(sd_card_t *card, sd_command_t *sd_cmd)
{
	bool result = FALSE;
	u32 payload = 0;

	DbgInfo(MODULE_UHS2_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s\n", __func__);

	result =
	    uhs2_access_reg(card, sd_cmd, UHS2_IOADDR_ST_REG, FALSE, FALSE, 1,
			    &payload);

	if (result == FALSE)
		DbgErr("uhs2 read status reg failed\n");

	DbgInfo(MODULE_UHS2_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, result);
	return result;
}

/*
 * Function Name: uhs2_sd_error_recovery
 *
 * Abstract: This Function is used do error recovery for uhs2
 *
 * Input:
 *			sd_card_t *card : The Command will send to which  Card
 *  sd_command_t *sd_cmd
 *
 * Return value:
 *	If the routine succeeds, it must return TRUE, and fill trans_reg_t  part.
 *	otherwize reutrn FALSE
 */

bool uhs2_sd_error_recovery(sd_card_t *card, sd_command_t *sd_cmd)
{
	bool result = FALSE;
	sd_command_t recover_cmd;

	DbgInfo(MODULE_UHS2_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s\n", __func__);

	if (sd_cmd == NULL)
		goto exit;

	/* If uncoverable do fullreset recover directly */
	if (uhs2_is_uncoverable(sd_cmd))
		goto full_reset;

	host_uhs2_reset(card->host, FALSE);

	/* do sd-tran */
	result = uhs2_trans_abort(card, &recover_cmd);
	if (result == FALSE)
		goto full_reset;

	result = uhs2_read_status_reg(card, &recover_cmd);
	if (result == FALSE)
		goto full_reset;

	/* send cmd12 and check whether */
	card_send_command12(card, &recover_cmd);
	if (recover_cmd.uhs2_nack != 0)
		goto full_reset;

	result = card_check_rw_ready(card, &recover_cmd, 150);
	if (result == FALSE)
		goto full_reset;

	goto exit;

full_reset:
	DbgErr("Do Full reset uhs2 recovery\n");
	host_uhs2_reset(card->host, FALSE);
	result = uhs2_full_reset_card(card);
	if (result)
		result = card_init(card, 1, TRUE);

exit:
	DbgInfo(MODULE_UHS2_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, result);
	return result;
}

u32 card_get_uhs2_freq(sd_card_t *card)
{
	sd_host_t *host = card->host;
	/* cfg_max_freq_item_t  *freq = &(host->cfg->host_item.max_freq_item); */
	u16 index = 0;
	u32 value;

	if (host->cfg == NULL || host->cfg->dmdn_tbl == NULL) {
		DbgErr("host cfg is null\n");
		return 0;
	}

	index = (u16) FREQ_UHS2M_START_INDEX + card->degrade_freq_level;
	if (index > (u16) FREQ_UHS2M_DEGRE_INDEX)
		index = (u16) FREQ_UHS2M_DEGRE_INDEX;
	value = host->cfg->dmdn_tbl[index];
	DbgInfo(MODULE_UHS2_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Get Uhs2 Dmdn=0x%08X\n", value);

	return value;
}
