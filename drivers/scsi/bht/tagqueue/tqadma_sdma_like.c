// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: tqadma_sdma_like.c
 *
 * Abstract: handle tagqueue sdma_like transfer cb ops
 *
 * Version: 1.00
 *
 * Author: Chuanjin
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 11/1/2014		Creation	Chuanjin
 */

#include "../include/basic.h"
#include "../include/tqapi.h"
#include "../include/debug.h"
#include "../include/cmdhandler.h"
#include "../include/card.h"
#include "tq_util.h"
#include "tq_trans_api.h"
#include "../include/util.h"
#include "../include/hostapi.h"

/*
 *
 * Function Name: tq_adma2_sdmalike_prebuild_io
 *
 * Abstract:
 *
 *		prebuild sdma-like mode adma2 io
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
static bool tq_adma2_sdmalike_prebuild_io(void *p, node_t *node)
{
	srb_ext_t *pext = node_2_srb_ext(node);
	request_t *req = &pext->req;
	dma_desc_buf_t *pdma = 0;
	bool ret = TRUE;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* 1. get sdma like buf address */
	pdma = &node->data_tbl;

	/* 2. mark it's sdma-like node */
	node->sdma_like = 1;

	/* 3. generate sdma like sglist table */

	if (gen_sdma_like_sgl(req, pdma) == FALSE) {
		DbgErr("%s sdma like sgl failed\n", __func__);
		ret = FALSE;
		goto exit;
	}

	/* 4. call adma2 build API */
	ret = tq_adma2_prebuild_io(p, node);
exit:
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return ret;
}

/*
 *
 * Function Name: tq_adma2_sdmalike_build_io
 *
 * Abstract:
 *
 *		build sdma-like io
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
bool tq_adma2_sdmalike_copy(void *p, node_t *node)
{
	srb_ext_t *pext = node_2_srb_ext(node);
	request_t *req = &pext->req;

	/* copy for write case */
	if (req->data_dir == DATA_DIR_OUT) {
		os_memcpy(node->data_tbl.va, req->srb_buff,
			  req->tag_req_t.sec_cnt * SD_BLOCK_LEN);
	}

	return TRUE;

}

static bool tq_adma2_sdmalike_build_io(void *p, node_t *node)
{
#if (!CFG_OS_LINUX)
	tq_adma2_sdmalike_copy(p, node);
#endif
	return tq_adma2_build_io(p, node);

}

/*
 *
 * Function Name: tq_adma2_sdmalike_mode_init
 *
 * Abstract:
 *
 *		init TQ ADMA2 sdma-like mode cbs
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
bool tq_adma2_sdmalike_mode_init(transfer_cb_t *ops)
{
	DbgInfo(MODULE_TQ_DMA, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	os_memset(ops, 0, sizeof(transfer_cb_t));
	tq_adma2_mode_init(ops);
	ops->build_io = tq_adma2_sdmalike_build_io;
	ops->prebuild_io = tq_adma2_sdmalike_prebuild_io;
	DbgInfo(MODULE_TQ_DMA, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return TRUE;
}

/*
 *
 * Function Name: tq_adma2_inf_sdmalike_build_io
 *
 * Abstract:
 *
 *		build adma2 infinite sdma-like io
 *
 * Input:
 *
 *		void * p [in]: Pointer to the bht_dev_ext_t
 *		 node_t *node [in]:pointer to node which build for
 *
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
static bool tq_adma2_inf_sdmalike_build_io(void *p, node_t *node)
{
#if (!CFG_OS_LINUX)
	tq_adma2_sdmalike_copy(p, node);
#endif
	return tq_adma2_inf_build_io(p, node);

}

/*
 *
 * Function Name: tq_adma2_inf_sdmalike_mode_init
 *
 * Abstract:
 *
 *		init TQ ADMA2 infinite sdma-like mode cbs
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
bool tq_adma2_inf_sdmalike_mode_init(transfer_cb_t *ops)
{
	DbgInfo(MODULE_TQ_DMA, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	os_memset(ops, 0, sizeof(transfer_cb_t));
	tq_adma2_sdmalike_mode_init(ops);
	ops->build_io = tq_adma2_inf_sdmalike_build_io;
	ops->issue_transfer = tq_adma2_inf_send_command;
	ops->unload = tq_adma2_inf_unload;
	DbgInfo(MODULE_TQ_DMA, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return TRUE;
}

/*
 *
 * Function Name: tq_adma2_sdmalike_prebuild_io
 *
 * Abstract:
 *
 *		prebuild sdma-like mode adma2 io
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
static bool tq_adma3_sdmalike_prebuild_io(void *p, node_t *node)
{
	srb_ext_t *pext = node_2_srb_ext(node);
	request_t *req = &pext->req;
	dma_desc_buf_t *pdma = 0;
	bool ret = TRUE;

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* 1. get sdma like buf address */
	pdma = &node->data_tbl;

	/* 2. mark it's sdma-like node */
	node->sdma_like = 1;

	/* 3. generate sdma like sglist table */

	if (gen_sdma_like_sgl(req, pdma) == FALSE) {
		DbgErr("%s sdma like sgl failed\n", __func__);
		ret = FALSE;
		goto exit;
	}

	/* 4. call adma3 build API */
	ret = tq_adma3_prebuild_io(p, node);
exit:
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s ret=%x\n",
		__func__, ret);
	return ret;
}

/*
 *
 * Function Name: tq_adma3_sdmalike_build_io
 *
 * Abstract:
 *
 *		build adma3 sdma-like io
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
static bool tq_adma3_sdmalike_build_io(void *p, node_t *node)
{
	if (tq_adma3_sdmalike_prebuild_io(p, node) == FALSE)
		return FALSE;
#if (!CFG_OS_LINUX)
	tq_adma2_sdmalike_copy(p, node);
#endif
	return tq_adma3_build_io(p, node);

}

/*
 *
 * Function Name: tq_adma3_sdmalike_mode_init
 *
 * Abstract:
 *
 *		init TQ ADMA2 sdma-like mode cbs
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
bool tq_adma3_sdmalike_mode_init(transfer_cb_t *ops)
{
	DbgInfo(MODULE_TQ_DMA, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	os_memset(ops, 0, sizeof(transfer_cb_t));
	tq_adma3_mode_init(ops);
	ops->build_io = tq_adma3_sdmalike_build_io;
	ops->prebuild_io = NULL;
	DbgInfo(MODULE_TQ_DMA, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return TRUE;
}
