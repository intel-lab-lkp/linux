/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2014-2015, Qualcomm Atheros, Inc.
 */

#ifndef AES_GCM_H
#define AES_GCM_H

#include <crypto/gcm.h>

#define GCM_AAD_LEN	32

static inline void ieee80211_aes_gcm_encrypt(struct aesgcm_ctx *ctx,
					     u8 *j_0, u8 *aad,  u8 *data,
					     size_t data_len, u8 *mic)
{
	aesgcm_encrypt(ctx, data, data, data_len,
		       aad + 2, be16_to_cpup((__be16 *)aad),
		       j_0, mic);
}

static inline bool ieee80211_aes_gcm_decrypt(struct aesgcm_ctx *ctx,
					     u8 *j_0, u8 *aad, u8 *data,
					     size_t data_len, u8 *mic)
{
	return aesgcm_decrypt(ctx, data, data, data_len,
			      aad + 2, be16_to_cpup((__be16 *)aad),
			      j_0, mic);
}

static inline int
ieee80211_aes_gcm_key_setup_encrypt(struct aesgcm_ctx *ctx,
				    const u8 key[], size_t key_len)
{
	return aesgcm_expandkey(ctx, key, key_len, IEEE80211_GCMP_MIC_LEN);
}

#endif /* AES_GCM_H */
