// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: tqpolicy.c
 *
 * Abstract: handle tagqueue policy for transfer mode
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
#include "../include/cardapi.h"
#include "../include/tqapi.h"
#include "../include/debug.h"
#include "tq_trans_api.h"
#include "../include/util.h"
#include "../include/cmdhandler.h"

/*
 * consider below condition:
 * 1.merge enable
 * 2.infinite transfer enable
 * 3.DMA mode: ADMA3 support multi IO
 */

/*
 * if return TRUE, mean only build one IO.
 * if return FALSE, maybe build more then one IO for one transfer.
 */
bool tag_queue_policy_break(bht_dev_ext_t *pdx)
{
	u32 dma_mode = pdx->tag_queue.cur_dma_mode;
	u32 merge_enable =
	    pdx->cfg->host_item.test_tag_queue_capability.enable_srb_merge;

	switch (dma_mode) {
	case CFG_TRANS_MODE_SDMA:
		return TRUE;
	case CFG_TRANS_MODE_ADMA2:
	case CFG_TRANS_MODE_ADMA2_SDMA_LIKE:
		if (merge_enable == TRUE)
			return FALSE;
		else
			return TRUE;
	case CFG_TRANS_MODE_ADMA3:
	case CFG_TRANS_MODE_ADMA3_SDMA_LIKE:
		return FALSE;
	default:
		return TRUE;
	}
}

static bool decision_policy_calculus(decision_mgr *mgr, bool bval)
{
	int i = 0;
	int sum = 0;
	/* add */
	mgr->slot[mgr->idx % mgr->scope] = bval;
	mgr->idx++;

	for (i = 0; i < mgr->scope; i++) {
		if (mgr->slot[i])
			sum++;
	}

	if (mgr->up_flg == TRUE) {
		if (sum <= mgr->low_thd) {
			mgr->up_flg = FALSE;
			mgr->out = FALSE;
			goto exit;
		}
	} else {
		if (sum >= mgr->up_thd) {
			mgr->up_flg = TRUE;
			mgr->out = TRUE;
			goto exit;

		}
	}
exit:
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "sum %d %d\n",
		sum, mgr->out);
	return mgr->out;
}

void se2_dma_mode_selector(void *p, bool flg)
{
	tag_queue_t *tq = (tag_queue_t *) p;

	if (flg == FALSE) {
		if (tq->cfg_dma_mode == CFG_TRANS_MODE_ADMA_MIX_SDMA_LIKE) {
			os_atomic_set(&tq->target_dma_mode,
				      CFG_TRANS_MODE_ADMA3_SDMA_LIKE);
		} else {
			os_atomic_set(&tq->target_dma_mode,
				      CFG_TRANS_MODE_ADMA3);
		}
	} else {
		if (tq->cfg_dma_mode == CFG_TRANS_MODE_ADMA_MIX_SDMA_LIKE) {
			os_atomic_set(&tq->target_dma_mode,
				      CFG_TRANS_MODE_ADMA2_SDMA_LIKE);
		} else {
			os_atomic_set(&tq->target_dma_mode,
				      CFG_TRANS_MODE_ADMA2);
		}
	}
}

void legacy_infinite_dma_mode_selector(void *p, bool flg)
{
	tag_queue_t *tq = (tag_queue_t *) p;

	if (flg == FALSE) {
		if (tq->cfg_dma_mode == CFG_TRANS_MODE_ADMA_MIX_SDMA_LIKE) {
			os_atomic_set(&tq->target_dma_mode,
				      CFG_TRANS_MODE_ADMA2_ONLY_SDMA_LIKE);
		} else {
			os_atomic_set(&tq->target_dma_mode,
				      CFG_TRANS_MODE_ADMA2_ONLY);
		}
	} else {
		if (tq->cfg_dma_mode == CFG_TRANS_MODE_ADMA_MIX_SDMA_LIKE) {
			os_atomic_set(&tq->target_dma_mode,
				      CFG_TRANS_MODE_ADMA2_SDMA_LIKE);
		} else {
			os_atomic_set(&tq->target_dma_mode,
				      CFG_TRANS_MODE_ADMA2);
		}
	}
}

void tq_dma_decision_init(decision_mgr *mgr, e_chip_type chip_id,
			  int scope_size, int up_threshold, int low_threshold)
{
	DbgInfo(MODULE_TQ_FLOW, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Enter %s chip_id %d\n", __func__, chip_id);
	memset(mgr, 0, sizeof(decision_mgr));

	if (scope_size > MAX_DECISION_SCOPE_SIZE)
		scope_size = MAX_DECISION_SCOPE_SIZE;

	if (up_threshold > MAX_DECISION_SCOPE_SIZE)
		up_threshold = MAX_DECISION_SCOPE_SIZE;

	if (low_threshold > up_threshold)
		low_threshold = up_threshold;

	mgr->scope = scope_size;
	mgr->up_thd = up_threshold;
	mgr->low_thd = low_threshold;
	mgr->up_flg = FALSE;
	/* chip related */
	switch (chip_id) {
	case CHIP_SEABIRD:
	case CHIP_FUJIN2:
	case CHIP_SEAEAGLE:
		mgr->dma_selector_cb = legacy_infinite_dma_mode_selector;
		break;
	case CHIP_SEAEAGLE2:
	case CHIP_GG8:
	case CHIP_ALBATROSS:
		mgr->dma_selector_cb = se2_dma_mode_selector;
		break;
	default:
		mgr->dma_selector_cb = 0;
		break;
	}
	DbgInfo(MODULE_TQ_FLOW, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

void tq_dma_decision_policy(tag_queue_t *tq, sd_card_t *card,
			    request_t *request)
{
	bool bcont = FALSE;
	decision_mgr *mgr = &tq->decision;

	bcont =
	    tq_judge_request_continuous(card_is_low_capacity(card),
					mgr->last_req.data_dir,
					mgr->last_req.sec_addr,
					mgr->last_req.sec_cnt,
					request->data_dir,
					request->tag_req_t.sec_addr);
	/* update */
	mgr->last_req.data_dir = request->data_dir;
	mgr->last_req.sec_addr = request->tag_req_t.sec_addr;
	mgr->last_req.sec_cnt = request->tag_req_t.sec_cnt;
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s IO:dir:%d addr:0x%x ,cnt:0x%x (next:0x%x)\n",
		__func__, request->data_dir, request->tag_req_t.sec_addr,
		request->tag_req_t.sec_cnt,
		request->tag_req_t.sec_addr + request->tag_req_t.sec_cnt);

	if (mgr->dma_selector_cb) {
		bool dflg = decision_policy_calculus(&tq->decision, bcont);

		mgr->dma_selector_cb(tq, dflg);
	}

}
