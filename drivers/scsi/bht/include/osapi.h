/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: osapi.h
 *
 * Abstract: This Include file used to define os independent apis
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

#ifndef _OSAPI_H
#define _OSAPI_H

#define MAX_TIMER_NUM	1

#if CFG_OS_LINUX

typedef enum {
	EVENT_CARD_CHG = 0,
	EVENT_TAG_IO,
	EVENT_GEN_IO,
	EVENT_RUNTIME_D3,
	EVENT_AUTO_TIMER,
	EVENT_SDIO,
	EVENT_TERMINATE,
	EVENT_PENDING,
	EVENT_NONE
} e_event_t;

#else

typedef enum {
	/* one event with task id solution. */
	EVENT_TASK_OCCUR = 0,
	EVENT_NONE
} e_event_t;

#endif

typedef enum {
	TASK_CARD_CHG = 0,
	TASK_TERMINATE,
	TASK_PENDING,
	TASK_TAG_IO,
	TASK_GEN_IO,
	TASK_RUNTIME_D3,
	TASK_AUTO_TIMER,
	TASK_SDIO,
	TASK_CAMOD_POLL_CARD_CHG,
	TASK_NONE
} e_task_t;

typedef enum {
	TIMER_AUTO = 0,
	TIMER_SUBID = 1
} e_timer_t;

#include "../linux_os/linux_api.h"

typedef struct {
	/* for Bar0 access */
	void __iomem *membase;
	/* for Bar1 mem access */
	void __iomem *membase2;
	struct pci_dev *pci_dev;
	byte irq;
	bool use_msi;
} t_pci_dev;

typedef t_linux_os_struct os_struct;
typedef struct list_head list_entry;
typedef linux_completion_t completion_t;
typedef linux_list_t list_t;

#define os_atomic_add(p, i) atomic_add(i, p)
#define os_atomic_sub(p, i) atomic_sub(i, p)
#define os_atomic_read(p) atomic_read(p)
#define os_atomic_set(p, i) atomic_set(p, i)

#define os_container_of(p, type, member) container_of(p, type, member)

typedef void (*thread_cb_t)(void *param);

#if CFG_OS_LINUX
void os_set_event(os_struct *os, e_event_t event);
void os_clear_event(os_struct *os, e_event_t event);
e_event_t os_wait_event(os_struct *os);
bool os_create_thread(thread_t *thr, void *param, thread_cb_t func);
void os_list_init(list_t *p);
void os_sleep(u32 time_ms);

#else
void os_set_event(void *pdx, os_struct *os, e_event_t event, e_task_t taskid);
void os_clear_event(void *pdx, os_struct *os, e_event_t event);
e_event_t os_wait_event(void *pdx, os_struct *os);
bool os_create_thread(void *pdx, thread_t *thr, void *param, thread_cb_t func);
void os_list_init(void *pdx, list_t *p);
void os_sleep(void *pdx, u32 time_ms);
#endif

bool os_thread_is_freeze(void *pdx);
bool os_stop_thread(os_struct *os, thread_t *thr);
void os_kill_thread(os_struct *os, thread_t *thr);
bool os_pending_thread(void *pdx, bool pending);
u64 os_get_performance_tick(u64 *cpu_freq);

/*
 * timeout 0 means wait infinite
 * timeout is in milli second
 */
void os_init_completion(void *pdx, completion_t *p);
void os_finish_completion(void *pdx, completion_t *completion);
bool os_wait_for_completion(void *pdx, completion_t *completion, s32 timeout);

/* os list ops */

list_entry *os_list_locked_remove_head(list_t *p);
void os_list_locked_insert_tail(list_t *p, list_entry *entry);
void os_list_locked_insert_head(list_t *p, list_entry *entry);

void os_set_dev_busy(void *pdx);
void os_set_dev_idle(void *pdx);

/*
 * This is called by req_global_init and req_global_uinit
 */
bool os_layer_init(void *pdx, os_struct *os);
bool os_layer_uinit(void *pdx, os_struct *os);

void os_start_timer(void *pdx, os_struct *os, e_timer_t t, u32 time_ms);
void os_cancel_timer(void *pdx, os_struct *os, e_timer_t t);
void os_stop_timer(void *pdx, os_struct *os, e_timer_t t);

void os_start_timer_s3s4(void *p, os_struct *os, e_timer_t t, u32 time_ms);

bool os_alloc_dma_buffer(void *pdx, void *ctx, u32 nbytes,
			 dma_desc_buf_t *dma_buff);

bool os_free_dma_buffer(void *pdx, dma_desc_buf_t *dma_buff);

u32 os_get_cur_tick(void);
bool os_is_timeout(u32 start_tck, u32 time_ms);

void os_udelay(u32 time_us);
void os_mdelay(u32 time_us);

void os_print(byte *s);

void *os_alloc_vbuff(u32 length);
void os_free_vbuff(void *vbuff);

u32 os_get_phy_addr32l(phy_addr_t phy_addr);
u32 os_get_phy_addr32h(phy_addr_t phy_addr);
u64 os_get_phy_addr64(phy_addr_t phy_addr);

void os_set_phy_addr32l(phy_addr_t *phy_addr, u32 addr);
void os_set_phy_addr32h(phy_addr_t *phy_addr, u32 addr);

void os_set_phy_add64(phy_addr_t *phy_addr, u64 addr);

void os_memcpy(void *dbuf, void *sbuf, s32 len);
void os_memset(void *buffer, byte fill, s32 len);

s32 os_memcpr(void *dbuf, void *sbuf, s32 len);

u32 os_get_sg_list(void *pdx, scsi_srb *Srb, sg_list_t *srb_sg_list);

void os_cfg_load(void *cfg_item, e_chip_type chip_type);

void os_pm_init(void *dev_evt);

void os_random_init(void);
u32 os_random_get(u32 max);

/*
 * Bus related api
 */
void os_bus_change(void *pdx);

void os_set_sdio_val(void *p, u8 val, bool need_set_did);

void os_rtd3_req_wait_wake(void *pdx);

bool os_pcr_pesistent_restore(u16 *addr_tb, u32 *val_tb, u32 tb_len);

bool os_pcr_pesistent_save(u16 *addr_tb, u32 *val_tb, u32 tb_len);

typedef void (*cb_enum_reg_t)(void *cfg, u32 type, u32 idx, u32 addr,
			      u32 value);
void os_enum_reg_cfg(void *cfg, e_chip_type chip_type, const byte *ustr,
		     cb_enum_reg_t func);
#endif
