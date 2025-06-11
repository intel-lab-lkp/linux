// SPDX-License-Identifier: GPL-2.0
/*
 * OpenSSL/Cryptogams accelerated Poly1305 transform for riscv
 *
 * Copyright (C) 2025 Institute of Software, CAS.
 */

#include <asm/hwcap.h>
#include <asm/simd.h>
#include <crypto/internal/poly1305.h>
#include <linux/cpufeature.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/unaligned.h>

asmlinkage void poly1305_block_init_arch(
	struct poly1305_block_state *state,
	const u8 raw_key[POLY1305_BLOCK_SIZE]);
EXPORT_SYMBOL_GPL(poly1305_block_init_arch);
asmlinkage void poly1305_blocks_arch(struct poly1305_block_state *state,
				const u8 *src, u32 len, u32 hibit);
EXPORT_SYMBOL_GPL(poly1305_blocks_arch);
asmlinkage void poly1305_emit_arch(const struct poly1305_state *state,
				   u8 digest[POLY1305_DIGEST_SIZE],
				   const u32 nonce[4]);
EXPORT_SYMBOL_GPL(poly1305_emit_arch);

bool poly1305_is_arch_optimized(void)
{
	/* We always can use since only Integer Multiplication extension is used. */
	return true;
}
EXPORT_SYMBOL(poly1305_is_arch_optimized);

MODULE_DESCRIPTION("Poly1305 authenticator (RISC-V accelerated)");
MODULE_LICENSE("GPL");