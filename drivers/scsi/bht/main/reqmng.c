// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: reqmng.c
 *
 * Abstract: This file is used to manage interface for OSEntry layer
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
#include "../include/reqapi.h"
#include "../include/tqapi.h"
#include "../include/transhapi.h"
#include "../include/debug.h"
#include "../include/hostapi.h"
#include "../include/cardapi.h"
#include "../include/funcapi.h"
#include "funcapi.h"
#include "../include/cmdhandler.h"
#include "../include/hostvenapi.h"

#ifdef MultiThread
extern void thread_card_chg(void *pdx);
extern void thread_card_init(void *pdx);
extern void thread_event_pending(void *pdx);
extern void thread_event_tag_io(void *pdx);
extern void thread_event_gen_io(void *pdx);
extern void thread_runtime_d3(void *pdx);
extern void thread_auto_timer(void *pdx);
extern void thread_event_terminate(void *pdx);
extern void thread_camod_poll_card_chg(void *pdx);
#endif

/*
 * Function Name: req_global_init
 *
 * Abstract:
 *
 *		This routine  init timer, tagqueue, host cap init,
 *		and vendor reg setting, host interrupt and init to workable status
 *		card and host software structure init (caller init pci_dev_t and global cfg).
 *
 * Input:
 *
 *		pdx [in]: The device extension object.
 *
 * Output:
 *
 *		None
 *
 * Return value:
 *
 *		None
 *
 * Notes:
 *
 * IRQL <= APC_LEVEL
 */
