// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/semaphore.h>
#include <linux/delay.h>

#include "hinic3_common.h"
#include "hinic3_csr.h"
#include "hinic3_hwif.h"
#include "hinic3_eqs.h"
#include "hinic3_mbox.h"
#include "hinic3_mgmt.h"

#define HINIC3_MSG_TO_MGMT_MAX_LEN    2016

#define MAX_PF_MGMT_BUF_SIZE          2048UL
#define MGMT_SEG_LEN_MAX              48
#define ASYNC_MSG_FLAG                0x8

/* Bogus sequence ID to prevent accidental match following partial message */
#define MGMT_BOGUS_SEQ_ID             (MAX_PF_MGMT_BUF_SIZE / MGMT_SEG_LEN_MAX + 1)

static void mgmt_resp_msg_handler(struct hinic3_msg_pf_to_mgmt *pf_to_mgmt,
				  struct hinic3_recv_msg *recv_msg)
{
	struct device *dev = pf_to_mgmt->hwdev->dev;

	/* Ignore async msg */
	if (recv_msg->msg_id & ASYNC_MSG_FLAG)
		return;

	spin_lock(&pf_to_mgmt->sync_event_lock);
	if (recv_msg->msg_id != pf_to_mgmt->sync_msg_id) {
		dev_err(dev, "msg id mismatch, send msg id: 0x%x, recv msg id: 0x%x, event state: %d\n",
			pf_to_mgmt->sync_msg_id, recv_msg->msg_id,
			pf_to_mgmt->event_flag);
	} else if (pf_to_mgmt->event_flag == SEND_EVENT_START) {
		pf_to_mgmt->event_flag = SEND_EVENT_SUCCESS;
		complete(&recv_msg->recv_done);
	} else {
		dev_err(dev, "Wait timeout, send msg id: 0x%x, recv msg id: 0x%x, event state: %d\n",
			pf_to_mgmt->sync_msg_id, recv_msg->msg_id,
			pf_to_mgmt->event_flag);
	}
	spin_unlock(&pf_to_mgmt->sync_event_lock);
}

static void recv_mgmt_msg_work_handler(struct work_struct *work)
{
	struct hinic3_mgmt_msg_handle_work *mgmt_work;
	struct hinic3_msg_pf_to_mgmt *pf_to_mgmt;
	struct mgmt_msg_head *ack_cmd;

	mgmt_work = container_of(work, struct hinic3_mgmt_msg_handle_work, work);

	/* At the moment, we do not expect any meaningful messages but if the
	 * sender expects an ACK we still need to provide one with "unsupported"
	 * status.
	 */
	if (mgmt_work->async_mgmt_to_pf)
		goto out;

	pf_to_mgmt = mgmt_work->pf_to_mgmt;
	ack_cmd = pf_to_mgmt->mgmt_ack_buf;
	memset(ack_cmd, 0, sizeof(*ack_cmd));
	ack_cmd->status = MGMT_CMD_UNSUPPORTED;

	hinic3_response_mbox_to_mgmt(pf_to_mgmt->hwdev, mgmt_work->mod,
				     mgmt_work->cmd, ack_cmd, sizeof(*ack_cmd),
				     mgmt_work->msg_id);

out:
	kfree(mgmt_work->msg);
	kfree(mgmt_work);
}

static int recv_msg_add_seg(struct hinic3_recv_msg *recv_msg,
			    u64 msg_header, const void *seg_data,
			    bool *is_complete)
{
	u8 seq_id, msg_id, seg_len, is_last;
	char *msg_buff;
	u32 offset;

	seg_len = HINIC3_MSG_HEADER_GET(msg_header, SEG_LEN);
	is_last = HINIC3_MSG_HEADER_GET(msg_header, LAST);
	seq_id  = HINIC3_MSG_HEADER_GET(msg_header, SEQID);
	msg_id = HINIC3_MSG_HEADER_GET(msg_header, MSG_ID);

	if (seg_len > MGMT_SEG_LEN_MAX)
		return -EINVAL;

	/* All segments but last must be of maximal size */
	if (seg_len != MGMT_SEG_LEN_MAX && !is_last)
		return -EINVAL;

	if (seq_id == 0) {
		recv_msg->seq_id = seq_id;
		recv_msg->msg_id = msg_id;
	} else if (seq_id != recv_msg->seq_id + 1 || msg_id != recv_msg->msg_id) {
		return -EINVAL;
	}

	offset = seq_id * MGMT_SEG_LEN_MAX;
	if (offset + seg_len > MAX_PF_MGMT_BUF_SIZE)
		return -EINVAL;

	msg_buff = recv_msg->msg;
	memcpy(msg_buff + offset, seg_data, seg_len);
	recv_msg->msg_len = offset + seg_len;
	recv_msg->seq_id = seq_id;
	*is_complete = !!is_last;
	return 0;
}

