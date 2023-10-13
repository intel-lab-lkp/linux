// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: tagqueue.c
 *
 * Abstract: handle tagqueue work flow
 *
 * Version: 1.00
 *
 * Author: Chuanjin
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 9/4/2014		Creation	Chuanjin
 */

#include "../include/basic.h"
#include "../include/host.h"
#include "../include/tqapi.h"
#include "../include/cardapi.h"
#include "../include/funcapi.h"
#include "../include/reqapi.h"
#include "../include/debug.h"
#include "tq_trans_api.h"
#include "../include/util.h"
#include "../include/cmdhandler.h"
#include "tq_util.h"
#include "../include/hostapi.h"

/*
 *
 * Function Name:  node_malloc
 *
 * Abstract:
 *
 *		get one node from tag queue node pool
 *
 * Input:
 *
 *		tag_queue_t *tq [in]: Pointer to the tag queue
 *
 * Output:
 *
 *		None.
 *
 * Return value: NULL means can't get one successful
 *
 * Notes:  must use node_mfree for release
 *
 * Caller:
 *
 */
static node_t *node_malloc(tag_queue_t *tq)
{
	if (tq == NULL) {
		DbgErr("%s tq NULL\n", __func__);
		return NULL;
	}
	return node_list_get_one(&tq->node_pool_list);
}

/*
 *
 * Function Name:  node_mfree
 *
 * Abstract:
 *
 *		put  the node back to  tag queue node pool
 *
 * Input:
 *
 *		tag_queue_t *tq [in]: Pointer to the tag queue
 *		node_t *node [nin] : the node which need free
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 * Notes:
 *
 * Caller:
 *
 */
static void node_mfree(tag_queue_t *tq, node_t *node)
{
	if (node)
		node_list_put_one(&tq->node_pool_list, node);
	else
		DbgErr("%s node NULL\n", __func__);
}

/*
 *
 * Function Name:  node_pool_init
 *
 * Abstract:
 *
 *		init node pool
 *
 * Input:
 *
 *		tag_queue_t *tq [in]: Pointer to the tag queue
 *		dma_desc_buf_t *dma_res [in]: system dma buffer resource
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 * Notes:  this call must call before tq_setup_dma_res() for use DMA resource
 *
 * Caller:
 *
 */
static bool node_pool_init(PVOID pdx, tag_queue_t *tq,
			   dma_desc_buf_t *dma_res, bool sdma_like)
{
	u32 i = 0;
	dma_desc_buf_t dma;
	node_t *node = 0;
	bool ret = FALSE;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Enter %s sdma_like:%d\n", __func__, sdma_like);
	/* check input */
	if (tq == NULL || dma_res == NULL) {
		ret = FALSE;
		DbgErr("%s tq or dma NULL\n", __func__);
		goto exit;
	}

	if ((dma_res->va == 0) || (dma_res->len == 0)) {
		ret = FALSE;
		DbgErr("%s dma va:%p len:%xh\n", __func__, dma_res->va,
		       dma_res->len);
		goto exit;
	}

	if (dma_res->len <= (tq->max_wq_req_size * MAX_GENERAL_DESC_TABLE_LEN)) {
		ret = FALSE;
		DbgErr("dma buf too small 0x%x <=(%x)\n", dma_res->len,
		       tq->max_wq_req_size * MAX_GENERAL_DESC_TABLE_LEN);
		goto exit;
	}
	/* init each node general desc buf */
	dma = *dma_res;
#if CFG_OS_LINUX
	os_list_init(&tq->node_pool_list);
#else
	os_list_init(pdx, &tq->node_pool_list);
#endif
	for (i = 0; i < tq->max_wq_req_size; i++) {
		node = &tq->node[i];
		node->general_desc_tbl = dma;
		node->general_desc_tbl.len = MAX_GENERAL_DESC_TABLE_LEN;
		node->general_desc_tbl_img = node->general_desc_tbl;
		resize_dma_buf(&dma, MAX_GENERAL_DESC_TABLE_LEN);
		node_list_put_one(&tq->node_pool_list, node);
		DbgInfo(MODULE_TQ_FLOW, FEATURE_DRIVER_INIT, NOT_TO_RAM,
			"general desc tbl len %x\n",
			node->general_desc_tbl.len);
	}
	ret = TRUE;
	/* update dma_res for usage */
	resize_dma_buf(dma_res,
		       tq->max_wq_req_size * MAX_GENERAL_DESC_TABLE_LEN);

	/* if sdma like mode enable, init each node SDMA like buf */
	if (sdma_like == TRUE) {
		/* dma buf align */
		if (dma_align(dma_res, DMA_BUF_ALIGN_SIZE) == FALSE) {
			DbgErr("%s sdma-like dma align failed\n", __func__);
			ret = FALSE;
			goto exit;
		}
		/* check buf enough */
		if (dma_res->len <=
		    tq->max_wq_req_size * MAX_SDMA_LIKE_DATA_SIZE) {
			ret = FALSE;
			DbgErr("dma resource too small 0x%x <(%x)\n",
			       dma_res->len,
			       tq->max_wq_req_size * MAX_SDMA_LIKE_DATA_SIZE);
			goto exit;
		}
		/* init each node for sdma-like mode */
		dma = *dma_res;
		for (i = 0; i < tq->max_wq_req_size; i++) {
			node = &tq->node[i];
			node->data_tbl = dma;
			node->data_tbl.len = MAX_SDMA_LIKE_DATA_SIZE;
			node->data_tbl_img = node->data_tbl;
			DbgInfo(MODULE_TQ_FLOW, FEATURE_DRIVER_INIT, NOT_TO_RAM,
				"data tbl len %x\n", node->data_tbl.len);
			if (FALSE ==
			    resize_dma_buf(&dma, MAX_SDMA_LIKE_DATA_SIZE)) {
				DbgErr
				    ("node sdma-like data buf resize failed\n");
				ret = FALSE;
				goto exit;
			}
		}
		/* update dma_res for usage */
		resize_dma_buf(dma_res,
			       tq->max_wq_req_size * MAX_SDMA_LIKE_DATA_SIZE);
	}
exit:
	DbgInfo(MODULE_TQ_FLOW, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Exit %s ret:%d\n", __func__, ret);
	return ret;
}

/*
 *
 * Function Name:  node_notify_complete
 *
 * Abstract:
 *
 *		complete one node by call upper layer callback
 *
 * Input:
 *
 *		bht_dev_ext_t *pdx [in]: Pointer to the bht adapter extension structure
 *		node_t *pnode [in]: the node which need complete
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 * Notes:
 *
 * Caller:
 *
 */
static int node_notify_complete(bht_dev_ext_t *pdx, node_t *pnode)
{
	srb_ext_t *psrb_ext = 0;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (pnode && pdx) {
		psrb_ext = (srb_ext_t *) pnode->psrb_ext;
		if (psrb_ext && psrb_ext->req.srb_done_cb) {
			if (psrb_ext->req.result == REQ_RESULT_OK) {
				if (pnode->sdma_like) {
					if (DATA_DIR_IN ==
					    psrb_ext->req.data_dir)
						os_memcpy(psrb_ext->req.srb_buff,
							  pnode->data_tbl.va,
							  psrb_ext->req.tag_req_t.sec_cnt
							  * SD_BLOCK_LEN);
				}
			}

			psrb_ext->req.srb_done_cb(pdx, psrb_ext);
		} else {
			DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
				"%s ext%p cb%p\n", __func__, psrb_ext,
				psrb_ext->req.srb_done_cb);
		}
	} else {
		DbgErr("%s NULL %p %p\n", __func__, pnode, pdx);
	}

	return 0;
}

/*
 *
 * Function Name:  node_mark_node_status
 *
 * Abstract:
 *
 *		the group for make node request result value
 *
 * Input:
 *
 *
 *		node_t *pnode [in]: the node which need make request status
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 * Notes:
 *
 * Caller:
 *
 */
static bool node_mark_node_status(node_t *pnode, void *ctx)
{
	bool ret = TRUE;
	srb_ext_t *psrb_ext = 0;
	e_req_result result = REQ_RESULT_ACCESS_ERR;

	if (ctx == NULL) {
		DbgErr("%s ctx null\n", __func__);
		ret = FALSE;
		goto exit;
	}

	if (pnode) {
		psrb_ext = (srb_ext_t *) pnode->psrb_ext;
		result = *((e_req_result *) ctx);
		if (psrb_ext)
			psrb_ext->req.result = result;
		DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
			"%s mark sta:%x\n", __func__, result);
	}
exit:
	return ret;
}

/*
 *
 * Function Name:  node_list_get_one
 *
 * Abstract:
 *
 *		get one node from  the queue
 *
 * Input:
 *
 *		list_t *p [in]: Pointer to the list
 *
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *		node_t *, NULL means empty queue
 *
 * Notes:
 *
 * Caller:
 *
 */
