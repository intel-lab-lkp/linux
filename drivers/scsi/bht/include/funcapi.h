/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: funcapi.h
 *
 * Abstract: include functional API
 *
 * Version: 1.00
 *
 * Environment:	OS Independent
 */

void func_autotimer_stop(bht_dev_ext_t *pdx);
void func_autotimer_start(bht_dev_ext_t *pdx);

void func_timer_callback(bht_dev_ext_t *pdx);
void func_autotimer_cancel(bht_dev_ext_t *pdx);

void thermal_init(bht_dev_ext_t *pdx);
void thermal_uninit(bht_dev_ext_t *pdx);
void func_thermal_update_time(bht_dev_ext_t *pdx);
bool func_thermal_control(sd_card_t *card);

bool func_cprm(sd_card_t *card, request_t *req);
bool func_io_reg(sd_card_t *card, request_t *req);
bool func_nsm(sd_card_t *card, request_t *req, bht_dev_ext_t *pdx);
bool erase_rw_blk_end_set(sd_card_t *card, sd_command_t *sd_cmd,
			  u32 sec_addr);
bool erase_rw_blk_start_set(sd_card_t *card, sd_command_t *sd_cmd,
			    u32 sec_addr);
bool func_erase(sd_card_t *card, sd_command_t *sd_cmd);

#define CPRM_IO_GETCSD   11
#define CPRM_IO_GETMKB   12
#define CPRM_IO_GETMID   13
#define CPRM_IO_GETWP    14

#define CPRM_IO_SETCERRN   15
#define CPRM_IO_GETCERRN   16
#define CPRM_IO_GETCERRES  17
#define CPRM_IO_SETCERRES  18

#define CPRM_IO_CHANGE_SA   19

#define CPRM_IO_READ   21
#define CPRM_IO_WRITE  22

#define CPRM_IO_SECURE_READ   23
#define CPRM_IO_SECURE_WRITE  24
#define CPRM_IO_REMOVE_UNIT   25
#define CPRM_IO_GETSDHC	    26
#define IO_READ_PCI_REG   60
#define IO_WRITE_PCI_REG   61
#define IO_READ_MEM_REG   62
#define IO_WRITE_MEM_REG   63

#define IO_NSM_CMD48     70
#define IO_NSM_CMD49     71
#define IO_NSM_CMD58     72
#define IO_NSM_CMD59     73

#define IO_NSM_CMD42     74

/* DDSendCSD */
#define IO_NSM_CMD9       75
/* DDSendCID  */
#define IO_NSM_CMD10     76
/* DDSDStatus */
#define IO_NSM_ACMD13   77
/* DDSendSCR */
#define IO_NSM_ACMD51   78

/* DDProgramCSD */
#define IO_NSM_CMD27    79
/* DDGenCmd */
#define IO_NSM_CMD56    80

/* DDSwitchMode */
#define IO_NSM_CMD6      81
