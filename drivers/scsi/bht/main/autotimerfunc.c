// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: autotimerfunc.c
 *
 * Abstract: This source file used to implemnt auto timer functions
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
#include "../include/host.h"
#include "../include/hostapi.h"
#include "../include/debug.h"
#include "../include/util.h"

/*
 * Function Name: autotimer_clear
 * Abstract: This Function is used to clear timer tick
 *
 * Input:
 *	bht_dev_ext_t *pdx
 *
 */
static void autotimer_clear(bht_dev_ext_t *pdx)
{
	pdx->auto_timer.auto_dmt_tick = 0;
	pdx->auto_timer.auto_poweroff_tick = 0;
	pdx->auto_timer.auto_cmd12_tick = 0;
	pdx->auto_timer.last_tick = 0;
	pdx->auto_timer.auto_led_off_tick = 0;
}

/*
 * Function Name: func_timer_callback
 * Abstract: This Function is used to calculate timer tick
 *
 * Input:
 *	bht_dev_ext_t *pdx
 *
 *
 * Notes:
 *
 *        so giving the routine another name requires you to modify the build tools.
 */

void func_timer_callback(bht_dev_ext_t *pdx)
{
	u32 cur_time, interval;
	bool event = FALSE;

	DbgInfo(MODULE_AUTOTIMER, FEATURE_TIMER_TRACE, 0, "Enter %s\n",
		__func__);
	if (pdx->auto_timer.enable == FALSE || pdx->auto_timer.cancel)
		goto exit;

	if (pdx->auto_timer.stop) {
		autotimer_clear(pdx);
		goto exit;
	}

	/* If card not workable don't use timer */
	if (pdx->card.card_present == FALSE)
		goto exit;
	if (pdx->card.card_type == CARD_NONE
	    || pdx->card.card_type == CARD_ERROR)
		goto exit;
	if (pdx->card.state == CARD_STATE_POWEROFF)
		goto exit;
	/* If rtd3 entered not do below steps */
	DbgInfo(MODULE_AUTOTIMER, FEATURE_TIMER_TRACE, 0,
		"auto timer really work\n");

	/* calculate the real interval */
	cur_time = os_get_cur_tick();
	if (pdx->auto_timer.last_tick == 0) {
		pdx->auto_timer.last_tick = cur_time;
		interval = AUTO_TIMER_TICK;
	} else {
		interval = cur_time - pdx->auto_timer.last_tick;
		pdx->auto_timer.last_tick = cur_time;
	}

	/* update and check timeout for each tick */
	if (pdx->auto_timer.auto_cmd12_enable && pdx->card.has_built_inf) {
		pdx->auto_timer.auto_cmd12_tick += interval;
		if (pdx->auto_timer.auto_cmd12_tick >=
		    pdx->auto_timer.auto_cmd12_time)
			event = TRUE;
	}

	if (pdx->auto_timer.auto_dmt_enable && pdx->card.card_type == CARD_UHS2
	    && pdx->card.state == CARD_STATE_WORKING) {
		pdx->auto_timer.auto_dmt_tick += interval;
		if (pdx->auto_timer.auto_dmt_tick >=
		    pdx->auto_timer.auto_dmt_time)
			event = TRUE;
	}

	if (pdx->auto_timer.auto_led_off_enable && (pdx->host.led_on)) {
		pdx->auto_timer.auto_led_off_tick += interval;
		if (pdx->auto_timer.auto_led_off_tick >=
		    pdx->auto_timer.auto_led_off_time)
			event = TRUE;
	}

	if (pdx->auto_timer.auto_poweroff_enable) {
		pdx->auto_timer.auto_poweroff_tick += interval;
		if (pdx->auto_timer.auto_poweroff_tick >=
		    pdx->auto_timer.auto_poweroff_time)
			event = TRUE;
	}

	if (pdx->auto_timer.cancel || pdx->auto_timer.stop)
		goto exit;

	if (event) {
		DbgInfo(MODULE_AUTOTIMER, FEATURE_TIMER_TRACE, 0,
			"auto timer set event\n");

#if CFG_OS_LINUX
		os_set_event(&pdx->os, EVENT_AUTO_TIMER);
#else
		os_set_event(pdx, &pdx->os, EVENT_TASK_OCCUR, EVENT_AUTO_TIMER);
#endif

	}
	os_start_timer(pdx, &pdx->os, TIMER_AUTO, AUTO_TIMER_TICK);

exit:
	DbgInfo(MODULE_AUTOTIMER, FEATURE_TIMER_TRACE, 0, "Exit %s\n",
		__func__);
}

