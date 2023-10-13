/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: cardcommon.h
 *
 * Abstract: Include card related common functions.
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

#ifndef _CARDCOMMON_H
#define _CARDCOMMON_H

#include "../include/card.h"

/* SD Legacy (UHSI, HS, DS) card initialization */
bool sd_legacy_init(sd_card_t *card);

/* MMC or eMMC card initialization */
bool emmc_init(sd_card_t *card, bool bemmc);
bool emmc_init_stage2(sd_card_t *card);
bool emmc_tuning(sd_card_t *card, sd_command_t *sd_cmd);

bool sd_tuning(sd_card_t *card, sd_command_t *sd_cmd, u32 timeout);

bool card_get_card_status(sd_card_t *card,
			  sd_command_t *sd_cmd, u32 *card_status);

bool card_reset_card(sd_card_t *card, sd_command_t *sd_cmd);

bool card_all_send_cid(sd_card_t *card, sd_command_t *sd_cmd);

bool card_get_rca(sd_card_t *card, sd_command_t *sd_cmd);

bool card_select_card(sd_card_t *card, sd_command_t *sd_cmd);

bool card_get_csd(sd_card_t *card, sd_command_t *sd_cmd);

bool card_send_command12(sd_card_t *card, sd_command_t *sd_cmd);

bool card_set_block_len(sd_card_t *card, sd_command_t *sd_cmd, u32 arg);

bool uhs2_card_init(sd_card_t *card);
void card_power_on(sd_card_t *card);

/*
 * (1) If uhs2 call uhs2 cmd handler
 * (2) generate sd_cmd_t structure and sd_data structure(pio only)
 * (3) call cmd_generate_reg(sd_cmd)
 * (4) call cmd_execute
 * (5) do error recover if necessary
 * (6) return result
 */

bool card_send_sdcmd(sd_card_t *card,
		     sd_command_t *sd_cmd,
		     byte cmd_index,
		     u32 argument,
		     u32 cmdflag, e_data_dir dir, byte *data, u32 datalen);

bool uhs2_native_ccmd(sd_card_t *card, sd_command_t *sd_cmd,
		      u16 ioaddr, bool broadcast, bool rwcmd, byte payload_num,
		      u32 *payload);

bool card_send_sdcmd_dma_timeout(sd_card_t *card,
				 sd_command_t *sd_cmd,
				 sd_data_t *sd_data,
				 byte cmd_index,
				 u32 argument,
				 u32 cmdflag,
				 e_data_dir dir,
				 byte *data, u32 datalen, u32 timeout);

bool card_select_card(sd_card_t *card, sd_command_t *sd_cmd);

bool sd_switch_function_check(sd_card_t *card, sd_command_t *sd_cmd);

bool sd_switch_function_set_pl(sd_card_t *card,
			       sd_command_t *sd_cmd, byte power_limit);

bool card_wr_protect(sd_card_t *card);

bool sd_card_identify(sd_card_t *card);

bool sd_init_get_info(sd_card_t *card);

bool sd_init_stage2(sd_card_t *card);

bool uhs2_enter_dmt(sd_card_t *card, sd_command_t *sd_cmd, sd_host_t *host,
		    bool hbr);
bool uhs2_resume_dmt(sd_card_t *card, sd_command_t *sd_cmd, sd_host_t *host,
		     bool hbr);

bool sd_card_select(sd_card_t *card);
bool uhs2_init_stage2(sd_card_t *card);
bool uhs2_full_reset_card(sd_card_t *card);

bool sd_switch_power_limit(sd_card_t *card, sd_command_t *sd_cmd, bool *bchg);

bool card_check_rw_ready(sd_card_t *card, sd_command_t *sd_cmd,
			 int timeout_ms);

void card_legacy_change_clock(sd_card_t *card, u32 clk_freq_khz,
			      bool ddr50_mode);

bool card_need_get_info(sd_card_t *card);

bool card_deselect_card(sd_card_t *card, sd_command_t *sd_cmd);

bool sd_program_csd(sd_card_t *card, sd_command_t *sd_cmd, byte *data);

bool sd_read_csd(sd_card_t *card, sd_command_t *sd_cmd, byte *data);

#endif