s32 req_global_init(bht_dev_ext_t *pdx)
{
	DbgInfo(MODULE_REQ_MNG, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* Initialize the host pointer for the card structure */
	pdx->card.host = &(pdx->host);
	pdx->scsi_init_flag = 0;
	/* Host vendor settings */
	host_vendor_feature_init(&(pdx->host));

	host_init_capbility(&(pdx->host));

	/* 6 Initialize the OS layer */

	/* DPC init has to be in HWinitpassive routine */
	os_layer_init(pdx, &pdx->os);

	/* card struct init */
	card_stuct_init(pdx);

	os_memset(&pdx->pre_card, 0, sizeof(pre_card_info_t));

	pdx->pre_card.s3s4_resume_for_card_init = FALSE;

	pdx->auto_timer.s3reusme_cardchg_issuefix_en = FALSE;
	pdx->auto_timer.s3s4_start_timer = FALSE;
	pdx->auto_timer.s3reusme_timer_expect_cnt = 0;
	pdx->auto_timer.s3reusme_timer_actual_cnt = 0;

	func_autotimer_init(pdx);

	/* dma api init */
	dma_api_io_init(pdx, &pdx->dma_buff);

	/* Tag queue sturct init */

	/* dump mode don't use tag queue */
	if (pdx->dump_mode == FALSE)
		tag_queue_init(pdx);

	os_memset(&pdx->scsi, 0, sizeof(scsi_mng_t));

	pdx->soft_irq.enable = 0;
	pdx->p_srb_ext = NULL;

	/* 7. Initialize the thread */

	/* dump mode don't use thread */
	if (pdx->dump_mode == FALSE)
		os_create_thread(&pdx->os.thread, pdx, thread_main);

	/* 8. Host controller initialize */
	host_init(&(pdx->host));
	thermal_init(pdx);
	pdx->signature = BHT_PDX_SIGNATURE;

	/* 9. PM init */
	pm_init(pdx);

	if (hostven_chk_card_present(&pdx->host)) {
		hostven_main_power_ctrl(&pdx->host, TRUE);
		pdx->card.card_present = TRUE;
	} else {
		hostven_main_power_ctrl(&pdx->host, FALSE);
	}

	/* Test Case Init */
	testcase_init(pdx);

	DbgInfo(MODULE_REQ_MNG, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);

	return 0;
}

/*
 *		This function is used context:
 *		(1) In Thread
 *		(2) (Card and Tagqueue is not working)
 *		(3) No other SRB will coming down
 */
void req_global_reinit(bht_dev_ext_t *pdx)
{

	DbgInfo(MODULE_REQ_MNG, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* Host vendor settings */
	host_vendor_feature_init(&(pdx->host));

	host_init_capbility(&(pdx->host));

	card_stuct_uinit(&pdx->card);

	func_autotimer_init(pdx);

	tag_queue_init(pdx);

	thermal_init(pdx);

	if (hostven_chk_card_present(&pdx->host)) {
		hostven_main_power_ctrl(&pdx->host, TRUE);
		pdx->card.card_present = TRUE;
	}

	DbgInfo(MODULE_REQ_MNG, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);

}

/*
 * Function Name: req_cancel_all_io
 * Abstract: This Function is used to cancel all not handled io
 *
 * Input:
 *		bht_dev_ext_t *pdx
 *
 * Output:
 *
 */
void req_cancel_all_io(bht_dev_ext_t *pdx)
{
	DbgInfo(MODULE_REQ_MNG, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (pdx->dump_mode)
		goto exit;
	/* try to cancel gen io */
	if (pdx->p_srb_ext != NULL) {
		if (pdx->p_srb_ext->req.srb_done_cb)
			pdx->p_srb_ext->req.srb_done_cb(pdx, pdx->p_srb_ext);
	} else {
		/* try to cancel tag io */
		tag_queue_abort(pdx, REQ_RESULT_ABORT);
	}
exit:
	DbgInfo(MODULE_REQ_MNG, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);

}

/*
 *
 * Function Name: req_global_uninit
 *
 * Abstract:
 *
 *		This routine  uninit os related variable, reset host, finish pending Srb
 *		reset host
 *
 * Input:
 *
 *		pdx [in]: The device extension object.
 *
 * Output:
 *
 *		None
 *
 * Return value:
 *
 *		None
 *
 * Notes:
 *
 *
 *
 *
 *                          IRQL <= APC_LEVEL
 */

s32 req_global_uninit(bht_dev_ext_t *pdx)
{
	DbgInfo(MODULE_REQ_MNG, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	func_autotimer_cancel(pdx);
	thermal_uninit(pdx);
	host_uninit(&pdx->host, TRUE);

	req_cancel_all_io(pdx);

	os_layer_uinit(pdx, &pdx->os);
	DbgInfo(MODULE_REQ_MNG, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return 0;
}

/*
 * Function Name: scsi_check_card_ready
 * Abstract: This Function is used to by scsi cmd to check whether UNIT IS READY OR NOT
 *
 * Input:
 *		bht_dev_ext_t *pdx
 *		sd_card_t *card,
 *
 * Output:
 *
 *
 * Return value:
 *		TRUE: Card is ready
 *		FALSE Card is not ready
 */

static bool scsi_check_card_ready(bht_dev_ext_t *pdx, sd_card_t *card)
{
	bool result = FALSE;

	if (pdx->scsi.scsi_eject)
		goto exit;

	/* if card not preset, return failed            */
	if (card->card_type == CARD_ERROR || card->card_type == CARD_NONE)
		goto exit;

	if (card->card_present == FALSE || card->initialized_once == FALSE
	    || card->card_chg)
		goto exit;

	if (card->locked)
		goto exit;

	result = TRUE;
exit:
	return result;
}

/*
 * Function Name: req_gen_io_add
 * Abstract: This Function is used to add srb request to General IO
 *
 * Input:
 *		bht_dev_ext_t *pdx
 *		srb_ext_t *srb_ext: caller need to fill request_t req;
 * Output:
 *
 *
 * Return value:
 *
 *		e_req_result: the result of this command,
 *		scsi layer use this value to determine scsi cmd status
 */
static bool append_general_io_queue(srb_ext_t **head, srb_ext_t *node)
{
	if (head == NULL)
		return FALSE;

	if (*head == NULL)
		*head = node;
	else {
		srb_ext_t *tail = *head;

		while (tail->next != NULL)
			tail = tail->next;
		tail->next = node;
	}
	return TRUE;
}

e_req_result req_gen_io_add(bht_dev_ext_t *pdx, srb_ext_t *srb_ext)
{
	e_req_result result = REQ_RESULT_QUEUE_BUSY;

	DbgInfo(MODULE_REQ_MNG, FEATURE_IOCTL_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* If currently working at general IO, we can't start tag queue io */
#if (0)
	if (pdx->p_srb_ext != NULL)
		if (pdx->p_srb_ext->req.type == REQ_TYPE_GEN_IO)
			goto exit;
#endif

	result = REQ_RESULT_PENDING;
	srb_ext->req.type = REQ_TYPE_GEN_IO;
	srb_ext->req.result = REQ_RESULT_PENDING;
	srb_ext->next = NULL;
	/* check whether the taq queue is empty     */
	if (tq_is_empty(pdx) == FALSE) {
		append_general_io_queue(&(pdx->p_srb_ext_bak), srb_ext);
	} else {
		append_general_io_queue(&(pdx->p_srb_ext), srb_ext);
		func_autotimer_stop(pdx);
#if CFG_OS_LINUX
		os_set_event(&pdx->os, EVENT_GEN_IO);
#else
		os_set_event(pdx, &pdx->os, EVENT_TASK_OCCUR, EVENT_GEN_IO);
#endif
	}

	DbgInfo(MODULE_REQ_MNG, FEATURE_IOCTL_TRACE, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, result);
	return result;

}

/*
 * Function Name: req_tag_io_add
 * Abstract: This Function is used to add srb request to tag queue
 *
 * Input:
 *		bht_dev_ext_t *pdx
 *		srb_ext_t *srb_ext: caller need to fill request_t req;
 * Output:
 *
 *
 * Return value:
 *
 *		e_req_result: the result of this command,
 *		scsi layer use this value to determine scsi cmd status
 */

e_req_result req_tag_io_add(bht_dev_ext_t *pdx, srb_ext_t *srb_ext)
{
	e_req_result result = REQ_RESULT_QUEUE_BUSY;
	sd_card_t *card = &pdx->card;
	u32 sec_cnt;
	u32 sec_addr;

	DbgInfo(MODULE_REQ_MNG, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* If currently working at general IO, we can't start tag queue io */
	if (pdx->p_srb_ext != NULL)
		if (pdx->p_srb_ext->req.type == REQ_TYPE_GEN_IO)
			goto exit;

	if (scsi_check_card_ready(pdx, card) == FALSE) {
		result = REQ_RESULT_NO_CARD;
		goto exit;
	}

	/* check write protected    */
	if (card->write_protected && srb_ext->req.data_dir == DATA_DIR_OUT) {
		result = REQ_RESULT_PROTECTED;
		goto exit;
	}

	sec_addr = srb_ext->req.tag_req_t.sec_addr;
	sec_cnt = srb_ext->req.tag_req_t.sec_cnt;

	/*
	 * check request oversize
	 * todo: emmc boot partition case
	 */
	if ((card->sec_count < sec_addr)
	    || (card->sec_count < sec_cnt)
	    || ((card->sec_count - sec_addr) < sec_cnt)) {
		result = REQ_RESULT_OVERSIZE;
		goto exit;
	}

	/* recaculate 1.0 card sector addr  */

	if (card_is_low_capacity(card)) {
		sec_addr *= SD_BLOCK_LEN;
		srb_ext->req.tag_req_t.sec_addr = sec_addr;
	}

	/* for PIO case , forware the request to gen io */
	if (pdx->cfg->host_item.test_dma_mode_setting.dma_mode == 0xF) {
		srb_ext->req.gen_req_t.code = GEN_IO_CODE_PIORW;
		srb_ext->req.gen_req_t.arg1 = sec_addr;
		srb_ext->req.gen_req_t.arg2 = sec_cnt;
		result = req_gen_io_add(pdx, srb_ext);
	} else {
		/* update the request accroing to card type     */
		srb_ext->req.type = REQ_TYPE_TAG_IO;
		srb_ext->req.result = REQ_RESULT_PENDING;
		result = tq_add_request(pdx, srb_ext, card);
		if (result == REQ_RESULT_PENDING)
			func_autotimer_stop(pdx);
	}
exit:
	DbgInfo(MODULE_REQ_MNG, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, result);
	return result;
}

/*
 * Function Name: req_card_ready
 * Abstract: This Function is used to by scsi cmd to check whether UNIT IS READY OR NOT
 *
 * Input:
 *		bht_dev_ext_t *pdx
 * Output:
 *
 *
 * Return value:
 *		TRUE: Card is ready
 *		FALSE Card is not ready
 */
bool req_card_ready(bht_dev_ext_t *pdx)
{
	return scsi_check_card_ready(pdx, &pdx->card);
}

/*
 * Function Name: scsi_check_card_ready
 * Abstract: This Function is used to by scsi cmd to check whether UNIT IS READY OR NOT
 *
 * Input:
 *		bht_dev_ext_t *pdx,
 *
 * Output:
 *		e_req_result *result
 *
 * Return value:
 *		reutrn card if card is initialized
 */
e_req_result req_chk_card_info(bht_dev_ext_t *pdx, srb_ext_t *srb_ext)
{
	sd_card_t *card = &pdx->card;
	e_req_result result = REQ_RESULT_NO_CARD;

	if (pdx->scsi.scsi_eject)
		goto exit;

	if (card->card_present == FALSE)
		goto exit;

	/* check whether card has initailaze once */
	if (card->initialized_once == FALSE) {
		if (pdx->dump_mode == FALSE)
			result = req_gen_io_add(pdx, srb_ext);
		else
			goto exit;

		if (result != REQ_RESULT_PENDING)
			DbgErr("req chk card info add gen io failed\n");
	} else {
		result = REQ_RESULT_OK;
	}

exit:
	return result;
}

/*
 * Function Name: req_eject
 * Abstract: This Function is used  by scsi cmd to eject(poweroff) sd card
 *
 * Input:
 *		bht_dev_ext_t *pdx,
 *
 * Output:
 *		e_req_result  result
 *
 * Return value:
 *		reutrn card if card is initialized
 */

e_req_result req_eject(bht_dev_ext_t *pdx, srb_ext_t *srb_ext)
{
	sd_card_t *card = &pdx->card;
	e_req_result result = REQ_RESULT_NO_CARD;

	DbgInfo(MODULE_REQ_MNG, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Enter %s ret=%d\n", __func__, result);

	if (card->card_present == FALSE)
		goto exit;

	/* If card not power off, send request to thread to do poweroff */
	if (card->state != CARD_STATE_POWEROFF) {
		result = req_gen_io_add(pdx, srb_ext);
		if (result != REQ_RESULT_PENDING)
			DbgErr("eject add gen io failed\n");
		goto exit;
	}

	result = REQ_RESULT_OK;

exit:
	DbgInfo(MODULE_REQ_MNG, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, result);
	return result;
}
