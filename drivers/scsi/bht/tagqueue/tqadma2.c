// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: tqadma2.c
 *
 * Abstract: handle tagqueue adma2 transfer cb ops.
 *
 * Version: 1.00
 *
 * Author: Chuanjin
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 9/5/2014		Creation	Chuanjin
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
#include "../include/hostapi.h"

/*
 *
 * Function Name: node_2_srb_ext
 *
 * Abstract:
 *
 *		convert node to srb_ext pointer
 *
 * Input:
 *
 *		node_t *node [in]: Pointer to node
 *
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 *		the srb_ext_t *pointer of the node
 * Notes:
 *
 * Caller:
 *
 */
srb_ext_t *node_2_srb_ext(node_t *node)
{
	srb_ext_t *psrb_ext = (srb_ext_t *) node->psrb_ext;
	return psrb_ext;
}

static bool tq_adma2_init_ctx(void *p)
{
	bool ret = TRUE;
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) p;
	req_queue_t *rq = pdx->tag_queue.wq_build;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	rq->adma2_last_req.data_dir = DATA_DIR_NONE;
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s %d\n",
		__func__, ret);
	return ret;
}

/*
 *
 * Function Name: req_build_cmd
 *
 * Abstract:
 *
 *		build cmd  index & cmd_flag
 *
 * Input:
 *
 *		sd_command_t *cmd [in]: Pointer to sd_cmd_t
 *		request_t *req [in] : the req need build
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
bool req_build_cmd(sd_card_t *card, sd_command_t *cmd, request_t *req)
{
	u32 merge_enable = 0;

	merge_enable =
	    card->host->cfg->host_item.test_tag_queue_capability.enable_srb_merge;

	/* set cmd index */

	/* read */
	if (req->data_dir == DATA_DIR_IN) {
		if ((req->tag_req_t.sec_cnt == 1)
		    && (card->inf_trans_enable == 0) && (merge_enable == 0))
			cmd->cmd_index = SD_CMD17;
		else {
			cmd->cmd_index = SD_CMD18;
			cmd->cmd_flag |= CMD_FLG_MULDATA;
		}
	} else {
		if ((req->tag_req_t.sec_cnt == 1)
		    && (card->inf_trans_enable == 0) && (merge_enable == 0))
			cmd->cmd_index = SD_CMD24;
		else {
			cmd->cmd_index = SD_CMD25;
			cmd->cmd_flag |= CMD_FLG_MULDATA;
		}
	}
	/* set arg */
	cmd->argument = req->tag_req_t.sec_addr;
	/* set flg */
	cmd->cmd_flag |= CMD_FLG_R1 | CMD_FLG_RESCHK;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Exit %s cmd_idx=%x flg=%x\n", __func__, cmd->cmd_index,
		cmd->cmd_flag);
	return TRUE;
}

/*
 *
 * Function Name: tq_adma2_prebuild_io
 *
 * Abstract:
 *
 *		build ADMA2 desc table, prepare cmd
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
bool tq_adma2_prebuild_io(void *p, node_t *node)
{
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) p;
	dma_desc_buf_t *pdma = 0;
	srb_ext_t *pext = node_2_srb_ext(node);
	request_t *req = &pext->req;
	sd_command_t *cmd = &pext->cmd;
	bool ret = FALSE;
	bool dma_64bit = node->card->host->bit64_enable ? TRUE : FALSE;
	bool data_26bit_len =
	    pdx->cfg->host_item.test_dma_mode_setting.enable_dma_26bit_len ? TRUE : FALSE;
	u32 dbg_var = 0;

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
	req_build_cmd(node->card, cmd, req);
	cmd->cmd_flag |= CMD_FLG_ADMA2;

	if (req->gg8_ddr200_workaround)
		cmd->gg8_ddr200_workaround = 1;
	else
		cmd->gg8_ddr200_workaround = 0;

	cmd_set_auto_cmd_flag(&pdx->card, &cmd->cmd_flag);

	/* 2.alloc dma desc buf */

	/* TODO max_len for 64bit dma */
	pdma = node_get_desc_res(node, MAX_ADMA2_TABLE_LEN);
	if (pdma == NULL) {
		DbgErr("%s get desc res failed\n", __func__);
		ret = FALSE;
		goto exit;
	}
	node->phy_node_buffer.head = *pdma;
	/* 3.build ADMA2 Desc */
	dbg_var = req->srb_sg_list[0].Length;

	if (req->gg8_ddr200_workaround) {
		DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM,
			"Add NOP desc\n");
		node->phy_node_buffer.end =
		    build_adma2_desc_nop(req->srb_sg_list, req->srb_sg_len,
					 (byte *) pdma->va, pdma->len,
					 dma_64bit, data_26bit_len);
	} else {
		node->phy_node_buffer.end = build_adma2_desc(req->srb_sg_list,
							     req->srb_sg_len,
							     (byte *) pdma->va,
							     pdma->len,
							     dma_64bit,
							     data_26bit_len);
	}

	if (node->phy_node_buffer.end.va == NULL)
		ret = FALSE;
	else
		ret = TRUE;

