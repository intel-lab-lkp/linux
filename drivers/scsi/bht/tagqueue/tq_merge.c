// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: tq_merge.c
 *
 * Abstract: handle tagqueue sdma_like transfer cb ops.
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

/*
 *
 * Function Name: queue_2_tb
 *
 * Abstract:
 *
 *		convert tq list to array format.
 *
 * Input:
 *
 *		req_queue_t *rq [in]: Pointer to queue
 *		pnode_t *tb [in] : array table
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 *		convert size
 * Notes:
 *
 * Caller:
 *
 */
static u32 queue_2_tb(req_queue_t *rq, pnode_t *tb)
{
	u32 len = 0;
	u32 i = 0;
	node_t *node = 0;

	/* check parameter */
	len = node_list_get_cnt(&rq->list);

	if (len > MAX_WORK_QUEUE_SIZE) {
		len = MAX_WORK_QUEUE_SIZE;
		DbgErr("%s overflow\n", __func__);
	}
	/* convert to array */
	for (i = 0; i < len; i++) {
		/* get one from queue */
		node = node_list_get_one(&rq->list);
		if (node == NULL)
			break;
		tb[i] = node;
		/* put back to queue */
		node_list_put_one(&rq->list, node);
	}

	return len;
}

/*
 *
 * Function Name: mark_continuous_flag
 *
 * Abstract:
 *
 *		mark continuos flag for transfer queue.
 *       if next node is continuous node, then mark current node flag means can merge
 *
 * Input:
 *
 *		sd_card_t *card [in]: card which for merge
 *		pnode_t *tb [in] : Pointer to array
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 *
 * Notes:
 *
 * Caller:
 *
 */
bool tq_judge_request_continuous(bool low_capacity_card, e_data_dir req_dir,
				 u32 req_sec_addr, u32 req_sec_cnt,
				 e_data_dir next_req_dir, u32 next_req_sec_addr)
{
	bool ret = FALSE;

	DbgInfo(MODULE_TQ_POLICY, FEATURE_RW_TRACE, NOT_TO_RAM,
		"req dir%x next dir%x\n", req_dir, next_req_dir);
	DbgInfo(MODULE_TQ_POLICY, FEATURE_RW_TRACE, NOT_TO_RAM,
		"req sec%x cnt%x, next %x\n", req_sec_addr, req_sec_cnt,
		next_req_sec_addr);
	/* same dir */
	if (req_dir == next_req_dir) {
		u32 factor = 1;
		/* for scsd card need */
		if (low_capacity_card)
			factor = SD_BLOCK_LEN;
		else
			factor = 1;
		/* calculate continuos case */
		if ((req_sec_addr + req_sec_cnt * factor) == next_req_sec_addr) {
			ret = TRUE;
			DbgInfo(MODULE_TQ_POLICY, FEATURE_RW_TRACE, NOT_TO_RAM,
				"can merge\n");
		}

	}
	return ret;
}