node_t *node_list_get_one(list_t *p)
{
	node_t *node = 0;
	list_entry *plist = 0;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_FUNC_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* check input */
	if (p == NULL) {
		DbgErr("%s:p NULL\n", __func__);
		node = NULL;
		goto exit;
	}
	/* get list entry */
	plist = os_list_locked_remove_head(p);
	if (plist == NULL) {
		node = NULL;
		goto exit;
	}

	/* sometime caller try to get empty queue, the counter no need sub if emtpy */
	if (os_atomic_read(&p->cnt))
		os_atomic_sub(&p->cnt, 1);
	node = os_container_of(plist, node_t, list);
exit:
	DbgInfo(MODULE_TQ_FLOW, FEATURE_FUNC_TRACE, NOT_TO_RAM,
		"Exit %s ret:%x\n", __func__, node);
	return node;
}

/*
 *
 * Function Name:  node_list_head_put_one
 *
 * Abstract:
 *
 *		put the node to  the queue head
 *
 * Input:
 *
 *		list_t *p [in]: Pointer to the list
 *		node_t *pnode [in] : the node to put
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 * Notes:
 *
 * Caller:
 *
 */
void node_list_head_put_one(list_t *p, node_t *pnode)
{
	if (p == NULL) {
		DbgErr("%s:p NULL\n", __func__);
		return;
	}
	os_atomic_add(&p->cnt, 1);
	os_list_locked_insert_head(p, &pnode->list);
}

/*
 *
 * Function Name:  node_list_put_one
 *
 * Abstract:
 *
 *		put the node to  the queue tail
 *
 * Input:
 *
 *		list_t *p [in]: Pointer to the list
 *		node_t *pnode [in] : the node to put
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 * Notes:
 *
 * Caller:
 *
 */
void node_list_put_one(list_t *p, node_t *pnode)
{
	if (p == NULL) {
		DbgErr("%s:p NULL\n", __func__);
		return;
	}
	os_atomic_add(&p->cnt, 1);
	os_list_locked_insert_tail(p, &pnode->list);
}

/*
 *
 * Function Name:  node_list_get_cnt
 *
 * Abstract:
 *
 *		get the counter of the queue
 *
 * Input:
 *
 *		list_t *p [in]: Pointer to the list
 *
 * Output:
 *
 *		None.
 *
 * Return value: the counter value.
 *
 * Notes:
 *
 * Caller:
 *
 */
u32 node_list_get_cnt(list_t *p)
{
	if (p == NULL) {
		DbgErr("%s:p NULL\n", __func__);
		return 0;
	}
	return os_atomic_read(&p->cnt);
}

/*
 *
 * Function Name:  req_queue_loop_ctx_ops
 *
 * Abstract:
 *
 *		do operation for all noed of  the queue.
 *
 * Input:
 *
 *		tag_queue_t *tq [in]: Pointer to the tag queue
 *		req_queue_node_ops_cb cb [in]: the operation callback
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 * Notes:
 *
 * Caller:
 *
 */
bool req_queue_loop_ctx_ops(req_queue_t *p, req_queue_node_ops_ctx_cb cb,
			    void *ctx)
{
	bool ret = TRUE;
	node_t *pnode = 0;
	u32 i = 0, qsize = 0;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* check  input */
	if ((cb == NULL) || (p == NULL)) {
		DbgErr("% NULL\n", __func__);
		ret = FALSE;
		goto exit;
	}
	/* check empty queue */
	if (node_list_get_cnt(&p->list) == 0) {
		DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
			"%s nobody in queue\n", __func__);
		goto exit;
	}

	/* do  loops ops */
	qsize = node_list_get_cnt(&p->list);
	for (i = 0; i < qsize; i++) {
		/* get one node */
		pnode = node_list_get_one(&p->list);
		if (pnode == NULL)
			break;
		if ((*cb) (pnode, ctx) == FALSE)
			ret = FALSE;
		/* put back to queue */
		node_list_put_one(&p->list, pnode);
	}
	/* check cnt */
	if (i != qsize)
		DbgErr("%s:size no equal(%d-%d)\n", __func__, i, qsize);
exit:
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s %d\n",
		__func__, ret);
	return ret;
}

/*
 *
 * Function Name:  req_queue_init
 *
 * Abstract:
 *
 *		init  the request queue
 *
 * Input:
 *
 *		req_queue_t *p [in]: the queue need init
 *
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *		TRUE: means ok
 *
 * Notes:
 *
 * Caller:
 *
 */
static bool req_queue_init(PVOID pdx, req_queue_t *p, u32 id)
{
	if (p == NULL) {
		DbgErr("%s:p NULL\n", __func__);
		return FALSE;
	}
#if CFG_OS_LINUX
	os_list_init(&p->list);
#else
	os_list_init(pdx, &p->list);
#endif
	p->id = id;
	return TRUE;
}

/*
 *
 * Function Name:  req_queue_mark_node_status
 *
 * Abstract:
 *
 *		setup tag queue dma resource.
 *
 * Input:
 *
 *		req_queue_t *p [in]: Pointer to the request queue
 *		e_req_result [in]: the mark request status value for queue
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 * Notes:
 *
 * Caller:
 *
 */
static void req_queue_mark_node_status(req_queue_t *p, e_req_result result)
{
	req_queue_loop_ctx_ops(p, node_mark_node_status, &result);
}

static bool req_queue_move_node(req_queue_t *dq, req_queue_t *sq)
{
	bool ret = FALSE;
	node_t *pnode = 0;

	if ((dq == NULL) || (sq == NULL)) {
		DbgErr("%s null\n", __func__);
		ret = FALSE;
		goto exit;
	}

	/* put request queue */
	for (;;) {
		pnode = node_list_get_one(&sq->list);
		if (pnode == NULL)
			break;
		node_list_head_put_one(&dq->list, pnode);
	}

exit:
	return ret;
}

bool req_queue_reverse_queue(PVOID pdx, req_queue_t *q)
{
	list_t tlist;
	bool ret = TRUE;
	u32 qsize = 0;
	node_t *pnode = 0;

	if (q == NULL) {
		DbgErr("%s null\n", __func__);
		ret = FALSE;
		goto exit;
	}
	/* init temp list */
#if CFG_OS_LINUX
	os_list_init(&tlist);
#else
	os_list_init(pdx, &tlist);
#endif

	qsize = node_list_get_cnt(&q->list);

	for (;;) {
		pnode = node_list_get_one(&q->list);
		if (pnode == NULL)
			break;
		node_list_head_put_one(&tlist, pnode);
	}

	for (;;) {
		pnode = node_list_get_one(&tlist);
		if (pnode == NULL)
			break;
		node_list_put_one(&q->list, pnode);
	}

	if (qsize != node_list_get_cnt(&q->list)) {
		DbgErr("%s:size no equal(%d-%d)\n", __func__, qsize);
		ret = FALSE;
	}
exit:
	return ret;

}

static bool req_queue_move_order_node_at_head(PVOID pdx, req_queue_t *dq,
					      req_queue_t *sq)
{
	bool ret = FALSE;
	node_t *pnode = 0;

	if ((dq == NULL) || (sq == NULL)) {
		DbgErr("%s null\n", __func__);
		ret = FALSE;
		goto exit;
	}

	ret = req_queue_reverse_queue(pdx, sq);
	if (ret == FALSE)
		DbgErr("%s: reverse failed)\n", __func__);

	/* put request queue */
	for (;;) {
		pnode = node_list_get_one(&sq->list);
		if (pnode == NULL)
			break;
		node_list_head_put_one(&dq->list, pnode);
	}

exit:
	return ret;
}

static e_tq_state work_queue_get_state(req_queue_t *rq)
{
	return rq->state;
}

