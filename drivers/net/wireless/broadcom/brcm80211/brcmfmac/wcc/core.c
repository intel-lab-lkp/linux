// SPDX-License-Identifier: ISC
/*
 * Copyright (c) 2022 Broadcom Corporation
 */
#include <linux/errno.h>
#include <linux/types.h>
#include <core.h>
#include <bus.h>
#include <fwvid.h>
#include <fwil.h>

#include "vops.h"

static int brcmf_wcc_attach(struct brcmf_pub *drvr)
{
	pr_debug("%s: executing\n", __func__);
	return 0;
}

static void brcmf_wcc_detach(struct brcmf_pub *drvr)
{
	pr_debug("%s: executing\n", __func__);
}

static int brcmf_wcc_set_sae_pwd(struct brcmf_if *ifp,
				 struct cfg80211_crypto_settings *crypto)
{
	struct brcmf_pub *drvr = ifp->drvr;
	struct brcmf_wsec_pmk_le pmk;
	int err;

	memset(&pmk, 0, sizeof(pmk));

	/* pass pmk directly */
	pmk.key_len = cpu_to_le16(crypto->sae_pwd_len);
	pmk.flags = cpu_to_le16(BRCMF_WSEC_PASSPHRASE);
	memcpy(pmk.key, crypto->sae_pwd, crypto->sae_pwd_len);

	/* store psk in firmware */
	err = brcmf_fil_cmd_data_set(ifp, BRCMF_C_SET_WSEC_PMK,
				     &pmk, sizeof(pmk));
	if (err < 0)
		bphy_err(drvr, "failed to change PSK in firmware (len=%u)\n",
			 crypto->sae_pwd_len);

	return err;
}

const struct brcmf_fwvid_ops brcmf_wcc_ops = {
	.attach = brcmf_wcc_attach,
	.detach = brcmf_wcc_detach,
	.set_sae_password = brcmf_wcc_set_sae_pwd,
};
