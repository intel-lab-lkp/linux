// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cryptographic API.
 *
 * SHA1 Secure Hash Algorithm.
 *
 * Derived from cryptoapi implementation, adapted for in-place
 * scatterlist interface.
 *
 * Copyright (c) Alan Smithee.
 * Copyright (c) Andrew McDonald <andrew@mcdonald.org.uk>
 * Copyright (c) Jean-Francois Dive <jef@linuxbe.org>
 */
#include <linux/types.h>
#include <linux/string.h>
#include <asm/byteorder.h>

#include "sha1_base.h"

static void sha1_generic_block_fn(struct sha1_state *sst, u8 const *src,
				  int blocks)
{
	u32 temp[SHA1_WORKSPACE_WORDS];

	while (blocks--) {
		sha1_transform(sst->state, (const char *)src, temp);
		src += SHA1_BLOCK_SIZE;
	}
	memzero_explicit(temp, sizeof(temp));
}

int crypto_sha1_update(struct sha1_state *desc, const u8 *data,
		       unsigned int len)
{
	return sha1_base_do_update(desc, data, len, sha1_generic_block_fn);
}

static int sha1_final(struct sha1_state *desc, u8 *out)
{
	sha1_base_do_finalize(desc, sha1_generic_block_fn);
	return sha1_base_finish(desc, out);
}

int crypto_sha1_finup(struct sha1_state *desc, const u8 *data,
		      unsigned int len, u8 *out)
{
	sha1_base_do_update(desc, data, len, sha1_generic_block_fn);
	return sha1_final(desc, out);
}
