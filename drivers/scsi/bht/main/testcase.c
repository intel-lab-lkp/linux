// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 BHT Inc.
 *
 * File Name: testcase.c
 *
 * Abstract: This source file used to implement testcase interface
 *
 * Version: 1.00
 *
 * Author: Peter.Guo
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 1/13/2015		Creation	Peter.Guo
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
#include "../card/cardcommon.h"
#include "../include/hostvenapi.h"

typedef struct {
	bool (*test_prepare)(bht_dev_ext_t *pdx);
	bool (*test_execute)(bht_dev_ext_t *pdx);
	char name[24];
} test_func_t;

/*
 * This function is used to init card
 */
static bool test_init_card(bht_dev_ext_t *pdx)
{
	bool result = FALSE;
	sd_card_t *card = &pdx->card;

	DbgInfo(MODULE_TEST, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	card->card_chg = FALSE;
#if CFG_OS_LINUX
	os_sleep(200);
#else
	os_sleep((PVOID) pdx, 200);

#endif
	/* Init card here   */
	result = card_init(card, 10, FALSE);

	DbgInfo(MODULE_TEST, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s ret=%d\n",
		__func__, result);
	return result;
}

/* ---------------UHS2 Test Case--------------- */
static bool test_uhs2_dmt(bht_dev_ext_t *pdx)
{
	sd_card_t *card = &pdx->card;
	sd_command_t sd_cmd;
	bool hbr = (pdx->testcase.test_param1 == 0) ? 0 : 1;

	if (card->uhs2_info.uhs2_cap.hibernate == 0)
		hbr = 0;

	if (card->card_type != CARD_UHS2 || card->card_present == FALSE) {
		DbgErr("Test uhs2 dmt failed for not uhs2 card\n");
		return FALSE;
	}

	if (uhs2_enter_dmt(card, &sd_cmd, card->host, hbr) == FALSE)
		return FALSE;

	if (uhs2_resume_dmt(card, &sd_cmd, card->host, hbr) == FALSE)
		return FALSE;

	return TRUE;
}

static bool test_uhs2_fullreset(bht_dev_ext_t *pdx)
{
	sd_card_t *card = &pdx->card;

	if (card->card_type != CARD_UHS2 || card->card_present == FALSE) {
		DbgErr("Test uhs2 fullreset failed for not uhs2 card\n");
		return FALSE;
	}

	if (uhs2_full_reset_card(card) == FALSE)
		return FALSE;

	return card_init(card, 1, TRUE);
}

test_func_t test_with_card_array[2] = {
	/* TEST CASE1 */
	{ test_init_card, test_uhs2_dmt, "uhs2dmt" },
	/* TEST CASE2 */
	{ test_init_card, test_uhs2_fullreset, "uhs2fullreset" },

};

test_func_t test_host_only_array[1] = {
	{ 0, 0, "nulltest" }

};

void testcase_main(bht_dev_ext_t *pdx, byte type)
{
	u32 id = pdx->testcase.test_id;
	u32 loop = 0;
	u32 testloop = pdx->testcase.test_loop;
	bool binfinite = (testloop) ? 0 : 1;
	test_func_t *test = NULL;

	if (pdx->card.card_present == 0) {
		host_init(&pdx->host);
		card_stuct_uinit(&pdx->card);
		PrintMsg("TEST remove card\n");
		return;
	}

	if (type != pdx->testcase.test_type)
		return;

	if (type == 1)
		test = &test_with_card_array[id - 1];
	else
		test = &test_host_only_array[id - 1];

	if (test == NULL || test->test_prepare == NULL) {
		DbgErr("Test(%d) prepare is null\n", id);
		return;
	}

	if (test->test_prepare(pdx) == FALSE) {
		DbgErr("Test(%s) prepare failed\n", test->name);
		return;
	}

	PrintMsg("Test(%s) type=%d begin infinite=%d totalloop=%d\n",
		 test->name, type, binfinite, pdx->testcase.test_loop);

	while (os_thread_is_freeze(pdx) == FALSE) {
		loop++;
		PrintMsg("Test(%s) loop=%d\n", test->name, loop);
		if (test->test_execute(pdx) == FALSE) {
			DbgErr("Test(%s) excute failed loop=%d\n", test->name,
			       loop);
			break;
		}

		if (binfinite == 0) {
			testloop--;
			if (testloop == 0)
				break;
		}
	}

	PrintMsg("Test(%s) loop=%d end\n", test->name, loop);
	host_poweroff(&pdx->host, 0);
}

void testcase_init(bht_dev_ext_t *pdx)
{
	if (pdx->testcase.test_type == 0)
		return;

	pdx->testcase.test_id = pdx->cfg->test_item.test_id;
	pdx->testcase.test_loop = pdx->cfg->test_item.test_loop;
	pdx->testcase.test_param1 = pdx->cfg->test_item.test_param1;
	pdx->testcase.test_param2 = pdx->cfg->test_item.test_param2;
	PrintMsg("testid=%d param1=0x%08X param2=0x%08X\n",
		 pdx->testcase.test_id, pdx->testcase.test_param1,
		 pdx->testcase.test_param2);

	/* If test id is out of range */
	if (pdx->testcase.test_id == 0)
		pdx->testcase.test_type = 0;

	/* card test */
	if (pdx->testcase.test_type == 1) {
		if (pdx->testcase.test_id >
		    sizeof(test_with_card_array) / sizeof(test_func_t))
			pdx->testcase.test_type = 0;

		if (pdx->card.card_present) {
#if CFG_OS_LINUX
			os_set_event(&pdx->os, EVENT_CARD_CHG);
#else
			os_set_event(pdx, &pdx->os, EVENT_TASK_OCCUR,
				     TASK_CARD_CHG);
#endif
		}
	}
	/* host only test */
	else if (pdx->testcase.test_type == 2) {
		if (pdx->testcase.test_id >
		    sizeof(test_host_only_array) / sizeof(test_func_t))
			pdx->testcase.test_type = 0;
		else {
			/* todo  insert test event */
		}
	}
}
