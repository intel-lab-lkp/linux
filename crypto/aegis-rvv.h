/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright 2026 Institute of Software, CAS
 */

#ifndef _AEGIS_RVV_H
#define _AEGIS_RVV_H

extern const u8 crypto_aes_sbox[];

void crypto_aegis128_init_rvv(void *state, const void *key, const void *iv);
void crypto_aegis128_update_rvv(void *state, const void *msg);
void crypto_aegis128_encrypt_chunk_rvv(void *state, void *dst, const void *src,
					unsigned int size);
void crypto_aegis128_decrypt_chunk_rvv(void *state, void *dst, const void *src,
					unsigned int size);
int crypto_aegis128_final_rvv(void *state, void *tag_xor,
			       unsigned int assoclen,
			       unsigned int cryptlen,
			       unsigned int authsize);
#endif