exit:
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s ret=%d\n",
		__func__, ret);
	return ret;
}

/*
 *
 * Function Name: tq_adma2_build_io
 *
 * Abstract:
 *
 *		build io for adma2
 *
 * Input:
 *
 *		void *p [in]: Pointer to the bht_dev_ext_t *
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
bool tq_adma2_build_io(void *p, node_t *node)
{
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) p;
	req_queue_t *rq = pdx->tag_queue.wq_build;
	srb_ext_t *pext = node_2_srb_ext(node);
	request_t *req = &pext->req;
	sd_command_t *cmd = &pext->cmd;
	sd_data_t *data = &rq->sd_data;
	bool ret = FALSE;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* 1.bind cmd to TQ current queue(can't bind when prebuild stage for sync) */
	rq->priv = cmd;
	/* 2.bind data to cmd */
	cmd->data = &rq->sd_data;
	data->dir = req->data_dir;
	data->data_mng.total_bytess = req->tag_req_t.sec_cnt * SD_BLOCK_LEN;
	/* 3.cfg system addr */
	data->data_mng.sys_addr = node->general_desc_tbl.pa;

	ret = TRUE;
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s ret=%d\n",
		__func__, ret);
	return ret;

}

/*
 *
 * Function Name: tq_adma2_send_command
 *
 * Abstract:
 *
 *		adma2 issue cmd
 *
 * Input:
 *
 *		void *p [in]: Pointer to the bht_dev_ext_t *
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 *		TRUE: issue cmd ok
 * Notes:
 *
 * Caller:
 *
 */
static bool tq_adma2_send_command(void *p)
{
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) p;
	sd_card_t *card = &(pdx->card);
	tag_queue_t *ptq = &pdx->tag_queue;
	req_queue_t *rq = pdx->tag_queue.wq_cur;
	host_cmd_req_t *cmd_irq_req = &ptq->cmd_req;
	sd_command_t *pcmd = (sd_command_t *) rq->priv;
	bool ret = FALSE;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* bind card */
	cmd_irq_req->card = card;
	/* set cmd type */
	pcmd->sd_cmd = 1;
	/* generate reg */
	cmd_generate_reg(card, pcmd);
	/* issue cmd */
	ret =
	    cmd_execute_sync3(card, pcmd, cmd_irq_req, tag_queue_isr,
			      tq_issue_post_cb);
#if DBG || _DEBUG
	if (ret == FALSE) {
		if (g_dbg_ctrl & DBG_CTRL_DUMP_DESC) {
			req_queue_loop_ctx_ops(ptq->wq_cur,
					       dump_node_adma2_desc, NULL);
		}
	}
#endif
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s ret=%d\n",
		__func__, ret);
	return ret;
}

bool tq_adma2_unload(void *p)
{
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) p;
	sd_card_t *card = &(pdx->card);
	bool ret = FALSE;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* Resorte current DMA mode */
	host_transfer_init(&pdx->host, card->inf_trans_enable, FALSE);
	ret = TRUE;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s ret=%d\n",
		__func__, ret);
	return ret;
}

