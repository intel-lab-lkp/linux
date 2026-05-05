// SPDX-License-Identifier: GPL-2.0-only
/*
 * AES-GMAC for IEEE 802.11 BIP-GMAC-128 and BIP-GMAC-256
 * Copyright 2015, Qualcomm Atheros, Inc.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/err.h>
#include <crypto/gf128hash.h>
#include <crypto/utils.h>

#include <net/mac80211.h>
#include "key.h"
#include "aes_gmac.h"

int ieee80211_aes_gmac(struct aesgcm_ctx *ctx, const u8 *aad, u8 *nonce,
		       const u8 *data, size_t data_len, u8 *mic)
{
	static const u8 zero[GHASH_BLOCK_SIZE];
	struct ghash_ctx ghash;
	u8 iv[AES_BLOCK_SIZE];
	size_t total_len = GMAC_AAD_LEN + data_len;
	__be64 tail[2] = {
		cpu_to_be64((u64)total_len * 8),
		0, /* no data since it's just GMAC */
	};
	u8 ghash_out[AES_BLOCK_SIZE];
	u8 enc_ctr[AES_BLOCK_SIZE];
	const __le16 *fc;

	if (data_len < IEEE80211_GMAC_MIC_LEN)
		return -EINVAL;

	ghash_init(&ghash, &ctx->ghash_key);

	ghash_update(&ghash, aad, GMAC_AAD_LEN);

	fc = (const __le16 *)aad;
	if (ieee80211_is_beacon(*fc)) {
		/* mask Timestamp field to zero */
		ghash_update(&ghash, zero, 8);
		ghash_update(&ghash, data + 8, data_len - 8 - IEEE80211_GMAC_MIC_LEN);
	} else {
		ghash_update(&ghash, data, data_len - IEEE80211_GMAC_MIC_LEN);
	}

	/* set MIC value to zero */
	ghash_update(&ghash, zero, IEEE80211_GMAC_MIC_LEN);
	/* pad */
	ghash_update(&ghash, zero, -total_len & (GHASH_BLOCK_SIZE - 1));

	ghash_update(&ghash, (const u8 *)&tail, sizeof(tail));

	ghash_final(&ghash, ghash_out);

	memcpy(iv, nonce, GMAC_NONCE_LEN);
	memset(iv + GMAC_NONCE_LEN, 0, sizeof(iv) - GMAC_NONCE_LEN);
	iv[AES_BLOCK_SIZE - 1] = 0x01;

	aes_encrypt(&ctx->aes_key, enc_ctr, (const u8 *)iv);
	crypto_xor_cpy(mic, ghash_out, enc_ctr, IEEE80211_GMAC_MIC_LEN);

	memzero_explicit(ghash_out, sizeof(ghash_out));
	memzero_explicit(enc_ctr, sizeof(enc_ctr));

	return 0;
}
