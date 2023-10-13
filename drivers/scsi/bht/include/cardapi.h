/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: cardapi.h
 *
 * Abstract: Include card related api definitions
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

#include "basic.h"

bool card_send_sdcmd(sd_card_t *card,
		     sd_command_t *sd_cmd,
		     byte cmd_index,
		     u32 argument,
		     u32 cmdflag, e_data_dir dir, byte *data, u32 datalen);
bool card_send_sdcmd_timeout(sd_card_t *card,
			     sd_command_t *sd_cmd,
			     byte cmd_index,
			     u32 argument,
			     u32 cmdflag,
			     e_data_dir dir,
			     byte *data, u32 datalen, u32 timeout);
/*
 *  init card control info, such as degrade info
 *  bind card to host
 *  and memset function
 */
void card_stuct_init(bht_dev_ext_t *pdev_ext);

/*
 *   this function is called by card remvoe and enter pm
 *   this function will only clear software  flag
 */
void card_stuct_uinit(sd_card_t *card);

bool gg8_sd70_card_init(sd_card_t *card);

bool card_init(sd_card_t *card, int retry_num, bool bfullreset);

bool card_init_stage2(sd_card_t *card);

void card_power_off(sd_card_t *card, bool directly);

bool card_is_poweroff(sd_card_t *card);

bool card_thermal_control(sd_card_t *card);

bool card_stop_infinite(sd_card_t *card, bool recover, sd_command_t *pcmd);
bool card_enter_sleep(sd_card_t *card, bool recover, bool deepslp);
bool card_resume_sleep(sd_card_t *card, bool recover);

/*
 *	Error recover will return ok or not
 *	if err_recovery need to do power off retry, it will set init card event
 */

bool card_piorw_data(sd_card_t *card, u32 sec_addr, u32 sec_cnt,
		     e_data_dir dir, byte *buff);

e_req_result card_recovery_flow(sd_card_t *card, sd_command_t *sd_cmd);

bool card_set_blkcnt(sd_card_t *card, sd_command_t *sd_cmd, u32 blkcnt);

bool card_dma_rw_data(sd_card_t *card, u32 dma_mode, u32 sec_addr, u32 sec_cnt,
		      e_data_dir dir, byte *data, sg_list_t *sglist,
		      u32 sg_len, sd_command_t *cmd_err);

bool card_adma2_rw_inf(sd_card_t *card, u32 sec_addr, u32 sec_cnt,
		       e_data_dir dir, sg_list_t *sglist, u32 sg_len,
		       sd_command_t *cmd_err);

bool card_read_csd(sd_card_t *card, byte *data);

bool card_program_csd(sd_card_t *card, byte *data);
