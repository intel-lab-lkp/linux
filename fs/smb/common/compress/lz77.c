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

static __always_inline const void *lz77_decode_match_len(const void *src, const void *end,
							 const void **nib, u32 *len)
{
	u32 mlen = *len & 7;

	/*
	 * *@len points to the initial match length decoded.
	 * We'll keep checking + decoding further if extra bits/bytes were used to encode larger
	 * lengths.
	 *
	 * Since we're reading from @src itself, this means any OOB read is an error
	 * (bug, malformed payload, etc).
	 */
	if (mlen == 7) {
		if (!*nib) {
			*nib = src;
			if (unlikely(src >= end))
				return NULL;

			mlen = mem_read8(src) & 15;
			src++;
		} else {
			mlen = mem_read8(*nib) >> 4;
			*nib = NULL;
		}

		if (mlen == 15) {
			if (unlikely(src >= end))
				return NULL;

			mlen = mem_read8(src);
			src++;

			if (mlen == 255) {
				if (unlikely(src + sizeof(u16) > end))
					return NULL;

				mlen = mem_read16(src);
				src += sizeof(u16);

				if (mlen == 0) {
					if (unlikely(src + sizeof(u32) > end))
						return NULL;

					mlen = mem_read32(src);
					src += sizeof(u32);
				}

				/* Unexpected match len < 15 + 7 (decoding bug) */
				if (unlikely(mlen < 23))
					return NULL;

				mlen -= (15 + 7);
			}
			mlen += 15;
		}
		mlen += 7;
	}
	mlen += 3;
	*len = mlen;

	return src;
}

/* @dlen is expected to be the _exact_ decompressed size of this payload, regardless of chaining */
noinline int smb_lz77_decompress(const void *src, const u32 slen, void *dst, const u32 dlen)
{
	const void *srcp = src, *end = src + slen, *nib = NULL;
	void *dstp = dst, *dst_end = dst + dlen;

	while (srcp + SMB_COMPRESS_RSTEP_SIZE <= end) {
		u32 flag, flag_count = LZ77_FLAG_MAX;

		/*
		 * Read flag.
		 *
		 * LZ77 flags are 32-bit bitmaps where 0s indicates a literal in the stream
		 * (straight copied from @srcp to @dstp) and 1s indicates a match (decoded from
		 * @srcp).
		 *
		 * Compressed payloads always starts with a flag.
		 */
		flag = mem_read32(srcp);
		srcp += SMB_COMPRESS_RSTEP_SIZE;

		do {
			u32 m = 0, l;

			/*
			 * Decode flag.
			 *
			 * Each bit in @flag represents a literal (0) or a match (1).
			 * Instead of processing them as individual bits, do it in batches:
			 *   (@m matches, @l literals)
			 *
			 * Count leading zeroes for that (flip @flag bits to compute matches).
			 *
			 * Notes:
			 * - @flag == 0 means we're are bound by @flag_count literals
			 * - __builtin_clz() yields UB if arg is 0
			 * - we can't rely on bit counting alone as bound-checking because the
			 *   final flag in compressed payload might contain lots of 1s
			 *   (((1 << (32 - @flag_count)) - 1), cf. smb_lz77_compress()).
			 *
			 * Also, matches can be encoded from 2 up to 10 bytes each, and since we
			 * don't know the size of each match beforehand, we can't determine a
			 * fixed limit, so we have to check @srcp limits before each match read.
			 */
			if (flag) {
				m = flag < 0xFFFFFFFF ? __builtin_clz(~flag) : LZ77_FLAG_MAX;
				flag_count -= m;
				if (m < 32)
					flag <<= m;
				else
					flag = 0;
			}

			l = flag ? umin(__builtin_clz(flag), flag_count) : flag_count;
			flag_count -= l;
			if (l < 32)
				flag <<= l;
			else
				flag = 0;

			/* Decoding bug (or, unlikely, __builtin_clz() bug) */
			if (WARN_ON_ONCE(l + m > LZ77_FLAG_MAX))
				return -EIO;

			while (m--) {
				const void *match;
				u32 dist, len;

				/*
				 * Final flag done (not a bug).
				 *
				 * Note that even if we reached here (@m wasn't 0), we can't rely
				 * on that value alone as a "true match flag counter" because of
				 * how the last flag is encoded (cf. smb_lz77_compress()).
				 */
				if (unlikely(srcp + sizeof(u16) > end)) {
					/* unexpected truncated input */
					if (unlikely(srcp < end))
						return -EFAULT;
					goto out;
				}

				/* Store match symbol in @len */
				len = mem_read16(srcp);
				srcp += sizeof(u16);
				dist = (len >> 3) + 1;
				srcp = lz77_decode_match_len(srcp, end, &nib, &len);
				if (unlikely(!srcp))
					return -EFAULT;

				/*
				 * Check bogus match values.
				 *
				 * We don't know what compression parameters (e.g. match max dist,
				 * min len) the server is using, so check against limits allowed
				 * by spec.
				 *
				 * Also check if within @dst boundaries so we can do a straight
				 * copy.
				 */
				if (unlikely(!dist || dist > SZ_8K || dstp - dst < dist))
					return -EFAULT;

				if (unlikely(len < 3 || len == U32_MAX || dst_end - dstp < len))
					return -EFAULT;

				/*
				 * If non-overlapping memory, we can use memcpy() (common case).
				 * Otherwise, we have to do it byte by byte.
				 *
				 * Note @match is always behind @dstp (@dist is at least 1).
				 */
				match = dstp - dist;
				if (likely(len < dist)) {
					memcpy(dstp, match, len);
					dstp += len;
				} else {
					while (len--)
						mem_write8(dstp++, mem_read8(match++));
				}
			}

			if (l) {
				if (unlikely(end - srcp < l || dst_end - dstp < l))
					return -EFAULT;

				memcpy(dstp, srcp, l);
				srcp += l;
				dstp += l;
			}
		} while (flag_count);
	}
out:
	/*
	 * @dstp > @dst_end is an OOB write (and should've been caught in the loop above).
	 * @dstp < @dst_end, without any other decoding errors, might be:
	 * - caller bug (wrong input arguments, e.g. wrong @src or @dlen)
	 * - decoding bug (compressed buffer decodes fine (passes all checks), but parsed a bogus
	 *   length value)
	 */
	if (WARN_ON_ONCE(dstp != dst_end))
		return -EIO;

	/*
	 * We've now fully parsed the compressed buffer without any processing errors.
	 * However, it's up to callers to determine the validity of @dst.
	 */
	return 0;
}
EXPORT_SYMBOL_GPL(smb_lz77_decompress);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SMB plain LZ77 compression");