static void init_mgmt_msg_work(struct hinic3_msg_pf_to_mgmt *pf_to_mgmt,
			       struct hinic3_recv_msg *recv_msg)
{
	struct hinic3_mgmt_msg_handle_work *mgmt_work;

	mgmt_work = kzalloc(sizeof(*mgmt_work), GFP_KERNEL);
	if (!mgmt_work)
		return;

	if (recv_msg->msg_len) {
		mgmt_work->msg = kzalloc(recv_msg->msg_len, GFP_KERNEL);
		if (!mgmt_work->msg) {
			kfree(mgmt_work);
			return;
		}
	}

	mgmt_work->pf_to_mgmt = pf_to_mgmt;
	mgmt_work->msg_len = recv_msg->msg_len;
	memcpy(mgmt_work->msg, recv_msg->msg, recv_msg->msg_len);
	mgmt_work->msg_id = recv_msg->msg_id;
	mgmt_work->mod = recv_msg->mod;
	mgmt_work->cmd = recv_msg->cmd;
	mgmt_work->async_mgmt_to_pf = recv_msg->async_mgmt_to_pf;

	INIT_WORK(&mgmt_work->work, recv_mgmt_msg_work_handler);
	queue_work_on(WORK_CPU_UNBOUND, pf_to_mgmt->workq, &mgmt_work->work);
}

static void recv_mgmt_msg_handler(struct hinic3_msg_pf_to_mgmt *pf_to_mgmt,
				  const u8 *header,
				  struct hinic3_recv_msg *recv_msg)
{
	struct hinic3_hwdev *hwdev = pf_to_mgmt->hwdev;
	const void *seg_data;
	bool is_complete;
	u64 msg_header;
	u8 dir, msg_id;
	int err;

	msg_header = *(const u64 *)header;
	dir = HINIC3_MSG_HEADER_GET(msg_header, DIRECTION);
	msg_id = HINIC3_MSG_HEADER_GET(msg_header, MSG_ID);
	/* Don't need to get anything from hw when cmd is async */
	if (dir == HINIC3_MSG_RESPONSE && (msg_id & ASYNC_MSG_FLAG))
		return;

	seg_data = header + sizeof(msg_header);
	err = recv_msg_add_seg(recv_msg, msg_header, seg_data, &is_complete);
	if (err) {
		dev_err(hwdev->dev, "invalid receive segment\n");
		/* set seq_id to invalid seq_id */
		recv_msg->seq_id = MGMT_BOGUS_SEQ_ID;
		return;
	}

	if (!is_complete)
		return;

	recv_msg->cmd = HINIC3_MSG_HEADER_GET(msg_header, CMD);
	recv_msg->mod = HINIC3_MSG_HEADER_GET(msg_header, MODULE);
	recv_msg->async_mgmt_to_pf = HINIC3_MSG_HEADER_GET(msg_header, NO_ACK);
	recv_msg->seq_id = MGMT_BOGUS_SEQ_ID;

	if (dir == HINIC3_MSG_RESPONSE)
		mgmt_resp_msg_handler(pf_to_mgmt, recv_msg);
	else
		init_mgmt_msg_work(pf_to_mgmt, recv_msg);
}

static int alloc_recv_msg(struct hinic3_recv_msg *recv_msg)
{
	recv_msg->seq_id = MGMT_BOGUS_SEQ_ID;

	recv_msg->msg = kzalloc(MAX_PF_MGMT_BUF_SIZE, GFP_KERNEL);
	if (!recv_msg->msg)
		return -ENOMEM;

	return 0;
}

static void free_recv_msg(struct hinic3_recv_msg *recv_msg)
{
	kfree(recv_msg->msg);
}

static int alloc_msg_buf(struct hinic3_msg_pf_to_mgmt *pf_to_mgmt)
{
	struct device *dev = pf_to_mgmt->hwdev->dev;
	int err;

	err = alloc_recv_msg(&pf_to_mgmt->recv_msg_from_mgmt);
	if (err) {
		dev_err(dev, "Failed to allocate recv msg\n");
		return err;
	}

	err = alloc_recv_msg(&pf_to_mgmt->recv_resp_msg_from_mgmt);
	if (err) {
		dev_err(dev, "Failed to allocate resp recv msg\n");
		goto free_msg_from_mgmt;
	}

	pf_to_mgmt->mgmt_ack_buf = kzalloc(MAX_PF_MGMT_BUF_SIZE, GFP_KERNEL);
	if (!pf_to_mgmt->mgmt_ack_buf) {
		err = -ENOMEM;
		goto free_resp_msg_from_mgmt;
	}

	return 0;

free_resp_msg_from_mgmt:
	free_recv_msg(&pf_to_mgmt->recv_resp_msg_from_mgmt);

free_msg_from_mgmt:
	free_recv_msg(&pf_to_mgmt->recv_msg_from_mgmt);
	return err;
}

