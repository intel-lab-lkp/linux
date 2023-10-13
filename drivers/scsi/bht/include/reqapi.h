/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: reqapi.h
 *
 * Abstract: This File is used to declare interface for OSEntry layer
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

/*
 *	uninit os related structure: thread, timer, buffer, tagqueue
 */
s32 req_global_uninit(bht_dev_ext_t *pdx);

/*
 * init thread, timer, buffer, tagqueue
 * host cap init, and vendor reg setting
 * host interrupt and init to workable status
 * card and host software structure init
 * caller init pci_dev_t and global cfg
 */
s32 req_global_init(bht_dev_ext_t *pdx);
void req_global_reinit(bht_dev_ext_t *pdx);

/*
 * pm related function
 */
void failsafe_fct(bht_dev_ext_t *pdx);
void req_enter_d3(bht_dev_ext_t *pdx);
void req_enter_d0(bht_dev_ext_t *pdx);
void req_enter_d0_sync(bht_dev_ext_t *pdx);
void req_pre_enter_d3(bht_dev_ext_t *pdx);

void pcie_weakup(bht_dev_ext_t *pdx, u32 Sx_flag, bool enable);

void thread_handle_card_event(bht_dev_ext_t *pdx);
e_req_result thread_wakeup_card(bht_dev_ext_t *pdx);
/*
 * Handle
 */
s32 req_os_shutdown(bht_dev_ext_t *pdx);

e_req_result req_tag_io_add(bht_dev_ext_t *pdx, srb_ext_t *srb_ext);
e_req_result req_gen_io_add(bht_dev_ext_t *pdx, srb_ext_t *srb_ext);

void req_bus_reset(bht_dev_ext_t *pdx);

bool req_card_ready(bht_dev_ext_t *pdx);

e_req_result req_eject(bht_dev_ext_t *pdx, srb_ext_t *srb_ext);

e_req_result req_chk_card_info(bht_dev_ext_t *pdx, srb_ext_t *srb_ext);

void req_cancel_all_io(bht_dev_ext_t *pdx);

void thread_main(void *param);
bool thread_is_lock(bht_dev_ext_t *pdx, e_event_t event);
#define GEN_IO_CODE_INIT_CARD	0
#define GEN_IO_CODE_EJECT	1
#define GEN_IO_CODE_PIORW	2
#define GEN_IO_CODE_CPRM	3
#define GEN_IO_CODE_IO		4
#define GEN_IO_CODE_NSM		5

#define GEN_IO_CODE_RECFG	6
#define GEN_IO_CODE_CSD		7

#define ENTRY_S3 3
#define ENTRY_S4 4
#define ENTRY_S5 5