/* only state machine API can change states, other only can get */
static bool work_queue_state_machine(req_queue_t *rq, e_tq_state_evt evt)
{
	bool ret = TRUE;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s (%d)%x %x\n", __func__, rq->id, rq->state, evt);

	switch (evt) {
	case QUEUE_EVT_GET:
		{
			/* pre-condition */
			if (rq->state == QUEUE_STATE_IDLE) {
				rq->state = QUEUE_STATE_BUILD;
			} else {
				DbgErr("queue state not idle\n");
				ret = FALSE;
				goto exit;
			}
		}
		break;
	case QUEUE_EVT_BUILD_OK:
		{
			/* pre-condition */
			if (rq->state == QUEUE_STATE_BUILD) {
				rq->state = QUEUE_STATE_READY;
			} else {
				DbgErr("queue state not build\n");
				ret = FALSE;
				goto exit;
			}
		}
		break;
	case QUEUE_EVT_BUILD_FAILED:
		{
			/* pre-condition */
			if (rq->state == QUEUE_STATE_BUILD) {
				rq->state = QUEUE_STATE_IDLE;
			} else {
				DbgErr("queue state not build\n");
				ret = FALSE;
				goto exit;
			}
		}
		break;
	case QUEUE_EVT_ISSUE:
		{

			/* pre-condition */
			if (rq->state == QUEUE_STATE_READY) {
				rq->state = QUEUE_STATE_WAIT_CPL;
			} else {
				DbgErr("queue state not ready\n");
				ret = FALSE;
				goto exit;
			}
		}
		break;
	case QUEUE_EVT_DONE:
		{

			/* pre-condition */
			if (rq->state == QUEUE_STATE_WAIT_CPL) {
				rq->state = QUEUE_STATE_IDLE;
			} else {
				DbgErr("queue state not wait complete\n");
				ret = FALSE;
				goto exit;
			}
		}
		break;
	case QUEUE_EVT_FORCE_FAILED:
		{
			rq->state = QUEUE_STATE_WAIT_CPL;
		}
		break;
	case QUEUE_EVT_INIT:
		{
			rq->state = QUEUE_STATE_IDLE;
		}
		break;
	case QUEUE_EVT_ABORT:
		{
			/* pre-condition */
			if (rq->state == QUEUE_STATE_READY
			    || rq->state == QUEUE_STATE_WAIT_CPL) {
				rq->state = QUEUE_STATE_IDLE;
			} else {
				DbgErr
				    ("queue state not ready or wait complete\n");
				ret = FALSE;
				goto exit;
			}
		}
		break;
	default:
		DbgErr("%s state error %d\n", __func__, rq->state);
	}
exit:

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Exit %s (%d)%x %x\n", __func__, rq->id, rq->state, evt);
	return ret;
}

/* side effect: dma will change size by use length */
static bool work_queue_init_adma3_dma_res(req_queue_t *rq,
					  dma_desc_buf_t *dma, u32 len)
{
	bool ret = FALSE;
	/* check input */
	if (dma->len < len) {
		DbgErr("%s dma res len too small %x\n", __func__, dma->len);
		ret = FALSE;
		goto exit;
	}
	/* assign */
	rq->adma3_integrate_tbl = *dma;
	rq->adma3_integrate_tbl.len = len;
	resize_dma_buf(dma, len);
	DbgInfo(MODULE_TQ_DMA, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"adma3 integrate tb len %x pa(%x)\n", len,
		os_get_phy_addr32l(rq->adma3_integrate_tbl.pa));

exit:
	return ret;

}

typedef struct {
	dma_desc_buf_t *desc_buf;
	u32 len;
} work_queue_init_adma3_arg;

static bool work_queue_init_adma3_res_cb(PVOID pdx, req_queue_t *rq, void *ctx)
{
	bool ret = FALSE;
	work_queue_init_adma3_arg *arg = ctx;
	dma_desc_buf_t *dma = arg->desc_buf;

	ret = work_queue_init_adma3_dma_res(rq, dma, arg->len);
	if (ret == FALSE)
		return FALSE;
	else
		return TRUE;
}

static bool work_queue_init_cb(PVOID pdx, req_queue_t *rq, void *ctx)
{
	tag_queue_t *tq = ctx;

	req_queue_init(pdx, rq, tq->queue_id_seed++);
	work_queue_state_machine(rq, QUEUE_EVT_INIT);
	return FALSE;
}

/*
 *	static bool work_queue_dump_state(PVOID pdx, req_queue_t *rq, void *ctx)
 *	{
 *		DbgErr("wq %d state(%d) cnt:%d\n", rq->id, rq->state,
 *			node_list_get_cnt(&rq->list));
 *		return FALSE;
 *	}
 */

static bool work_queue_find_spec_state_queue_cb(PVOID pdx, req_queue_t *rq,
						void *ctx)
{
	e_tq_state *pstate = (e_tq_state *) ctx;

	if (ctx == NULL) {
		DbgErr("%s null ctx\n", ctx);
		return FALSE;
	}
	if (*pstate == work_queue_get_state(rq))
		return TRUE;
	else
		return FALSE;
}

static bool work_queue_reset_state_cb(PVOID pdx, req_queue_t *rq, void *ctx)
{
	work_queue_state_machine(rq, QUEUE_EVT_INIT);
	return FALSE;
}

typedef bool (*work_queue_ops_cb)(PVOID pdx, req_queue_t *rq, void *ctx);

static req_queue_t *tq_work_queue_find_ops(void *pdx, tag_queue_t *ptq,
					   work_queue_ops_cb pred, void *ctx)
{
	req_queue_t *rq = NULL, *retq = NULL;
	u32 i = 0;

	for (i = 0; i < TQ_WORK_QUEUE_SIZE; i++) {
		rq = &ptq->work_queues[i];
		if (pred((PVOID) pdx, rq, ctx) == TRUE) {
			retq = rq;
			break;
		}
	}
	return retq;
}

/*
 *
 * Function Name:  tq_work_queue_init_adma3_res
 *
 * Abstract:
 *
 *		setup tag queue dma resource.
 *
 * Input:
 *
 *		bht_dev_ext_t *pdx [in]: Pointer to the bht adapter extension structure
 *		dma_desc_buf_t *desc_buf [in]: the system DMA buffer resource
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 * Notes: the caller must set request status before call this function.
 *
 * Caller:
 *
 */

static bool tq_work_queue_init_adma3_res(PVOID pdx, tag_queue_t *ptq,
					 dma_desc_buf_t *desc_buf)
{
	bool ret = FALSE;

	work_queue_init_adma3_arg arg;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* check input parameters */
	if ((ptq == NULL) || (desc_buf == NULL)) {
		ret = FALSE;
		DbgErr("%s tq or dma NULL\n", __func__);
		goto exit;
	}
	if ((desc_buf->va == 0) || (desc_buf->len == 0)) {
		ret = FALSE;
		DbgErr("%s dma va:%p len:%xh\n", __func__, desc_buf->va,
		       desc_buf->len);
		goto exit;
	}

	/* adma3 integrate table resource init */
	arg.desc_buf = desc_buf;
	arg.len =
	    MAX_ADMA3_INTERGATE_TABLE_LEN_PER_QUEUE_PER_NODE *
	    ptq->max_wq_req_size;
	if (NULL !=
	    tq_work_queue_find_ops(pdx, ptq, work_queue_init_adma3_res_cb,
				   &arg)) {
		DbgErr("%s amd3 wq init res failed\n", __func__);
		ret = FALSE;
		goto exit;
	}

	ret = TRUE;
exit:
	DbgInfo(MODULE_TQ_FLOW, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Exit %s ret:%d\n", __func__, ret);
	return ret;

}

static bool dma_need_integrate_desc_buffer(u32 dma_mode)
{
	if ((dma_mode == CFG_TRANS_MODE_ADMA3_SDMA_LIKE) ||
	    (dma_mode == CFG_TRANS_MODE_ADMA3) ||
	    (dma_mode == CFG_TRANS_MODE_ADMA_MIX_SDMA_LIKE) ||
	    (dma_mode == CFG_TRANS_MODE_ADMA_MIX))
		return TRUE;
	else
		return FALSE;

}

static bool tq_work_queue_init(PVOID pdx, tag_queue_t *tq,
			       dma_desc_buf_t *desc_buf, u32 dma_mode)
{
	bool ret = TRUE;
	/* init basic work queue */
	tq_work_queue_find_ops(pdx, tq, work_queue_init_cb, tq);
	/* ADMA3 integrate descriptor table res need */
	if (dma_need_integrate_desc_buffer(dma_mode) == TRUE) {
		ret = tq_work_queue_init_adma3_res(pdx, tq, desc_buf);
		if (ret == FALSE)
			DbgErr("%s dma setup failed\n", __func__);
	}

	return ret;
}

static bool tq_work_queue_reset_state(PVOID pdx, tag_queue_t *tq)
{
	bool ret = TRUE;
	/* init basic work queue */
	tq_work_queue_find_ops(pdx, tq, work_queue_reset_state_cb, 0);

	return ret;
}

/*
 *
 * Function Name:   tq_work_queue_complete_cur_wq
 *
 * Abstract:
 *
 *		complete all requsts which in cur work queue.
 *
 * Input:
 *
 *		bht_dev_ext_t *pdx [in]: Pointer to the bht adapter extension structure
 *
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 * Notes: the caller must set request status before call this function.
 *
 * Caller:
 *
 */
static void tq_work_queue_complete_cur_wq(bht_dev_ext_t *pdx)
{
	tag_queue_t *ptq = &pdx->tag_queue;
	req_queue_t *pwq = ptq->wq_cur;
	node_t *pnode = 0;

	if (pwq == NULL) {
		DbgErr("%s wq cur is NULL\n", __func__);
		goto exit;
	}

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s tq have(%d)\n", __func__,
		os_atomic_read(&ptq->req_cnt));

	if (pwq->state != QUEUE_STATE_WAIT_CPL) {
		DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
			"try to complete non-wait-cpl queue\n");
	}

	if (os_atomic_read(&ptq->req_cnt) == 0) {
		DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
			"try to complete empty queue\n");
	}

	for (;;) {
		pnode = node_list_get_one(&pwq->list);

		if (pnode == NULL) {
			DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
				"Exit %s tq have(%d)\n", __func__,
				os_atomic_read(&ptq->req_cnt));
			goto exit;
		}

		if (os_atomic_read(&ptq->req_cnt))
			os_atomic_sub(&(ptq->req_cnt), 1);
		else
			DbgErr("tq complete req but cnt 0\n");
		node_notify_complete(pdx, pnode);
		/* free node memory to pool */
		node_mfree(ptq, pnode);
	}
