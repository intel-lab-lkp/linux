// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: thread.c
 *
 * Abstract: This file is used to handle thread event
 *
 * Version: 1.00
 *
 * Author: Peter.Guo
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 8/25/2014		Creation	Peter.Guo
 */

#include "../include/basic.h"
#include "../include/cardapi.h"
#include "../include/reqapi.h"
#include "../include/tqapi.h"
#include "../include/hostapi.h"
#include "../include/cmdhandler.h"
#include "../include/funcapi.h"
#include "funcapi.h"
#include "../include/debug.h"
#include "../include/hostvenapi.h"
#include "../linux_os/linux_scsi.h"

/*
 * Function Name: thread_init_card
 * Abstract: This Function is used to by thread to init card
 *
 * Input:
 *		bht_dev_ext_t *pdx,
 *		bool init_err_card,  for error card case need to do init or not
 *		bool send_bus_chg : need to send bus change or not
 *		int retry_num : init retry time
 *
 * Return value:
 *		TRUE: means ok
 *		others error
 *
 * Notes:
 *		run in thread context
 */
static bool thread_init_card(bht_dev_ext_t *pdx, bool init_err_card,
			     bool send_bus_chg, int retry_num)
{
	bool result = FALSE;
	sd_card_t *card = &pdx->card;

	DbgInfo(MODULE_MAIN_THR, FEATURE_THREAD_TRACE, NOT_TO_RAM,
		"Enter %s init_err=%d\n", __func__, init_err_card);

	/* If error card and don't reinit err_card  */
	if (card->card_type == CARD_ERROR && init_err_card == FALSE)
		goto exit;

	card->card_chg = FALSE;

	if (card->card_type != CARD_ERROR && card->card_type != CARD_NONE) {
		if (card->state == CARD_STATE_WORKING) {
			result = TRUE;
			goto exit;
		}
	}

	/* Init card here   */
	result = card_init(card, retry_num, FALSE);

	/* todo degrade mode control if init failed */

	/* If Card Init ok and scsi last presetn is 0 and need to enable to send bus change */
	if (result == TRUE && pdx->scsi.last_present == 0 && send_bus_chg
	    && card->card_type != CARD_SD70) {

		DbgInfo(MODULE_MAIN_THR, FEATURE_THREAD_TRACE, NOT_TO_RAM,
			"Exec Bus Change for Ins Card\n");

		/* add for LCFC s3/s4 card change issue 20160830 */
		if ((pdx->pre_card.pre_sec_count != 0)
		    && ((pdx->pre_card.pre_sec_count != pdx->card.sec_count)
			||
			(os_memcpr
			 (&(pdx->pre_card.pre_cid[0]),
			  &(pdx->card.info.raw_cid[0]), CID_LEN) != CID_LEN))
		    && (pdx->pre_card.s3s4_resume_for_card_init)) {
			DbgInfo(MODULE_MAIN_THR, FEATURE_THREAD_TRACE,
				NOT_TO_RAM,
				"LCFC s3 s4 resume card change issue!\n");
			pdx->card.card_present = FALSE;
			thread_exec_high_prio_job(pdx, os_bus_change, pdx);
			/* Use 500ms timer to set the card present to TRUE and send bus change */
			pdx->auto_timer.s3reusme_cardchg_issuefix_en = TRUE;
			pdx->auto_timer.s3reusme_timer_expect_cnt = 2;
			pdx->auto_timer.s3reusme_timer_actual_cnt = 2;
		} else {
			/* callback execute successfully */
			if (thread_exec_high_prio_job(pdx, os_bus_change, pdx))
				pdx->scsi.last_present = 1;
		}
		pdx->pre_card.pre_sec_count = pdx->card.sec_count;
		os_memcpy(&(pdx->pre_card.pre_cid[0]),
			  &(pdx->card.info.raw_cid[0]), CID_LEN);

	}

exit:

	DbgInfo(MODULE_MAIN_THR, FEATURE_THREAD_TRACE, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, result);
	return result;

}

/*
 * Function Name: thread_wakeup_card
 * Abstract: This Function is used to by thread to wake up card from low power state
 *
 * Input:
 *		bht_dev_ext_t *pdx,
 *
 * Return value:
 *		TRUE: means ok
 *		others error
 *
 * Notes:
 *		run in thread context
 */
