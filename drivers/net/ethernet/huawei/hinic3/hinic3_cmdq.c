// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/dma-mapping.h>
#include <linux/bitfield.h>

#include "hinic3_cmdq.h"
#include "hinic3_hwdev.h"
#include "hinic3_mbox.h"
#include "hinic3_hwif.h"

#define HINIC3_CMDQ_BUF_SIZE  2048U
#define CMDQ_WQEBB_SIZE       64

#define CMDQ_CMD_TIMEOUT               5000
#define WAIT_CMDQ_ENABLE_TIMEOUT       300

#define CMDQ_CTXT_CURR_WQE_PAGE_PFN_MASK  GENMASK_ULL(51, 0)
#define CMDQ_CTXT_EQ_ID_MASK              GENMASK_ULL(60, 53)
#define CMDQ_CTXT_CEQ_ARM_MASK            BIT_ULL(61)
#define CMDQ_CTXT_CEQ_EN_MASK             BIT_ULL(62)
#define CMDQ_CTXT_HW_BUSY_BIT_MASK        BIT_ULL(63)

#define CMDQ_CTXT_WQ_BLOCK_PFN_MASK       GENMASK_ULL(51, 0)
#define CMDQ_CTXT_CI_MASK                 GENMASK_ULL(63, 52)
#define CMDQ_CTXT_SET(val, member)  \
	FIELD_PREP(CMDQ_CTXT_##member##_MASK, val)

#define CMDQ_WQE_HDR_BUFDESC_LEN_MASK        GENMASK(7, 0)
#define CMDQ_WQE_HDR_COMPLETE_FMT_MASK       BIT(15)
#define CMDQ_WQE_HDR_DATA_FMT_MASK           BIT(22)
#define CMDQ_WQE_HDR_COMPLETE_REQ_MASK       BIT(23)
#define CMDQ_WQE_HDR_COMPLETE_SECT_LEN_MASK  GENMASK(28, 27)
#define CMDQ_WQE_HDR_CTRL_LEN_MASK           GENMASK(30, 29)
#define CMDQ_WQE_HDR_HW_BUSY_BIT_MASK        BIT(31)
#define CMDQ_WQE_HDR_SET(val, member)  \
	FIELD_PREP(CMDQ_WQE_HDR_##member##_MASK, val)
#define CMDQ_WQE_HDR_GET(val, member)  \
	FIELD_GET(CMDQ_WQE_HDR_##member##_MASK, val)

#define CMDQ_CTRL_PI_MASK              GENMASK(15, 0)
#define CMDQ_CTRL_CMD_MASK             GENMASK(23, 16)
#define CMDQ_CTRL_MOD_MASK             GENMASK(28, 24)
#define CMDQ_CTRL_HW_BUSY_BIT_MASK     BIT(31)
#define CMDQ_CTRL_SET(val, member)  \
	FIELD_PREP(CMDQ_CTRL_##member##_MASK, val)
#define CMDQ_CTRL_GET(val, member)  \
	FIELD_GET(CMDQ_CTRL_##member##_MASK, val)

#define WQE_ERRCODE_VAL_MASK              GENMASK(30, 0)
#define WQE_ERRCODE_GET(val, member)  \
	FIELD_GET(WQE_ERRCODE_##member##_MASK, val)

#define CMDQ_DB_INFO_HI_PROD_IDX_MASK  GENMASK(7, 0)
#define CMDQ_DB_INFO_SET(val, member)  \
	FIELD_PREP(CMDQ_DB_INFO_##member##_MASK, val)

#define CMDQ_DB_HEAD_QUEUE_TYPE_MASK   BIT(23)
#define CMDQ_DB_HEAD_CMDQ_TYPE_MASK    GENMASK(26, 24)
#define CMDQ_DB_HEAD_SET(val, member)  \
	FIELD_PREP(CMDQ_DB_HEAD_##member##_MASK, val)

#define CEQE_CMDQ_TYPE_MASK               GENMASK(2, 0)
#define CEQE_CMDQ_GET(val, member)  \
	FIELD_GET(CEQE_CMDQ_##member##_MASK, val)

#define WQE_HEADER(wqe)           ((struct hinic3_cmdq_header *)(wqe))
#define WQE_COMPLETED(ctrl_info)  CMDQ_CTRL_GET(ctrl_info, HW_BUSY_BIT)

#define CMDQ_PFN(addr)  ((addr) >> 12)

/* cmdq work queue's chip logical address table is up to 512B */
#define CMDQ_WQ_CLA_SIZE  512

/* Completion codes: send, direct sync, force stop */
#define CMDQ_SEND_CMPT_CODE         10
#define CMDQ_DIRECT_SYNC_CMPT_CODE  11
#define CMDQ_FORCE_STOP_CMPT_CODE   12

enum data_format {
	DATA_SGE = 0,
	DATA_DIRECT = 1,
};

enum ctrl_sect_len {
	CTRL_SECT_LEN        = 1,
	CTRL_DIRECT_SECT_LEN = 2,
};

enum bufdesc_len {
	BUFDESC_LCMD_LEN = 2,
	BUFDESC_SCMD_LEN = 3,
};

enum completion_format {
	COMPLETE_DIRECT = 0,
	COMPLETE_SGE    = 1,
};

enum cmdq_cmd_type {
	SYNC_CMD_DIRECT_RESP,
	SYNC_CMD_SGE_RESP,
};

#define NUM_WQEBBS_FOR_CMDQ_WQE  1

static struct hinic3_cmdq_wqe *cmdq_read_wqe(struct hinic3_wq *wq, u16 *ci)
{
	if (hinic3_wq_get_used(wq) == 0)
		return NULL;

	*ci = wq->cons_idx & wq->idx_mask;
	return get_q_element(&wq->qpages, wq->cons_idx, NULL);
}

struct hinic3_cmd_buf *hinic3_alloc_cmd_buf(struct hinic3_hwdev *hwdev)
{
	struct hinic3_cmd_buf *cmd_buf;
	struct hinic3_cmdqs *cmdqs;

	cmdqs = hwdev->cmdqs;

	cmd_buf = kzalloc(sizeof(*cmd_buf), GFP_ATOMIC);
	if (!cmd_buf)
		return NULL;

	cmd_buf->buf = dma_pool_alloc(cmdqs->cmd_buf_pool, GFP_ATOMIC,
				      &cmd_buf->dma_addr);
	if (!cmd_buf->buf) {
		dev_err(hwdev->dev, "Failed to allocate cmdq cmd buf from the pool\n");
		goto err_alloc_pci_buf;
	}

	cmd_buf->size = HINIC3_CMDQ_BUF_SIZE;
	atomic_set(&cmd_buf->ref_cnt, 1);

	return cmd_buf;

err_alloc_pci_buf:
	kfree(cmd_buf);
	return NULL;
}

void hinic3_free_cmd_buf(struct hinic3_hwdev *hwdev, struct hinic3_cmd_buf *cmd_buf)
{
	struct hinic3_cmdqs *cmdqs;

	if (!atomic_dec_and_test(&cmd_buf->ref_cnt))
		return;

	cmdqs = hwdev->cmdqs;

	dma_pool_free(cmdqs->cmd_buf_pool, cmd_buf->buf, cmd_buf->dma_addr);
	kfree(cmd_buf);
}

static void cmdq_clear_cmd_buf(struct hinic3_cmdq_cmd_info *cmd_info,
			       struct hinic3_hwdev *hwdev)
{
	if (cmd_info->buf_in) {
		hinic3_free_cmd_buf(hwdev, cmd_info->buf_in);
		cmd_info->buf_in = NULL;
	}
}

static void clear_wqe_complete_bit(struct hinic3_cmdq *cmdq,
				   struct hinic3_cmdq_wqe *wqe, u16 ci)
{
	struct hinic3_cmdq_header *hdr = WQE_HEADER(wqe);
	u32 header_info = hdr->header_info;
	struct hinic3_ctrl *ctrl;
	enum data_format df;

	df = CMDQ_WQE_HDR_GET(header_info, DATA_FMT);
	if (df == DATA_SGE)
		ctrl = &wqe->wqe_lcmd.ctrl;
	else
		ctrl = &wqe->wqe_scmd.ctrl;

	/* clear HW busy bit */
	ctrl->ctrl_info = 0;
	cmdq->cmd_infos[ci].cmd_type = HINIC3_CMD_TYPE_NONE;
	wmb(); /* verify wqe is clear */
	hinic3_wq_put_wqebbs(&cmdq->wq, NUM_WQEBBS_FOR_CMDQ_WQE);
}

static void cmdq_update_cmd_status(struct hinic3_cmdq *cmdq, u16 prod_idx,
				   struct hinic3_cmdq_wqe *wqe)
{
	struct hinic3_cmdq_cmd_info *cmd_info;
	struct hinic3_cmdq_wqe_lcmd *wqe_lcmd;
	u32 status_info;

	wqe_lcmd = &wqe->wqe_lcmd;
	cmd_info = &cmdq->cmd_infos[prod_idx];
	if (cmd_info->errcode) {
		status_info = wqe_lcmd->status.status_info;
		*cmd_info->errcode = WQE_ERRCODE_GET(status_info, VAL);
	}

	if (cmd_info->direct_resp)
		*cmd_info->direct_resp = wqe_lcmd->completion.resp.direct.val;
}

static void cmdq_sync_cmd_handler(struct hinic3_cmdq *cmdq,
				  struct hinic3_cmdq_wqe *wqe, u16 ci)
{
	spin_lock(&cmdq->cmdq_lock);
	cmdq_update_cmd_status(cmdq, ci, wqe);
	if (cmdq->cmd_infos[ci].cmpt_code) {
		*cmdq->cmd_infos[ci].cmpt_code = CMDQ_DIRECT_SYNC_CMPT_CODE;
		cmdq->cmd_infos[ci].cmpt_code = NULL;
	}

	/* Ensure that completion code has been updated before updating done */
	smp_rmb();
	if (cmdq->cmd_infos[ci].done) {
		complete(cmdq->cmd_infos[ci].done);
		cmdq->cmd_infos[ci].done = NULL;
	}
	spin_unlock(&cmdq->cmdq_lock);

	cmdq_clear_cmd_buf(&cmdq->cmd_infos[ci], cmdq->hwdev);
	clear_wqe_complete_bit(cmdq, wqe, ci);
}

void hinic3_cmdq_ceq_handler(struct hinic3_hwdev *hwdev, u32 ceqe_data)
{
	enum hinic3_cmdq_type cmdq_type = CEQE_CMDQ_GET(ceqe_data, TYPE);
	struct hinic3_cmdqs *cmdqs = hwdev->cmdqs;
	struct hinic3_cmdq_wqe_lcmd *wqe_lcmd;
	struct hinic3_cmdq_cmd_info *cmd_info;
	struct hinic3_cmdq_wqe *wqe;
	struct hinic3_cmdq *cmdq;
	u32 ctrl_info;
	u16 ci;

	if (unlikely(cmdq_type >= ARRAY_SIZE(cmdqs->cmdq)))
		return;

	cmdq = &cmdqs->cmdq[cmdq_type];
	while ((wqe = cmdq_read_wqe(&cmdq->wq, &ci)) != NULL) {
		cmd_info = &cmdq->cmd_infos[ci];
		switch (cmd_info->cmd_type) {
		case HINIC3_CMD_TYPE_NONE:
			return;
		case HINIC3_CMD_TYPE_TIMEOUT:
			dev_warn(hwdev->dev, "Cmdq timeout, q_id: %u, ci: %u\n", cmdq_type, ci);
			fallthrough;
		case HINIC3_CMD_TYPE_FAKE_TIMEOUT:
			cmdq_clear_cmd_buf(cmd_info, hwdev);
			clear_wqe_complete_bit(cmdq, wqe, ci);
			break;
		default:
			/* only arm bit is using scmd wqe, the other wqe is lcmd */
			wqe_lcmd = &wqe->wqe_lcmd;
			ctrl_info = wqe_lcmd->ctrl.ctrl_info;
			if (!WQE_COMPLETED(ctrl_info))
				return;

			dma_rmb();
			/* For FORCE_STOP cmd_type, we also need to wait for
			 * the firmware processing to complete to prevent the
			 * firmware from accessing the released cmd_buf
			 */
			if (cmd_info->cmd_type == HINIC3_CMD_TYPE_FORCE_STOP) {
				cmdq_clear_cmd_buf(cmd_info, hwdev);
				clear_wqe_complete_bit(cmdq, wqe, ci);
			} else {
				cmdq_sync_cmd_handler(cmdq, wqe, ci);
			}

			break;
		}
	}
}

static int wait_cmdqs_enable(struct hinic3_cmdqs *cmdqs)
{
	unsigned long end;

	end = jiffies + msecs_to_jiffies(WAIT_CMDQ_ENABLE_TIMEOUT);
	do {
		if (cmdqs->status & HINIC3_CMDQ_ENABLE)
			return 0;
	} while (time_before(jiffies, end) && !cmdqs->disable_flag);

	cmdqs->disable_flag = 1;

	return -EBUSY;
}

static void cmdq_set_completion(struct hinic3_cmdq_completion *complete,
				struct hinic3_cmd_buf *buf_out)
{
	struct hinic3_sge *sge = &complete->resp.sge;

	hinic3_set_sge(sge, buf_out->dma_addr, HINIC3_CMDQ_BUF_SIZE);
}

static struct hinic3_cmdq_wqe *cmdq_get_wqe(struct hinic3_wq *wq, u16 *pi)
{
	if (!hinic3_wq_free_wqebbs(wq))
		return NULL;

	return hinic3_wq_get_one_wqebb(wq, pi);
}

static void cmdq_set_lcmd_bufdesc(struct hinic3_cmdq_wqe_lcmd *wqe,
				  struct hinic3_cmd_buf *buf_in)
{
	hinic3_set_sge(&wqe->buf_desc.sge, buf_in->dma_addr, buf_in->size);
}

static void cmdq_set_db(struct hinic3_cmdq *cmdq,
			enum hinic3_cmdq_type cmdq_type, u16 prod_idx)
{
	u8 __iomem *db_base = cmdq->hwdev->cmdqs->cmdqs_db_base;
	u16 db_ofs = (prod_idx & 0xFF) << 3;
	struct hinic3_cmdq_db db;

	db.db_info = CMDQ_DB_INFO_SET(prod_idx >> 8, HI_PROD_IDX);
	db.db_head = CMDQ_DB_HEAD_SET(1, QUEUE_TYPE) |
		     CMDQ_DB_HEAD_SET(cmdq_type, CMDQ_TYPE);
	writeq(*(u64 *)&db, db_base + db_ofs);
}

static void cmdq_wqe_fill(struct hinic3_cmdq_wqe *hw_wqe,
			  const struct hinic3_cmdq_wqe *shadow_wqe)
{
	const struct hinic3_cmdq_header *src = (struct hinic3_cmdq_header *)shadow_wqe;
	struct hinic3_cmdq_header *dst = (struct hinic3_cmdq_header *)hw_wqe;
	size_t len;

	len = sizeof(struct hinic3_cmdq_wqe) - sizeof(struct hinic3_cmdq_header);
	memcpy(dst + 1, src + 1, len);
	/* Header should be written last */
	wmb();
	WRITE_ONCE(*dst, *src);
}

static void cmdq_prepare_wqe_ctrl(struct hinic3_cmdq_wqe *wqe, u8 wrapped,
				  u8 mod, u8 cmd, u16 prod_idx,
				  enum completion_format complete_format,
				  enum data_format data_format,
				  enum bufdesc_len buf_len)
{
	struct hinic3_cmdq_header *hdr = WQE_HEADER(wqe);
	struct hinic3_cmdq_wqe_lcmd *wqe_lcmd;
	struct hinic3_cmdq_wqe_scmd *wqe_scmd;
	enum ctrl_sect_len ctrl_len;
	struct hinic3_ctrl *ctrl;

	if (data_format == DATA_SGE) {
		wqe_lcmd = &wqe->wqe_lcmd;
		wqe_lcmd->status.status_info = 0;
		ctrl = &wqe_lcmd->ctrl;
		ctrl_len = CTRL_SECT_LEN;
	} else {
		wqe_scmd = &wqe->wqe_scmd;
		wqe_scmd->status.status_info = 0;
		ctrl = &wqe_scmd->ctrl;
		ctrl_len = CTRL_DIRECT_SECT_LEN;
	}

	ctrl->ctrl_info =
		CMDQ_CTRL_SET(prod_idx, PI) |
		CMDQ_CTRL_SET(cmd, CMD) |
		CMDQ_CTRL_SET(mod, MOD);

	hdr->header_info =
		CMDQ_WQE_HDR_SET(buf_len, BUFDESC_LEN) |
		CMDQ_WQE_HDR_SET(complete_format, COMPLETE_FMT) |
		CMDQ_WQE_HDR_SET(data_format, DATA_FMT) |
		CMDQ_WQE_HDR_SET(1, COMPLETE_REQ) |
		CMDQ_WQE_HDR_SET(3, COMPLETE_SECT_LEN) |
		CMDQ_WQE_HDR_SET(ctrl_len, CTRL_LEN) |
		CMDQ_WQE_HDR_SET(wrapped, HW_BUSY_BIT);
}

static void cmdq_set_lcmd_wqe(struct hinic3_cmdq_wqe *wqe,
			      enum cmdq_cmd_type cmd_type,
			      struct hinic3_cmd_buf *buf_in,
			      struct hinic3_cmd_buf *buf_out, u8 wrapped,
			      u8 mod, u8 cmd, u16 prod_idx)
{
	enum completion_format complete_format = COMPLETE_DIRECT;
	struct hinic3_cmdq_wqe_lcmd *wqe_lcmd = &wqe->wqe_lcmd;

	switch (cmd_type) {
	case SYNC_CMD_DIRECT_RESP:
		wqe_lcmd->completion.resp.direct.val = 0;
		break;
	case SYNC_CMD_SGE_RESP:
		if (buf_out) {
			complete_format = COMPLETE_SGE;
			cmdq_set_completion(&wqe_lcmd->completion, buf_out);
		}
		break;
	}

	cmdq_prepare_wqe_ctrl(wqe, wrapped, mod, cmd, prod_idx, complete_format,
			      DATA_SGE, BUFDESC_LCMD_LEN);
	cmdq_set_lcmd_bufdesc(wqe_lcmd, buf_in);
}

static int hinic3_cmdq_sync_timeout_check(struct hinic3_cmdq *cmdq,
					  struct hinic3_cmdq_wqe *wqe, u16 pi)
{
	struct hinic3_cmdq_wqe_lcmd *wqe_lcmd;
	struct hinic3_ctrl *ctrl;
	u32 ctrl_info;

	wqe_lcmd = &wqe->wqe_lcmd;
	ctrl = &wqe_lcmd->ctrl;
	ctrl_info = ctrl->ctrl_info;
	if (!WQE_COMPLETED(ctrl_info)) {
		dev_dbg(cmdq->hwdev->dev, "Cmdq sync command check busy bit not set\n");
		return -EFAULT;
	}
	cmdq_update_cmd_status(cmdq, pi, wqe);
	return 0;
}

static void clear_cmd_info(struct hinic3_cmdq_cmd_info *cmd_info,
			   const struct hinic3_cmdq_cmd_info *saved_cmd_info)
{
	if (cmd_info->errcode == saved_cmd_info->errcode)
		cmd_info->errcode = NULL;

	if (cmd_info->done == saved_cmd_info->done)
		cmd_info->done = NULL;

	if (cmd_info->direct_resp == saved_cmd_info->direct_resp)
		cmd_info->direct_resp = NULL;
}

static int wait_cmdq_sync_cmd_completion(struct hinic3_cmdq *cmdq,
					 struct hinic3_cmdq_cmd_info *cmd_info,
					 struct hinic3_cmdq_cmd_info *saved_cmd_info,
					 u64 curr_msg_id, u16 curr_prod_idx,
					 struct hinic3_cmdq_wqe *curr_wqe,
					 u32 timeout)
{
	ulong timeo = msecs_to_jiffies(timeout);
	int err;

	if (wait_for_completion_timeout(saved_cmd_info->done, timeo))
		return 0;

	spin_lock_bh(&cmdq->cmdq_lock);
	if (cmd_info->cmpt_code == saved_cmd_info->cmpt_code)
		cmd_info->cmpt_code = NULL;

	if (*saved_cmd_info->cmpt_code == CMDQ_DIRECT_SYNC_CMPT_CODE) {
		dev_dbg(cmdq->hwdev->dev, "Cmdq direct sync command has been completed\n");
		spin_unlock_bh(&cmdq->cmdq_lock);
		return 0;
	}

	if (curr_msg_id == cmd_info->cmdq_msg_id) {
		err = hinic3_cmdq_sync_timeout_check(cmdq, curr_wqe,
						     curr_prod_idx);
		if (err)
			cmd_info->cmd_type = HINIC3_CMD_TYPE_TIMEOUT;
		else
			cmd_info->cmd_type = HINIC3_CMD_TYPE_FAKE_TIMEOUT;
	} else {
		err = -ETIMEDOUT;
		dev_err(cmdq->hwdev->dev, "Cmdq sync command current msg id mismatch cmd_info msg id\n");
	}

	clear_cmd_info(cmd_info, saved_cmd_info);
	spin_unlock_bh(&cmdq->cmdq_lock);
	return err;
}

static int cmdq_sync_cmd_direct_resp(struct hinic3_cmdq *cmdq, u8 mod, u8 cmd,
				     struct hinic3_cmd_buf *buf_in, u64 *out_param)
{
	struct hinic3_cmdq_cmd_info *cmd_info, saved_cmd_info;
	struct hinic3_cmdq_wqe *curr_wqe, wqe;
	int cmpt_code = CMDQ_SEND_CMPT_CODE;
	struct hinic3_wq *wq = &cmdq->wq;
	u16 curr_prod_idx, next_prod_idx;
	struct completion done;
	u64 curr_msg_id;
	int errcode;
	u8 wrapped;
	int err;

	spin_lock_bh(&cmdq->cmdq_lock);
	curr_wqe = cmdq_get_wqe(wq, &curr_prod_idx);
	if (!curr_wqe) {
		spin_unlock_bh(&cmdq->cmdq_lock);
		return -EBUSY;
	}

	memset(&wqe, 0, sizeof(wqe));
	wrapped = cmdq->wrapped;
	next_prod_idx = curr_prod_idx + NUM_WQEBBS_FOR_CMDQ_WQE;
	if (next_prod_idx >= wq->q_depth) {
		cmdq->wrapped ^= 1;
		next_prod_idx -= wq->q_depth;
	}

	cmd_info = &cmdq->cmd_infos[curr_prod_idx];
	init_completion(&done);
	atomic_inc(&buf_in->ref_cnt);
	cmd_info->cmd_type = HINIC3_CMD_TYPE_DIRECT_RESP;
	cmd_info->done = &done;
	cmd_info->errcode = &errcode;
	cmd_info->direct_resp = out_param;
	cmd_info->cmpt_code = &cmpt_code;
	cmd_info->buf_in = buf_in;
	saved_cmd_info = *cmd_info;
	cmdq_set_lcmd_wqe(&wqe, SYNC_CMD_DIRECT_RESP, buf_in, NULL,
			  wrapped, mod, cmd, curr_prod_idx);

	cmdq_wqe_fill(curr_wqe, &wqe);
	(cmd_info->cmdq_msg_id)++;
	curr_msg_id = cmd_info->cmdq_msg_id;
	cmdq_set_db(cmdq, HINIC3_CMDQ_SYNC, next_prod_idx);
	spin_unlock_bh(&cmdq->cmdq_lock);

	err = wait_cmdq_sync_cmd_completion(cmdq, cmd_info, &saved_cmd_info,
					    curr_msg_id, curr_prod_idx,
					    curr_wqe, CMDQ_CMD_TIMEOUT);
	if (err) {
		dev_err(cmdq->hwdev->dev, "Cmdq sync command timeout, mod: %u, cmd: %u, prod idx: 0x%x\n",
			mod, cmd, curr_prod_idx);
		err = -ETIMEDOUT;
	}

	if (cmpt_code == CMDQ_FORCE_STOP_CMPT_CODE) {
		dev_dbg(cmdq->hwdev->dev, "Force stop cmdq cmd, mod: %u, cmd: %u\n", mod, cmd);
		err = -EAGAIN;
	}

	smp_rmb(); /* read error code after completion */

	return err ? err : errcode;
}

int hinic3_cmdq_direct_resp(struct hinic3_hwdev *hwdev, u8 mod, u8 cmd,
			    struct hinic3_cmd_buf *buf_in, u64 *out_param)
{
	struct hinic3_cmdqs *cmdqs;
	int err;

	cmdqs = hwdev->cmdqs;
	err = wait_cmdqs_enable(cmdqs);
	if (err) {
		dev_err(hwdev->dev, "Cmdq is disabled\n");
		return err;
	}

	err = cmdq_sync_cmd_direct_resp(&cmdqs->cmdq[HINIC3_CMDQ_SYNC],
					mod, cmd, buf_in, out_param);
	return err;
}

static void cmdq_init_queue_ctxt(struct hinic3_hwdev *hwdev, u8 cmdq_id,
				 struct cmdq_ctxt_info *ctxt_info)
{
	const struct hinic3_cmdqs *cmdqs;
	u64 cmdq_first_block_paddr, pfn;
	const struct hinic3_wq *wq;

	cmdqs = hwdev->cmdqs;
	wq = &cmdqs->cmdq[cmdq_id].wq;
	pfn = CMDQ_PFN(hinic3_wq_get_first_wqe_page_addr(wq));

	ctxt_info->curr_wqe_page_pfn =
		CMDQ_CTXT_SET(1, HW_BUSY_BIT) |
		CMDQ_CTXT_SET(1, CEQ_EN)	|
		CMDQ_CTXT_SET(1, CEQ_ARM)	|
		CMDQ_CTXT_SET(0, EQ_ID) |
		CMDQ_CTXT_SET(pfn, CURR_WQE_PAGE_PFN);

	if (!WQ_IS_0_LEVEL_CLA(wq)) {
		cmdq_first_block_paddr = cmdqs->wq_block_paddr;
		pfn = CMDQ_PFN(cmdq_first_block_paddr);
	}

	ctxt_info->wq_block_pfn =
		CMDQ_CTXT_SET(wq->cons_idx, CI) |
		CMDQ_CTXT_SET(pfn, WQ_BLOCK_PFN);
}

static int init_cmdq(struct hinic3_cmdq *cmdq, struct hinic3_hwdev *hwdev,
		     enum hinic3_cmdq_type q_type)
{
	int err;

	cmdq->cmdq_type = q_type;
	cmdq->wrapped = 1;
	cmdq->hwdev = hwdev;

	spin_lock_init(&cmdq->cmdq_lock);

	cmdq->cmd_infos = kcalloc(cmdq->wq.q_depth, sizeof(*cmdq->cmd_infos),
				  GFP_KERNEL);
	if (!cmdq->cmd_infos) {
		err = -ENOMEM;
		return err;
	}

	return 0;
}

static void free_cmdq(struct hinic3_cmdq *cmdq)
{
	kfree(cmdq->cmd_infos);
}

static int hinic3_set_cmdq_ctxt(struct hinic3_hwdev *hwdev, u8 cmdq_id)
{
	struct comm_cmd_cmdq_ctxt cmdq_ctxt;
	u32 out_size = sizeof(cmdq_ctxt);
	int err;

	memset(&cmdq_ctxt, 0, sizeof(cmdq_ctxt));
	cmdq_init_queue_ctxt(hwdev, cmdq_id, &cmdq_ctxt.ctxt);
	cmdq_ctxt.func_id = hinic3_global_func_id(hwdev);
	cmdq_ctxt.cmdq_id = cmdq_id;

	err = hinic3_send_mbox_to_mgmt(hwdev, HINIC3_MOD_COMM,
				       COMM_MGMT_CMD_SET_CMDQ_CTXT,
				       &cmdq_ctxt, sizeof(cmdq_ctxt),
				       &cmdq_ctxt, &out_size, 0);
	if (err || !out_size || cmdq_ctxt.head.status) {
		dev_err(hwdev->dev, "Failed to set cmdq ctxt, err: %d, status: 0x%x, out_size: 0x%x\n",
			err, cmdq_ctxt.head.status, out_size);
		return -EFAULT;
	}

	return 0;
}

static int hinic3_set_cmdq_ctxts(struct hinic3_hwdev *hwdev)
{
	struct hinic3_cmdqs *cmdqs = hwdev->cmdqs;
	u8 cmdq_type;
	int err;

	for (cmdq_type = 0; cmdq_type < cmdqs->cmdq_num; cmdq_type++) {
		err = hinic3_set_cmdq_ctxt(hwdev, cmdq_type);
		if (err)
			return err;
	}

	cmdqs->status |= HINIC3_CMDQ_ENABLE;
	cmdqs->disable_flag = 0;

	return 0;
}

static int create_cmdq_wq(struct hinic3_hwdev *hwdev, struct hinic3_cmdqs *cmdqs)
{
	u8 cmdq_type;
	int err;

	for (cmdq_type = 0; cmdq_type < cmdqs->cmdq_num; cmdq_type++) {
		err = hinic3_wq_create(hwdev, &cmdqs->cmdq[cmdq_type].wq,
				       HINIC3_CMDQ_DEPTH, CMDQ_WQEBB_SIZE);
		if (err) {
			dev_err(hwdev->dev, "Failed to create cmdq wq\n");
			goto destroy_wq;
		}
	}

	/* 1-level Chip Logical Address (CLA) must put all cmdq's wq page addr in one wq block */
	if (!WQ_IS_0_LEVEL_CLA(&cmdqs->cmdq[HINIC3_CMDQ_SYNC].wq)) {
		if (cmdqs->cmdq[HINIC3_CMDQ_SYNC].wq.qpages.num_pages >
		    CMDQ_WQ_CLA_SIZE / sizeof(u64)) {
			err = -EINVAL;
			dev_err(hwdev->dev, "Cmdq number of wq pages exceeds limit: %lu\n",
				CMDQ_WQ_CLA_SIZE / sizeof(u64));
			goto destroy_wq;
		}

		cmdqs->wq_block_vaddr = dma_alloc_coherent(hwdev->dev,
							   HINIC3_MIN_PAGE_SIZE,
							   &cmdqs->wq_block_paddr,
							   GFP_KERNEL);
		if (!cmdqs->wq_block_vaddr) {
			err = -ENOMEM;
			goto destroy_wq;
		}

		for (cmdq_type = 0; cmdq_type < cmdqs->cmdq_num; cmdq_type++)
			memcpy((u8 *)cmdqs->wq_block_vaddr +
			       CMDQ_WQ_CLA_SIZE * cmdq_type,
			       cmdqs->cmdq[cmdq_type].wq.wq_block_vaddr,
			       cmdqs->cmdq[cmdq_type].wq.qpages.num_pages * sizeof(__be64));
	}

	return 0;

destroy_wq:
	while (cmdq_type > 0) {
		cmdq_type--;
		hinic3_wq_destroy(hwdev, &cmdqs->cmdq[cmdq_type].wq);
	}

	return err;
}

static void destroy_cmdq_wq(struct hinic3_hwdev *hwdev, struct hinic3_cmdqs *cmdqs)
{
	u8 cmdq_type;

	if (cmdqs->wq_block_vaddr)
		dma_free_coherent(hwdev->dev, HINIC3_MIN_PAGE_SIZE,
				  cmdqs->wq_block_vaddr, cmdqs->wq_block_paddr);

	for (cmdq_type = 0; cmdq_type < cmdqs->cmdq_num; cmdq_type++)
		hinic3_wq_destroy(hwdev, &cmdqs->cmdq[cmdq_type].wq);
}

static int init_cmdqs(struct hinic3_hwdev *hwdev)
{
	struct hinic3_cmdqs *cmdqs;

	cmdqs = kzalloc(sizeof(*cmdqs), GFP_KERNEL);
	if (!cmdqs)
		return -ENOMEM;

	hwdev->cmdqs = cmdqs;
	cmdqs->hwdev = hwdev;
	cmdqs->cmdq_num = hwdev->max_cmdq;

	cmdqs->cmd_buf_pool = dma_pool_create("hinic3_cmdq", hwdev->dev,
					      HINIC3_CMDQ_BUF_SIZE, HINIC3_CMDQ_BUF_SIZE, 0ULL);
	if (!cmdqs->cmd_buf_pool) {
		dev_err(hwdev->dev, "Failed to create cmdq buffer pool\n");
		kfree(cmdqs);
		return -ENOMEM;
	}

	return 0;
}

static void cmdq_flush_sync_cmd(struct hinic3_cmdq_cmd_info *cmd_info)
{
	if (cmd_info->cmd_type != HINIC3_CMD_TYPE_DIRECT_RESP)
		return;

	cmd_info->cmd_type = HINIC3_CMD_TYPE_FORCE_STOP;

	if (cmd_info->cmpt_code &&
	    *cmd_info->cmpt_code == CMDQ_SEND_CMPT_CODE)
		*cmd_info->cmpt_code = CMDQ_FORCE_STOP_CMPT_CODE;

	if (cmd_info->done) {
		complete(cmd_info->done);
		cmd_info->done = NULL;
		cmd_info->cmpt_code = NULL;
		cmd_info->direct_resp = NULL;
		cmd_info->errcode = NULL;
	}
}

static void hinic3_cmdq_flush_cmd(struct hinic3_hwdev *hwdev,
				  struct hinic3_cmdq *cmdq)
{
	struct hinic3_cmdq_cmd_info *cmd_info;
	u16 ci;

	spin_lock_bh(&cmdq->cmdq_lock);
	while (cmdq_read_wqe(&cmdq->wq, &ci)) {
		hinic3_wq_put_wqebbs(&cmdq->wq, NUM_WQEBBS_FOR_CMDQ_WQE);
		cmd_info = &cmdq->cmd_infos[ci];
		if (cmd_info->cmd_type == HINIC3_CMD_TYPE_DIRECT_RESP)
			cmdq_flush_sync_cmd(cmd_info);
	}
	spin_unlock_bh(&cmdq->cmdq_lock);
}

void hinic3_cmdq_flush_sync_cmd(struct hinic3_hwdev *hwdev)
{
	struct hinic3_cmdq *cmdq;
	u16 wqe_cnt, wqe_idx, i;
	struct hinic3_wq *wq;

	cmdq = &hwdev->cmdqs->cmdq[HINIC3_CMDQ_SYNC];
	spin_lock_bh(&cmdq->cmdq_lock);
	wq = &cmdq->wq;
	wqe_cnt = hinic3_wq_get_used(wq);
	for (i = 0; i < wqe_cnt; i++) {
		wqe_idx = (wq->cons_idx + i) & wq->idx_mask;
		cmdq_flush_sync_cmd(cmdq->cmd_infos + wqe_idx);
	}
	spin_unlock_bh(&cmdq->cmdq_lock);
}

static void cmdq_reset_all_cmd_buff(struct hinic3_cmdq *cmdq)
{
	u16 i;

	for (i = 0; i < cmdq->wq.q_depth; i++)
		cmdq_clear_cmd_buf(&cmdq->cmd_infos[i], cmdq->hwdev);
}

int hinic3_reinit_cmdq_ctxts(struct hinic3_hwdev *hwdev)
{
	struct hinic3_cmdqs *cmdqs = hwdev->cmdqs;
	u8 cmdq_type;

	for (cmdq_type = 0; cmdq_type < cmdqs->cmdq_num; cmdq_type++) {
		hinic3_cmdq_flush_cmd(hwdev, &cmdqs->cmdq[cmdq_type]);
		cmdq_reset_all_cmd_buff(&cmdqs->cmdq[cmdq_type]);
		cmdqs->cmdq[cmdq_type].wrapped = 1;
		hinic3_wq_reset(&cmdqs->cmdq[cmdq_type].wq);
	}

	return hinic3_set_cmdq_ctxts(hwdev);
}

int hinic3_cmdqs_init(struct hinic3_hwdev *hwdev)
{
	struct hinic3_cmdqs *cmdqs;
	void __iomem *db_base;
	u8 cmdq_type;
	int err;

	err = init_cmdqs(hwdev);
	if (err)
		return err;

	cmdqs = hwdev->cmdqs;
	err = create_cmdq_wq(hwdev, cmdqs);
	if (err)
		goto err_create_wq;

	err = hinic3_alloc_db_addr(hwdev, &db_base, NULL);
	if (err) {
		dev_err(hwdev->dev, "Failed to allocate doorbell address\n");
		goto err_alloc_db;
	}
	cmdqs->cmdqs_db_base = db_base;

	for (cmdq_type = 0; cmdq_type < cmdqs->cmdq_num; cmdq_type++) {
		err = init_cmdq(&cmdqs->cmdq[cmdq_type], hwdev, cmdq_type);
		if (err) {
			dev_err(hwdev->dev, "Failed to initialize cmdq type :%d\n", cmdq_type);
			goto err_init_cmdq;
		}
	}

	err = hinic3_set_cmdq_ctxts(hwdev);
	if (err)
		goto err_init_cmdq;

	return 0;

err_init_cmdq:
	while (cmdq_type > 0) {
		cmdq_type--;
		free_cmdq(&cmdqs->cmdq[cmdq_type]);
	}

	hinic3_free_db_addr(hwdev, cmdqs->cmdqs_db_base, NULL);

err_alloc_db:
	destroy_cmdq_wq(hwdev, cmdqs);

err_create_wq:
	dma_pool_destroy(cmdqs->cmd_buf_pool);
	kfree(cmdqs);

	return err;
}

void hinic3_cmdqs_free(struct hinic3_hwdev *hwdev)
{
	struct hinic3_cmdqs *cmdqs = hwdev->cmdqs;
	u8 cmdq_type;

	cmdqs->status &= ~HINIC3_CMDQ_ENABLE;

	for (cmdq_type = 0; cmdq_type < cmdqs->cmdq_num; cmdq_type++) {
		hinic3_cmdq_flush_cmd(hwdev, &cmdqs->cmdq[cmdq_type]);
		cmdq_reset_all_cmd_buff(&cmdqs->cmdq[cmdq_type]);
		free_cmdq(&cmdqs->cmdq[cmdq_type]);
	}

	hinic3_free_db_addr(hwdev, cmdqs->cmdqs_db_base, NULL);
	destroy_cmdq_wq(hwdev, cmdqs);
	dma_pool_destroy(cmdqs->cmd_buf_pool);
	kfree(cmdqs);
}

bool hinic3_cmdq_idle(struct hinic3_cmdq *cmdq)
{
	return hinic3_wq_get_used(&cmdq->wq) == 0;
}
