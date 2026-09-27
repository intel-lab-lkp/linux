// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2019-2020 Arm Ltd.

#include <linux/compiler.h>
#include <linux/kasan-checks.h>
#include <linux/kernel.h>

#include <asm/simd.h>

#include <net/checksum.h>

#include "csum-copy.h"

/* Looks dumb, but generates nice-ish code */
static u64 accumulate(u64 sum, u64 data)
{
	__uint128_t tmp = (__uint128_t)sum + data;
	return tmp + (tmp >> 64);
}

/*
 * We over-read the buffer and this makes KASAN unhappy. Instead, disable
 * instrumentation and call kasan explicitly.
 */
unsigned int __no_sanitize_address do_csum(const unsigned char *buff, int len)
{
	unsigned int offset, shift, sum;
	const u64 *ptr;
	u64 data, sum64 = 0;

	if (unlikely(len <= 0))
		return 0;

	offset = (unsigned long)buff & 7;
	/*
	 * This is to all intents and purposes safe, since rounding down cannot
	 * result in a different page or cache line being accessed, and @buff
	 * should absolutely not be pointing to anything read-sensitive. We do,
	 * however, have to be careful not to piss off KASAN, which means using
	 * unchecked reads to accommodate the head and tail, for which we'll
	 * compensate with an explicit check up-front.
	 */
	kasan_check_read(buff, len);
	ptr = (u64 *)(buff - offset);
	len = len + offset - 8;

	/*
	 * Head: zero out any excess leading bytes. Shifting back by the same
	 * amount should be at least as fast as any other way of handling the
	 * odd/even alignment, and means we can ignore it until the very end.
	 */
	shift = offset * 8;
	data = *ptr++;
#ifdef __LITTLE_ENDIAN
	data = (data >> shift) << shift;
#else
	data = (data << shift) >> shift;
#endif

	/*
	 * Body: straightforward aligned loads from here on (the paired loads
	 * underlying the quadword type still only need dword alignment). The
	 * main loop strictly excludes the tail, so the second loop will always
	 * run at least once.
	 */
	while (unlikely(len > 64)) {
		__uint128_t tmp1, tmp2, tmp3, tmp4;

		tmp1 = *(__uint128_t *)ptr;
		tmp2 = *(__uint128_t *)(ptr + 2);
		tmp3 = *(__uint128_t *)(ptr + 4);
		tmp4 = *(__uint128_t *)(ptr + 6);

		len -= 64;
		ptr += 8;

		/* This is the "don't dump the carry flag into a GPR" idiom */
		tmp1 += (tmp1 >> 64) | (tmp1 << 64);
		tmp2 += (tmp2 >> 64) | (tmp2 << 64);
		tmp3 += (tmp3 >> 64) | (tmp3 << 64);
		tmp4 += (tmp4 >> 64) | (tmp4 << 64);
		tmp1 = ((tmp1 >> 64) << 64) | (tmp2 >> 64);
		tmp1 += (tmp1 >> 64) | (tmp1 << 64);
		tmp3 = ((tmp3 >> 64) << 64) | (tmp4 >> 64);
		tmp3 += (tmp3 >> 64) | (tmp3 << 64);
		tmp1 = ((tmp1 >> 64) << 64) | (tmp3 >> 64);
		tmp1 += (tmp1 >> 64) | (tmp1 << 64);
		tmp1 = ((tmp1 >> 64) << 64) | sum64;
		tmp1 += (tmp1 >> 64) | (tmp1 << 64);
		sum64 = tmp1 >> 64;
	}
	while (len > 8) {
		__uint128_t tmp;

		sum64 = accumulate(sum64, data);
		tmp = *(__uint128_t *)ptr;

		len -= 16;
		ptr += 2;

#ifdef __LITTLE_ENDIAN
		data = tmp >> 64;
		sum64 = accumulate(sum64, tmp);
#else
		data = tmp;
		sum64 = accumulate(sum64, tmp >> 64);
#endif
	}
	if (len > 0) {
		sum64 = accumulate(sum64, data);
		data = *ptr;
		len -= 8;
	}
	/*
	 * Tail: zero any over-read bytes similarly to the head, again
	 * preserving odd/even alignment.
	 */
	shift = len * -8;
#ifdef __LITTLE_ENDIAN
	data = (data << shift) >> shift;
#else
	data = (data >> shift) << shift;
#endif
	sum64 = accumulate(sum64, data);

	/* Finally, folding */
	sum64 += (sum64 >> 32) | (sum64 << 32);
	sum = sum64 >> 32;
	sum += (sum >> 16) | (sum << 16);
	if (offset & 1)
		return (u16)swab32(sum);

	return sum >> 16;
}

/*
 * scoped_ksimd() puts the caller-provided FPSIMD state buffer (a 528-byte
 * struct user_fpsimd_state) on the stack. Keep it out of line so that the
 * common, short, scalar calls do not carry that frame, which matters for
 * the deep softirq call chains csum_partial_copy_nocheck() is reached from.
 */
static noinline __wsum csum_partial_copy_neon(const void *src, void *dst,
					      int len)
{
	__wsum sum;

	scoped_ksimd()
		sum = __csum_partial_copy_neon(src, dst, len);

	return sum;
}

/**
 * csum_partial_copy_nocheck - copy a buffer and compute its Internet checksum
 * @src: source buffer
 * @dst: destination buffer; must not overlap @src
 * @len: number of bytes to copy; nothing is copied for len <= 0
 *
 * Copies @len bytes from @src to @dst in a single pass and returns the
 * unfolded 32-bit ones' complement sum of the data, exactly as
 * csum_partial(@src, @len, 0) would, so the result can be passed to
 * csum_block_add() or csum_fold(). The returned sum is the checksum of the
 * bytes written to @dst even if @src is concurrently modified.
 *
 * Callable from task and softirq context. For len >= CSUM_COPY_NEON_MIN_LEN
 * the copy uses kernel-mode NEON when may_use_simd() allows it, so it must
 * not be called from within another kernel_neon_begin()/kernel_neon_end()
 * section in task context (kernel_neon_begin() only nests from softirq).
 * From hardirq/NMI context, or when FPSIMD is not usable, the general
 * purpose register implementation is used for any length.
 *
 * Return: the unfolded ones' complement sum of the copied data; 0 if len <= 0.
 */
__wsum csum_partial_copy_nocheck(const void *src, void *dst, int len)
{
	if (unlikely(len <= 0))
		return 0;

	if (len >= CSUM_COPY_NEON_MIN_LEN && likely(may_use_simd()))
		return csum_partial_copy_neon(src, dst, len);

	return __csum_partial_copy_scalar(src, dst, len);
}
EXPORT_SYMBOL(csum_partial_copy_nocheck);

__sum16 csum_ipv6_magic(const struct in6_addr *saddr,
			const struct in6_addr *daddr,
			__u32 len, __u8 proto, __wsum csum)
{
	__uint128_t src, dst;
	u64 sum = (__force u64)csum;

	src = *(const __uint128_t *)saddr->s6_addr;
	dst = *(const __uint128_t *)daddr->s6_addr;

	sum += (__force u32)htonl(len);
#ifdef __LITTLE_ENDIAN
	sum += (u32)proto << 24;
#else
	sum += proto;
#endif
	src += (src >> 64) | (src << 64);
	dst += (dst >> 64) | (dst << 64);

	sum = accumulate(sum, src >> 64);
	sum = accumulate(sum, dst >> 64);

	sum += ((sum >> 32) | (sum << 32));
	return csum_fold((__force __wsum)(sum >> 32));
}
EXPORT_SYMBOL(csum_ipv6_magic);
