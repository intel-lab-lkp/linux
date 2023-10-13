/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: globalcfg.h
 *
 * Abstract: This Include file is used for Global Configuration Macros
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

#ifndef _GLOBALCFG_H
#define _GLOBALCFG_H

#ifdef __linux__
#define CFG_OS_LINUX	1
#endif

/* #define MultiThread */

#define SD_BLOCK_LEN	512
#define GLOBAL_ENABLE_BOOT	0
/* Below Item is for EMMC boot Setting only when GLOBAL_ENABLE_BOOT is 1 */
#define EMMC_BOOT_NONE	0x00000000
#define EMMC_BOOT_HS200 0x80000022
#define EMMC_BOOT_HS400 0x80000042
#define EMMC_BOOT_DDR50_33V 0x80002001
#define EMMC_BOOT_DDR50_18V 0x80002003
#define EMMC_BOOT_HS_33V 0x80002000
#define EMMC_BOOT_HS_18V 0x80002002

#define GLOBAL_EMMC_BOOT_CFG EMMC_BOOT_NONE
#define GET_TIMER_PRECISE 1

/* Max Transfer Size */
#define CFG_MAX_TRANSFER_LENGTH	(1024 * 1024)
/* 256 + 1 */
#define MAX_SGL_RANGE             (258)
#define DBG	1
#define _DEBUG  0
#define BHT_LINUX_ENABLE_RTD3	0

#define MAX_WORK_QUEUE_SIZE	(32)
#define TQ_WORK_QUEUE_SIZE 2
#define ADMA2_MAX_DESC_LINE_SIZE   (256)
#define GBL_ASYNC_PERFEATCH_IO 1

#define MAX_EMMC_PARTION	3

#define SUPPORT_CHIP_COUNT	10

/* 10S */
#define SOFT_INTR_TIMEOUT	(10 * 1000)

#define AUTO_TIMER_TICK	20

#define BHT_PDX_SIGNATURE	0xAA5555AA
#define TUNING_ADDRESS_OFFSET 0xFF

#define CARD_FIRST_INIT_RETRY	5
#define CARD_REINIT_RETRY		3
#define CARD_INIT_DEGARDE_TIME		2

#define CARD_DEGRADE_FREQ_TIMES		3

#define DEVICE_STATUS_CHIPLOST 1
#define DEVICE_STATUS_D0_MODE  2
#define DEVICE_STATUS_D3_MODE  3

#endif