exit:

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Exit %s tq have(%d)\n", __func__,
		os_atomic_read(&ptq->req_cnt));
}

/*
 *
 * Function Name:   tq_work_queue_move_update_node_status
 *
 * Abstract:
 *
 *		update request status in tag queue.
 *
 * Input:
 *
 *		bht_dev_ext_t *pdx [in]: Pointer to the bht adapter extension structure
 *		e_req_result req_status [in]: the complete status
 *		bool queue_select_all [in]:  select all request or just work queue
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 * Notes:
 *
 * Caller:
 *
 */

static bool tq_work_queue_move_node_to_cur_wq_cb(PVOID pdx, req_queue_t *rq,
						 void *ctx)
{
	tag_queue_t *tq = (tag_queue_t *) ctx;

	if (tq->wq_cur == NULL) {
		DbgErr("%s cur wq null\n", __func__);
		goto exit;
	}
	if (tq->wq_cur != rq) {
		if (work_queue_get_state(rq) == QUEUE_STATE_READY) {
			req_queue_move_node(tq->wq_cur, rq);
			/* update queue state to idle */
			work_queue_state_machine(rq, QUEUE_EVT_ABORT);
		}
	}

exit:

	return FALSE;
}

/*
 *
 * Function Name:   tq_work_queue_thread_io_done
 *
 * Abstract:
 *
 *		a wapper function for complete request.
 *
 * Input:
 *
 *		bht_dev_ext_t *pdx [in]: Pointer to the bht adapter extension structure
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 * Notes:
 *
 * Caller:
 *
 */
static void tq_work_queue_thread_io_done(void *p)
{
	tq_work_queue_complete_cur_wq(p);
}

/*
 *	static void tq_work_queue_dump_state(PVOID pdx, tag_queue_t *tq)
 *	{
 *		if (tq->wq_cur)
 *			DbgErr("wq cur (%d)\n", tq->wq_cur->id, tq->wq_cur->state);
 *		tq_work_queue_find_ops(pdx, tq, work_queue_dump_state, 0);
 *	}
 */

static bool tq_work_queue_rollback_node_cb(PVOID pdx, req_queue_t *rq,
					   void *ctx)
{
	tag_queue_t *tq = (tag_queue_t *) ctx;

	req_queue_move_node(&tq->req_queue, rq);
	/* loop all */
	return FALSE;
}

static void tq_work_queue_rollback_all_node(PVOID pdx, tag_queue_t *tq)
{
	tq_work_queue_find_ops(pdx, tq, tq_work_queue_rollback_node_cb, tq);

}

static void tq_all_queue_move_node_to_cur_wq(bht_dev_ext_t *pdx)
{
	tag_queue_t *ptq = &pdx->tag_queue;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* check cur wq */
	if (ptq->wq_cur == NULL) {
		DbgErr("%s wq cur is NULL\n", __func__);
		goto exit;
	}

	/* put request queue io to cur_wq */
	req_queue_move_node(ptq->wq_cur, &ptq->req_queue);

	/* put left queue io to cur wq */
	tq_work_queue_find_ops((PVOID) pdx, ptq,
			       tq_work_queue_move_node_to_cur_wq_cb, ptq);

exit:
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/*
 *
 * Function Name:   tq_thread_failed_queue
 *
 * Abstract:
 *
 *		tag queue use this function for complete request in thread context.
 *
 * Input:
 *
 *		bht_dev_ext_t *pdx [in]: Pointer to the bht adapter extension structure
 *		e_req_result req_status [in]: the complete status
 *		bool queue_select_all [in]:  select all request or just work queue
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 * Notes:
 *
 * Caller:
 *
 */

static void tq_thread_failed_queue(bht_dev_ext_t *pdx, e_req_result req_status,
				   bool queue_select_all)
{
	tag_queue_t *ptq = &pdx->tag_queue;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s sta:%d select:%d\n", __func__, req_status,
		queue_select_all);
	/* check cur wq */
	if (ptq->wq_cur == NULL) {
		DbgErr("%s wq cur is NULL, so force assign a cur wq\n",
		       __func__);
		ptq->wq_cur = &ptq->work_queues[0];
	}
	if (queue_select_all == TRUE)
		tq_all_queue_move_node_to_cur_wq(pdx);

	/* mark  status */
	req_queue_mark_node_status(ptq->wq_cur, req_status);

	/* check status */
	work_queue_state_machine(ptq->wq_cur, QUEUE_EVT_FORCE_FAILED);

	/* check empty */
	if (node_list_get_cnt(&ptq->wq_cur->list) != 0) {
		DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "%s %d\n",
			__func__, ptq->req_cnt);
		/* complete node to OS */
		thread_exec_high_prio_job(pdx, tq_work_queue_thread_io_done,
					  pdx);
	} else {
		DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
			"%s empty %d\n", __func__, ptq->req_cnt);
	}

	/* update to idle */
	work_queue_state_machine(ptq->wq_cur, QUEUE_EVT_DONE);

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

bool tq_get_hardware_idle_state(tag_queue_t *ptq)
{
	return (ptq->hw_idle == TRUE) ? TRUE : FALSE;
}

void tq_set_hardware_idle_state(tag_queue_t *ptq, bool idle)
{
	ptq->hw_idle = idle;
}

bool tq_wait_stratege(req_queue_t *q)
{
	return TRUE;
}

/* get a idle trans ctx for next build */
static req_queue_t *tq_trans_ctx_get_spec_one(PVOID pdx, tag_queue_t *ptq,
					      e_tq_state sta)
{
	req_queue_t *rq = NULL;

	rq = tq_work_queue_find_ops(pdx, ptq,
				    work_queue_find_spec_state_queue_cb, &sta);

	return rq;
}

static req_queue_t *tq_trans_ctx_get_idle_one(PVOID pdx, tag_queue_t *ptq)
{
	req_queue_t *rq = NULL;

	rq = tq_trans_ctx_get_spec_one(pdx, ptq, QUEUE_STATE_IDLE);
	if (rq != NULL)
		work_queue_state_machine(rq, QUEUE_EVT_GET);
	return rq;
}

static req_queue_t *tq_trans_ctx_get_ready_one(PVOID pdx, tag_queue_t *ptq)
{
	req_queue_t *rq = NULL;

	rq = tq_trans_ctx_get_spec_one(pdx, ptq, QUEUE_STATE_READY);
	return rq;
}

static e_build_ctx_result tq_trans_ctx_build_only(bht_dev_ext_t *pdx,
						  tag_queue_t *ptq,
						  req_queue_t *build_queue)
{
	node_t *pnode = 0;
	u32 i = 0;
	e_build_ctx_result ret = TQ_BUILD_IO_ERROR;
	/* when build ctx maybe host transfer data for current queue */
	ptq->wq_build = build_queue;
	/* check */
	if (work_queue_get_state(build_queue) != QUEUE_STATE_BUILD) {
		DbgErr("TQ queue state error\n");
		ret = TQ_BUILD_IO_ERROR;
		goto exit;
	}

	/* 1. init context one transfer. */
	if (ptq->ops.init_io) {
		if (ptq->ops.init_io(pdx) == FALSE) {
			DbgErr("Tag Queue InitIo failed\n");
			ret = TQ_BUILD_IO_ERROR;
			goto exit;
		}
	}
	/* 2. build IOs context stage */

	for (i = 0; i < ptq->max_build_limit; i++) {
		/* 2.1 get one node from request queue list */
		pnode = node_list_get_one(&ptq->req_queue.list);
		if (pnode == NULL) {
			if (i == 0) {
				DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE,
					NOT_TO_RAM,
					"no request in request queue\n");
				/* can't got any node, so exit. */
				ret = TQ_BUILD_IO_EMPTY;
				goto exit;
			}
			/* can't fetch anything more node, so goto issue stage. */
			break;
		}
		/* 2.2 build one IO context for the node */
		/* prebuild io */
		if (ptq->ops.prebuild_io) {
			if (ptq->ops.prebuild_io(pdx, pnode) == FALSE)
				goto build_error_handle;
		}

		if (ptq->ops.build_io) {

			if (ptq->ops.build_io(pdx, pnode) == FALSE) {
				/*
				 * put this node backto request queue.
				 * 1.for ADMA2 merge case:no the continuous SRB
				 * 2.for ADMA3 maybe no enough buf resource,so must put it back
				 */
build_error_handle:
				node_list_head_put_one(&ptq->req_queue.list,
						       pnode);
				/* check empty condition */
				if (node_list_get_cnt(&build_queue->list) == 0) {
					DbgErr
					    ("Tag Queue transfer queue empty\n");
					/* abort */
					ret = TQ_BUILD_IO_ERROR;
					goto exit;
				}
				/* goto issue stage */
				break;
			}
		}
		node_list_put_one(&build_queue->list, pnode);

		/* 2.3 TQ policy need break. */
		if (tag_queue_policy_break(pdx) == TRUE) {
			DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
				"%s TQ policy break\n", __func__);
			break;
		}
		/* hardware idle */
		if (tq_get_hardware_idle_state(ptq) == TRUE) {
			DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
				"%s HW idle break\n", __func__);
			break;
		}
	}

	ret = TQ_BUILD_IO_OK;
