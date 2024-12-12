/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC3_MBOX_H
#define HINIC3_MBOX_H

#include <linux/mutex.h>
#include <linux/bitfield.h>

struct hinic3_hwdev;

#define HINIC3_MSG_HEADER_SRC_GLB_FUNC_IDX_MASK  GENMASK_ULL(12, 0)
#define HINIC3_MSG_HEADER_STATUS_MASK            BIT_ULL(13)
#define HINIC3_MSG_HEADER_SOURCE_MASK            BIT_ULL(15)
#define HINIC3_MSG_HEADER_AEQ_ID_MASK            GENMASK_ULL(17, 16)
#define HINIC3_MSG_HEADER_MSG_ID_MASK            GENMASK_ULL(21, 18)
#define HINIC3_MSG_HEADER_CMD_MASK               GENMASK_ULL(31, 22)
#define HINIC3_MSG_HEADER_MSG_LEN_MASK           GENMASK_ULL(42, 32)
#define HINIC3_MSG_HEADER_MODULE_MASK            GENMASK_ULL(47, 43)
#define HINIC3_MSG_HEADER_SEG_LEN_MASK           GENMASK_ULL(53, 48)
#define HINIC3_MSG_HEADER_NO_ACK_MASK            BIT_ULL(54)
#define HINIC3_MSG_HEADER_DATA_TYPE_MASK         BIT_ULL(55)
#define HINIC3_MSG_HEADER_SEQID_MASK             GENMASK_ULL(61, 56)
#define HINIC3_MSG_HEADER_LAST_MASK              BIT_ULL(62)
#define HINIC3_MSG_HEADER_DIRECTION_MASK         BIT_ULL(63)

#define HINIC3_MSG_HEADER_SET(val, member) \
	FIELD_PREP(HINIC3_MSG_HEADER_##member##_MASK, val)
#define HINIC3_MSG_HEADER_GET(val, member) \
	FIELD_GET(HINIC3_MSG_HEADER_##member##_MASK, val)

#define HINIC3_MGMT_FUNC_ID       0x1FFF
#define IS_DMA_MBX_MSG(dst_func)  ((dst_func) == HINIC3_MGMT_FUNC_ID)
#define COMM_F_MBOX_SEGMENT       BIT(3)
#define SUPPORT_SEGMENT(feature)  ((feature) & COMM_F_MBOX_SEGMENT)

enum hinic3_msg_direction_type {
	HINIC3_MSG_DIRECT_SEND = 0,
	HINIC3_MSG_RESPONSE    = 1,
};

enum hinic3_msg_segment_type {
	NOT_LAST_SEGMENT = 0,
	LAST_SEGMENT     = 1,
};

enum hinic3_msg_ack_type {
	HINIC3_MSG_ACK    = 0,
	HINIC3_MSG_NO_ACK = 1,
};

enum hinic3_data_type {
	HINIC3_DATA_INLINE = 0,
	HINIC3_DATA_DMA    = 1,
};

enum hinic3_msg_src_type {
	HINIC3_MSG_FROM_MBOX = 1,
};

enum hinic3_msg_aeq_type {
	HINIC3_AEQ_FOR_EVENT = 0,
	HINIC3_AEQ_FOR_MBOX  = 1,
};

#define HINIC3_MBOX_WQ_NAME  "hinic3_mbox"

struct mbox_msg_info {
	u8 msg_id;
	u8 status;
};

struct hinic3_msg_desc {
	void   *msg;
	u16    msg_len;
	u8     seq_id;
	u8     mod;
	u16    cmd;
	struct mbox_msg_info msg_info;
};

struct hinic3_msg_channel {
	struct   hinic3_msg_desc resp_msg;
	struct   hinic3_msg_desc recv_msg;
};

struct hinic3_send_mbox {
	u8 __iomem *data;
	void       *wb_vaddr;
	dma_addr_t wb_paddr;
};

enum mbox_event_state {
	EVENT_START   = 0,
	EVENT_FAIL    = 1,
	EVENT_SUCCESS = 2,
	EVENT_TIMEOUT = 3,
	EVENT_END     = 4,
};

struct mbox_dma_msg {
	u32 xor;
	u32 dma_addr_high;
	u32 dma_addr_low;
	u32 msg_len;
	u64 rsvd;
};

struct mbox_dma_queue {
	void       *dma_buff_vaddr;
	dma_addr_t dma_buff_paddr;
	u16        depth;
	u16        prod_idx;
	u16        cons_idx;
};

struct hinic3_mbox {
	struct hinic3_hwdev       *hwdev;
	/* lock for send mbox message and ack message */
	struct mutex              mbox_send_lock;
	/* lock for send mbox message */
	struct mutex              msg_send_lock;
	struct hinic3_send_mbox   send_mbox;
	struct mbox_dma_queue     sync_msg_queue;
	struct mbox_dma_queue     async_msg_queue;
	struct workqueue_struct   *workq;
	/* driver and MGMT CPU */
	struct hinic3_msg_channel mgmt_msg;
	/* VF to PF */
	struct hinic3_msg_channel *func_msg;
	u8                        send_msg_id;
	enum mbox_event_state     event_flag;
	/* lock for mbox event flag */
	spinlock_t                mbox_lock;
};

void hinic3_mbox_func_aeqe_handler(struct hinic3_hwdev *hwdev, u8 *header, u8 size);
int hinic3_init_mbox(struct hinic3_hwdev *hwdev);
void hinic3_free_mbox(struct hinic3_hwdev *hwdev);

int hinic3_send_mbox_to_mgmt(struct hinic3_hwdev *hwdev, u8 mod, u16 cmd,
			     const void *buf_in, u32 in_size, void *buf_out,
			     u32 *out_size, u32 timeout);
int hinic3_send_mbox_to_mgmt_no_ack(struct hinic3_hwdev *hwdev, u8 mod, u16 cmd,
				    const void *buf_in, u32 in_size);

#endif