/*
 * Function Name: func_autotimer_init
 * Abstract: This Function is used to init timer function variables
 *
 * Input:
 *	bht_dev_ext_t *pdx
 *
 *
 * Notes:
 *
 *        so giving the routine another name requires you to modify the build tools.
 */

void func_autotimer_init(bht_dev_ext_t *pdx)
{
	cfg_item_t *cfg = pdx->cfg;

	os_memset(&pdx->auto_timer, 0, sizeof(pdx->auto_timer));

	pdx->auto_timer.auto_dmt_time =
	    cfg->timer_item.auto_dormant_timer.time_ms;
	pdx->auto_timer.auto_dmt_enable =
	    (bool)cfg->timer_item.auto_dormant_timer.enable_dmt_func;
	pdx->auto_timer.enable_hibernate =
	    (bool)cfg->timer_item.auto_dormant_timer.enable_hbr;

	pdx->auto_timer.auto_poweroff_enable = FALSE;
	pdx->auto_timer.auto_poweroff_time = 10 * 1000;

	pdx->auto_timer.auto_led_off_enable = pdx->host.feature.hw_led_fix;
	/* led off set to 1s */
	pdx->auto_timer.auto_led_off_time = 1000;

	pdx->auto_timer.enable = pdx->auto_timer.auto_cmd12_enable |
	    pdx->auto_timer.auto_poweroff_enable |
	    pdx->auto_timer.auto_dmt_enable |
	    pdx->auto_timer.auto_led_off_enable;

	DbgInfo(MODULE_AUTOTIMER, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Autopower off enable=%d time=%dms\n",
		pdx->auto_timer.auto_poweroff_enable,
		pdx->auto_timer.auto_poweroff_time);
	DbgInfo(MODULE_AUTOTIMER, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Autodmt=%d time=%dms bhrb=%d\n",
		pdx->auto_timer.auto_dmt_enable, pdx->auto_timer.auto_dmt_time,
		pdx->auto_timer.enable_hibernate);
	DbgInfo(MODULE_AUTOTIMER, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"AutoStopInf enable=%d time=%dms\n",
		pdx->auto_timer.auto_cmd12_enable,
		pdx->auto_timer.auto_cmd12_time);
}

/*
 * Function Name: func_timer_thread
 * Abstract: This Function is used to calculate timer tick
 *
 * Input:
 *	bht_dev_ext_t *pdx
 *
 *
 * Notes:
 *
 *        This function is called by thread to do real job
 */