e_req_result thread_wakeup_card(bht_dev_ext_t *pdx)
{
	e_req_result result = REQ_RESULT_OK;
	sd_card_t *card = &pdx->card;

	DbgInfo(MODULE_MAIN_THR, FEATURE_RW_TRACE | FEATURE_IOCTL_TRACE,
		NOT_TO_RAM, "Enter %s\n", __func__);

	if (card->card_present == FALSE) {
		result = REQ_RESULT_NO_CARD;
		goto exit;
	}

	if (card->state == CARD_STATE_SLEEP ||
	    card->state == CARD_STATE_DEEP_SLEEP) {
		if (card_resume_sleep(card, TRUE) == FALSE)
			result = REQ_RESULT_ACCESS_ERR;
	}
	/* If card is power off then reinit card */
	if (card->state == CARD_STATE_POWEROFF) {
		if (thread_init_card(pdx, FALSE, FALSE, CARD_REINIT_RETRY) ==
		    FALSE) {
			result = REQ_RESULT_ACCESS_ERR;
			goto exit;
		}
	}

exit:
	if (result == FALSE)
		DbgErr("Wakeup card failed\n");

	DbgInfo(MODULE_MAIN_THR, FEATURE_RW_TRACE | FEATURE_IOCTL_TRACE,
		NOT_TO_RAM, "Exit %s result=%d\n", __func__, result);
	return result;
}

typedef struct {
	bht_dev_ext_t *pdx;
	srb_ext_t *srb_ext;
} req_io_item_t;

static void thread_io_done(void *p_item)
{
	req_io_item_t *item = p_item;

	if (item->srb_ext->req.srb_done_cb)
		item->srb_ext->req.srb_done_cb(item->pdx, item->srb_ext);
	/* default handler */
	else {
		if (item->pdx)
			item->pdx->p_srb_ext = NULL;
	}

}

/*
 * Function Name: thread_tag_io
 * Abstract: This Function is used to by thread to trigger tag io
 *
 * Input:
 *		bht_dev_ext_t *pdx,
 *
 * Return value:
 *		TRUE: means ok
 *		others error
 *
 * Notes:
 *		run in thread context
 */