static void free_msg_buf(struct hinic3_msg_pf_to_mgmt *pf_to_mgmt)
{
	kfree(pf_to_mgmt->mgmt_ack_buf);

	free_recv_msg(&pf_to_mgmt->recv_resp_msg_from_mgmt);
	free_recv_msg(&pf_to_mgmt->recv_msg_from_mgmt);
}

int hinic3_pf_to_mgmt_init(struct hinic3_hwdev *hwdev)
{
	struct hinic3_msg_pf_to_mgmt *pf_to_mgmt;
	int err;

	pf_to_mgmt = kzalloc(sizeof(*pf_to_mgmt), GFP_KERNEL);
	if (!pf_to_mgmt)
		return -ENOMEM;

	hwdev->pf_to_mgmt = pf_to_mgmt;
	pf_to_mgmt->hwdev = hwdev;
	spin_lock_init(&pf_to_mgmt->sync_event_lock);
	pf_to_mgmt->workq = create_singlethread_workqueue(HINIC3_MGMT_WQ_NAME);
	if (!pf_to_mgmt->workq) {
		dev_err(hwdev->dev, "Failed to initialize MGMT workqueue\n");
		err = -ENOMEM;
		goto err_create_mgmt_workq;
	}

	err = alloc_msg_buf(pf_to_mgmt);
	if (err) {
		dev_err(hwdev->dev, "Failed to allocate msg buffers\n");
		goto err_alloc_msg_buf;
	}

	return 0;

err_alloc_msg_buf:
	destroy_workqueue(pf_to_mgmt->workq);

err_create_mgmt_workq:
	kfree(pf_to_mgmt);

	return err;
}

void hinic3_pf_to_mgmt_free(struct hinic3_hwdev *hwdev)
{
	struct hinic3_msg_pf_to_mgmt *pf_to_mgmt = hwdev->pf_to_mgmt;

	/* destroy workqueue before free related pf_to_mgmt resources in case of
	 * illegal resource access
	 */
	destroy_workqueue(pf_to_mgmt->workq);

	free_msg_buf(pf_to_mgmt);
	kfree(pf_to_mgmt);
}

void hinic3_flush_mgmt_workq(struct hinic3_hwdev *hwdev)
{
	if (hwdev->aeqs)
		flush_workqueue(hwdev->aeqs->workq);

	if (HINIC3_IS_PF(hwdev) && hwdev->pf_to_mgmt)
		flush_workqueue(hwdev->pf_to_mgmt->workq);
}

void hinic3_mgmt_msg_aeqe_handler(struct hinic3_hwdev *hwdev, u8 *header, u8 size)
{
	struct hinic3_msg_pf_to_mgmt *pf_to_mgmt;
	struct hinic3_recv_msg *recv_msg;
	bool is_send_dir;

	if ((HINIC3_MSG_HEADER_GET(*(u64 *)header, SOURCE) ==
	     HINIC3_MSG_FROM_MBOX)) {
		hinic3_mbox_func_aeqe_handler(hwdev, header, size);
		return;
	}

	pf_to_mgmt = hwdev->pf_to_mgmt;
	if (!pf_to_mgmt)
		return;

	is_send_dir = (HINIC3_MSG_HEADER_GET(*(u64 *)header, DIRECTION) ==
		       HINIC3_MSG_DIRECT_SEND) ? true : false;

	recv_msg = is_send_dir ? &pf_to_mgmt->recv_msg_from_mgmt :
		   &pf_to_mgmt->recv_resp_msg_from_mgmt;

	recv_mgmt_msg_handler(pf_to_mgmt, header, recv_msg);
}

int hinic3_get_chip_present_flag(const struct hinic3_hwdev *hwdev)
{
	return hwdev->chip_present_flag;
}

int hinic3_msg_to_mgmt_sync(struct hinic3_hwdev *hwdev, u8 mod, u16 cmd, const void *buf_in,
			    u32 in_size, void *buf_out, u32 *out_size,
			    u32 timeout)
{
	if (hinic3_get_chip_present_flag(hwdev) == 0)
		return -EPERM;

	return hinic3_send_mbox_to_mgmt(hwdev, mod, cmd, buf_in, in_size,
					buf_out, out_size, timeout);
}

int hinic3_msg_to_mgmt_no_ack(struct hinic3_hwdev *hwdev, u8 mod, u16 cmd, const void *buf_in,
			      u32 in_size)
{
	if (hinic3_get_chip_present_flag(hwdev) == 0)
		return -EPERM;

	return hinic3_send_mbox_to_mgmt_no_ack(hwdev, mod, cmd, buf_in, in_size);
}