exit:
	return ret;
}

static e_build_ctx_result tq_trans_ctx_build(bht_dev_ext_t *pdx,
					     tag_queue_t *ptq,
					     req_queue_t *build_queue)
{
	e_build_ctx_result ret = TQ_BUILD_IO_ERROR;
	/* build */
	ret = tq_trans_ctx_build_only(pdx, ptq, build_queue);
	/* update state */
	if (ret == TQ_BUILD_IO_OK)
		work_queue_state_machine(build_queue, QUEUE_EVT_BUILD_OK);
	else
		work_queue_state_machine(build_queue, QUEUE_EVT_BUILD_FAILED);
	return ret;
}

static bool tq_trans_ctx_rebuild(bht_dev_ext_t *pdx, tag_queue_t *tq)
{
	bool ret = FALSE;

	/* put all build ios back to request queue */
	req_queue_move_order_node_at_head((PVOID) pdx, &tq->req_queue,
					  tq->wq_cur);
	work_queue_state_machine(tq->wq_cur, QUEUE_EVT_ABORT);
	/* get idle ctx */
	tq->wq_cur = tq_trans_ctx_get_idle_one((PVOID) pdx, tq);
	if (tq->wq_cur == NULL) {
		ret = FALSE;
		DbgErr("%s null wq_cur\n", __func__);
		goto exit;
	}
	/* rebuild ctx */
	if (tq_trans_ctx_build(pdx, tq, tq->wq_cur) != TQ_BUILD_IO_OK) {
		/* empty request queue or build failed */
		ret = FALSE;
		DbgErr("%s build failed\n", __func__);
		goto exit;
	}
	/* update */
	work_queue_state_machine(tq->wq_cur, QUEUE_EVT_ISSUE);
	ret = TRUE;
exit:
	return ret;
}

static bool tq_trans_ctx_rebuild_node_cb(node_t *pnode, void *ctx)
{
	bool ret = TRUE;
	bht_dev_ext_t *pdx = ctx;
	tag_queue_t *ptq = &pdx->tag_queue;

	/* build one IO context for the node */

	if (ptq->ops.prebuild_io) {
		if (ptq->ops.prebuild_io(pdx, pnode) == FALSE) {
			ret = FALSE;
			goto exit;
		}
	}

	if (ptq->ops.build_io) {

		if (ptq->ops.build_io(pdx, pnode) == FALSE) {
			ret = FALSE;
			goto exit;
		}
	}
exit:
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s %d\n",
		__func__, ret);
	return ret;
}

static bool tq_trans_ctx_rebuild_each_node(bht_dev_ext_t *pdx,
					   tag_queue_t *ptq,
					   req_queue_t *build_queue)
{
	bool ret = TRUE;

	ptq->wq_build = build_queue;

	/* 1. init context one transfer. */
	if (ptq->ops.init_io) {
		if (ptq->ops.init_io(pdx) == FALSE) {
			DbgErr("Tag Queue InitIo failed\n");
			ret = FALSE;
			goto exit;
		}
	}

	ret =
	    req_queue_loop_ctx_ops(ptq->wq_cur, tq_trans_ctx_rebuild_node_cb,
				   pdx);

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s %d\n",
		__func__, ret);

exit:
	return ret;
}

bool tq_dma_mode_switch(bht_dev_ext_t *pdx, tag_queue_t *tq)
{
	bool ret = FALSE;
	u32 target_dma_mode = os_atomic_read(&tq->target_dma_mode);

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* need switch */
	if (target_dma_mode == tq->cur_dma_mode) {
		ret = TRUE;
		goto exit;
	}

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		" %s switch(%d,%d)\n", __func__, target_dma_mode,
		tq->cur_dma_mode);
	/* call each mode clean ops */
	if (tq->ops.unload) {
		ret = tq->ops.unload(pdx);
		/* need error recovery,switch failed. */
		if (ret == FALSE) {
			DbgErr("%s unload failed\n", __func__);
			ret = FALSE;
			goto exit;
		}
	}
	/* config new mode */
	os_memset(&tq->ops, 0, sizeof(transfer_cb_t));
	switch (target_dma_mode) {
	case CFG_TRANS_MODE_ADMA3:
		tq_adma3_mode_init(&tq->ops);
		break;
	case CFG_TRANS_MODE_ADMA2:
		tq_adma2_inf_mode_init(&tq->ops);
		break;
	case CFG_TRANS_MODE_ADMA2_SDMA_LIKE:
		tq_adma2_inf_sdmalike_mode_init(&tq->ops);
		break;
	case CFG_TRANS_MODE_ADMA3_SDMA_LIKE:
		tq_adma3_sdmalike_mode_init(&tq->ops);
		break;
	case CFG_TRANS_MODE_ADMA2_ONLY:
		host_transfer_init(&pdx->host, FALSE, TRUE);
		tq_adma2_mode_init(&tq->ops);
		break;
	case CFG_TRANS_MODE_ADMA2_ONLY_SDMA_LIKE:
		tq_adma2_sdmalike_mode_init(&tq->ops);
		break;
	default:
		DbgErr("error dma mode\n");
	}
	tq->cur_dma_mode = target_dma_mode;
	/* rebuild io */
	tq_trans_ctx_rebuild(pdx, tq);

	ret = TRUE;
exit:
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return ret;
}

static bool tq_dma_support_mix_mode(u32 dma_mode)
{

	if ((dma_mode == CFG_TRANS_MODE_ADMA_MIX) ||
	    (dma_mode == CFG_TRANS_MODE_ADMA_MIX_SDMA_LIKE))
		return TRUE;
	else
		return FALSE;

}

u32 tq_issue_post_cb(void *p)
{
	bht_dev_ext_t *pdx = p;
	tag_queue_t *ptq = &pdx->tag_queue;
	req_queue_t *build_queue = NULL;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	tq_wait_stratege(ptq->wq_cur);

	/* try build it now */
	if (tq_trans_ctx_get_ready_one((PVOID) pdx, ptq) == NULL) {
		build_queue = tq_trans_ctx_get_idle_one((PVOID) pdx, ptq);
		if (build_queue != NULL) {
			if (ptq->wq_cur == build_queue) {
				DbgErr("%s wq_cur == build_queue\n",
				       __func__);
				goto exit;

			}
			if (TQ_BUILD_IO_OK !=
			    tq_trans_ctx_build(pdx, ptq, build_queue)) {
				DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE,
					NOT_TO_RAM, "not build trans ctx\n");
			} else {
				DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE,
					NOT_TO_RAM, "build trans ctx ok\n");
			}
		}
	}
exit:
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return 0;
}

/*
 *
 * Function Name:  tag_queue_rw_data_issue_stage
 *
 * Abstract:
 *
 *		issue cmd for tag_queue_rw_data()
 *
 * Input:
 *
 *		bht_dev_ext_t *pdx [in]: Pointer to the bht adapter extension structure
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 * Notes:
 *
 * Caller: only for tag_queue_rw_data()
 *
 */

