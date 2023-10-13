// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: tqsdma.c
 *
 * Abstract: handle tagqueue sdma transfer cb ops
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
#include "tq_trans_api.h"
#include "../include/util.h"
#include "../include/cardapi.h"

/*
 *
 * Function Name: tq_sdma_prebuild_io
 *
 * Abstract:
 *
 *		prebuild SDMA CMD argument
 *
 * Input:
 *
 *		void * p [in]: Pointer to the bht_dev_ext_t *
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
bool tq_sdma_prebuild_io(void *p, node_t *node)
{
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) p;
	srb_ext_t *pext = node_2_srb_ext(node);
	request_t *req = &pext->req;
	sd_command_t *cmd = &pext->cmd;
	bool ret = FALSE;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* check parameters */
	if (req->srb_buff == 0) {
		DbgErr("%s null srb buf\n", __func__);
		ret = FALSE;
		goto exit;
	}

	/* 1.build cmd arg */
	os_memset(cmd, 0, sizeof(sd_command_t));
	req_build_cmd(node->card, cmd, req);
	cmd->cmd_flag |= CMD_FLG_SDMA;
	cmd_set_auto_cmd_flag(&pdx->card, &cmd->cmd_flag);

	ret = TRUE;

exit:
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return ret;
}

/*
 *
 * Function Name: tq_sdma_build_io
 *
 * Abstract:
 *
 *		build SDMA CMD ,such as  dma resource
 *
 * Input:
 *
 *		void * p [in]: Pointer to the bht_dev_ext_t *
 *		node_t *node [in] : the node need build
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
bool tq_sdma_build_io(void *p, node_t *node)
{
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) p;
	req_queue_t *rq = pdx->tag_queue.wq_build;
	srb_ext_t *pext = node_2_srb_ext(node);
	request_t *req = &pext->req;
	sd_command_t *cmd = &pext->cmd;
	sd_data_t *data = &rq->sd_data;
	bool ret = FALSE;
	u32 sdma_bd_len = get_sdma_boudary_size(pdx->cfg);
	u32 min_size = 0;
	data_dma_mng_t *mgr = &data->data_mng;
	dma_desc_buf_t dma;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	if (tq_sdma_prebuild_io(p, node) == FALSE) {
		DbgErr("%s prebuild io failed\n", __func__);
		ret = FALSE;
		goto exit;
	}

	/* 1.bind cmd to TQ current queue(can't bind when prebuild stage for sync) */
	rq->priv = cmd;
	/* 2.bind data to cmd */
	cmd->data = &rq->sd_data;
	data->dir = req->data_dir;
	mgr->total_bytess = req->tag_req_t.sec_cnt * SD_BLOCK_LEN;
	mgr->srb_buffer[0].buff = req->srb_buff;
	mgr->offset = 0;
	/* fix to 1 */
	mgr->srb_cnt = 1;

	/* 3.cfg system addr */

	/* align dma buffer */
#define SDMA_BOUNDARY_MAX_SIZE (512*1024)
	if (sdma_bd_len > SDMA_BOUNDARY_MAX_SIZE) {
		DbgErr("%s boundary over max %x\n", __func__, sdma_bd_len);
		ret = FALSE;
		goto exit;
	} else {
		dma = node->data_tbl;
		if (dma_align(&dma, sdma_bd_len) == FALSE) {
			DbgErr("%s align failed\n", __func__);
			ret = FALSE;
			goto exit;
		}
	}
	data->data_mng.sys_addr = dma.pa;
	/* set host driver buffer */
	data->data_mng.driver_buff = (byte *) dma.va;
	/* for write data to card,need fill data first before transfer */
	if (cmd->data->dir == DATA_DIR_OUT) {
		min_size = os_min(sdma_bd_len, mgr->total_bytess);
		os_memcpy(mgr->driver_buff,
			  mgr->srb_buffer[0].buff + mgr->offset, min_size);
		mgr->offset += min_size;
	}
	ret = TRUE;
exit:
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s ret=%d\n",
		__func__, ret);
	return ret;
}

/*
 *
 * Function Name: tq_sdma_send_command
 *
 * Abstract:
 *
 *		issue SDMA CMD
 * Input:
 *
 *		void * p [in]: Pointer to the bht_dev_ext_t *
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 *		TRUE: issue ok
 * Notes:
 *		for SMDA mode, now no support auto CMD23,
 *		driver issue CMD23 before issue read/write CMD.
 * Caller:
 *
 */
bool tq_sdma_send_command(void *p)
{

	bht_dev_ext_t *pdx = (bht_dev_ext_t *) p;
	sd_card_t *card = &(pdx->card);
	tag_queue_t *ptq = &pdx->tag_queue;
	req_queue_t *rq = pdx->tag_queue.wq_cur;
	host_cmd_req_t *cmd_irq_req = &ptq->cmd_req;
	sd_command_t *pcmd = (sd_command_t *) rq->priv;
	bool ret = FALSE;
	sd_command_t cmd23;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* bind card */
	cmd_irq_req->card = card;
	/* set cmd type */
	pcmd->sd_cmd = 1;

	/* generate reg */

	/* SDMA don't use auto CMD23 */
	if ((card->card_type != CARD_UHS2) && (pcmd->cmd_flag & CMD_FLG_AUTO23)) {
		/* clear auto23 flag */
		pcmd->cmd_flag &= ~CMD_FLG_AUTO23;
		ret =
		    card_set_blkcnt(card, &cmd23,
				    pcmd->data->data_mng.total_bytess /
				    SD_BLOCK_LEN);
		if (ret == FALSE) {
			DbgErr("%s issue cmd23 failed\n", __func__);
			goto exit;
		}
	}

	if (cmd_generate_reg(card, pcmd) == FALSE) {
		DbgErr("%s cmd generate reg error\n", __func__);
		ret = FALSE;
		goto exit;
	}
	/* issue cmd */
	ret = cmd_execute_sync2(card, pcmd, cmd_irq_req, tag_queue_isr);
exit:
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s ret=%d\n",
		__func__, ret);
	return ret;

}

/*
 *
 * Function Name: tq_sdma_mode_init
 *
 * Abstract:
 *
 *		init sdma mode
 *
 * Input:
 *
 *		transfer_cb_t *ops [in]: Pointer to the callback
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 *		TRUE: init
 * Notes:
 *		now, SDMA no support infinite  & merge feature.
 * Caller:
 *
 */
bool tq_sdma_mode_init(transfer_cb_t *ops)
{
	DbgInfo(MODULE_TQ_DMA, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	os_memset(ops, 0, sizeof(transfer_cb_t));
	ops->build_io = tq_sdma_build_io;
	ops->issue_transfer = tq_sdma_send_command;
	DbgInfo(MODULE_TQ_DMA, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return TRUE;
}
