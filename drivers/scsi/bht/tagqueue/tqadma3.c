// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: tqadma3.c
 *
 * Abstract: handle tagqueue adma3 transfer cb ops
 *
 * Version: 1.00
 *
 * Author: Chuanjin
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 9/11/2014		Creation	Chuanjin
 */

#include "../include/basic.h"
#include "../include/tqapi.h"
#include "../include/debug.h"
#include "../include/cmdhandler.h"
#include "../include/card.h"
#include "tq_util.h"
#include "../include/util.h"
#include "tq_trans_api.h"

/*
 *
 * Function Name:  get_one_desc_res
 *
 * Abstract:
 *
 *		get one descriptor dma buffer resource
 *
 * Input:
 *
 *		dma_desc_buf_t *cur [in]: Pointer to the dma buffer resource
 *		u32 max_use_size [in] : the max use size for overflow buffer check
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 *		NULL: failed to get dma resource
 *		other: get the dma resource
 * Notes:
 *
 * Caller:
 *
 */
dma_desc_buf_t *get_one_desc_res(dma_desc_buf_t *cur, u32 max_use_size)
{
	dma_desc_buf_t *p = cur;

	if (max_use_size > p->len) {
		DbgErr("%s no enough buf for desc %x > (%x)\n", __func__,
		       max_use_size, p->len);
		return NULL;
	}
	return cur;
}

/*
 *
 * Function Name:  put_one_desc_res
 *
 * Abstract:
 *
 *		put one descriptor size for update dma buffer resource
 *
 * Input:
 *
 *		dma_desc_buf_t *cur [in]: Pointer to the dma buffer resource
 *		size [in] : the use size
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 *		TRUE: put successful
 *		FALSE: put failed
 * Notes:
 *
 * Caller:
 *
 */
bool put_one_desc_res(dma_desc_buf_t *cur, u32 size)
{
	return resize_dma_buf(cur, size);
}

dma_desc_buf_t *get_one_integrate_desc_res(req_queue_t *rq)
{
	return &rq->adma3_integrate_tbl_cur;
}

bool put_one_integrate_desc(req_queue_t *rq, u32 size)
{
	return resize_dma_buf(&rq->adma3_integrate_tbl_cur, size);
}

bool tq_adma3_reset_integrate(req_queue_t *rq)
{
	bool ret = FALSE;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s rq(%d)\n", __func__, rq->id);
	rq->adma3_integrate_tbl_cur = rq->adma3_integrate_tbl;
	if (rq->adma3_integrate_tbl_cur.va == NULL) {
		DbgErr("%s null va\n", __func__);
		goto exit;
		ret = FALSE;
	}
	os_memset(rq->adma3_integrate_tbl_cur.va, 0,
		  rq->adma3_integrate_tbl.len);
	ret = TRUE;
exit:
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return TRUE;
}

/*
 *
 * Function Name:  tq_adma3_init_ctx
 *
 * Abstract:
 *
 *		init adma3 IO context
 *
 * Input:
 *
 *		void *p [in]: Pointer to the bht_dev_ext_t
 *
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 *		TRUE: init successful
 *		FALSE: init failed
 * Notes:
 *
 * Caller:
 *
 */
static bool tq_adma3_init_ctx(void *p)
{
	bool ret = FALSE;
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) p;
	req_queue_t *rq = pdx->tag_queue.wq_build;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	ret = tq_adma3_reset_integrate(rq);
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s %d\n",
		__func__, ret);
	return ret;
}

/*
 *
 * Function Name: tq_adma3_prebuild_io
 *
 * Abstract:
 *
 *		build ADMA3 cmd & adma2 desc table, however not build integrate table.
 *
 * Input:
 *
 *		void *p [in]: Pointer to the bht_dev_ext_t *
 *		node_t *node [in] : the node need prebuild
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 *		TRUE: build ok
 * Notes:
 *
 * Caller:
 *
 */