/*
 *
 * Function Name: tq_adma2_mode_init
 *
 * Abstract:
 *
 *		init TQ ADMA2 mode cbs
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

bool tq_adma2_mode_init(transfer_cb_t *ops)
{
	DbgInfo(MODULE_TQ_DMA, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	os_memset(ops, 0, sizeof(transfer_cb_t));
	ops->init_io = tq_adma2_init_ctx;
	ops->prebuild_io = tq_adma2_prebuild_io;
	ops->build_io = tq_adma2_build_io;
	ops->issue_transfer = tq_adma2_send_command;
	ops->unload = tq_adma2_unload;
	DbgInfo(MODULE_TQ_DMA, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return TRUE;
}

#define ADMA2_DESC_LINK_SUPPORT 1

#if (ADMA2_DESC_LINK_SUPPORT)

/*
 *	static bool clean_int_for_adma2_inf_tb(u8 *link_addr, bool dma_64bit)
 *	{
 *		u32 *ptb = 0;
 *
 *		ptb = (u32 *) (link_addr);
 *		ptb--;
 *		if (dma_64bit == TRUE) {
 *			ptb--;
 *			ptb--;
 *		}
 *		ptb--;
 *		ptb--;
 *		*ptb &= ~(ADMA2_DESC_INT_BIT);
 *		return TRUE;
 *	}
 */

static bool update_link_for_adma2_inf_tb(u8 **link_addr, bool dma_64bit)
{
	u32 *ptb = 0;

	ptb = (u32 *) (*link_addr);
	if (ptb == NULL) {
		DbgErr("%s invalid poniter\n", __func__);
		return FALSE;
	}
	ptb--;
	if (dma_64bit == TRUE) {
		ptb--;
		ptb--;
	}
	ptb--;
	ptb--;
	*ptb = ADMA2_DESC_LINK_VALID;
	/* update pa */
	ptb++;
	(*link_addr) = (u8 *) ptb;

	return TRUE;
}
#else
static bool merge_adma2_inf_tb(u8 *pdesc, u8 *pdesc_end, u8 **link_addr,
			       bool dma_64bit)
{
	u32 *ptb = 0;
	u8 *pbuf = 0;

	ptb = (u32 *) (*link_addr);
	ptb--;
	if (dma_64bit == TRUE) {
		ptb--;
		ptb--;
	}
	ptb--;
	ptb--;
	/* merge adma2 tb */
	pbuf = (u8 *) ptb;
	for (; pdesc < pdesc_end;)
		*(pbuf++) = *(pdesc++);
	/* update pa */
	(*link_addr) = pbuf;

	return TRUE;

}

#endif

