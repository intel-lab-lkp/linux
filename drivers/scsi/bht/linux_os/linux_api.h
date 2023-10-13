/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: linux_api.h
 *
 * Abstract: Include apis for Linux
 *
 * Version: 1.00
 *
 * Author: Peter.Guo
 *
 * Environment:	Linux
 *
 * History:
 *
 * 5/20/2015		Creation	Peter.Guo
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/pm.h>
#include <linux/vmalloc.h>

#include "../include/basic.h"
#define TASK_STATUS_IDLE 0
#define TASK_STATUS_OCCUR 1

typedef struct completion linux_completion_t;

typedef struct {
	spinlock_t lock;
	/* add for list */
	struct list_head list_hd;
	atomic_t cnt;
} linux_list_t;

typedef struct {
	struct task_struct *pthread;
	bool pending_lock;
	bool freeze;
	linux_completion_t break_pending;
} thread_t;

typedef void (*timer_cb_t)(PVOID, PVOID);

typedef struct {
	void *timer;
	timer_cb_t timer_callback;
} linux_timer_t;

typedef struct {
	wait_queue_head_t evt_control;
	/* Command control thread - command in flag */
	atomic_t evt_comming;
	unsigned long evt_flag;
	spinlock_t lock;
} t_linux_event;

typedef struct {
	u8 status;
	u32 id;
	u8 shared_task_cnt;
} win_task_t;

typedef struct {
	win_task_t tasks[10];
	u32 task_cnt;
} task_mgr_t;

typedef struct {
	struct timer_list timer;
	thread_t thread;

	task_mgr_t task_mgr;
	t_linux_event event;
	spinlock_t lock;
	dma_desc_buf_t dma_info;
	struct Scsi_Host *scsi_host;
	byte *virt_buff;
	/*
	 * If os_get_sg_list called successful, set this flag to 1.
	 * and need call dma_unmap_sg when transfer done
	 */
	bool dma_mapped;
	int rt_pm_cnt;
	bool thread_terminate_stop;
} t_linux_os_struct;

void os_free_sg_list(void *p, scsi_srb *Srb);
