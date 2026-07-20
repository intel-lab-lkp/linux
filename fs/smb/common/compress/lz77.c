// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024-2026, SUSE LLC
 * Copyright (C) 2026 Namjae Jeon <linkinjeon@kernel.org>
 *
 * Authors: Enzo Matsumiya <ematsumiya@suse.de>
 *          Namjae Jeon <linkinjeon@kernel.org>
 *
 * Implementation of the LZ77 "plain" compression algorithm, as per MS-XCA spec.
 */
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/count_zeros.h>
#include <linux/unaligned.h>
#include <linux/module.h>
#include <linux/overflow.h>

#include "compress.h"

/*
 * Compression parameters.
 *
 * LZ77_MATCH_MAX_DIST:		Farthest back a match can be from current position (can be 1 - 8K).
 * LZ77_SKIP_TRIGGER:		ilog2 value for adaptive skipping, i.e. to progressively skip input
 *				bytes when we can't find matches.  Default is 4.
 *				Higher values (>0) will decrease compression time, but will result
 *				in worse compression ratio.  Lower values will give better
 *				compression ratio (more matches found), but will increase time.
 */
#define LZ77_MATCH_MAX_DIST	SZ_8K
#define LZ77_SKIP_TRIGGER	4

#define LZ77_FLAG_MAX		32

/**
 * lz77_encode_match() - Match encoding.
 * @dst:	compressed buffer
 * @nib:	pointer to an address in @dst
 * @dist:	match distance
 * @len:	match length
 *
 * Assumes all args were previously checked.
 *
 * Return: @dst advanced to new position
 *
 * Ref: MS-XCA 2.3.4 "Plain LZ77 Compression Algorithm Details" - "Processing"
 */
static __always_inline void *lz77_encode_match(void *dst, void **nib, u16 dist, u32 len)
{
	len -= 3;
	dist--;
	dist <<= 3;

	if (len < 7) {
		mem_write16(dst, dist + len);

		return dst + sizeof(u16);
	}

	dist |= 7;
	mem_write16(dst, dist);
	dst += sizeof(u16);
	len -= 7;

	if (!*nib) {
		mem_write8(dst, umin(len, 15));
		*nib = dst;
		dst++;
	} else {
		u8 *b = *nib;

		mem_write8(b, *b | umin(len, 15) << 4);
		*nib = NULL;
	}

	if (len < 15)
		return dst;

	len -= 15;
	if (len < 255) {
		mem_write8(dst, len);

		return dst + 1;
	}

	mem_write8(dst, 0xff);
	dst++;
	len += 7 + 15;
	if (len <= 0xffff) {
		mem_write16(dst, len);

		return dst + sizeof(u16);
	}

	mem_write16(dst, 0);
	dst += sizeof(u16);
	mem_write32(dst, len);

	return dst + sizeof(u32);
}

/**
 * lz77_encode_literals() - Literals encoding.
 * @start:	where to start copying literals (uncompressed buffer)
 * @end:	when to stop copying (uncompressed buffer)
 * @dst:	compressed buffer
 * @f:		pointer to current flag value
 * @fc:		pointer to current flag count
 * @fp:		pointer to current flag address
 *
 * Batch copy literals from @start to @dst, updating flag values accordingly.
 * Assumes all args were previously checked.
 *
 * Return: @dst advanced to new position
 *
 * MS-XCA 2.3.4 "Plain LZ77 Compression Algorithm Details" - "Processing"
 */
static __always_inline void *lz77_encode_literals(const void *start, const void *end, void *dst,
						  long *f, u32 *fc, void **fp)
{
	if (start >= end)
		return dst;

	do {
		const u32 len = umin(end - start, LZ77_FLAG_MAX - *fc);

		memcpy(dst, start, len);

		dst += len;
		start += len;

		*f <<= len;
		*fc += len;
		if (*fc == LZ77_FLAG_MAX) {
			mem_write32(*fp, *f);
			*fc = 0;
			*fp = dst;
			dst += sizeof(u32);
		}
	} while (start < end);

	return dst;
}

