// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: card_ddr200_support.c
 *
 * Abstract: check whether the card supports DDR200/DDR225 mode
 *
 * Version: 1.00
 *
 * Author: Fred
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 12/17/2021   Creation    Fred
 */

#include "../include/basic.h"
#include "../include/hostapi.h"
#include "../include/debug.h"
#include "card_ddr200_support.h"

bool sandisk_ddr_support(sd_card_t *card, bool ddr_mode)
{
	bool ret = FALSE;
	u32 prv_tmp;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"oemid is 0x%04x,\n"
		"prod_name[0] is %x,\n"
		"prod_name[1] is %x,\n"
		"prod_name[2] is %x,\n"
		"prod_name[3] is %x,\n"
		"prod_name[4] is %x,\n"
		"prv is 0x%x\n", card->info.cid.oemid,
		card->info.cid.prod_name[0], card->info.cid.prod_name[1],
		card->info.cid.prod_name[2], card->info.cid.prod_name[3],
		card->info.cid.prod_name[4], card->info.cid.prv);

	/*
	 * check whether support DDR200 or DDR225
	 * support DDR200 mode if prv is 0x85
	 * support DDR225 mode if prv is 0x86
	 */
	if (ddr_mode)
		prv_tmp = 0x85;
	else
		prv_tmp = 0x86;

	if (card->info.cid.oemid == 0x4453
	    && card->info.cid.prod_name[0] == 0x53
	    && (card->info.cid.prod_name[1] == 0x4E
		|| card->info.cid.prod_name[1] == 0x46
		|| card->info.cid.prod_name[1] == 0x52))
		ret = TRUE;
	else if ((card->info.cid.oemid == 0x4453
		  || card->info.cid.oemid == 0x5744)
		 && card->info.cid.prv == prv_tmp)
		ret = TRUE;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

bool lexar_transend_ddr200_support(sd_card_t *card)
{
	bool ret = FALSE;
	card_info_t *card_info = &(card->info);

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"reserved is 0x%x, Group 2 vendor spcific is 0x%x\n",
		card->info.cid.reserved,
		card_info->sw_func_cap.sd_command_system);

	if ((card->info.cid.reserved == 0xA)
	    && ((card_info->sw_func_cap.sd_command_system) & (1 << 6)))
		ret = TRUE;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

bool phison_kingston_ddr200_support(sd_card_t *card)
{
	bool ret = FALSE;
	card_info_t *card_info = &(card->info);

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"sd_specx is 0x%04x, reserved_B0 is 0x%04x, reserved_B1 is 0x%04x\n",
		card_info->scr.sd_specx, card_info->scr.reserved_B0,
		card_info->scr.reserved_B1);

	if ((card_info->scr.sd_specx >= 2)
	    && (card_info->scr.reserved_B0 == 0x32)
	    && (card_info->scr.reserved_B1 == 0x64))
		ret = TRUE;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

bool manuefecture_ddr200_support(sd_card_t *card, u32 check_methood)
{
	bool ret = FALSE;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	card->ddr225_card_flag = FALSE;

	if (card->info.cid.prv == 0x86) {
		ret = sandisk_ddr_support(card, FALSE);
		if (ret) {
			card->ddr225_card_flag = TRUE;
			DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"DDR225 Check Stag: host support DDR225 mode\n");
		} else {
			DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"DDR225 Check Stag: host not support DDR225 mode\n");
		}

		goto exit;
	}

	switch (check_methood) {
	case SANDISK:
		ret = sandisk_ddr_support(card, TRUE);
		break;
	case LEXAR:
	case TRANSEND:
		ret = lexar_transend_ddr200_support(card);
		break;
	case PHISON:
	case KINGSTON:
		ret = phison_kingston_ddr200_support(card);
		break;
	default:
		break;
	}

exit:
	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);
	return ret;
}

bool sd_ddr_support(sd_card_t *card)
{
	byte i = 0;
	bool ret = FALSE;
	sd_host_t *host = card->host;
	card_info_t *card_info = &(card->info);
	cfg_item_t *cfg = card->host->cfg;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if ((sdhci_readl(host, 0x110) & (1 << 16)) &&
	    (sdhci_readl(host, 0x3e) & (1 << 3))) {
		if ((card_info->sw_func_cap.sd_access_mode & (1 << 3)) &&
		    (card_info->sw_func_cap.sd_command_system) & (1 << 6)) {
			while (i <= MAX_DDR200_CHECK_METHOD) {
				ret = manuefecture_ddr200_support(card, i);
				if (ret)
					break;

				i++;
			}
		}
	}

	/*
	 * 1.card support DDR200
	 * 2.driver registry control
	 */
	if (ret && (cfg->card_item.test_max_access_mode.value == 0x5))
		ret = TRUE;

	DbgInfo(MODULE_SD_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit(%d) %s\n",
		ret, __func__);

	return ret;
}