bool tq_adma3_prebuild_io(void *p, node_t *node)
{
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) p;
	dma_desc_buf_t *pdma = 0, dma_buf;
	srb_ext_t *pext = node_2_srb_ext(node);
	request_t *req = &pext->req;
	sd_command_t *cmd = &pext->cmd;
	sd_card_t *card = node->card;
	bool ret = FALSE;
	sd_data_t mdata;
	u32 size = 0;
	bool dma_64bit = card->host->bit64_enable ? TRUE : FALSE;
	bool data_26bit_len =
	    pdx->cfg->host_item.test_dma_mode_setting.enable_dma_26bit_len ? TRUE : FALSE;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* check parameters */
	if (req->srb_sg_len == 0 || req->srb_sg_list == NULL) {
		DbgErr("%s null sglist\n", __func__);
		ret = FALSE;
		goto exit;
	}

	/* 1.build cmd arg */
	os_memset(cmd, 0, sizeof(sd_command_t));
	req_build_cmd(card, cmd, req);
	cmd_set_auto_cmd_flag(&pdx->card, &cmd->cmd_flag);
	cmd->cmd_flag |= CMD_FLG_ADMA3;
	/* 2. generate regs */
	cmd->data = &mdata;
	mdata.dir = req->data_dir;
	mdata.data_mng.total_bytess = req->tag_req_t.sec_cnt * SD_BLOCK_LEN;
	/* set cmd type */
	cmd->sd_cmd = 1;
	cmd_generate_reg(card, cmd);

	/* 3.alloc dma desc buf */

	/* TODO max size */
	pdma = node_get_desc_res(node, MAX_ADMA2_TABLE_LEN);

	if (pdma == NULL) {
		DbgErr("Adma3 Get desc res failed\n");
		ret = FALSE;
		goto exit;
	}
	dma_buf = *pdma;
	/* no change node desc buffer, or cause len small */
	pdma = &dma_buf;
	node->phy_node_buffer.head = *pdma;

	/* 4.build cmd desc */
	size = build_card_cmd_desc(card, pdma->va, cmd);

	resize_dma_buf(pdma, size);
	/* 5.build ADMA2 Desc */
	node->phy_node_buffer.end = build_adma2_desc(req->srb_sg_list,
						     req->srb_sg_len,
						     (byte *) pdma->va,
						     pdma->len, dma_64bit,
						     data_26bit_len);
	if (node->phy_node_buffer.end.va == NULL) {
		DbgErr("%s build adm2 desc failed\n", __func__);
		ret = FALSE;
		goto exit;
	} else
		ret = TRUE;
	/* integrate table must delay to build stage. or can't support multi */

exit:
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s ret=%d\n",
		__func__, ret);
	return ret;
}

/*
 *
 * Function Name:  tq_adma3_build_io
 *
 * Abstract:
 *
 *		build adma3 context
 *
 * Input:
 *
 *		void *p [in]: Pointer to the bht_dev_ext_t
 *		node_t *node [in]: pointer to node which need build
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 *		TRUE: build successful
 *		FALSE: build failed
 * Notes:
 *
 * Caller:
 *
 */
bool tq_adma3_build_io(void *p, node_t *node)
{
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) p;
	dma_desc_buf_t *pdma = 0;
	req_queue_t *rq = pdx->tag_queue.wq_build;
	request_t *req = &node_2_srb_ext(node)->req;
	sd_command_t *cmd = &node_2_srb_ext(node)->cmd;
	sd_data_t *data = &rq->sd_data;
	bool ret = FALSE;
	byte *pdesc = 0;
	u32 size = 0;
	bool dma_64bit = node->card->host->bit64_enable ? TRUE : FALSE;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (tq_adma3_prebuild_io(p, node) == FALSE)
		goto exit;
	/* cfg integrated desc */
	pdma = get_one_integrate_desc_res(rq);
	pdesc =
	    build_integrated_desc(pdma->va, &(node->phy_node_buffer.head.pa),
				  dma_64bit);
	size = pp_ofs(pdesc, pdma->va);
	put_one_integrate_desc(rq, size);
	/* 1.bind cmd to TQ current queue(can't bind when prebuild stage for sync) */
	rq->priv = cmd;
	/* 2.bind data to cmd */
	cmd->data = &rq->sd_data;
	data->dir = req->data_dir;
	data->data_mng.total_bytess = req->tag_req_t.sec_cnt * SD_BLOCK_LEN;
	/* 3.cfg system addr */
	data->data_mng.sys_addr = rq->adma3_integrate_tbl.pa;
	ret = TRUE;
exit:
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s ret=%d\n",
		__func__, ret);
	return ret;
}

/*
 *
 * Function Name:  tq_adma3_send_command
 *
 * Abstract:
 *
 *		send adma3 command
 *
 * Input:
 *
 *		void *p [in]: Pointer to the bht_dev_ext_t
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 *		TRUE: issue cmd successful
 *		FALSE: issue failed
 * Notes:
 *
 * Caller:
 *
 */

