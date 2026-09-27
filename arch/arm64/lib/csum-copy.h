/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fused copy + Internet checksum for arm64: internals shared between the
 * general-purpose register and NEON implementations and their dispatcher.
 *
 * Copyright (C) 2026 Demian Shulhan <demyansh@gmail.com>
 */
#ifndef __ARM64_LIB_CSUM_COPY_H
#define __ARM64_LIB_CSUM_COPY_H

#include <linux/build_bug.h>
#include <linux/types.h>

/*
 * Both implement
 *
 *	__wsum fn(const void *src, void *dst, int len)
 *
 * copying @len bytes from @src to @dst while accumulating the ones'
 * complement sum of the data, and return a 32-bit unfolded __wsum.
 *
 * The sum is taken over the data as laid out in memory, i.e. over 16-bit
 * little-endian words starting at @src (arm64 Linux is little-endian only:
 * CONFIG_CPU_BIG_ENDIAN depends on BROKEN), with a trailing odd byte
 * treated as the low half of a final zero-padded word. This is what
 * csum_partial() computes, so the result can be fed straight into
 * csum_block_add()/csum_fold().
 *
 * @src and @dst may have any alignment but must not overlap (memcpy() on
 * arm64 happens to be memmove()-safe; these routines are not). All loads
 * stay within [src, src + len) and all stores within [dst, dst + len).
 * Every byte of @dst is written from the very load that was summed, so the
 * returned value is the checksum of @dst even if @src is modified
 * concurrently.
 *
 * __csum_partial_copy_scalar() accepts any len >= 0.
 * __csum_partial_copy_neon() requires len >= CSUM_COPY_NEON_MIN_LEN_HW and
 * must be called between kernel_neon_begin() and kernel_neon_end().
 */
__wsum __csum_partial_copy_scalar(const void *src, void *dst, int len);
__wsum __csum_partial_copy_neon(const void *src, void *dst, int len);

/*
 * Smallest length __csum_partial_copy_neon() can handle: up to 15 bytes to
 * align @dst to 16 bytes, then one unconditional 64-byte main loop
 * iteration.
 */
#define CSUM_COPY_NEON_MIN_LEN_HW	(15 + 64)

/*
 * Smallest length for which the dispatcher uses NEON at all. The fused
 * scalar path is already a single pass over the data, so NEON only has to
 * beat it by the fixed cost of kernel_neon_begin()/kernel_neon_end(): a
 * local_bh_disable()/enable() pair plus, when the interrupted context's
 * FPSIMD (or SVE/SME, when live) state has not been saved yet, that save
 * and its deferred reload on return to userspace. Where the two paths
 * break even depends on the core: ~512 bytes on an Apple M-series core,
 * ~1.25 KiB on Neoverse-N1 (both with the FPSIMD state already saved).
 * 1024 keeps MTU-sized buffers on the NEON path everywhere while costing
 * at most ~10% against the scalar path in the 1024..1280 window on N1.
 * The threshold is deliberately a constant rather than calibrated at boot:
 * a boot-time measurement is noisy (cold caches, low initial clock, steal
 * time in VMs), would differ between the core types of a big.LITTLE
 * system and would make the kernel's behaviour vary from boot to boot,
 * for a gain of a few percent in a narrow length window. See the commit
 * message for numbers.
 */
#define CSUM_COPY_NEON_MIN_LEN		1024

static_assert(CSUM_COPY_NEON_MIN_LEN >= CSUM_COPY_NEON_MIN_LEN_HW);

/*
 * Fold a 64-bit ones' complement accumulator to a 32-bit __wsum with
 * end-around carry. The first add produces at most a 33-bit value, so the
 * second add cannot carry again.
 */
static __always_inline __wsum csum_fold64(u64 sum)
{
	sum = (sum & 0xffffffff) + (sum >> 32);
	sum = (sum & 0xffffffff) + (sum >> 32);
	return (__force __wsum)(u32)sum;
}

#endif /* __ARM64_LIB_CSUM_COPY_H */