noinline int smb_lz77_compress(const void *src, const u32 slen, void *dst, u32 *dlen)
{
	const void *srcp, *rlim, *end, *anchor;
	u32 *htable, hash, flag_count = 0;
	void *dstp, *nib, *flag_pos;
	long flag = 0;

	/* This is probably a bug, so throw a warning. */
	if (WARN_ON_ONCE(*dlen < smb_lz77_compressed_alloc_size(slen)))
		return -EINVAL;

	srcp = src;
	anchor = src;
	end = srcp + slen; /* absolute end */
	rlim = end - SMB_COMPRESS_MSTEP_SIZE; /* read limit (for mem_match_len()) */
	dstp = dst;
	flag_pos = dstp;
	dstp += sizeof(u32);
	nib = NULL;

	htable = kvcalloc(SMB_COMPRESS_HASH_SIZE, sizeof(*htable), GFP_KERNEL);
	if (!htable)
		return -ENOMEM;

	MEM_PREFETCH(srcp + SMB_COMPRESS_RSTEP_SIZE);

	/*
	 * Adjust @srcp so we don't get a false positive match on first iteration.
	 * Then prepare hash for first loop iteration (don't advance @srcp again).
	 */
	hash = smb_compress_hash_ptr(srcp++);
	htable[hash] = 0;
	hash = smb_compress_hash_ptr(srcp);

	/*
	 * Main loop.
	 *
	 * @dlen is >= smb_lz77_compressed_alloc_size(), so run without
	 * bound-checking @dstp.
	 *
	 * This code was crafted in a way to best utilise fetch-decode-execute CPU flow.
	 * Any attempt to optimize it, or even organize it, can lead to huge performance loss.
	 */
	do {
		const void *match, *next = srcp;
		u32 len, step = 1, skip = 1U << LZ77_SKIP_TRIGGER;

		/* Match finding (hot path -- don't change the read/check/write order). */
		do {
			const u32 cur_hash = hash;

			srcp = next;
			next += step;

			/*
			 * Adaptive skipping.
			 *
			 * Increment @step every (1 << LZ77_SKIP_TRIGGER, 16 in our case) bytes
			 * without a match.
			 * Reset to 1 when a match is found.
			 */
			step = (skip++ >> LZ77_SKIP_TRIGGER);
			if (unlikely(next > rlim))
				goto out;

			hash = smb_compress_hash_ptr(next);
			match = src + htable[cur_hash];
			htable[cur_hash] = srcp - src;
		} while (likely(match + LZ77_MATCH_MAX_DIST < srcp) ||
			 mem_read32(match) != mem_read32(srcp));

		/*
		 * Match found.  Warm/cold path; begin parsing @srcp and writing to @dstp:
		 * - flush literals
		 * - compute match length (*)
		 * - encode match
		 *
		 * (*) Current minimum match length is defined by the memory read size above, so
		 * here we already know that we have 4 matching bytes, but it's just faster to
		 * redundantly compute it again in lz77_match_len() than to adjust pointers/len.
		 */
		dstp = lz77_encode_literals(anchor, srcp, dstp, &flag, &flag_count, &flag_pos);
		len = mem_match_len(match, srcp, end);
		dstp = lz77_encode_match(dstp, &nib, srcp - match, len);
		srcp += len;
		anchor = srcp;

		MEM_PREFETCH(srcp);

		flag = (flag << 1) | 1;
		flag_count++;
		if (flag_count == LZ77_FLAG_MAX) {
			mem_write32(flag_pos, flag);
			flag_count = 0;
			flag_pos = dstp;
			dstp += sizeof(u32);
		}

		if (unlikely(srcp > rlim))
			break;

		/* Prepare for next loop. */
		hash = smb_compress_hash_ptr(srcp);
	} while (srcp < end);
out:
	dstp = lz77_encode_literals(anchor, end, dstp, &flag, &flag_count, &flag_pos);
	flag_count = LZ77_FLAG_MAX - flag_count;
	flag <<= flag_count;
	flag |= (1UL << flag_count) - 1;
	mem_write32(flag_pos, flag);

	/* Compression is successful from our POV -- let caller decide if @dlen suits them. */
	*dlen = dstp - dst;
	kvfree(htable);

	return 0;
}
EXPORT_SYMBOL_GPL(smb_lz77_compress);

static int lz77_decode_match_len(const u8 **src, const u8 *end, u16 token,
				 u8 *nibble, bool *have_nibble, u32 *len)
{
	u8 extra;

	*len = (token & 0x7) + 3;
	if ((token & 0x7) != 0x7)
		return 0;

	if (!*have_nibble) {
		if (*src >= end)
			return -EINVAL;
		*nibble = *(*src)++;
		extra = *nibble & 0xf;
		*have_nibble = true;
	} else {
		extra = *nibble >> 4;
		*have_nibble = false;
	}

	*len += extra;
	if (extra == 0xf) {
		u8 b;

		if (*src >= end)
			return -EINVAL;
		b = *(*src)++;
		if (b != 0xff) {
			*len += b;
		} else {
			u16 w;

			if (end - *src < 2)
				return -EINVAL;
			w = mem_read16(*src);
			*src += 2;
			if (w) {
				*len = w + 3;
			} else {
				u32 long_len;

				if (end - *src < 4)
					return -EINVAL;
				long_len = mem_read32(*src);
				*src += 4;
				if (check_add_overflow(long_len, 3, len))
					return -EINVAL;
			}
		}
	}

	return 0;
}

int smb_lz77_decompress(const void *src, const u32 slen, void *dst,
			const u32 dlen)
{
	const u8 *sp = src, *send = sp + slen;
	u8 *dp = dst, *dend = dp + dlen;
	u32 flags = 0;
	int flag_count = 0;
	u8 nibble = 0;
	bool have_nibble = false;

	while (dp < dend) {
		u32 len, dist;
		u16 token;

		if (!flag_count) {
			if (send - sp < 4)
				return -EINVAL;
			flags = mem_read32(sp);
			sp += 4;
			flag_count = 32;
		}

		if (!(flags & 0x80000000)) {
			if (sp >= send)
				return -EINVAL;
			*dp++ = *sp++;
			flags <<= 1;
			flag_count--;
			continue;
		}

		flags <<= 1;
		flag_count--;

		if (send - sp < 2)
			return -EINVAL;

		token = mem_read16(sp);
		sp += 2;

		dist = (token >> 3) + 1;
		if (dist > dp - (u8 *)dst)
			return -EINVAL;

		if (lz77_decode_match_len(&sp, send, token, &nibble,
					  &have_nibble, &len))
			return -EINVAL;

		if (len > dend - dp)
			return -EINVAL;

		while (len--) {
			*dp = *(dp - dist);
			dp++;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(smb_lz77_decompress);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SMB plain LZ77 compression");