static bool mark_continuous_flag(sd_card_t *card, pnode_t *tb, u32 len)
{
	u32 i = 0;
	srb_ext_t *pext = 0;
	request_t *req = 0;
	request_t *next_req = 0;

	DbgInfo(MODULE_TQ_POLICY, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/*
	 * loop for mark
	 * len-1 :make sure no over flow for mark
	 */
	for (i = 0; i < (len - 1); i++) {
		pext = node_2_srb_ext(tb[i]);
		req = &pext->req;
		/* get next req */
		pext = node_2_srb_ext(tb[i + 1]);
		next_req = &pext->req;

		/* clear or will false set */
		tb[i]->flag =
		    tq_judge_request_continuous(card_is_low_capacity(card),
						req->data_dir,
						req->tag_req_t.sec_addr,
						req->tag_req_t.sec_cnt,
						next_req->data_dir,
						next_req->tag_req_t.sec_addr);
	}
	DbgInfo(MODULE_TQ_POLICY, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return TRUE;
}

/*
 *
 * Function Name: update_adma3_blk_cnt
 *
 * Abstract:
 *
 *		update adma3 command descriptor for block counter
 *
 * Input:
 *
 *		sd_card_t *card [in]: card which for merge
 *		dma_desc_buf_t *pdma [in] : Pointer to adma3 command descriptor buffer
 *
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 *
 * Notes:
 *
 * Caller:
 *
 */
int update_adma3_blk_cnt(sd_card_t *card, const dma_desc_buf_t *pdma, u32 cnt)
{
	u32 *ptb = (u32 *) pdma->va;

	if (card->card_type == CARD_UHS2) {
		*(ptb + 3) = cnt;
		*(ptb + 9) = swapu32(cnt);
	} else {
		*(ptb + 1) = cnt;
	}
	return 0;
}

/*
 *
 * Function Name:  merge_continous_io
 *
 * Abstract:
 *
 *		merge one continuous IOs until first break flag occur
 *
 * Input:
 *
 *		sd_card_t *card [in]: card which for merge
 *
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *		merge number io.
 *
 * Notes:
 *
 * Caller:
 *
 */
static int merge_continous_io(req_queue_t *rq, sd_card_t *card, pnode_t *tb,
			      int len, bool dma_64bit)
{
	int i = 0;
	dma_desc_buf_t *pdma = 0;
	int merge_cnt = 0;
	srb_ext_t *pext = 0;
	request_t *req = 0;
	request_t *next_req = 0;
	byte *pdesc = 0;
	u32 size = 0;

	DbgInfo(MODULE_TQ_POLICY, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s len:%d dma_64b:%d\n", __func__, len, dma_64bit);
	merge_cnt = 0;
	/* get one integrate desc items buffer */
	pdma = get_one_integrate_desc_res(rq);

	pext = node_2_srb_ext(tb[i]);
	req = &pext->req;

	merge_cnt = req->tag_req_t.sec_cnt;
	/* do merge if any */
	for (i = 0; i < len - 1; i++) {
		pext = node_2_srb_ext(tb[i]);
		req = &pext->req;
		/* get next req */
		pext = node_2_srb_ext(tb[i + 1]);
		next_req = &pext->req;

		if (tb[i]->flag == TRUE) {
			phy_addr_t m_pa;
			u32 cmd_desc_len = 0;

			merge_cnt += next_req->tag_req_t.sec_cnt;
			/* get next adma2 table physical address */
			m_pa = tb[i + 1]->phy_node_buffer.head.pa;
			if (card->card_type == CARD_UHS2) {
				cmd_desc_len =
				    ADMA3_CMDDESC_ITEM_LENGTH *
				    ADMA3_CMDDESC_ITEM_NUM_UHSII;
			} else {
				cmd_desc_len =
				    ADMA3_CMDDESC_ITEM_LENGTH *
				    ADMA3_CMDDESC_ITEM_NUM_UHSI;
			}
			pa_offset_pa(&m_pa, cmd_desc_len);
			/* update adma2 table */
			link_adma2_desc(tb[i]->phy_node_buffer.end.va, &m_pa,
					dma_64bit);
		} else {
			DbgInfo(MODULE_TQ_POLICY, FEATURE_RW_TRACE, NOT_TO_RAM,
				"no continues (%x)\n", i);
			break;
		}
	}

	pdesc =
	    build_integrated_desc(pdma->va, &(tb[0]->phy_node_buffer.head.pa),
				  dma_64bit);
	size = pp_ofs(pdesc, pdma->va);
	put_one_integrate_desc(rq, size);
	/* update cnt */
	update_adma3_blk_cnt(card, &tb[0]->phy_node_buffer.head, merge_cnt);
	DbgInfo(MODULE_TQ_POLICY, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Exit%s ret:%d\n", __func__, i + 1);
	return (i + 1);
}

/*
 *
 * Function Name: _update_io_descriptor
 *
 * Abstract:
 *
 *		only forcus on merge IOs descriptor for queue
 *
 * Input:
 *
 *		sd_card_t *card [in]: card which for merge
 *
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 *
 * Notes:
 *
 * Caller:
 *
 */
static int _update_io_descriptor(req_queue_t *rq, sd_card_t *card,
				 pnode_t *tb, int len, bool dma_64bit)
{
	int sz = 0, left = 0;
	int i = 0;

	left = len;
	for (i = 0; i < len;) {
		sz = merge_continous_io(rq, card, &tb[i], left, dma_64bit);
		DbgInfo(MODULE_TQ_POLICY, FEATURE_RW_TRACE, NOT_TO_RAM,
			"merge sz=%x,len=%x\n", sz, len);
		left -= sz;
		i += sz;
	}
	return 0;

}

/*
 *
 * Function Name: update_io_descriptor
 *
 * Abstract:
 *
 *		update merge IOs descriptor for queue
 *
 * Input:
 *
 *		sd_card_t *card [in]: card which for merge
 *
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 *
 * Notes:
 *
 * Caller:
 *
 */
static bool update_io_descriptor(sd_card_t *card, req_queue_t *rq,
				 pnode_t *tb, u32 len, bool dma_64bit)
{
	/* reset */
	tq_adma3_reset_integrate(rq);
	/* update descriptor */
	_update_io_descriptor(rq, card, &tb[0], len, dma_64bit);
	/* end table */
	adma3_end_integrated_tb(rq->adma3_integrate_tbl_cur.va, dma_64bit);

	return 0;
}

/*
 *
 * Function Name: adma3_merge_io_descriptor
 *
 * Abstract:
 *
 *		adma3 merge IOs descriptor for queue
 *
 * Input:
 *
 *		sd_card_t *card [in]: card which for merge
 *
 *
 * Output:
 *
 *		None.
 *
 * Return value:
 *
 *
 * Notes:
 *
 * Caller:
 *
 */
bool adma3_merge_io_descriptor(req_queue_t *rq, sd_card_t *card,
			       bool dma_64bit)
{
	u32 len = 0;
	node_t *tb[MAX_WORK_QUEUE_SIZE];
	bool ret = 0;

	len = node_list_get_cnt(&rq->list);
	DbgInfo(MODULE_TQ_POLICY, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s len:%d dma_64b:%d\n", __func__, len, dma_64bit);
	/* check */
	if (len < 2) {
		/* no need merge for one SRB */
		return TRUE;
	}

	/* convert to node array */
	len = queue_2_tb(rq, tb);
	mark_continuous_flag(card, tb, len);
	ret = update_io_descriptor(card, rq, tb, len, dma_64bit);
	DbgInfo(MODULE_TQ_POLICY, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Exit %s ret:%d\n", __func__, ret);
	return ret;
}