void dump_adma3_integrate_desc(u8 *desc, bool dma_64bit, u32 cnt)
{
	u32 size = 0;

	if (dma_64bit == TRUE)
		size = cnt * ADMA3_INTEGRATEDDESC_128BIT_ITEM_LEN;
	else
		size = cnt * ADMA3_INTEGRATEDDESC_ITEM_LEN;
	DbgErr("%s integrate cnt=%d\n", __func__, cnt);
	dbg_dump_general_desc_tb(desc, size);
}

/*
 *
 * Function Name:dump_node_adma3_desc
 *
 * Abstract:
 *
 *		dump node adma3 desc
 *
 * Input:
 *
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 * Notes:
 *
 * Caller:
 *
 */
static bool dump_node_adma3_desc(node_t *node, void *ctx)
{
	phy_addr_t sys_addr;
	u8 *desc = node->phy_node_buffer.head.va;
	u8 *desc_end = node->phy_node_buffer.end.va;

	sys_addr = node->phy_node_buffer.head.pa;
	DbgErr("sys addrl %x addrh %x\n", os_get_phy_addr32l(sys_addr),
	       os_get_phy_addr32h(sys_addr));
	dump_adma2_desc(desc, desc_end);
	return TRUE;
}

bool tq_adma3_send_command(void *p)
{
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) p;
	sd_card_t *card = &(pdx->card);
	tag_queue_t *ptq = &pdx->tag_queue;
	req_queue_t *rq = pdx->tag_queue.wq_cur;
	host_cmd_req_t *cmd_irq_req = &ptq->cmd_req;
	sd_command_t *pcmd = (sd_command_t *) rq->priv;
	bool ret = FALSE;
	bool dma_64bit = card->host->bit64_enable ? TRUE : FALSE;
	u32 merge_enable =
	    pdx->cfg->host_item.test_tag_queue_capability.enable_srb_merge;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* 1. generate integare table */
	adma3_end_integrated_tb(rq->adma3_integrate_tbl_cur.va, dma_64bit);

	/* bind card */
	cmd_irq_req->card = card;

	/* merge test */
	if (merge_enable)
		adma3_merge_io_descriptor(rq, card, dma_64bit);

	/* 3. issue command */
	ret =
	    cmd_execute_sync3(card, pcmd, cmd_irq_req, tag_queue_isr,
			      tq_issue_post_cb);
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s ret=%d\n",
		__func__, ret);
#if DBG || _DEBUG
	if (ret == FALSE) {
		if (g_dbg_ctrl & DBG_CTRL_DUMP_DESC) {
			u32 cnt = 0;

			cnt = node_list_get_cnt(&ptq->wq_cur->list);
			DbgErr("ADMA3 sys addrl %x addrh %x\n",
			       os_get_phy_addr32l(rq->adma3_integrate_tbl.pa),
			       os_get_phy_addr32h(rq->adma3_integrate_tbl.pa));
			dump_adma3_integrate_desc(rq->adma3_integrate_tbl.va,
						  dma_64bit, cnt);
			req_queue_loop_ctx_ops(ptq->wq_cur,
					       dump_node_adma3_desc, NULL);
		}
	}
#endif
	return ret;
}

bool tq_adma3_poweroff_need_rebuild(void *p)
{
	/* when card poweroff, adma3 need rebuild transfer ctx */
	return TRUE;
}

/*
 *
 * Function Name: tq_adma3_mode_init
 *
 * Abstract:
 *
 *		init TQ ADMA3 mode cbs
 *
 * Input:
 *
 *		transfer_cb_t *ops [in]: Pointer to the transfer_cb_t
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 *		TRUE: init ok
 * Notes:
 *
 * Caller:
 *
 */
bool tq_adma3_mode_init(transfer_cb_t *ops)
{
	DbgInfo(MODULE_TQ_DMA, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	os_memset(ops, 0, sizeof(transfer_cb_t));
	ops->init_io = tq_adma3_init_ctx;
	/* for amda3 auto poweroff case, adma3 can't get card type(uhs2 or legacy) */
	ops->prebuild_io = NULL;
	ops->build_io = tq_adma3_build_io;
	ops->issue_transfer = tq_adma3_send_command;
	ops->poweroff_need_rebuild = tq_adma3_poweroff_need_rebuild;
	DbgInfo(MODULE_TQ_DMA, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return TRUE;
}