e_req_result tag_queue_rw_data_issue_stage(bht_dev_ext_t *pdx,
					   tag_queue_t *ptq, sd_card_t *card,
					   host_cmd_req_t *irq)
{
	/* cfg retry times */
	int retry = TQ_MAX_RECOVERY_ERROR_RETRY_TIMES;
	sd_command_t *sd_cmd = 0;
	e_req_result ret = REQ_RESULT_OK;
	bool card_poweroff_flg = TRUE;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* check cur wq */
	if (ptq->wq_cur == NULL) {
		ret = REQ_RESULT_ABORT;
		DbgErr("%s wq cur is NULL, so exit\n", __func__);
		goto exit;
	}

	/* update */
	work_queue_state_machine(ptq->wq_cur, QUEUE_EVT_ISSUE);

	os_set_dev_busy(pdx);
	card_poweroff_flg = card_is_poweroff(&pdx->card);
	ret = thread_wakeup_card(pdx);
	if (ret != REQ_RESULT_OK) {
		DbgErr("Tag Queue wakeup card failed\n");
		tq_thread_failed_queue(pdx, REQ_RESULT_ACCESS_ERR, TRUE);
		ret = REQ_RESULT_ACCESS_ERR;
		goto exit;
	}

	if (card_poweroff_flg == TRUE) {
		/*
		 * for ADMA3 case, if card poweroff, adma3 dma ctx may invalid,
		 * a) card init may degrade from uhs2 to uhs1.
		 * b) card power, the cmd descriptor may also build wrong.
		 */
		if (ptq->ops.poweroff_need_rebuild) {
			if (ptq->ops.poweroff_need_rebuild(pdx) == TRUE) {
				/* maybe card mode change during error recovery */
				if (FALSE ==
				    tq_trans_ctx_rebuild_each_node(pdx, ptq,
								   ptq->wq_cur)) {
					DbgErr("Tag Queue rebuild failed\n");
					ret = REQ_RESULT_ACCESS_ERR;
					tq_thread_failed_queue(pdx,
							       REQ_RESULT_ACCESS_ERR,
							       TRUE);
					goto exit;
				}
			}
		}
	}

	if (func_thermal_control(card) == FALSE) {
		if (card_recovery_flow(card, NULL) == FALSE) {
			DbgErr("%s thremal control recover failed\n",
			       __func__);
			/* can't handle anything */
			tq_thread_failed_queue(pdx, REQ_RESULT_ACCESS_ERR,
					       TRUE);
			ret = REQ_RESULT_ACCESS_ERR;
			goto exit;
		}
	}

	/* 3.1 issue transfer */

	if (TRUE ==
	    tq_dma_support_mix_mode(pdx->cfg->host_item.test_dma_mode_setting.dma_mode)) {
		ret = tq_dma_mode_switch(pdx, ptq);
		if (ret == FALSE)
			goto error_handle;
	}
tq_trans_again:
	if (os_thread_is_freeze(pdx) || card->card_type == CARD_ERROR) {
		tq_thread_failed_queue(pdx, REQ_RESULT_ABORT, TRUE);
		ret = REQ_RESULT_ABORT;
		DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
			"%s card err or thread freeze\n", __func__);
		goto final;
	}

	if (card->info.scr.sd_spec <= SCR_SPEC_VER_1) {
		/*
		 * For SD1.1/SD1.0 card, need add delay between two
		 * read/write command when using infinite transfer mode
		 */
		os_udelay(100);
	}

	/* hardware idle not sync with queue status */
	tq_set_hardware_idle_state(&pdx->tag_queue, FALSE);
	retry--;
	if (ptq->ops.issue_transfer(pdx) == FALSE) {
		/* error recovery flow */
error_handle:
		DbgErr("Error Recovery for ReadWrite\n");
		sd_cmd = irq->private;
		ret = card_recovery_flow(card, sd_cmd);
		if (ret == REQ_RESULT_OK) {
			if (retry > 0) {
				DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE,
					NOT_TO_RAM, "%s TQ retry %d\n",
					__func__, retry);
				/* maybe card mode change during error recovery */
				if (FALSE ==
				    tq_trans_ctx_rebuild_each_node(pdx, ptq,
								   ptq->wq_cur)) {
					DbgErr("Tag Queue rebuild failed\n");
					ret = REQ_RESULT_ACCESS_ERR;
					tq_thread_failed_queue(pdx,
							       REQ_RESULT_ACCESS_ERR,
							       TRUE);
					goto exit;
				} else
					goto tq_trans_again;
			} else {
				DbgErr
				    ("Tag Queue Error Recovery retry exceed\n");

				/*
				 * set card error after retry fail.
				 * to avoid process one more tag io that already in queue.
				 */
				pdx->card.card_type = CARD_ERROR;

				tq_thread_failed_queue(pdx,
						       REQ_RESULT_ACCESS_ERR,
						       FALSE);
				ret = REQ_RESULT_ACCESS_ERR;
				goto exit;
			}
		} else {
			DbgErr("Tag Queue Error card Recovery failed\n");
			tq_thread_failed_queue(pdx, ret, TRUE);
			goto exit;
		}
	}
	tq_set_hardware_idle_state(&pdx->tag_queue, TRUE);
	/* queue final idle */
	work_queue_state_machine(ptq->wq_cur, QUEUE_EVT_DONE);

exit:
	os_set_dev_idle(pdx);
final:

	if (ptq->wq_cur->state != QUEUE_STATE_IDLE)
		DbgErr("%s cur_wq state is not idle\n", __func__);
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Exit %s ret=%x\n", __func__, ret);
	return ret;
}

bool tq_work_queue_state_recovery(PVOID pdx, tag_queue_t *tq)
{
	bool ret = FALSE;

	/* roll back all IOs to request queue, then reset TQ wq */

	/* 1. rollback all IOs to request queue */
	tq_work_queue_rollback_all_node(pdx, tq);
	/* 2.reset TQ wq */
	tq_work_queue_reset_state(pdx, tq);

	return ret;
}

/*
 *
 * Function Name:  tag_queue_rw_data
 *
 * Abstract:
 *
 *		it handle tag queue read/write request work flow.
 *
 * Input:
 *
 *		bht_dev_ext_t *pdx [in]: Pointer to the bht adapter extension structure
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 * Notes:
 *
 * Caller: thread for complete TQ request
 *
 */
e_req_result tag_queue_rw_data(bht_dev_ext_t *pdx)
{
	tag_queue_t *ptq = 0;
	host_cmd_req_t *irq = 0;
	e_req_result ret = REQ_RESULT_OK;
	sd_card_t *card = 0;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* check input */
	if (pdx == NULL) {
		DbgErr("%s:pdx NULL\n", __func__);
		/* can't handle anything */
		ret = REQ_RESULT_ABORT;
		goto exit;
	}
	/* init variable */
	ptq = &pdx->tag_queue;
	irq = &ptq->cmd_req;
	card = &pdx->card;
	/* check TQ status */
	if (ptq->init_magic_number != TAG_QUEUE_INIT_MAGIC_NUMBER) {
		DbgErr("%s TQ no init ok\n", __func__);
		/* can't handle anything */
		tq_thread_failed_queue(pdx, REQ_RESULT_ABORT, TRUE);
		ret = REQ_RESULT_ABORT;
		goto exit;
	}

	if (ptq->ops.issue_transfer == NULL) {
		DbgErr("%s TQ no issue cb\n", __func__);
		tq_thread_failed_queue(pdx, REQ_RESULT_ABORT, TRUE);
		ret = REQ_RESULT_ABORT;
		goto exit;
	}

	/* start jobs */
	while (1) {
		if (card->card_chg || card->card_present == FALSE) {
			DbgErr("Tag Queue card not ready failed\n");
			/* can't handle anything */
			tq_thread_failed_queue(pdx, REQ_RESULT_NO_CARD, TRUE);
			ret = REQ_RESULT_NO_CARD;
			goto exit;
		}

		/* Thomas add based on UHS2 issue 11. */
		if (card->card_type == CARD_ERROR) {
			DbgErr("Tag Queue aborted for card error\n");

			ret = REQ_RESULT_ACCESS_ERR;
			goto exit;
		}

		ptq->wq_cur = tq_trans_ctx_get_ready_one((PVOID) pdx, ptq);
		if (ptq->wq_cur == NULL) {
			/* need build it now */
			ptq->wq_cur =
			    tq_trans_ctx_get_idle_one((PVOID) pdx, ptq);
			if (ptq->wq_cur == NULL) {
				/* impossible path(assert case),but need check */
				DbgErr
				    ("%s fatal error,failed to get free trans ctx\n",
				     __func__);
				tq_work_queue_state_recovery((PVOID) pdx, ptq);
				ptq->wq_cur =
				    tq_trans_ctx_get_idle_one((PVOID) pdx, ptq);

				if (ptq->wq_cur == NULL) {
					DbgErr
					    ("%s recovery but still get null cur wq\n",
					     __func__);
					tq_thread_failed_queue(pdx,
							       REQ_RESULT_ACCESS_ERR,
							       TRUE);
					ret = REQ_RESULT_ACCESS_ERR;
					goto exit;
				}

			}
			/* build ctx */
			{
				e_build_ctx_result build_ret =
				    tq_trans_ctx_build(pdx, ptq, ptq->wq_cur);
				switch (build_ret) {
				case TQ_BUILD_IO_ERROR:
					{
						/* can't handle anything */
						tq_thread_failed_queue(pdx,
								       REQ_RESULT_ACCESS_ERR,
								       TRUE);
						/* empty request queue or build failed */
						ret = REQ_RESULT_ACCESS_ERR;
						goto exit;
					}
					break;
				case TQ_BUILD_IO_OK:
					break;
				case TQ_BUILD_IO_EMPTY:
					ret = REQ_RESULT_OK;
					goto exit;
					break;
				default:
					DbgErr("%s build state error\n");
				}
			}
		}

		/* ---------------issue stage--------------- */
		ret = tag_queue_rw_data_issue_stage(pdx, ptq, card, irq);
		if (ret != REQ_RESULT_OK) {
			DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
				"Exit %s ret=%x\n", __func__, ret);
			goto exit;
		}

	}
	ret = REQ_RESULT_OK;
