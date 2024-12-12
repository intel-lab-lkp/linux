/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC3_CMDQ_H
#define HINIC3_CMDQ_H

#include <linux/dmapool.h>

#include "hinic3_wq.h"
#include "hinic3_hw_intf.h"

#define HINIC3_CMDQ_DEPTH  4096

struct hinic3_cmd_buf {
	void       *buf;
	dma_addr_t dma_addr;
	u16        size;
	atomic_t   ref_cnt;
};

struct hinic3_cmdq_db {
	u32 db_head;
	u32 db_info;
};

/* hw defined cmdq wqe header */
struct hinic3_cmdq_header {
	u32 header_info;
	u32 saved_data;
};

struct hinic3_lcmd_bufdesc {
	struct hinic3_sge sge;
	u64               rsvd2;
	u64               rsvd3;
};

struct hinic3_status {
	u32 status_info;
};

struct hinic3_ctrl {
	u32 ctrl_info;
};

struct hinic3_direct_resp {
	u64 val;
	u64 rsvd;
};

struct hinic3_cmdq_completion {
	union {
		struct hinic3_sge         sge;
		struct hinic3_direct_resp direct;
	} resp;
};

struct hinic3_cmdq_wqe_scmd {
	struct hinic3_cmdq_header     header;
	u64                           rsvd3;
	struct hinic3_status          status;
	struct hinic3_ctrl            ctrl;
	struct hinic3_cmdq_completion completion;
	u32                           rsvd10[6];
};

struct hinic3_cmdq_wqe_lcmd {
	struct hinic3_cmdq_header     header;
	struct hinic3_status          status;
	struct hinic3_ctrl            ctrl;
	struct hinic3_cmdq_completion completion;
	struct hinic3_lcmd_bufdesc    buf_desc;
};

struct hinic3_cmdq_wqe {
	union {
		struct hinic3_cmdq_wqe_scmd   wqe_scmd;
		struct hinic3_cmdq_wqe_lcmd   wqe_lcmd;
	};
};

static_assert(sizeof(struct hinic3_cmdq_wqe) == 64);

enum hinic3_cmdq_status {
	HINIC3_CMDQ_ENABLE = BIT(0),
};

enum hinic3_cmdq_cmd_type {
	HINIC3_CMD_TYPE_NONE,
	HINIC3_CMD_TYPE_DIRECT_RESP,
	HINIC3_CMD_TYPE_FAKE_TIMEOUT,
	HINIC3_CMD_TYPE_TIMEOUT,
	HINIC3_CMD_TYPE_FORCE_STOP,
};

struct hinic3_cmdq_cmd_info {
	enum hinic3_cmdq_cmd_type cmd_type;
	struct completion         *done;
	int                       *errcode;
	/* completion code */
	int                       *cmpt_code;
	u64                       *direct_resp;
	u64                       cmdq_msg_id;
	struct hinic3_cmd_buf     *buf_in;
};

struct hinic3_cmdq {
	struct hinic3_wq            wq;
	enum hinic3_cmdq_type       cmdq_type;
	u8                          wrapped;
	/* synchronize command submission with completions via event queue */
	spinlock_t                  cmdq_lock;
	struct hinic3_cmdq_cmd_info *cmd_infos;
	struct hinic3_hwdev         *hwdev;
};

struct hinic3_cmdqs {
	struct hinic3_hwdev *hwdev;
	struct hinic3_cmdq  cmdq[HINIC3_MAX_CMDQ_TYPES];
	struct dma_pool     *cmd_buf_pool;
	/* doorbell area */
	u8 __iomem          *cmdqs_db_base;

	/* When command queue uses multiple memory pages (1-level CLA), this
	 * block will hold aggregated indirection table for all command queues
	 * of cmdqs. Not used for small cmdq (0-level CLA).
	 */
	dma_addr_t          wq_block_paddr;
	void                *wq_block_vaddr;

	u32                 status;
	u32                 disable_flag;
	u8                  cmdq_num;
};

int hinic3_cmdqs_init(struct hinic3_hwdev *hwdev);
void hinic3_cmdqs_free(struct hinic3_hwdev *hwdev);

struct hinic3_cmd_buf *hinic3_alloc_cmd_buf(struct hinic3_hwdev *hwdev);
void hinic3_free_cmd_buf(struct hinic3_hwdev *hwdev, struct hinic3_cmd_buf *cmd_buf);
void hinic3_cmdq_ceq_handler(struct hinic3_hwdev *hwdev, u32 ceqe_data);

int hinic3_cmdq_direct_resp(struct hinic3_hwdev *hwdev, u8 mod, u8 cmd,
			    struct hinic3_cmd_buf *buf_in, u64 *out_param);

void hinic3_cmdq_flush_sync_cmd(struct hinic3_hwdev *hwdev);
int hinic3_reinit_cmdq_ctxts(struct hinic3_hwdev *hwdev);
bool hinic3_cmdq_idle(struct hinic3_cmdq *cmdq);

#endif
