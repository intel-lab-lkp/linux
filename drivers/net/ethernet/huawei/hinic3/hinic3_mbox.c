// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/dma-mapping.h>

#include "hinic3_mbox.h"
#include "hinic3_common.h"
#include "hinic3_hwdev.h"
#include "hinic3_hwif.h"
#include "hinic3_csr.h"

#define HINIC3_MBOX_INT_DST_AEQN_MASK        GENMASK(11, 10)
#define HINIC3_MBOX_INT_SRC_RESP_AEQN_MASK   GENMASK(13, 12)
#define HINIC3_MBOX_INT_STAT_DMA_MASK        GENMASK(19, 14)
/* TX size, expressed in 4 bytes units */
#define HINIC3_MBOX_INT_TX_SIZE_MASK         GENMASK(24, 20)
/* SO_RO == strong order, relaxed order */
#define HINIC3_MBOX_INT_STAT_DMA_SO_RO_MASK  GENMASK(26, 25)
#define HINIC3_MBOX_INT_WB_EN_MASK           BIT(28)
#define HINIC3_MBOX_INT_SET(val, field)  \
	FIELD_PREP(HINIC3_MBOX_INT_##field##_MASK, val)

#define HINIC3_MBOX_CTRL_TRIGGER_AEQE_MASK   BIT(0)
#define HINIC3_MBOX_CTRL_TX_STATUS_MASK      BIT(1)
#define HINIC3_MBOX_CTRL_DST_FUNC_MASK       GENMASK(28, 16)
#define HINIC3_MBOX_CTRL_SET(val, field)  \
	FIELD_PREP(HINIC3_MBOX_CTRL_##field##_MASK, val)

#define MBOX_MSG_POLLING_TIMEOUT  8000
#define HINIC3_MBOX_COMP_TIME     40000U

#define MBOX_MAX_BUF_SZ           2048U
#define MBOX_HEADER_SZ            8

/* MBOX size is 64B, 8B for mbox_header, 8B reserved */
#define MBOX_SEG_LEN              48
#define MBOX_SEG_LEN_ALIGN        4
#define MBOX_WB_STATUS_LEN        16UL

#define SEQ_ID_START_VAL          0
#define SEQ_ID_MAX_VAL            42
#define MBOX_LAST_SEG_MAX_LEN  \
	(MBOX_MAX_BUF_SZ - SEQ_ID_MAX_VAL * MBOX_SEG_LEN)

/* mbox write back status is 16B, only first 4B is used */
#define MBOX_WB_STATUS_ERRCODE_MASK       0xFFFF
#define MBOX_WB_STATUS_MASK               0xFF
#define MBOX_WB_ERROR_CODE_MASK           0xFF00
#define MBOX_WB_STATUS_FINISHED_SUCCESS   0xFF
#define MBOX_WB_STATUS_NOT_FINISHED       0x00

#define MBOX_STATUS_FINISHED(wb)  \
	(((wb) & MBOX_WB_STATUS_MASK) != MBOX_WB_STATUS_NOT_FINISHED)
#define MBOX_STATUS_SUCCESS(wb)  \
	(((wb) & MBOX_WB_STATUS_MASK) == MBOX_WB_STATUS_FINISHED_SUCCESS)
#define MBOX_STATUS_ERRCODE(wb)  \
	((wb) & MBOX_WB_ERROR_CODE_MASK)

#define MBOX_DMA_MSG_QUEUE_DEPTH    32
#define MBOX_BODY_FROM_HDR(header)  ((u8 *)(header) + MBOX_HEADER_SZ)
#define MBOX_AREA(hwif)  \
	((hwif)->cfg_regs_base + HINIC3_FUNC_CSR_MAILBOX_DATA_OFF)

#define MBOX_MQ_CI_OFFSET  \
	(HINIC3_CFG_REGS_FLAG + HINIC3_FUNC_CSR_MAILBOX_DATA_OFF + \
	 MBOX_HEADER_SZ + MBOX_SEG_LEN)

#define MBOX_MQ_SYNC_CI_MASK   GENMASK(7, 0)
#define MBOX_MQ_ASYNC_CI_MASK  GENMASK(15, 8)
#define MBOX_MQ_CI_GET(val, field)  \
	FIELD_GET(MBOX_MQ_##field##_CI_MASK, val)

static struct hinic3_msg_desc *get_mbox_msg_desc(struct hinic3_mbox *mbox,
						 enum hinic3_msg_direction_type dir,
						 u16 src_func_id)
{
	struct hinic3_msg_channel *msg_ch;

	msg_ch = (src_func_id == HINIC3_MGMT_FUNC_ID) ?
		&mbox->mgmt_msg : mbox->func_msg;
	return (dir == HINIC3_MSG_DIRECT_SEND) ?
		&msg_ch->recv_msg : &msg_ch->resp_msg;
}

static void resp_mbox_handler(struct hinic3_mbox *mbox,
			      const struct hinic3_msg_desc *msg_desc)
{
	spin_lock(&mbox->mbox_lock);
	if (msg_desc->msg_info.msg_id == mbox->send_msg_id &&
	    mbox->event_flag == EVENT_START)
		mbox->event_flag = EVENT_SUCCESS;
	spin_unlock(&mbox->mbox_lock);
}

static bool mbox_segment_valid(struct hinic3_mbox *mbox,
			       struct hinic3_msg_desc *msg_desc,
			       u64 mbox_header)
{
	u8 seq_id, seg_len, msg_id, mod;
	u16 src_func_idx, cmd;

	seq_id = HINIC3_MSG_HEADER_GET(mbox_header, SEQID);
	seg_len = HINIC3_MSG_HEADER_GET(mbox_header, SEG_LEN);
	msg_id = HINIC3_MSG_HEADER_GET(mbox_header, MSG_ID);
	mod = HINIC3_MSG_HEADER_GET(mbox_header, MODULE);
	cmd = HINIC3_MSG_HEADER_GET(mbox_header, CMD);
	src_func_idx = HINIC3_MSG_HEADER_GET(mbox_header, SRC_GLB_FUNC_IDX);

	if (seq_id > SEQ_ID_MAX_VAL || seg_len > MBOX_SEG_LEN ||
	    (seq_id == SEQ_ID_MAX_VAL && seg_len > MBOX_LAST_SEG_MAX_LEN))
		goto err_seg;

	if (seq_id == 0) {
		msg_desc->seq_id = seq_id;
		msg_desc->msg_info.msg_id = msg_id;
		msg_desc->mod = mod;
		msg_desc->cmd = cmd;
	} else {
		if (seq_id != msg_desc->seq_id + 1 || msg_id != msg_desc->msg_info.msg_id ||
		    mod != msg_desc->mod || cmd != msg_desc->cmd)
			goto err_seg;

		msg_desc->seq_id = seq_id;
	}

	return true;

err_seg:
	dev_err(mbox->hwdev->dev,
		"Mailbox segment check failed, src func id: 0x%x, front seg info: seq id: 0x%x, msg id: 0x%x, mod: 0x%x, cmd: 0x%x\n",
		src_func_idx, msg_desc->seq_id, msg_desc->msg_info.msg_id,
		msg_desc->mod, msg_desc->cmd);
	dev_err(mbox->hwdev->dev,
		"Current seg info: seg len: 0x%x, seq id: 0x%x, msg id: 0x%x, mod: 0x%x, cmd: 0x%x\n",
		seg_len, seq_id, msg_id, mod, cmd);

	return false;
}

static void recv_mbox_handler(struct hinic3_mbox *mbox,
			      u64 *header, struct hinic3_msg_desc *msg_desc)
{
	void *mbox_body = MBOX_BODY_FROM_HDR(((void *)header));
	u64 mbox_header = *header;
	u8 seq_id, seg_len;
	int pos;

	if (!mbox_segment_valid(mbox, msg_desc, mbox_header)) {
		msg_desc->seq_id = SEQ_ID_MAX_VAL;
		return;
	}

	seq_id = HINIC3_MSG_HEADER_GET(mbox_header, SEQID);
	seg_len = HINIC3_MSG_HEADER_GET(mbox_header, SEG_LEN);

	pos = seq_id * MBOX_SEG_LEN;
	memcpy((u8 *)msg_desc->msg + pos, mbox_body, seg_len);

	if (!HINIC3_MSG_HEADER_GET(mbox_header, LAST))
		return;

	msg_desc->msg_len = HINIC3_MSG_HEADER_GET(mbox_header, MSG_LEN);
	msg_desc->msg_info.status = HINIC3_MSG_HEADER_GET(mbox_header, STATUS);

	if (HINIC3_MSG_HEADER_GET(mbox_header, DIRECTION) ==
	    HINIC3_MSG_RESPONSE) {
		resp_mbox_handler(mbox, msg_desc);
		return;
	}
}

void hinic3_mbox_func_aeqe_handler(struct hinic3_hwdev *hwdev, u8 *header, u8 size)
{
	u64 mbox_header = *((u64 *)header);
	enum hinic3_msg_direction_type dir;
	struct hinic3_mbox *mbox;
	struct hinic3_msg_desc *msg_desc;
	u16 src_func_id;

	mbox = hwdev->mbox;
	dir = HINIC3_MSG_HEADER_GET(mbox_header, DIRECTION);
	src_func_id = HINIC3_MSG_HEADER_GET(mbox_header, SRC_GLB_FUNC_IDX);
	msg_desc = get_mbox_msg_desc(mbox, dir, src_func_id);
	recv_mbox_handler(mbox, (u64 *)header, msg_desc);
}

static int init_mbox_dma_queue(struct hinic3_hwdev *hwdev, struct mbox_dma_queue *mq)
{
	u32 size;

	mq->depth = MBOX_DMA_MSG_QUEUE_DEPTH;
	mq->prod_idx = 0;
	mq->cons_idx = 0;

	size = mq->depth * MBOX_MAX_BUF_SZ;
	mq->dma_buff_vaddr = dma_alloc_coherent(hwdev->dev, size, &mq->dma_buff_paddr,
						GFP_KERNEL);
	if (!mq->dma_buff_vaddr)
		return -ENOMEM;

	return 0;
}

static void deinit_mbox_dma_queue(struct hinic3_hwdev *hwdev, struct mbox_dma_queue *mq)
{
	dma_free_coherent(hwdev->dev, mq->depth * MBOX_MAX_BUF_SZ,
			  mq->dma_buff_vaddr, mq->dma_buff_paddr);
}

static int hinic3_init_mbox_dma_queue(struct hinic3_mbox *mbox)
{
	u32 val;
	int err;

	err = init_mbox_dma_queue(mbox->hwdev, &mbox->sync_msg_queue);
	if (err)
		return err;

	err = init_mbox_dma_queue(mbox->hwdev, &mbox->async_msg_queue);
	if (err) {
		deinit_mbox_dma_queue(mbox->hwdev, &mbox->sync_msg_queue);
		return err;
	}

	val = hinic3_hwif_read_reg(mbox->hwdev->hwif, MBOX_MQ_CI_OFFSET);
	val &= ~MBOX_MQ_SYNC_CI_MASK;
	val &= ~MBOX_MQ_ASYNC_CI_MASK;
	hinic3_hwif_write_reg(mbox->hwdev->hwif, MBOX_MQ_CI_OFFSET, val);

	return 0;
}

static void hinic3_deinit_mbox_dma_queue(struct hinic3_mbox *mbox)
{
	deinit_mbox_dma_queue(mbox->hwdev, &mbox->sync_msg_queue);
	deinit_mbox_dma_queue(mbox->hwdev, &mbox->async_msg_queue);
}

static int alloc_mbox_msg_channel(struct hinic3_msg_channel *msg_ch)
{
	msg_ch->resp_msg.msg = kzalloc(MBOX_MAX_BUF_SZ, GFP_KERNEL);
	if (!msg_ch->resp_msg.msg)
		return -ENOMEM;

	msg_ch->recv_msg.msg = kzalloc(MBOX_MAX_BUF_SZ, GFP_KERNEL);
	if (!msg_ch->recv_msg.msg) {
		kfree(msg_ch->resp_msg.msg);
		return -ENOMEM;
	}

	msg_ch->resp_msg.seq_id = SEQ_ID_MAX_VAL;
	msg_ch->recv_msg.seq_id = SEQ_ID_MAX_VAL;
	return 0;
}

static void free_mbox_msg_channel(struct hinic3_msg_channel *msg_ch)
{
	kfree(msg_ch->recv_msg.msg);
	kfree(msg_ch->resp_msg.msg);
}

static int init_mgmt_msg_channel(struct hinic3_mbox *mbox)
{
	int err;

	err = alloc_mbox_msg_channel(&mbox->mgmt_msg);
	if (err) {
		dev_err(mbox->hwdev->dev, "Failed to alloc mgmt message channel\n");
		return err;
	}

	err = hinic3_init_mbox_dma_queue(mbox);
	if (err) {
		dev_err(mbox->hwdev->dev, "Failed to init mbox dma queue\n");
		free_mbox_msg_channel(&mbox->mgmt_msg);
	}

	return err;
}

static void deinit_mgmt_msg_channel(struct hinic3_mbox *mbox)
{
	hinic3_deinit_mbox_dma_queue(mbox);
	free_mbox_msg_channel(&mbox->mgmt_msg);
}

static int hinic3_init_func_mbox_msg_channel(struct hinic3_hwdev *hwdev)
{
	struct hinic3_mbox *mbox;
	int err;

	mbox = hwdev->mbox;
	mbox->func_msg = kzalloc(sizeof(*mbox->func_msg), GFP_KERNEL);
	if (!mbox->func_msg)
		return -ENOMEM;

	err = alloc_mbox_msg_channel(mbox->func_msg);
	if (err)
		goto err_undo_alloc_func_msg;

	return 0;

err_undo_alloc_func_msg:
	kfree(mbox->func_msg);
	mbox->func_msg = NULL;
	return -ENOMEM;
}

static void hinic3_deinit_func_mbox_msg_channel(struct hinic3_hwdev *hwdev)
{
	struct hinic3_mbox *mbox = hwdev->mbox;

	free_mbox_msg_channel(mbox->func_msg);
	kfree(mbox->func_msg);
	mbox->func_msg = NULL;
}

static void prepare_send_mbox(struct hinic3_mbox *mbox)
{
	struct hinic3_send_mbox *send_mbox = &mbox->send_mbox;

	send_mbox->data = MBOX_AREA(mbox->hwdev->hwif);
}

static int alloc_mbox_wb_status(struct hinic3_mbox *mbox)
{
	struct hinic3_send_mbox *send_mbox = &mbox->send_mbox;
	struct hinic3_hwdev *hwdev = mbox->hwdev;
	u32 addr_h, addr_l;

	send_mbox->wb_vaddr = dma_alloc_coherent(hwdev->dev,
						 MBOX_WB_STATUS_LEN,
						 &send_mbox->wb_paddr,
						 GFP_KERNEL);
	if (!send_mbox->wb_vaddr)
		return -ENOMEM;

	addr_h = upper_32_bits(send_mbox->wb_paddr);
	addr_l = lower_32_bits(send_mbox->wb_paddr);
	hinic3_hwif_write_reg(hwdev->hwif, HINIC3_FUNC_CSR_MAILBOX_RESULT_H_OFF,
			      addr_h);
	hinic3_hwif_write_reg(hwdev->hwif, HINIC3_FUNC_CSR_MAILBOX_RESULT_L_OFF,
			      addr_l);

	return 0;
}

static void free_mbox_wb_status(struct hinic3_mbox *mbox)
{
	struct hinic3_send_mbox *send_mbox = &mbox->send_mbox;
	struct hinic3_hwdev *hwdev = mbox->hwdev;

	hinic3_hwif_write_reg(hwdev->hwif, HINIC3_FUNC_CSR_MAILBOX_RESULT_H_OFF,
			      0);
	hinic3_hwif_write_reg(hwdev->hwif, HINIC3_FUNC_CSR_MAILBOX_RESULT_L_OFF,
			      0);

	dma_free_coherent(hwdev->dev, MBOX_WB_STATUS_LEN,
			  send_mbox->wb_vaddr, send_mbox->wb_paddr);
}

static int hinic3_mbox_pre_init(struct hinic3_hwdev *hwdev,
				struct hinic3_mbox **mbox)
{
	(*mbox) = kzalloc(sizeof(struct hinic3_mbox), GFP_KERNEL);
	if (!(*mbox))
		return -ENOMEM;

	(*mbox)->hwdev = hwdev;
	mutex_init(&(*mbox)->mbox_send_lock);
	mutex_init(&(*mbox)->msg_send_lock);
	spin_lock_init(&(*mbox)->mbox_lock);

	(*mbox)->workq = create_singlethread_workqueue(HINIC3_MBOX_WQ_NAME);
	if (!(*mbox)->workq) {
		dev_err(hwdev->dev, "Failed to initialize MBOX workqueue\n");
		kfree((*mbox));
		return -ENOMEM;
	}
	hwdev->mbox = (*mbox);

	return 0;
}

int hinic3_init_mbox(struct hinic3_hwdev *hwdev)
{
	struct hinic3_mbox *mbox;
	int err;

	err = hinic3_mbox_pre_init(hwdev, &mbox);
	if (err)
		return err;

	err = init_mgmt_msg_channel(mbox);
	if (err)
		goto err_init_mgmt_msg_ch;

	err = hinic3_init_func_mbox_msg_channel(hwdev);
	if (err)
		goto err_init_func_msg_ch;

	err = alloc_mbox_wb_status(mbox);
	if (err) {
		dev_err(hwdev->dev, "Failed to alloc mbox write back status\n");
		goto err_alloc_wb_status;
	}

	prepare_send_mbox(mbox);

	return 0;

err_alloc_wb_status:
	hinic3_deinit_func_mbox_msg_channel(hwdev);

err_init_func_msg_ch:
	deinit_mgmt_msg_channel(mbox);

err_init_mgmt_msg_ch:
	destroy_workqueue(mbox->workq);
	kfree(mbox);

	return err;
}

void hinic3_free_mbox(struct hinic3_hwdev *hwdev)
{
	struct hinic3_mbox *mbox = hwdev->mbox;

	destroy_workqueue(mbox->workq);
	free_mbox_wb_status(mbox);
	hinic3_deinit_func_mbox_msg_channel(hwdev);
	deinit_mgmt_msg_channel(mbox);
	kfree(mbox);
}

#define MBOX_DMA_MSG_INIT_XOR_VAL    0x5a5a5a5a
#define MBOX_XOR_DATA_ALIGN          4
static u32 mbox_dma_msg_xor(u32 *data, u32 msg_len)
{
	u32 xor = MBOX_DMA_MSG_INIT_XOR_VAL;
	u32 dw_len = msg_len / sizeof(u32);
	u32 i;

	for (i = 0; i < dw_len; i++)
		xor ^= data[i];

	return xor;
}

#define MQ_ID_MASK(mq, idx)    ((idx) & ((mq)->depth - 1))
#define IS_MSG_QUEUE_FULL(mq) \
	(MQ_ID_MASK(mq, (mq)->prod_idx + 1) == MQ_ID_MASK(mq, (mq)->cons_idx))

static int mbox_prepare_dma_entry(struct hinic3_mbox *mbox, struct mbox_dma_queue *mq,
				  struct mbox_dma_msg *dma_msg, const void *msg, u32 msg_len)
{
	u64 dma_addr, offset;
	void *dma_vaddr;

	if (IS_MSG_QUEUE_FULL(mq)) {
		dev_err(mbox->hwdev->dev, "Mbox sync message queue is busy, pi: %u, ci: %u\n",
			mq->prod_idx, MQ_ID_MASK(mq, mq->cons_idx));
		return -EBUSY;
	}

	/* copy data to DMA buffer */
	offset = mq->prod_idx * MBOX_MAX_BUF_SZ;
	dma_vaddr = (u8 *)mq->dma_buff_vaddr + offset;
	memcpy(dma_vaddr, msg, msg_len);
	dma_addr = mq->dma_buff_paddr + offset;
	dma_msg->dma_addr_high = upper_32_bits(dma_addr);
	dma_msg->dma_addr_low = lower_32_bits(dma_addr);
	dma_msg->msg_len = msg_len;
	/* The firmware obtains message based on 4B alignment. */
	dma_msg->xor = mbox_dma_msg_xor(dma_vaddr, ALIGN(msg_len, MBOX_XOR_DATA_ALIGN));
	mq->prod_idx++;
	mq->prod_idx = MQ_ID_MASK(mq, mq->prod_idx);
	return 0;
}

static int mbox_prepare_dma_msg(struct hinic3_mbox *mbox, enum hinic3_msg_ack_type ack_type,
				struct mbox_dma_msg *dma_msg, const void *msg, u32 msg_len)
{
	struct mbox_dma_queue *mq;
	u32 val;

	val = hinic3_hwif_read_reg(mbox->hwdev->hwif, MBOX_MQ_CI_OFFSET);
	if (ack_type == HINIC3_MSG_ACK) {
		mq = &mbox->sync_msg_queue;
		mq->cons_idx = MBOX_MQ_CI_GET(val, SYNC);
	} else {
		mq = &mbox->async_msg_queue;
		mq->cons_idx = MBOX_MQ_CI_GET(val, ASYNC);
	}

	return mbox_prepare_dma_entry(mbox, mq, dma_msg, msg, msg_len);
}

static void clear_mbox_status(struct hinic3_send_mbox *mbox)
{
	__be64 *wb_status = mbox->wb_vaddr;

	*wb_status = 0;
	/* clear mailbox write back status */
	wmb();
}

static void mbox_dword_write(const void *src, void __iomem *dst, u32 count)
{
	u32 __iomem *dst32 = dst;
	const u32 *src32 = src;
	u32 i;

	/* Data written to mbox is arranged in structs with little endian fields
	 * but when written to HW every dword (32bits) should be swapped since
	 * the HW will swap it again. This is a mandatory swap regardless of the
	 * CPU endianness.
	 */
	for (i = 0; i < count; i++)
		__raw_writel(swab32(src32[i]), dst32 + i);
}

static void mbox_copy_header(struct hinic3_hwdev *hwdev,
			     struct hinic3_send_mbox *mbox, u64 *header)
{
	mbox_dword_write(header, mbox->data, MBOX_HEADER_SZ / sizeof(u32));
}

static void mbox_copy_send_data(struct hinic3_hwdev *hwdev,
				struct hinic3_send_mbox *mbox, void *seg,
				u32 seg_len)
{
	u32 __iomem *dst = (u32 __iomem *)(mbox->data + MBOX_HEADER_SZ);
	u32 count, leftover, last_dword;
	const u32 *src = seg;

	count = seg_len / sizeof(u32);
	leftover = seg_len % sizeof(u32);
	if (count > 0)
		mbox_dword_write(src, dst, count);

	if (leftover > 0) {
		last_dword = 0;
		memcpy(&last_dword, src + count, leftover);
		mbox_dword_write(&last_dword, dst + count, 1);
	}
}

static void write_mbox_msg_attr(struct hinic3_mbox *mbox,
				u16 dst_func, u16 dst_aeqn, u32 seg_len)
{
	struct hinic3_hwif *hwif = mbox->hwdev->hwif;
	u32 mbox_int, mbox_ctrl, tx_size;

	tx_size = ALIGN(seg_len + MBOX_HEADER_SZ, MBOX_SEG_LEN_ALIGN) >> 2;

	mbox_int = HINIC3_MBOX_INT_SET(dst_aeqn, DST_AEQN) |
		   HINIC3_MBOX_INT_SET(0, STAT_DMA) |
		   HINIC3_MBOX_INT_SET(tx_size, TX_SIZE) |
		   HINIC3_MBOX_INT_SET(0, STAT_DMA_SO_RO) |
		   HINIC3_MBOX_INT_SET(1, WB_EN);

	mbox_ctrl = HINIC3_MBOX_CTRL_SET(1, TX_STATUS) |
		    HINIC3_MBOX_CTRL_SET(0, TRIGGER_AEQE) |
		    HINIC3_MBOX_CTRL_SET(dst_func, DST_FUNC);

	hinic3_hwif_write_reg(hwif, HINIC3_FUNC_CSR_MAILBOX_INT_OFF, mbox_int);
	hinic3_hwif_write_reg(hwif, HINIC3_FUNC_CSR_MAILBOX_CONTROL_OFF, mbox_ctrl);
}

static u16 get_mbox_status(const struct hinic3_send_mbox *mbox)
{
	__be64 *wb_status = mbox->wb_vaddr;
	u64 wb_val;

	wb_val = be64_to_cpu(*wb_status);
	/* verify reading before check */
	rmb();
	return (u16)(wb_val & MBOX_WB_STATUS_ERRCODE_MASK);
}

static enum hinic3_wait_return check_mbox_wb_status(void *priv_data)
{
	struct hinic3_mbox *mbox = priv_data;
	u16 wb_status;

	wb_status = get_mbox_status(&mbox->send_mbox);
	return MBOX_STATUS_FINISHED(wb_status) ?
		WAIT_PROCESS_CPL : WAIT_PROCESS_WAITING;
}

static int send_mbox_seg(struct hinic3_mbox *mbox, u64 header,
			 u16 dst_func, void *seg, u32 seg_len, void *msg_info)
{
	struct hinic3_send_mbox *send_mbox = &mbox->send_mbox;
	struct hinic3_hwdev *hwdev = mbox->hwdev;
	u8 num_aeqs = hwdev->hwif->attr.num_aeqs;
	enum hinic3_msg_direction_type dir;
	u16 dst_aeqn, wb_status, errcode;
	int err;

	/* mbox to mgmt cpu, hardware doesn't care about dst aeq id */
	if (num_aeqs > HINIC3_AEQ_FOR_MBOX) {
		dir = HINIC3_MSG_HEADER_GET(header, DIRECTION);
		dst_aeqn = (dir == HINIC3_MSG_DIRECT_SEND) ?
			   HINIC3_AEQ_FOR_EVENT : HINIC3_AEQ_FOR_MBOX;
	} else {
		dst_aeqn = 0;
	}

	clear_mbox_status(send_mbox);
	mbox_copy_header(hwdev, send_mbox, &header);
	mbox_copy_send_data(hwdev, send_mbox, seg, seg_len);
	write_mbox_msg_attr(mbox, dst_func, dst_aeqn, seg_len);

	err = hinic3_wait_for_timeout(mbox, check_mbox_wb_status,
				      MBOX_MSG_POLLING_TIMEOUT, USEC_PER_MSEC);
	wb_status = get_mbox_status(send_mbox);
	if (err) {
		dev_err(hwdev->dev, "Send mailbox segment timeout, wb status: 0x%x\n",
			wb_status);
		return -ETIMEDOUT;
	}

	if (!MBOX_STATUS_SUCCESS(wb_status)) {
		dev_err(hwdev->dev, "Send mailbox segment to function %u error, wb status: 0x%x\n",
			dst_func, wb_status);
		errcode = MBOX_STATUS_ERRCODE(wb_status);
		return errcode ? errcode : -EFAULT;
	}

	return 0;
}

static int send_mbox_msg(struct hinic3_mbox *mbox, u8 mod, u16 cmd,
			 const void *msg, u32 msg_len, u16 dst_func,
			 enum hinic3_msg_direction_type direction,
			 enum hinic3_msg_ack_type ack_type,
			 struct mbox_msg_info *msg_info)
{
	enum hinic3_data_type data_type = HINIC3_DATA_INLINE;
	struct hinic3_hwdev *hwdev = mbox->hwdev;
	struct mbox_dma_msg dma_msg;
	u32 seg_len = MBOX_SEG_LEN;
	u64 header = 0;
	u32 seq_id = 0;
	u16 rsp_aeq_id;
	u8 *msg_seg;
	int err = 0;
	u32 left;

	if (hwdev->hwif->attr.num_aeqs > HINIC3_AEQ_FOR_MBOX)
		rsp_aeq_id = HINIC3_AEQ_FOR_MBOX;
	else
		rsp_aeq_id = 0;

	mutex_lock(&mbox->msg_send_lock);

	if (IS_DMA_MBX_MSG(dst_func) && !SUPPORT_SEGMENT(hwdev->features[0])) {
		err = mbox_prepare_dma_msg(mbox, ack_type, &dma_msg, msg, msg_len);
		if (err)
			goto err_send;

		msg = &dma_msg;
		msg_len = sizeof(dma_msg);
		data_type = HINIC3_DATA_DMA;
	}

	msg_seg = (u8 *)msg;
	left = msg_len;

	header = HINIC3_MSG_HEADER_SET(msg_len, MSG_LEN) |
		 HINIC3_MSG_HEADER_SET(mod, MODULE) |
		 HINIC3_MSG_HEADER_SET(seg_len, SEG_LEN) |
		 HINIC3_MSG_HEADER_SET(ack_type, NO_ACK) |
		 HINIC3_MSG_HEADER_SET(data_type, DATA_TYPE) |
		 HINIC3_MSG_HEADER_SET(SEQ_ID_START_VAL, SEQID) |
		 HINIC3_MSG_HEADER_SET(NOT_LAST_SEGMENT, LAST) |
		 HINIC3_MSG_HEADER_SET(direction, DIRECTION) |
		 HINIC3_MSG_HEADER_SET(cmd, CMD) |
		 HINIC3_MSG_HEADER_SET(msg_info->msg_id, MSG_ID) |
		 HINIC3_MSG_HEADER_SET(rsp_aeq_id, AEQ_ID) |
		 HINIC3_MSG_HEADER_SET(HINIC3_MSG_FROM_MBOX, SOURCE) |
		 HINIC3_MSG_HEADER_SET(!!msg_info->status, STATUS);

	while (!(HINIC3_MSG_HEADER_GET(header, LAST))) {
		if (left <= MBOX_SEG_LEN) {
			header &= ~HINIC3_MSG_HEADER_SEG_LEN_MASK;
			header |= HINIC3_MSG_HEADER_SET(left, SEG_LEN);
			header |= HINIC3_MSG_HEADER_SET(LAST_SEGMENT, LAST);
			seg_len = left;
		}

		err = send_mbox_seg(mbox, header, dst_func, msg_seg,
				    seg_len, msg_info);
		if (err) {
			dev_err(hwdev->dev, "Failed to send mbox seg, seq_id=0x%llx\n",
				HINIC3_MSG_HEADER_GET(header, SEQID));
			goto err_send;
		}

		left -= MBOX_SEG_LEN;
		msg_seg += MBOX_SEG_LEN;
		seq_id++;
		header &= ~HINIC3_MSG_HEADER_SEG_LEN_MASK;
		header |= HINIC3_MSG_HEADER_SET(seq_id, SEQID);
	}

err_send:
	mutex_unlock(&mbox->msg_send_lock);
	return err;
}

static void set_mbox_to_func_event(struct hinic3_mbox *mbox,
				   enum mbox_event_state event_flag)
{
	spin_lock(&mbox->mbox_lock);
	mbox->event_flag = event_flag;
	spin_unlock(&mbox->mbox_lock);
}

static enum hinic3_wait_return check_mbox_msg_finish(void *priv_data)
{
	struct hinic3_mbox *mbox = priv_data;

	return (mbox->event_flag == EVENT_SUCCESS) ?
		WAIT_PROCESS_CPL : WAIT_PROCESS_WAITING;
}

static int wait_mbox_msg_completion(struct hinic3_mbox *mbox,
				    u32 timeout)
{
	u32 wait_time;
	int err;

	wait_time = (timeout != 0) ? timeout : HINIC3_MBOX_COMP_TIME;
	err = hinic3_wait_for_timeout(mbox, check_mbox_msg_finish,
				      wait_time, USEC_PER_MSEC);
	if (err) {
		set_mbox_to_func_event(mbox, EVENT_TIMEOUT);
		return -ETIMEDOUT;
	}
	set_mbox_to_func_event(mbox, EVENT_END);
	return 0;
}

static int hinic3_mbox_to_func(struct hinic3_hwdev *hwdev, u8 mod, u16 cmd,
			       u16 dst_func, const void *buf_in, u32 in_size, void *buf_out,
			       u32 *out_size, u32 timeout)
{
	struct hinic3_mbox *mbox = hwdev->mbox;
	struct mbox_msg_info msg_info = {};
	struct hinic3_msg_desc *msg_desc;
	int err;

	/* expect response message */
	msg_desc = get_mbox_msg_desc(mbox, HINIC3_MSG_RESPONSE, dst_func);
	mutex_lock(&mbox->mbox_send_lock);
	msg_info.msg_id = (msg_info.msg_id + 1) & 0xF;
	mbox->send_msg_id = msg_info.msg_id;
	set_mbox_to_func_event(mbox, EVENT_START);

	err = send_mbox_msg(mbox, mod, cmd, buf_in, in_size, dst_func,
			    HINIC3_MSG_DIRECT_SEND, HINIC3_MSG_ACK, &msg_info);
	if (err) {
		dev_err(hwdev->dev, "Send mailbox mod %u, cmd %u failed, msg_id: %u, err: %d\n",
			mod, cmd, msg_info.msg_id, err);
		set_mbox_to_func_event(mbox, EVENT_FAIL);
		goto err_send;
	}

	if (wait_mbox_msg_completion(mbox, timeout)) {
		dev_err(hwdev->dev,
			"Send mbox msg timeout, msg_id: %u\n", msg_info.msg_id);
		err = -ETIMEDOUT;
		goto err_send;
	}

	if (mod != msg_desc->mod || cmd != msg_desc->cmd) {
		dev_err(hwdev->dev,
			"Invalid response mbox message, mod: 0x%x, cmd: 0x%x, expect mod: 0x%x, cmd: 0x%x\n",
			msg_desc->mod, msg_desc->cmd, mod, cmd);
		err = -EFAULT;
		goto err_send;
	}

	if (msg_desc->msg_info.status) {
		err = msg_desc->msg_info.status;
		goto err_send;
	}

	if (buf_out && out_size) {
		if (*out_size < msg_desc->msg_len) {
			dev_err(hwdev->dev,
				"Invalid response mbox message length: %u for mod %d cmd %u, should less than: %u\n",
				msg_desc->msg_len, mod, cmd, *out_size);
			err = -EFAULT;
			goto err_send;
		}

		if (msg_desc->msg_len)
			memcpy(buf_out, msg_desc->msg, msg_desc->msg_len);

		*out_size = msg_desc->msg_len;
	}

err_send:
	mutex_unlock(&mbox->mbox_send_lock);
	return err;
}

static int hinic3_mbox_to_func_no_ack(struct hinic3_hwdev *hwdev, u8 mod, u16 cmd,
				      u16 dst_func, const void *buf_in, u32 in_size)
{
	struct hinic3_mbox *mbox = hwdev->mbox;
	struct mbox_msg_info msg_info = {};
	int err;

	mutex_lock(&mbox->mbox_send_lock);
	err = send_mbox_msg(mbox, mod, cmd, buf_in, in_size,
			    dst_func, HINIC3_MSG_DIRECT_SEND,
			    HINIC3_MSG_NO_ACK, &msg_info);
	if (err)
		dev_err(hwdev->dev, "Send mailbox no ack failed\n");

	mutex_unlock(&mbox->mbox_send_lock);

	return err;
}

int hinic3_send_mbox_to_mgmt(struct hinic3_hwdev *hwdev, u8 mod, u16 cmd,
			     const void *buf_in, u32 in_size, void *buf_out,
			     u32 *out_size, u32 timeout)
{
	return hinic3_mbox_to_func(hwdev, mod, cmd, HINIC3_MGMT_FUNC_ID,
				   buf_in, in_size, buf_out, out_size, timeout);
}

int hinic3_send_mbox_to_mgmt_no_ack(struct hinic3_hwdev *hwdev, u8 mod, u16 cmd,
				    const void *buf_in, u32 in_size)
{
	return hinic3_mbox_to_func_no_ack(hwdev, mod, cmd, HINIC3_MGMT_FUNC_ID,
					  buf_in, in_size);
}
