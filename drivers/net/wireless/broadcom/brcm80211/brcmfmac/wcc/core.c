// SPDX-License-Identifier: ISC
/*
 * Copyright (c) 2022 Broadcom Corporation
 */
#include <linux/errno.h>
#include <linux/types.h>
#include <core.h>
#include <bus.h>
#include <fwvid.h>
#include <cfg80211.h>
#include <feature.h>
#include <fweh.h>

#include "vops.h"

/* Firmware events of the Apple firmware for its AWDL interface */
#define BRCMF_WCC_E_AWDL_AW		96
#define BRCMF_WCC_E_AWDL_ROLE		97
#define BRCMF_WCC_E_AWDL_EVENT		98
#define BRCMF_WCC_E_AWDL_STATS		150
#define BRCMF_WCC_E_SDB_STATE		152
#define BRCMF_WCC_E_LAST		213

static const struct brcmf_fweh_event_map brcmf_wcc_event_map = {
	.items = {
		{ BRCMF_E_AWDL_AW, BRCMF_WCC_E_AWDL_AW },
		{ BRCMF_E_AWDL_ROLE, BRCMF_WCC_E_AWDL_ROLE },
		{ BRCMF_E_AWDL_EVENT, BRCMF_WCC_E_AWDL_EVENT },
		{ BRCMF_E_AWDL_STATS, BRCMF_WCC_E_AWDL_STATS },
		{ BRCMF_E_SDB_STATE, BRCMF_WCC_E_SDB_STATE },
	},
	.n_items = 5
};

static int brcmf_wcc_set_sae_pwd(struct brcmf_if *ifp,
				 struct cfg80211_crypto_settings *crypto)
{
	return brcmf_set_wsec(ifp, crypto->sae_pwd, crypto->sae_pwd_len,
			      BRCMF_WSEC_PASSPHRASE);
}

static int brcmf_wcc_alloc_fweh_info(struct brcmf_pub *drvr)
{
	struct brcmf_fweh_info *fweh;

	fweh = kzalloc_flex(*fweh, evt_handler, BRCMF_WCC_E_LAST);
	if (!fweh)
		return -ENOMEM;

	fweh->num_event_codes = BRCMF_WCC_E_LAST;
	fweh->event_map = &brcmf_wcc_event_map;
	drvr->fweh = fweh;
	return 0;
}

/*
 * The AWDL interface reports these events, they go to user space. The
 * firmware runs AWDL with all of them enabled in the event mask.
 */
static void brcmf_wcc_register_event_handlers(struct brcmf_pub *drvr)
{
	static const enum brcmf_fweh_event_code events[] = {
		BRCMF_E_AWDL_AW, BRCMF_E_AWDL_ROLE, BRCMF_E_AWDL_EVENT,
		BRCMF_E_AWDL_STATS, BRCMF_E_SDB_STATE,
	};
	int i;

	if (!(drvr->feat_flags & BIT(BRCMF_FEAT_AWDL)))
		return;
	for (i = 0; i < ARRAY_SIZE(events); i++)
		brcmf_fweh_register(drvr, events[i], brcmf_awdl_fweh_event);
}

const struct brcmf_fwvid_ops brcmf_wcc_ops = {
	.set_sae_password = brcmf_wcc_set_sae_pwd,
	.alloc_fweh_info = brcmf_wcc_alloc_fweh_info,
	.register_event_handlers = brcmf_wcc_register_event_handlers,
};
