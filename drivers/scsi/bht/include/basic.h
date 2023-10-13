/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: basic.h
 *
 * Abstract: This Include file used to define basic types
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

#ifndef _BASIC_H
#define _BASIC_H

#include "globalcfg.h"

#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/page.h>
#include <asm/byteorder.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/proc_fs.h>
#include <linux/reboot.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#include <scsi/sg.h>
#include <linux/stat.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/spinlock_types.h>
#include <linux/semaphore.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kdev_t.h>
#include <linux/highmem.h>
#include <linux/export.h>
#include <linux/wait.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi.h>

#define TRUE	1
#define FALSE	0

#define os_min(x, y) ((x > y) ? y:x)
#define os_max(x, y) ((x > y) ? x:y)
typedef void *PVOID;
typedef u8 byte;
typedef struct scsi_cmnd scsi_srb;
typedef u64 phy_addr_t;

typedef struct {
	u64 Address;
	u32 Length;
} ADDRESS_LIST;

typedef ADDRESS_LIST sg_list_t;

typedef enum {
	/* Sandstorm #0 */
	CHIP_SDS0 = 0,
	/* Sandstorm #1 */
	CHIP_SDS1,
	/* Fujin2 */
	CHIP_FUJIN2,
	/* Seabird */
	CHIP_SEABIRD,
	/* SeaEagle */
	CHIP_SEAEAGLE,
	/* GG8 */
	CHIP_GG8,
	/* SeaEagle2 */
	CHIP_SEAEAGLE2,
	CHIP_ALBATROSS,
	CHIP_SDS2_SD0,
	CHIP_SDS2_SD1,
	/* Last generate chip like FJ/RJ, ect. */
	CHIP_OLD,
	/* Chip type unknown */
	CHIP_UNKNOWN
} e_chip_type;

typedef enum {
	CARD_NONE = 0,
	CARD_ERROR,
	CARD_UHS2,
	CARD_SD,
	CARD_MMC,
	CARD_EMMC,
	CARD_SDIO,
	CARD_SD70,
	CARD_TEST
} e_card_type;

typedef enum {
	CARD_SDSC_V1 = 0,
	CARD_SDSC_V2V3,
	CARD_SDHC,
	CARD_SDXC
} e_capacity_type;

typedef enum {
	INF_NONE = 0,
	INF_BUILT,
	INF_CONTINUE
} e_infinite_mode;

typedef enum {
	TRANS_NONDATA = 0,
	TRANS_SDMA,
	TRANS_ADMA2,
	TRANS_ADMA3,
	TRANS_PIO,
	TRANS_ADMA2_SDMA_LIKE,
} e_trans_type;

typedef enum {
	DATA_DIR_NONE = 0,
	/* Card -> host */
	DATA_DIR_IN,
	/* Host -> card */
	DATA_DIR_OUT
} e_data_dir;

/*
 *	init  request_t structure and
 *	cmd_index = 0 means don't use cmd_index to generate command
 *	upper layer need to fill srb_ext_t->req
 */
typedef enum {
	REQ_RESULT_PENDING = 0,
	REQ_RESULT_OK,
	REQ_RESULT_PROTECTED,
	REQ_RESULT_OVERSIZE,
	REQ_RESULT_QUEUE_BUSY,
	REQ_RESULT_INVALID_CMD,
	REQ_RESULT_NO_CARD,
	REQ_RESULT_ACCESS_ERR,
	REQ_RESULT_ABORT
} e_req_result;

typedef enum {
	/* to use Tag queue io */
	REQ_TYPE_TAG_IO = 0,
	/* to use general io */
	REQ_TYPE_GEN_IO
} e_req_type;

typedef struct {
	byte *buff;
	u32 len;
	u32 ofs;
} virt_buffer_t;

typedef struct {
	u32 len;
	/* phy address */
	phy_addr_t pa;
	/* virtual address */
	void *va;
} dma_desc_buf_t;

#include "osapi.h"
#include "cfgmng.h"
#include "card.h"
#include "function.h"
#include "tq.h"

typedef void (*cb_srb_done_t)(void *pdx, void *srb_ext);

typedef struct {
	/* enum RWReq, ioctl */
	e_req_type type;

	/* srb buffer */
	byte *srb_buff;
	/* srb buffer sg list */
	sg_list_t srb_sg_list[MAX_SGL_RANGE];
	u32 srb_sg_len;

	bool gg8_ddr200_workaround;
	req_card_access_info_t last_req;

	e_data_dir data_dir;
	cb_srb_done_t srb_done_cb;
	e_req_result result;

	union {
		struct {
			u32 sec_addr;
			u32 sec_cnt;

			/* this indicate whether use cmd_index to generate sd_cmd */
			byte use_cmd;
			/* this is valid when use_cmd is 1 */
			byte cmd_index;
		} tag_req_t;

		struct {
			u32 code;
			u32 data_len;
			u32 arg1;
			u32 arg2;

		} gen_req_t;
	};

} request_t;

