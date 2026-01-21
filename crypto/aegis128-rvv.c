// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2026 Institute of Software, CAS
 * Author: Chunyan Zhang <zhangchunyan@iscas.ac.cn>
 */

#include <asm/vector.h>

#include "aegis.h"
#include "aegis-rvv.h"

bool crypto_aegis128_have_simd(void)
{
	return IS_ENABLED(CONFIG_RISCV_ISA_V);
}

void crypto_aegis128_init_simd(struct aegis_state *state,
			       const union aegis_block *key,
			       const u8 *iv)
{
	kernel_vector_begin();
	crypto_aegis128_init_rvv(state, key, iv);
	kernel_vector_end();
}

void crypto_aegis128_update_simd(struct aegis_state *state, const void *msg)
{
	kernel_vector_begin();
	crypto_aegis128_update_rvv(state, msg);
	kernel_vector_end();
}

void crypto_aegis128_encrypt_chunk_simd(struct aegis_state *state, u8 *dst,
					const u8 *src, unsigned int size)
{
	kernel_vector_begin();
	crypto_aegis128_encrypt_chunk_rvv(state, dst, src, size);
	kernel_vector_end();
}

void crypto_aegis128_decrypt_chunk_simd(struct aegis_state *state, u8 *dst,
					const u8 *src, unsigned int size)
{
	kernel_vector_begin();
	crypto_aegis128_decrypt_chunk_rvv(state, dst, src, size);
	kernel_vector_end();
}

int crypto_aegis128_final_simd(struct aegis_state *state,
			       union aegis_block *tag_xor,
			       unsigned int assoclen,
			       unsigned int cryptlen,
			       unsigned int authsize)
{
	int ret;

	kernel_vector_begin();
	ret = crypto_aegis128_final_rvv(state, tag_xor, assoclen, cryptlen,
					 authsize);
	kernel_vector_end();

	return ret;
}