exit:
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s %d\n",
		__func__, ret);
	return ret;
}

/*
 *
 * Function Name: tq_add_request
 *
 * Abstract:
 *
 *		add one request to tag queue, and do prebuild callback if have.
 *
 * Input:
 *
 *		bht_dev_ext_t *pdx [in]: Pointer to the bht adapter extension structure
 *		srb_ext* srb_ext [in]: Pointer to the request data structure.
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 *		REQ_RESULT_OK: tag queue have put this request successful in queue.
 *		REQ_RESULT_QUEUE_BUSY:  tag queue is full, please retry late.
 * Notes:
 *
 * Caller: StartIO. async add request to tq
 *
 */

e_req_result tq_add_request(bht_dev_ext_t *pdx, srb_ext_t *srb_ext,
			    sd_card_t *card)
{
	tag_queue_t *tq = 0;
	node_t *pnode = 0;
	e_req_result ret = 0;
	u32 dma_mode;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* check */
	if (pdx == NULL) {
		ret = REQ_RESULT_ABORT;
		DbgErr("%s null pdx\n", __func__);
		goto exit;
	}

	tq = &pdx->tag_queue;
	/* init done check */
	if (tq->init_magic_number != TAG_QUEUE_INIT_MAGIC_NUMBER) {
		DbgErr("%s TQ no init\n", __func__);
		ret = REQ_RESULT_ABORT;
		goto exit;
	}

	/* over size check */
	if ((int)os_atomic_read(&tq->req_cnt) < ((int)tq->max_wq_req_size)) {
		/* get one node_t */
		pnode = node_malloc(tq);
		if (pnode == NULL) {
			DbgErr("%s node malloc failed\n", __func__);
			ret = REQ_RESULT_QUEUE_BUSY;
			goto exit;
		}
		/* bind srb_ext to node_t */
		pnode->psrb_ext = srb_ext;
		/* bind card to node */
		pnode->card = card;

		dma_mode = pdx->cfg->host_item.test_dma_mode_setting.dma_mode;
#if CFG_OS_LINUX
		if (cfg_dma_need_sdma_like_buffer(dma_mode) == TRUE)
			tq_adma2_sdmalike_copy(pdx, pnode);
#endif

		/* put node to request queue & add cnt */
		os_atomic_add(&(tq->req_cnt), 1);
		DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
			"%s,tq cnt:%x\n", __func__,
			os_atomic_read(&tq->req_cnt));
		node_list_put_one(&(tq->req_queue.list), pnode);

#if CFG_OS_LINUX
		os_set_event(&pdx->os, EVENT_TAG_IO);
#else
		os_set_event(pdx, &pdx->os, EVENT_TASK_OCCUR, EVENT_TAG_IO);
#endif

		if (TRUE ==
		    tq_dma_support_mix_mode(pdx->cfg->host_item.test_dma_mode_setting.dma_mode)) {
			tq_dma_decision_policy(tq, card, &srb_ext->req);
		}

		ret = REQ_RESULT_PENDING;
		goto exit;
	} else {
		/* TQ full */
		ret = REQ_RESULT_QUEUE_BUSY;
		goto exit;
	}
exit:
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Exit %s,ret=%x\n", __func__, ret);
	return ret;

}

/*
 *
 * Function Name: tq_is_empty
 *
 * Abstract:
 *
 *		get tag queue running status.
 *
 * Input:
 *
 *		bht_dev_ext_t *pdx [in]: Pointer to the bht adapter extension structure
 *
 * Output:
 *

 * Return value:
 *
 *		TRUE: empty
 *		FALSE: no empty
 * Notes:
 *
 * IO_control case for corrently crash.
 */

bool tq_is_empty(bht_dev_ext_t *pdx)
{
	tag_queue_t *tq = 0;
	bool ret = TRUE;

	if (pdx == NULL) {
		DbgErr("%s pdx null\n", __func__);
		ret = FALSE;
		goto exit;
	}

	tq = &pdx->tag_queue;

	if (tq->init_magic_number != TAG_QUEUE_INIT_MAGIC_NUMBER) {
		DbgErr("%s TQ no init\n", __func__);
		ret = TRUE;
		goto exit;
	}

	if (os_atomic_read(&tq->req_cnt))
		ret = FALSE;
exit:
	return ret;
}

/*
 *
 * Function Name: tag_queue_dma_size_init
 *
 * Abstract:
 *
 *		try to get TQ cfg for DMA buffer size
 *
 * Input:
 *
 *		bht_dev_ext_t *pdx [in]: Pointer to the bht adapter extension structure
 *
 * Output:
 *

 * Return value:
 *
 *
 * Notes:
 *
 * IO_control case for corrently crash.
 */

bool tag_queue_dma_size_init(bht_dev_ext_t *pdx, u32 buf_size)
{
	u32 max_req_numb = 0;
	tag_queue_t *tq = &pdx->tag_queue;
	u32 dma_mode = 0;
	u32 cnt = 0;
	u32 infinite_mode_enable = 0;
	bool sdma_like_mode = FALSE;
	u32 node_size = 0;
	bool ret = FALSE;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	max_req_numb = pdx->cfg->host_item.test_tag_queue_capability.max_srb;
	dma_mode = pdx->cfg->host_item.test_dma_mode_setting.dma_mode;
	infinite_mode_enable =
	    pdx->cfg->host_item.test_infinite_transfer_mode.enable_inf;

	max_req_numb += TQ_RESERVED_NODE_SIZE;

	/* get cfg value */
	if (max_req_numb == 0) {
		tq->max_wq_req_size = 1;
	} else {
		if (max_req_numb > MAX_WORK_QUEUE_SIZE)
			max_req_numb = MAX_WORK_QUEUE_SIZE;
		tq->max_wq_req_size = max_req_numb;
	}

	/* check can use sdma-like mode or not */
	if (cfg_dma_need_sdma_like_buffer(dma_mode) == TRUE) {
		/*
		 * try get max for sdma-like mode first,
		 * if can't support at least two for sdma-like mode,
		 * then degrade to non-sdma-like mode
		 */
		if (buf_size >= (2 * MAX_SDMA_LIKE_MODE_NODE_BUF_SIZE))
			sdma_like_mode = TRUE;
		else {
			sdma_like_mode = FALSE;
			/* can't use sdma_like mode ,so disable this cfg to non-sdma-like mode */
			DbgErr
			    ("TQ degrade sdma-like mode to non-sdma-like mode\n");
			if (dma_mode == CFG_TRANS_MODE_ADMA2_SDMA_LIKE)
				cfg_dma_mode_dec(pdx->cfg,
						 CFG_TRANS_MODE_ADMA2);
			if (dma_mode == CFG_TRANS_MODE_ADMA3_SDMA_LIKE)
				cfg_dma_mode_dec(pdx->cfg,
						 CFG_TRANS_MODE_ADMA3);
			if (dma_mode == CFG_TRANS_MODE_ADMA_MIX_SDMA_LIKE)
				cfg_dma_mode_dec(pdx->cfg,
						 CFG_TRANS_MODE_ADMA_MIX);
			if (dma_mode == CFG_TRANS_MODE_SDMA) {
				DbgErr("SDMA mode degrade adma2 mode\n");
				cfg_dma_mode_dec(pdx->cfg,
						 CFG_TRANS_MODE_ADMA2);
			}
		}
	}

	/* get node size */
	if (sdma_like_mode == TRUE)
		node_size = MAX_SDMA_LIKE_MODE_NODE_BUF_SIZE;
	else
		node_size = MAX_NODE_BUF_SIZE;

	/* make sure have DMA buffer align size */
	cnt = buf_size / (node_size);
	/* small req size is over buffer resource */
	if (tq->max_wq_req_size >= cnt)
		tq->max_wq_req_size = cnt;

	/* if infinite mode enable, however only cfg one node.  it can't work for link table. */
	if (infinite_mode_enable) {
		if (tq->max_wq_req_size <= 1) {
			DbgErr
			    ("infinite mode can't work when only one node.force TQ stall\n");
			tq->max_wq_req_size = 0;
		}
	}

	/* check cnt value */
	if (tq->max_wq_req_size == 0) {
		DbgErr("TQ failed to get any dma buffer for only one desc\n");
		ret = FALSE;
	} else
		ret = TRUE;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Exit %s max_req:%d dma_mode:%d\n", __func__,
		tq->max_wq_req_size, dma_mode);
	return ret;

}

