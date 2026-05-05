/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2015, Qualcomm Atheros, Inc.
 */

#ifndef AES_GMAC_H
#define AES_GMAC_H

#include <crypto/gcm.h>

#define GMAC_AAD_LEN	20
#define GMAC_NONCE_LEN	12

static inline int
ieee80211_aes_gmac_key_setup(struct aesgcm_ctx *ctx,
			     const u8 key[], size_t key_len)
{
	return aesgcm_expandkey(ctx, key, key_len, IEEE80211_GCMP_MIC_LEN);
}

int ieee80211_aes_gmac(struct aesgcm_ctx *ctx, const u8 *aad, u8 *nonce,
		       const u8 *data, size_t data_len, u8 *mic);

#endif /* AES_GMAC_H */
