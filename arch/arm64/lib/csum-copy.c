// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fused copy + Internet checksum for arm64: general-purpose register path
 *
 * Copyright (C) 2026 Demian Shulhan <demyansh@gmail.com>
 *
 * Used for buffers too small to amortise kernel_neon_begin()/end(), and
 * whenever may_use_simd() is false (hardirq/NMI, or FPSIMD not usable).
 * See csum-copy.h for the contract.
 */

#include <linux/types.h>
#include <linux/unaligned.h>

#include "csum-copy.h"

/*
 * Ones' complement add: 64-bit add with end-around carry. Same result as
 * accumulate() in csum.c; written with a compare rather than __uint128_t
 * so that the carry chains of the four accumulators below stay independent.
 */
static __always_inline u64 csum_add64(u64 sum, u64 data)
{
	u64 res = sum + data;

	return res + (res < data);
}

__wsum __csum_partial_copy_scalar(const void *src, void *dst, int len)
{
	const u8 *s = src;
	u8 *d = dst;
	u64 sum0 = 0, sum1 = 0, sum2 = 0, sum3 = 0;

	/*
	 * 32 bytes per iteration into four independent accumulators. A single
	 * accumulator would serialise every word on both the register and the
	 * carry flag (one 64-bit word per cycle at best); with independent
	 * chains the loop becomes store-bound on out-of-order cores.
	 */
	while (len >= 32) {
		u64 a = get_unaligned((const u64 *)s);
		u64 b = get_unaligned((const u64 *)(s + 8));
		u64 c = get_unaligned((const u64 *)(s + 16));
		u64 e = get_unaligned((const u64 *)(s + 24));

		put_unaligned(a, (u64 *)d);
		put_unaligned(b, (u64 *)(d + 8));
		put_unaligned(c, (u64 *)(d + 16));
		put_unaligned(e, (u64 *)(d + 24));

		sum0 = csum_add64(sum0, a);
		sum1 = csum_add64(sum1, b);
		sum2 = csum_add64(sum2, c);
		sum3 = csum_add64(sum3, e);

		s += 32;
		d += 32;
		len -= 32;
	}

	if (len & 16) {
		u64 a = get_unaligned((const u64 *)s);
		u64 b = get_unaligned((const u64 *)(s + 8));

		put_unaligned(a, (u64 *)d);
		put_unaligned(b, (u64 *)(d + 8));
		sum0 = csum_add64(sum0, a);
		sum1 = csum_add64(sum1, b);
		s += 16;
		d += 16;
	}
	if (len & 8) {
		u64 a = get_unaligned((const u64 *)s);

		put_unaligned(a, (u64 *)d);
		sum2 = csum_add64(sum2, a);
		s += 8;
		d += 8;
	}
	if (len & 4) {
		u32 w = get_unaligned((const u32 *)s);

		put_unaligned(w, (u32 *)d);
		sum3 = csum_add64(sum3, w);
		s += 4;
		d += 4;
	}
	if (len & 2) {
		u16 h = get_unaligned((const u16 *)s);

		put_unaligned(h, (u16 *)d);
		sum0 = csum_add64(sum0, h);
		s += 2;
		d += 2;
	}
	if (len & 1) {
		u8 b = *s;

		*d = b;
		/* Zero-padded final word: the byte is its low half */
		sum1 = csum_add64(sum1, b);
	}

	sum0 = csum_add64(sum0, sum1);
	sum2 = csum_add64(sum2, sum3);
	return csum_fold64(csum_add64(sum0, sum2));
}