void func_timer_thread(bht_dev_ext_t *pdx)
{
	int busy = 0;

	DbgInfo(MODULE_AUTOTIMER, FEATURE_TIMER_TRACE, 0, "Enter %s\n",
		__func__);

	if (pdx->auto_timer.enable == FALSE || pdx->auto_timer.cancel
	    || pdx->auto_timer.stop)
		goto clear;

	/* If card not workable don't use timer */
	if (pdx->card.card_present == FALSE)
		goto clear;
	if (pdx->card.card_type == CARD_NONE
	    || pdx->card.card_type == CARD_ERROR)
		goto clear;
	if (pdx->card.state == CARD_STATE_POWEROFF)
		goto clear;

	/* If rtd3 entered not call below function */

	if (pdx->auto_timer.auto_poweroff_enable) {
		if (pdx->auto_timer.auto_poweroff_tick >=
		    pdx->auto_timer.auto_poweroff_time) {
			DbgInfo(MODULE_AUTOTIMER, FEATURE_CARD_OPS, 0,
				"auto poweroff\n");
			if (busy == 0) {
				os_set_dev_busy(pdx);
				busy = 1;
			}
			card_power_off(&pdx->card, FALSE);
			autotimer_clear(pdx);
			goto next;
		}
	}

	if (pdx->auto_timer.auto_cmd12_enable && pdx->card.has_built_inf) {
		if (pdx->auto_timer.auto_cmd12_tick >=
		    pdx->auto_timer.auto_cmd12_time) {
			if (busy == 0) {
				os_set_dev_busy(pdx);
				busy = 1;
			}
			pdx->auto_timer.auto_cmd12_tick = 0;
			DbgInfo(MODULE_AUTOTIMER, FEATURE_CARD_OPS, 0,
				"auto stop infinite\n");
			card_stop_infinite(&pdx->card, TRUE, NULL);
		}
	}

	if (pdx->auto_timer.auto_dmt_enable && pdx->card.card_type == CARD_UHS2
	    && pdx->card.state == CARD_STATE_WORKING) {
		if (pdx->auto_timer.auto_dmt_tick >=
		    pdx->auto_timer.auto_dmt_time) {
			if (busy == 0) {
				os_set_dev_busy(pdx);
				busy = 1;
			}
			pdx->auto_timer.auto_dmt_tick = 0;
			DbgInfo(MODULE_AUTOTIMER, FEATURE_CARD_OPS, 0,
				"auto enter sleep\n");
			card_enter_sleep(&pdx->card, TRUE,
					 pdx->auto_timer.enable_hibernate);
		}
	}

	if (pdx->auto_timer.auto_led_off_enable && (pdx->host.led_on)) {
		if (pdx->auto_timer.auto_led_off_tick >=
		    pdx->auto_timer.auto_led_off_time) {
			if (busy == 0) {
				os_set_dev_busy(pdx);
				busy = 1;
			}
			DbgInfo(MODULE_AUTOTIMER, FEATURE_CARD_OPS, 0,
				"auto led off\n");
			pdx->auto_timer.auto_led_off_tick = 0;
			host_led_ctl(&pdx->host, FALSE);
		}
	}

next:
	if (busy)
		os_set_dev_idle(pdx);
	goto exit;

clear:
	autotimer_clear(pdx);

exit:
	DbgInfo(MODULE_AUTOTIMER, FEATURE_TIMER_TRACE, 0, "Exit %s\n",
		__func__);
}

void func_autotimer_stop(bht_dev_ext_t *pdx)
{
	if (pdx->auto_timer.enable == FALSE)
		return;
	DbgInfo(MODULE_AUTOTIMER, FEATURE_TIMER_TRACE, 0, "stop autotimer\n",
		__func__);
	pdx->auto_timer.stop = 1;
	os_stop_timer(pdx, &pdx->os, TIMER_AUTO);
	autotimer_clear(pdx);

}

void func_autotimer_start(bht_dev_ext_t *pdx)
{
	if (pdx->auto_timer.enable == FALSE || pdx->auto_timer.cancel)
		return;
	DbgInfo(MODULE_AUTOTIMER, FEATURE_TIMER_TRACE, 0, "start autotimer\n",
		__func__);
	autotimer_clear(pdx);
	pdx->auto_timer.stop = 0;
	os_start_timer(pdx, &pdx->os, TIMER_AUTO, AUTO_TIMER_TICK);
}

void func_autotimer_cancel(bht_dev_ext_t *pdx)
{
	if (pdx->auto_timer.enable == FALSE || pdx->auto_timer.cancel)
		return;
	DbgInfo(MODULE_AUTOTIMER, FEATURE_TIMER_TRACE, 0, "cancel autotimer\n",
		__func__);
	pdx->auto_timer.cancel = 1;
	os_cancel_timer(pdx, &pdx->os, TIMER_AUTO);
	autotimer_clear(pdx);
}