static void thread_tag_io(bht_dev_ext_t *pdx)
{
	e_req_result result;

	DbgInfo(MODULE_MAIN_THR, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	result = tag_queue_rw_data(pdx);
	if (result == REQ_RESULT_NO_CARD || result == REQ_RESULT_ABORT) {
		/* do nothing for low level api will cancel all io */
	} else if (result == REQ_RESULT_ACCESS_ERR) {
		if (tq_is_empty(pdx) == FALSE)
			os_set_event(&pdx->os, EVENT_TAG_IO);
	}

	DbgInfo(MODULE_MAIN_THR, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);

}

/*
 * Function Name: thread_remove_card
 * Abstract: This Function is used to by thread to trigger tag io
 *
 * Input:
 *		bht_dev_ext_t *pdx,
 *
 * Return value:
 *		TRUE: means ok
 *		others error
 *
 * Notes:
 *		run in thread context
 */
static void thread_remove_card(bht_dev_ext_t *pdx, bool eject)
{

	DbgInfo(MODULE_MAIN_THR, FEATURE_THREAD_TRACE, NOT_TO_RAM,
		"Enter %s eject=%d\n", __func__, eject);
	if (eject == FALSE) {
		host_init(&pdx->host);

		card_stuct_uinit(&pdx->card);
		/* host reset for all and reopen card init */
		if (pdx->scsi.last_present == 1) {
			/* callback execute successfully */
			if (thread_exec_high_prio_job(pdx, os_bus_change, pdx))
				pdx->scsi.last_present = 0;
		}

		/* When card removed or not present, let Hardware control the host main power. */
		hostven_main_power_ctrl(&pdx->host, FALSE);
		/* todo call ltr featre */
	}
	/* card is still present, and this remove is called by eject */

	DbgInfo(MODULE_MAIN_THR, FEATURE_THREAD_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);

}

/*
 * Function Name: thread_gen_io
 * Abstract: This Function is used to by thread to trigger tag io
 *
 * Input:
 *		bht_dev_ext_t *pdx,
 *
 * Return value:
 *		TRUE: means ok
 *		others error
 *
 * Notes:
 *		run in thread context
 */
static void thread_gen_io(bht_dev_ext_t *pdx)
{
	e_req_result result = REQ_RESULT_OK;
	srb_ext_t *srb_ext = pdx->p_srb_ext;
	sd_card_t *card = &pdx->card;
	req_io_item_t item;
	bool ret = FALSE;

	DbgInfo(MODULE_MAIN_THR, FEATURE_IOCTL_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	if (srb_ext == NULL)
		goto exit;

	if (srb_ext->req.type != REQ_TYPE_GEN_IO) {
		DbgWarn(MODULE_MAIN_THR, NOT_TO_RAM,
			"pdx srb_ext should be genio type\n");
		goto exit;
	}

	item.pdx = pdx;
	item.srb_ext = srb_ext;
	os_set_dev_busy(pdx);

	switch (srb_ext->req.gen_req_t.code) {
	case GEN_IO_CODE_INIT_CARD:
		if (thread_init_card(pdx, FALSE, TRUE, 3) == FALSE)
			result = REQ_RESULT_NO_CARD;
		break;
	case GEN_IO_CODE_EJECT:
		/* TODO currently we can only do phy card poweroff */
		thread_remove_card(pdx, TRUE);
		break;
	case GEN_IO_CODE_PIORW:
		result = thread_wakeup_card(pdx);
		if (result == REQ_RESULT_OK) {
			ret = card_piorw_data(card, srb_ext->req.gen_req_t.arg1,
					      srb_ext->req.gen_req_t.arg2,
					      srb_ext->req.data_dir,
					      srb_ext->req.srb_buff);
			if (ret == TRUE)
				break;
			if (card->card_present == FALSE)
				result = REQ_RESULT_NO_CARD;
			else
				result = REQ_RESULT_ACCESS_ERR;
		}
		break;
	case GEN_IO_CODE_CPRM:
		result = thread_wakeup_card(pdx);
		if (result == REQ_RESULT_OK) {
			ret = func_cprm(card, &(srb_ext->req));
			if (ret == TRUE)
				break;
			result = REQ_RESULT_ACCESS_ERR;
			if (card->card_present == FALSE)
				result = REQ_RESULT_NO_CARD;
			else
				result = REQ_RESULT_ACCESS_ERR;
		}
		break;
	case GEN_IO_CODE_IO:
		ret = func_io_reg(card, &(srb_ext->req));

		break;
	case GEN_IO_CODE_NSM:
		result = thread_wakeup_card(pdx);
		if (result == REQ_RESULT_OK) {
			ret = func_nsm(card, &(srb_ext->req), pdx);
			if (ret == TRUE)
				break;
			result = REQ_RESULT_ACCESS_ERR;
			if (card->card_present == FALSE)
				result = REQ_RESULT_NO_CARD;
			else
				result = REQ_RESULT_ACCESS_ERR;
		}
		break;
	case GEN_IO_CODE_RECFG:
		DbgInfo(MODULE_MAIN_THR,
			FEATURE_DRIVER_INIT | FEATURE_IOCTL_TRACE, 0,
			"Begin do Reload Cfg\n");
		func_autotimer_stop(pdx);
		card_power_off(&pdx->card, FALSE);

		cfgmng_init_chipcfg(pdx->host.chip_type, pdx->cfg, TRUE);
		pdx->cfg =
		    cfgmng_get(pdx, pdx->host.chip_type, pdx->cfg->boot_flag);
		pdx->host.cfg = pdx->cfg;

		req_global_reinit(pdx);

		if (card->card_present) {
			if (thread_init_card
			    (pdx, FALSE, FALSE, CARD_REINIT_RETRY) == FALSE) {
				result = REQ_RESULT_ACCESS_ERR;
				break;
			}
		}

		result = REQ_RESULT_OK;
		break;
	case GEN_IO_CODE_CSD:

		result = thread_wakeup_card(pdx);
		if (result == REQ_RESULT_OK) {

			if (srb_ext->req.data_dir == DATA_DIR_OUT) {

				ret =
				    card_program_csd(&pdx->card,
						     (u8 *) srb_ext->req.srb_buff);
				if (!ret)
					result = REQ_RESULT_ACCESS_ERR;

			} else if (srb_ext->req.data_dir == DATA_DIR_IN) {

				ret =
				    card_read_csd(&pdx->card,
						  (u8 *) srb_ext->req.srb_buff);
				if (!ret)
					result = REQ_RESULT_ACCESS_ERR;

			}

		}

		break;

	default:
		/* CPRM function is to do */
		break;
	}

	srb_ext->req.result = result;

	/* As card is removed we dont' need to enable timer */
	if (srb_ext->req.gen_req_t.code != GEN_IO_CODE_EJECT)
		thread_exec_high_prio_job(pdx,
					  (cb_soft_intr_t) func_autotimer_start,
					  pdx);

	os_set_dev_idle(pdx);
	thread_exec_high_prio_job(pdx, thread_io_done, &item);

exit:
	DbgInfo(MODULE_MAIN_THR, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
		"Exit %s %d\n", __func__);
}

void thread_handle_card_event(bht_dev_ext_t *pdx)
{
	if (pdx->card.card_present) {
		if (thread_init_card(pdx, TRUE, TRUE, CARD_FIRST_INIT_RETRY) ==
		    TRUE)
			thread_exec_high_prio_job(pdx, (cb_soft_intr_t)
						  func_autotimer_start, pdx);
		else if (pdx->card.card_type == CARD_SDIO) {
			DbgInfo(MODULE_MAIN_THR, FEATURE_CARD_INIT, TO_RAM,
				"Failed Init with SDIO\n");
			os_set_sdio_val(pdx, 1, FALSE);
		}

		DbgInfo(MODULE_MAIN_THR, FEATURE_CARD_INIT, TO_RAM,
			"THREAD init card\n");
	} else {
		thread_remove_card(pdx, FALSE);
		DbgInfo(MODULE_MAIN_THR, FEATURE_CARD_INIT, TO_RAM,
			"THREAD remove card\n");
	}
	pdx->pre_card.s3s4_resume_for_card_init = FALSE;

}

void thread_main(void *param)
{
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) param;
	e_event_t evt = EVENT_NONE;
	os_struct *os = &pdx->os;

	for (;;) {
		DbgInfo(MODULE_MAIN_THR, FEATURE_THREAD_TRACE, NOT_TO_RAM,
			"thread wait evt\n");
		evt = os_wait_event(os);
		os_clear_event(os, evt);
		DbgInfo(MODULE_MAIN_THR, FEATURE_THREAD_TRACE, NOT_TO_RAM,
			"Thr get Event=%d\n", evt);
		switch (evt) {
		case EVENT_CARD_CHG:
			os_set_dev_busy(pdx);
			if (pdx->pm_state.rtd3_en && pdx->pm_state.rtd3_entered)
				break;

			if (pdx->testcase.test_type == 0
			    || pdx->testcase.test_type == 3)
				thread_handle_card_event(pdx);
			else
				/* with card test */
				testcase_main(pdx, 1);

			os_set_dev_idle(pdx);
			break;
		case EVENT_TERMINATE:
			DbgInfo(MODULE_MAIN_THR, FEATURE_THREAD_TRACE, TO_RAM,
				"terminal task\n");
			goto exit;
		case EVENT_PENDING:
			os->thread.pending_lock = TRUE;
			DbgInfo(MODULE_MAIN_THR, FEATURE_THREAD_TRACE, TO_RAM,
				"pending thread start freeze=%d\n",
				os->thread.freeze);
			if (os->thread.freeze)
				os_wait_for_completion(pdx,
						       &os->thread.break_pending, 0);
			DbgInfo(MODULE_MAIN_THR, FEATURE_THREAD_TRACE, TO_RAM,
				"pending thread stop\n");
			break;
		case EVENT_TAG_IO:
			calc_thr_start(&pdx->tick);
			thread_tag_io(pdx);
			DbgInfo(MODULE_MAIN_THR, FEATURE_RW_TRACE, NOT_TO_RAM,
				"rw evt task\n");
			break;
		case EVENT_GEN_IO:
			thread_gen_io(pdx);
			DbgInfo(MODULE_MAIN_THR, FEATURE_IOCTL_TRACE,
				NOT_TO_RAM, "io_ctl task\n");
			break;
		case EVENT_RUNTIME_D3:
			DbgInfo(MODULE_MAIN_THR, FEATURE_PM_TRACE, TO_RAM,
				"rtd3 task\n");
			os_rtd3_req_wait_wake(pdx);
			break;
		case EVENT_AUTO_TIMER:
			func_timer_thread(pdx);
			DbgInfo(MODULE_MAIN_THR, FEATURE_THREAD_TRACE,
				NOT_TO_RAM, "autoTimer task\n");
			break;
		case EVENT_SDIO:
			DbgInfo(MODULE_MAIN_THR, MODULE_OTHER_CARD, TO_RAM,
				"sdio task\n");
			break;

		default:
			break;
		}

		DbgInfo(MODULE_MAIN_THR, FEATURE_THREAD_TRACE, NOT_TO_RAM,
			"Thr done Event=%d\n", evt);

	}
exit:
	DbgInfo(MODULE_MAIN_THR, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"exit thread\n");
}