typedef struct srb_ext_t {
	/* t_dev_ext_t pdx; */
	scsi_srb *psrb;

	request_t req;

	/* below item is maintained by card layer, defined here just for apply memory */
	struct srb_ext_t *prev;
	sd_command_t cmd;
	struct srb_ext_t *next;
} srb_ext_t;

typedef struct {
	byte scsi_eject;
	byte sense_key;
	byte sense_code;
	/* when this bit change from 1->0 or 0->1 then bus change occur */
	byte last_present;
	byte own_id;
	byte prevent_eject;
} scsi_mng_t;

typedef void (*cb_soft_intr_t)(void *);
typedef struct {
	void *data;
	completion_t completion;
	cb_soft_intr_t cb_func;
	byte enable;
} soft_irq_t;

typedef struct {
	bool enable;
	bool use_i2c;
	bool timeout;
	bool enable_timer_chk;
	u32 check_period_ms;
	u32 last_check_ms;
} thermal_t;

typedef struct {
	/* Below info is used to check 1MB data transfer */
	u64 tick_start;
	u64 tick_thr_start;
	u64 tick_io_end;
	u64 last_dir;

	u64 avg_start_2_thr;
	u64 avg_thr_2_iodone;
	u64 avg_iodone_2_next;
	u32 io_cnt;

	/* Below info is used to report performance */
	u64 start_io_tick;
	u64 io_duration;
} tPerTick;

typedef struct {
	u32 address;
	u32 size;
} cprm_t;

typedef struct {
	u32 address;
	u32 size;
} nsm_t;

typedef struct {
	/* 0 no test, 1: with card test, 2 host only test */
	byte test_type;
	s32 test_id;
	u32 test_loop;
	s32 test_param1;
	s32 test_param2;
} testcase_t;

typedef struct {
	byte pre_cid[16];
	u64 pre_sec_count;
	bool s3s4_resume_for_card_init;
	bool card_current_present;
} pre_card_info_t;

typedef struct bht_dev_ext {
	nsm_t nsm;
	cprm_t cprm;
	/* store host related variables */
	sd_host_t host;
	/* store card related variables */
	sd_card_t card;
	pre_card_info_t pre_card;
	/* store os related viraiable thread dpc event and so on */
	os_struct os;
	/* store auot function related variables */
	time_struct auto_timer;
	/* store dynamic configuratin info */
	cfg_item_t *cfg;
	tag_queue_t tag_queue;
	dma_trans_api_t dma_api;
	pm_state_t pm_state;
	dma_desc_buf_t dma_buff;
	soft_irq_t soft_irq;
	scsi_mng_t scsi;
	thermal_t thermal;
	/* temporay use variable    */
	/* store current srb info for gen io case */
	srb_ext_t *p_srb_ext;
	/* bakeup current gen io srb ext for tag/gen parallel */
	srb_ext_t *p_srb_ext_bak;
	u32 signature;
	/* default is  zero, set to 1 to indicate dump mode driver */
	bool dump_mode;
	struct device *dev;
	tPerTick tick;
	bool scsi_init_flag;
	req_card_access_info_t last_req;
	testcase_t testcase;
} bht_dev_ext_t;

typedef struct {
	bool dump_mode;
	/* for dumpmode is the sub value us each time, for normal mode it is start tick */
	u32 tick;
	/* for dump mode unit is us; normal mode unit is ms */
	u32 timeout;
} loop_wait_t;

extern cfg_item_t g_cfg_item[SUPPORT_CHIP_COUNT * 2];

/* 1 Bit define */

#define	BIT0						(0x01)
#define	BIT1						(0x02)
#define	BIT2						(0x04)
#define	BIT3						(0x08)
#define	BIT4						(0x10)
#define	BIT5						(0x20)
#define	BIT6						(0x40)
#define	BIT7						(0x80)
#define	BIT8						(0x0100)
#define	BIT9						(0x0200)
#define	BIT10						(0x0400)
#define	BIT11						(0x0800)
#define	BIT12						(0x1000)
#define	BIT13						(0x2000)
#define	BIT14						(0x4000)
#define	BIT15						(0x8000)
#define	BIT16						(0x010000)
#define	BIT17						(0x020000)
#define	BIT18						(0x040000)
#define	BIT19						(0x080000)
#define	BIT20						(0x100000)
#define	BIT21						(0x200000)
#define	BIT22						(0x400000)
#define	BIT23						(0x800000)
#define	BIT24						(0x01000000)
#define	BIT25						(0x02000000)
#define	BIT26						(0x04000000)
#define	BIT27						(0x08000000)
#define	BIT28						(0x10000000)
#define	BIT29						(0x20000000)
#define	BIT30						(0x40000000)
#define	BIT31						(0x80000000)

#define REGL_INVALID_VAL			(0xffffffff)
#define REGW_INVALID_VAL			(0xffff)
#define REGB_INVALID_VAL			(0xff)

bool sdhci_irq(void *param);

#endif