/*
 *
 * Function Name: tq_adma2_inf_build_io
 *
 * Abstract:
 *
 *		build adma2 infinite io
 *
 * Input:
 *
 *		void *p [in]: Pointer to the bht_dev_ext_t
 *		 node_t *node [in]:pointer to node which build for
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
bool tq_adma2_inf_build_io(void *p, node_t *node)
{
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) p;
	req_queue_t *rq = pdx->tag_queue.wq_build;
	srb_ext_t *pext = node_2_srb_ext(node);
	request_t *req = &pext->req;
	sd_command_t *cmd = &pext->cmd;
	sd_data_t *data = &rq->sd_data;
	bool ret = FALSE;
	u32 flg = 0;
	bool dma_64bit = node->card->host->bit64_enable ? TRUE : FALSE;
	u32 merge_enable =
	    pdx->cfg->host_item.test_tag_queue_capability.enable_srb_merge;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (merge_enable == TRUE) {
		if (rq->adma2_last_req.data_dir != DATA_DIR_NONE) {
			if (FALSE ==
			    tq_judge_request_continuous(card_is_low_capacity
							(node->card),
							rq->adma2_last_req.data_dir,
							rq->adma2_last_req.sec_addr,
							rq->adma2_last_req.sec_cnt,
							req->data_dir,
							req->tag_req_t.sec_addr)) {
				/* not continue build case */
				ret = FALSE;
				goto exit;
			} else {
				if (pdx->tag_queue.adma2_inf_link_addr == NULL) {
					/*
					 * not link case, for seq but nfcu not match case
					 * fix adma2 merge transfer bug(LinuxStorDriver Issue #3:
					 * Insert USHII Card ,Lead  the Os Hang)
					 */
					ret = FALSE;
					goto exit;
				}
				/* non-first node */
				flg =
				    cmd_can_use_inf(node->card, data->dir,
						    cmd->argument,
						    req->tag_req_t.sec_cnt);
				if (flg == 0) {
					/* can't merge case */
					ret = FALSE;
					goto exit;
				}
#if (ADMA2_DESC_LINK_SUPPORT)
				if (FALSE ==
				    update_link_for_adma2_inf_tb(&
								 (pdx->tag_queue.adma2_inf_link_addr),
								 dma_64bit)) {
					/* can't merge case */
					ret = FALSE;
					goto exit;
				}
				update_adma2_inf_tb(node->phy_node_buffer.end.va,
						    &(pdx->tag_queue.adma2_inf_link_addr),
						    &node->phy_node_buffer.head.pa,
							dma_64bit);
#else
				merge_adma2_inf_tb(node->phy_node_buffer.head.va,
						   node->phy_node_buffer.end.va,
						   &(pdx->tag_queue.adma2_inf_link_addr),
						   dma_64bit);
				update_adma2_inf_tb(pdx->tag_queue.adma2_inf_link_addr,
						    &(pdx->tag_queue.adma2_inf_link_addr),
						    NULL, dma_64bit);

#endif
				rq->sd_data.data_mng.total_bytess +=
				    req->tag_req_t.sec_cnt * SD_BLOCK_LEN;
				/* update */
				rq->adma2_last_req.data_dir = req->data_dir;
				rq->adma2_last_req.sec_addr =
				    req->tag_req_t.sec_addr;
				rq->adma2_last_req.sec_cnt =
				    req->tag_req_t.sec_cnt;
				/* update cmd arg for cmd layer merge case update last_sec */
				if (card_is_low_capacity(node->card))
					cmd->argument =
					    (cmd->argument +
					     req->tag_req_t.sec_cnt *
					     SD_BLOCK_LEN) -
					    rq->sd_data.data_mng.total_bytess;
				else
					cmd->argument =
					    (cmd->argument +
					     req->tag_req_t.sec_cnt) -
					    (rq->sd_data.data_mng.total_bytess /
					     SD_BLOCK_LEN);

				ret = TRUE;
				goto exit;
			}
		}
		/* update */
		rq->adma2_last_req.data_dir = req->data_dir;
		rq->adma2_last_req.sec_addr = req->tag_req_t.sec_addr;
		rq->adma2_last_req.sec_cnt = req->tag_req_t.sec_cnt;
	}

	/* update */

	/* 1. build basic adma2 io */
	if (tq_adma2_build_io(p, node) == FALSE) {
		DbgErr("buid adm2 io failed\n");
		goto exit;
	}

	/* 3. build inifinte table if need */

	flg =
	    cmd_can_use_inf(node->card, data->dir, cmd->argument,
			    req->tag_req_t.sec_cnt);

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM,
		"%s card:%x flg:%x\n", __func__, node->card->has_built_inf,
		flg);

	/* have build infinite */
	if (node->card->has_built_inf) {

		if (CMD_FLG_INF_CON & flg) {
			/* continue case */
			update_adma2_inf_tb(node->phy_node_buffer.end.va,
					    &(pdx->tag_queue.adma2_inf_link_addr),
					    &node->phy_node_buffer.head.pa,
					    dma_64bit);
		} else {
			/* need stop infinite */

			/* can build inf */
			if (CMD_FLG_INF_BUILD & flg) {
				update_adma2_inf_tb(node->phy_node_buffer.end.va,
						    &(pdx->tag_queue.adma2_inf_link_addr),
						    NULL, dma_64bit);
			} else {
				pdx->tag_queue.adma2_inf_link_addr = NULL;
			}
		}
	} else {
		/* can build inf */
		if (CMD_FLG_INF_BUILD & flg) {
			update_adma2_inf_tb(node->phy_node_buffer.end.va,
					    &(pdx->tag_queue.adma2_inf_link_addr),
					    NULL, dma_64bit);
		} else {
			pdx->tag_queue.adma2_inf_link_addr = NULL;
		}

	}
	/* updae cmd flag */
	cmd->cmd_flag |= flg;

	ret = TRUE;
