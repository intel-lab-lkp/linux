/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AMD AE4DMA driver
 *
 * Copyright (c) 2024, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Basavaraj Natikar <Basavaraj.Natikar@amd.com>
 */
#ifndef __AE4DMA_H__
#define __AE4DMA_H__

#include "../common/amd_dma.h"

#define MAX_AE4_HW_QUEUES		16

#define AE4_DESC_COMPLETED		0x3
#define AE4_DMA_VERSION			4

struct ae4_msix {
	int msix_count;
	struct msix_entry msix_entry[MAX_AE4_HW_QUEUES];
};

struct ae4_cmd_queue {
	struct ae4_device *ae4;
	struct pt_cmd_queue cmd_q;
	struct list_head cmd;
	/* protect command operations */
	struct mutex cmd_lock;
	struct delayed_work p_work;
	struct workqueue_struct *pws;
	struct completion cmp;
	wait_queue_head_t q_w;
	atomic64_t intr_cnt;
	atomic64_t done_cnt;
	u64 q_cmd_count;
	u32 dridx;
	u32 id;
};

union dwou {
	u32 dw0;
	struct dword0 {
	u8	byte0;
	u8	byte1;
	u16	timestamp;
	} dws;
};

struct dword1 {
	u8	status;
	u8	err_code;
	u16	desc_id;
};

struct ae4dma_desc {
	union dwou dwouv;
	struct dword1 dw1;
	u32 length;
	u32 rsvd;
	u32 src_hi;
	u32 src_lo;
	u32 dst_hi;
	u32 dst_lo;
};

struct ae4_device {
	struct pt_device pt;
	struct ae4_msix *ae4_msix;
	struct ae4_cmd_queue ae4cmd_q[MAX_AE4_HW_QUEUES];
	unsigned int ae4_irq[MAX_AE4_HW_QUEUES];
	unsigned int cmd_q_count;
};

int ae4_core_init(struct ae4_device *ae4);
void ae4_destroy_work(struct ae4_device *ae4);
#endif
