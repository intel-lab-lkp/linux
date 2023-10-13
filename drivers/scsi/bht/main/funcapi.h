/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: funcapi.h
 *
 * Abstract: This include file define interface for each function
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

void func_autotimer_init(bht_dev_ext_t *pdx);

void pm_init(bht_dev_ext_t *pdx);

void func_timer_thread(bht_dev_ext_t *pdx);

/*
 * This function is called by thread
 * (1) If auotpowered off  wake up card
 * (2) Thermal contorl if tagqueue is not running
 */
void tagio_event_handler(bht_dev_ext_t *pdx);
void genio_event_hanlder(bht_dev_ext_t *pdx);
void rtd3_event_hanlder(bht_dev_ext_t *pdx);

void testcase_main(bht_dev_ext_t *pdx, byte type);
void testcase_init(bht_dev_ext_t *pdx);