exit:
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s ret=%d\n",
		__func__, ret);
	return ret;

}

/*
 *
 * Function Name: tq_adma2_inf_send_command
 *
 * Abstract:
 *
 *		issue adma2 infinite command
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
 *		TRUE: issue ok
 * Notes:
 *
 * Caller:
 *
 */
bool tq_adma2_inf_send_command(void *p)
{
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) p;
	sd_card_t *card = &(pdx->card);
	tag_queue_t *ptq = &pdx->tag_queue;
	req_queue_t *rq = pdx->tag_queue.wq_cur;
	host_cmd_req_t *cmd_irq_req = &ptq->cmd_req;
	sd_command_t *pcmd = (sd_command_t *) rq->priv;
	bool ret = FALSE;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* 1.bind card */
	cmd_irq_req->card = card;
	/* 2.set cmd type */
	pcmd->sd_cmd = 1;

	if (card->has_built_inf) {
		if (!(pcmd->cmd_flag & CMD_FLG_INF_CON)) {
			sd_command_t sd_cmd;

			ret = card_stop_infinite(card, FALSE, &sd_cmd);
			if (ret == FALSE) {
				DbgErr("%s stop infinite failed\n",
				       __func__);
				pcmd->err = sd_cmd.err;
				goto exit;
			}

		}
	} else {
		if (pcmd->cmd_flag & CMD_FLG_INF_CON) {
			DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM,
				"%s clear INF_CON for infinite no build\n",
				__func__);
			pcmd->cmd_flag &= ~(CMD_FLG_INF_CON);
			pcmd->cmd_flag |= CMD_FLG_INF_BUILD;
		}
	}
	/* 3.generate reg */

	/* todo check return */
	cmd_generate_reg(card, pcmd);

	/* 4.issue cmd */
	ret =
	    cmd_execute_sync3(card, pcmd, cmd_irq_req, tag_queue_isr,
			      tq_issue_post_cb);
#if DBG || _DEBUG
	if (ret == FALSE) {
		if (g_dbg_ctrl & DBG_CTRL_DUMP_DESC) {
			req_queue_loop_ctx_ops(ptq->wq_cur,
					       dump_node_adma2_desc, NULL);
		}
	}
#endif
exit:
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s ret=%d\n",
		__func__, ret);
	return ret;
}

bool tq_adma2_inf_unload(void *p)
{
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) p;
	sd_card_t *card = &(pdx->card);
	req_queue_t *rq = pdx->tag_queue.wq_cur;
	sd_command_t *pcmd = (sd_command_t *) rq->priv;
	bool ret = FALSE;

	if (card->has_built_inf) {
		sd_command_t sd_cmd;

		ret = card_stop_infinite(card, FALSE, &sd_cmd);
		if (ret == FALSE) {
			DbgErr("%s stop infinite failed\n", __func__);
			pcmd->err = sd_cmd.err;
			goto exit;
		}
	}
	ret = TRUE;
exit:
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s ret=%d\n",
		__func__, ret);
	return ret;
}

/*
 *
 * Function Name: tq_adma2_inf_mode_init
 *
 * Abstract:
 *
 *		init adma2 infinite mode
 *
 * Input:
 *
 *		transfer_cb_t *ops [in]: Pointer to the callback
 *
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
bool tq_adma2_inf_mode_init(transfer_cb_t *ops)
{
	DbgInfo(MODULE_TQ_DMA, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	os_memset(ops, 0, sizeof(transfer_cb_t));
	tq_adma2_mode_init(ops);
	ops->build_io = tq_adma2_inf_build_io;
	ops->issue_transfer = tq_adma2_inf_send_command;
	ops->unload = tq_adma2_inf_unload;
	DbgInfo(MODULE_TQ_DMA, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return TRUE;
}