/*
 *
 * Function Name:   tag_queue_isr
 *
 * Abstract:
 *
 *		it's the cb_req_complete function for ISR handle.
 *
 * Input:
 *
 *		bht_dev_ext_t *pdx [in]: Pointer to the bht adapter extension structure
 *		cmd_err_t *err [in] : the error information
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 * Notes:
 *
 * Caller: ISR
 *
 */
u32 tag_queue_isr(void *p, cmd_err_t *err)
{
	bht_dev_ext_t *pdx = p;
	tag_queue_t *tq = &pdx->tag_queue;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s (%s)\n",
		__func__, err->error_code ? "err" : "ok");

	tq_set_hardware_idle_state(&pdx->tag_queue, TRUE);
	if (err->error_code) {
		/*
		 * just update transfer queue requests to ERROR,
		 * wait for thread handle later.(retry or other)
		 */

		req_queue_mark_node_status(tq->wq_cur, REQ_RESULT_ACCESS_ERR);
	} else {
		/* complete these request now, mark ok first(for error recovery successful case) */

		req_queue_mark_node_status(tq->wq_cur, REQ_RESULT_OK);
		tq_work_queue_complete_cur_wq(pdx);
	}
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return TRUE;
}

/*
 *
 * Function Name:  tag_queue_abort
 *
 * Abstract:
 *
 *		flush all request with error status in TQ for abort
 *
 * Input:
 *
 *		bht_dev_ext_t *pdx [in]: Pointer to the bht adapter extension structure
 *		e_req_result result [in] : the abort status for request
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 * Notes:
 *
 * Caller: thread when wakeup card failed, need abort.
 *
 */
void tag_queue_abort(bht_dev_ext_t *pdx, e_req_result result)
{
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s req_result:%x\n", __func__, result);

	if (pdx == NULL) {
		DbgErr("%s pdx null\n", __func__);
		goto exit;
	}

	if (pdx->tag_queue.init_magic_number != TAG_QUEUE_INIT_MAGIC_NUMBER) {
		DbgErr("TQ no init\n");
		goto exit;
	}

	tq_thread_failed_queue(pdx, result, TRUE);
exit:
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/*
 *
 * Function Name: tag_queue_init
 *
 * Abstract:
 *
 *		initialize tag queue.
 *
 * Input:
 *
 *		bht_dev_ext_t *pdx [in]: Pointer to the bht adapter extension structure
 *
 * Output:
 *
 *		None
 *
 * Return value:
 *
 *		None.
 *
 * Notes:
 *
 *		Caller:
 *		1. This function is called req_global_init
 *		2. tag queue must use DMA buffer for generate DMA desc table.
 */

bool tag_queue_init(bht_dev_ext_t *pdx)
{
	tag_queue_t *tq = 0;
	dma_desc_buf_t dma_res = { 0 };
	bool ret = FALSE, sdma_mode = FALSE;
	u32 dma_mode = 0;
	u32 merge_enable = 0;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* input check */
	if (pdx == NULL) {
		DbgErr("%s pdx null\n", __func__);
		goto exit;
	}

	os_memset(&(pdx->tag_queue), 0, sizeof(tag_queue_t));

	tag_queue_dma_size_init(pdx, pdx->dma_buff.len);
	/* init var */
	dma_mode = pdx->cfg->host_item.test_dma_mode_setting.dma_mode;
	merge_enable =
	    pdx->cfg->host_item.test_tag_queue_capability.enable_srb_merge;
	tq = &pdx->tag_queue;

	dma_res = pdx->dma_buff;
	if (dma_res.len == 0 || dma_res.va == NULL) {
		DbgErr("%s dma_res null\n", __func__);
		ret = FALSE;
		goto exit;
	}

	/* check req size */
	if (tq->max_wq_req_size == 0) {
		DbgErr("%s req size =0\n", __func__);
		ret = FALSE;
		goto exit;
	}

	/* reset TQ state */
	os_atomic_set(&tq->req_cnt, 0);
	req_queue_init((PVOID) pdx, &tq->req_queue, tq->queue_id_seed++);
	tq->wq_cur = NULL;
	/* bind completion */
	tq->tran_cpl = &tq->cmd_req.done;
	/*
	 * init node memory pool , this call must call
	 * before tq_setup_dma_res() for use DMA resource
	 */
	if (cfg_dma_need_sdma_like_buffer(dma_mode) == TRUE)
		sdma_mode = TRUE;
	else
		sdma_mode = FALSE;
	if (node_pool_init((PVOID) pdx, tq, &dma_res, sdma_mode) == FALSE) {
		DbgErr("%s node pool failed\n", __func__);
		ret = FALSE;
		goto exit;
	}

	/* work queue init */
	if (tq_work_queue_init((PVOID) pdx, tq, &dma_res, dma_mode) == FALSE) {
		ret = FALSE;
		goto exit;
	}

	/* cfg tag queue */

	/* transfer cbs */
	/*
	 * 0: SDMA, 1: ADMA2, 2: ADMA3, 3: ADMA2_SDMA_Like,
	 * 4: ADMA3_SDMA_Like, 0xF: PIO, other: Reserved
	 */
	switch (dma_mode) {
	case CFG_TRANS_MODE_SDMA:
		tq_sdma_mode_init(&tq->ops);
		break;
	case CFG_TRANS_MODE_ADMA2:
		tq_adma2_inf_mode_init(&tq->ops);
		break;
	case CFG_TRANS_MODE_ADMA3:
		tq_adma3_mode_init(&tq->ops);
		break;
	case CFG_TRANS_MODE_ADMA2_SDMA_LIKE:
		tq_adma2_inf_sdmalike_mode_init(&tq->ops);
		break;
	case CFG_TRANS_MODE_ADMA3_SDMA_LIKE:
		tq_adma3_sdmalike_mode_init(&tq->ops);
		break;
	case CFG_TRANS_MODE_ADMA_MIX:
		tq_adma2_inf_mode_init(&tq->ops);
		break;
	case CFG_TRANS_MODE_ADMA_MIX_SDMA_LIKE:
		tq_adma2_inf_sdmalike_mode_init(&tq->ops);
		break;
	case CFG_TRANS_MODE_ADMA2_ONLY:
		tq_adma2_mode_init(&tq->ops);
		break;
	case CFG_TRANS_MODE_ADMA2_ONLY_SDMA_LIKE:
		tq_adma2_sdmalike_mode_init(&tq->ops);
		break;
	default:
		/* ADMA2,no support PIO mode in tag queue now. */
		DbgErr("%s dma_mode(%d) invliad,so use default mode\n",
		       __func__, dma_mode);
		tq_adma2_mode_init(&tq->ops);
		break;
	}
	tq->cur_dma_mode = dma_mode;
	os_atomic_set(&(tq->target_dma_mode), dma_mode);
	tq->cfg_dma_mode = dma_mode;
	tq_dma_decision_init(&tq->decision, pdx->host.chip_type, 4, 2, 0);
	/* signature code for init done (TQ internal use) */
	tq->init_magic_number = TAG_QUEUE_INIT_MAGIC_NUMBER;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"%s max req total support:%d\n", __func__,
		tq->max_wq_req_size);
	if (tq->max_wq_req_size > 0)
		/*
		 * for dma infinite transfer limit: need reserved one for
		 * keep link address in descriptor buf.
		 */
		tq->max_wq_req_size -= TQ_RESERVED_NODE_SIZE;
	DbgInfo(MODULE_TQ_FLOW, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"%s max req use:%d\n", __func__, tq->max_wq_req_size);

#if (TQ_WORK_QUEUE_SIZE >= 2)

#define MIN_PINGPONG_BUILD_FUNCTION_SIZE 4
#define MAX_PINGPONG_BUILD_FUNCTION_SIZE 16

	if (tq->max_wq_req_size >= MIN_PINGPONG_BUILD_FUNCTION_SIZE) {
		tq->max_build_limit = tq->max_wq_req_size;
		tq->max_build_limit = tq->max_build_limit / 2;
		if (tq->max_build_limit >= MAX_PINGPONG_BUILD_FUNCTION_SIZE)
			tq->max_build_limit = MAX_PINGPONG_BUILD_FUNCTION_SIZE;
		DbgInfo(MODULE_TQ_FLOW, FEATURE_DRIVER_INIT, NOT_TO_RAM,
			"max build limit=%d\n", tq->max_build_limit);
	} else
		tq->max_build_limit = tq->max_wq_req_size;
#else
	tq->max_build_limit = tq->max_wq_req_size;
#endif

	ret = TRUE;
exit:
	DbgInfo(MODULE_TQ_FLOW, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return ret;
}
