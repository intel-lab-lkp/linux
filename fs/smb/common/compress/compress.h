/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Namjae Jeon <linkinjeon@kernel.org>
 * Copyright (C) 2026, SUSE LLC, author: Enzo Matsumiya <ematsumiya@suse.de>
 *
 * Common helpers and definitions for SMB2 compression/decompression.
 *
 * Note that, as helpers, most assume their args were previously checked by callers.
 */
#ifndef _COMMON_SMB_COMPRESS_H
#define _COMMON_SMB_COMPRESS_H
#include <linux/count_zeros.h>
#include <linux/string.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "../smb2pdu.h"
#include "huffman.h"
#include "lz77.h"

#define SMB2_COMPRESSION_CHAINED_HDR_LEN \
	offsetof(struct smb2_compression_hdr, CompressionAlgorithm)
#define SMB2_COMPRESSION_PAYLOAD_BASE_LEN \
	(sizeof(struct smb2_compression_payload_hdr) - sizeof(__le32))
#define SMB_COMPRESS_MIN_LEN		PAGE_SIZE

/*
 * Memory ops helpers.
 */
#undef MEM_UNALIGNED_READ
#undef MEM_UNALIGNED_WRITE

/* Read prefetch */
#define MEM_PREFETCH(ptr)		__builtin_prefetch((ptr), 0, 3)

/*
 * x86 safely handles unaligned reads by pointer deref.
 * Use the unaligned helpers for other archs.
 */
#ifdef CONFIG_X86
# define MEM_UNALIGNED_READ(ptr, t)	(*(const t *)(ptr))
# define MEM_UNALIGNED_WRITE(ptr, v, t)	(*(t *)(ptr) = (t)(v))
#else
# include <linux/unaligned.h>
# define MEM_UNALIGNED_READ(ptr, t)	get_unaligned((const t *)(ptr))
# define MEM_UNALIGNED_WRITE(ptr, v, t)	put_unaligned((v), (t *)(ptr))
#endif /* !CONFIG_X86 */

#define mem_read8(ptr)			MEM_UNALIGNED_READ(ptr, u8)
#define mem_read16(ptr)			MEM_UNALIGNED_READ(ptr, u16)
#define mem_read32(ptr)			MEM_UNALIGNED_READ(ptr, u32)
#define mem_read64(ptr)			MEM_UNALIGNED_READ(ptr, u64)
#define mem_write8(ptr, v)		MEM_UNALIGNED_WRITE(ptr, v, u8)
#define mem_write16(ptr, v)		MEM_UNALIGNED_WRITE(ptr, v, u16)
#define mem_write32(ptr, v)		MEM_UNALIGNED_WRITE(ptr, v, u32)
/* mem_write64() not implemented -- not used anywhere yet */

/**
 * SMB_COMPRESS_RSTEP_SIZE:	Number of bytes to read from input buffer for hashing and initial
 *				match check (default 4 bytes).
 * SMB_COMPRESS_MSTEP_SIZE:	Number of bytes to extend-compare a found match (default 8 bytes).
 */
#define SMB_COMPRESS_RSTEP_SIZE	sizeof(u32)
#define SMB_COMPRESS_MSTEP_SIZE	sizeof(u64)

/**
 * mem_match_len() - Fast (batch) count matching bytes on a linear buffer.
 * @start: start of buffer
 * @head: current position on buffer
 * @end: end of buffer
 *
 * Compare 8 bytes of @start and @head until a mismatch is found or @head reaches @end.
 *
 * Requirements:
 * - no args can be NULL (must be asserted by caller)
 * - all args must point to the same allocated memory (must be asserted by caller)
 * - @start < @head + 8 <= end (asserted here)
 *
 * Return: number of matching bytes (0 if last requirement fails)
 */
static __always_inline size_t mem_match_len(const void *start, const void *head, const void *end)
{
	const void *cur = head;

	if (unlikely(start >= head || head + SMB_COMPRESS_MSTEP_SIZE > end))
		return 0;

	do {
		const u64 diff = mem_read64(head) ^ mem_read64(start);

		if (!diff) {
			head += SMB_COMPRESS_MSTEP_SIZE;
			start += SMB_COMPRESS_MSTEP_SIZE;

			continue;
		}

		/* This computes the number of common bytes in @diff. */
		head += count_trailing_zeros(diff) >> 3;

		return (head - cur);
	} while (likely(head + SMB_COMPRESS_MSTEP_SIZE <= end));

	/* Fallback to byte-by-byte comparison for last bytes (< SMB_COMPRESS_MSTEP_SIZE). */
	while (head < end && mem_read8(start) == mem_read8(head)) {
		head++;
		start++;
	}

	return (head - cur);
}

/*
 * Hashing parameters.
 * Same for all algorithms.
 *
 * XXX: these are fixed for now, might make them tunables in the future.
 */

/**
 * SMB_COMPRESS_HASH_LOG:	ilog2 hash size (recommended to be 13 - 18, default 15).
 * SMB_COMPRESS_HASH_SIZE:	Hashtable size (default is 32k (1 << SMB_COMPRESS_HASH_LOG))).
 */
#define SMB_COMPRESS_HASH_LOG	15
#define SMB_COMPRESS_HASH_SIZE	BIT(SMB_COMPRESS_HASH_LOG)

static __always_inline u32 smb_compress_hash(const u32 v)
{
	return ((v ^ 0x9E3779B9U) * 0x85EBCA6BU) >> (32 - SMB_COMPRESS_HASH_LOG);
}

static __always_inline u32 smb_compress_hash_ptr(const void *ptr)
{
	return smb_compress_hash(mem_read32(ptr));
}

/*
 * SMB3_COMPRESS_NONE is valid only in chained payload headers. It is never
 * negotiated as a compression algorithm.
 */
static __always_inline bool smb_compress_alg_valid(__le16 alg, bool valid_none)
{
	switch (alg) {
	case SMB3_COMPRESS_NONE:
		return valid_none;
	case SMB3_COMPRESS_LZ77:
	case SMB3_COMPRESS_LZ77_HUFF:
	case SMB3_COMPRESS_PATTERN:
		return true;
	}

	return false;
}

static __always_inline bool is_compress_hdr(const void *buf)
{
	const struct smb2_compression_hdr *hdr = buf;

	return likely(hdr) && hdr->ProtocolId == SMB2_COMPRESSION_TRANSFORM_ID;
}

/**
 * smb_decompress_alloc_size() - Validate received SMB2 compression header + return allocation size
 *				 for the decompressed buffer.
 * @buf: received buffer
 * @pdu_size: size of @buf
 * @dmin: minimum decompressed size allowed (inclusive)
 * @dmax: maximum decompressed size allowed (inclusive)
 * @chained: if chaining was negotiated
 *
 * Return: decompressed size, or 0 if invalid.
 */
static __always_inline u32 smb_decompress_alloc_size(const void *buf, const u32 pdu_size,
						     const u32 dmin, const u32 dmax, bool chained)
{
	const struct smb2_compression_hdr *hdr = buf;
	__le16 alg, flags;
	u32 len;

	if (unlikely(!is_compress_hdr(buf) || pdu_size < sizeof(*hdr))) {
		pr_warn("invalid SMB2 compressed buffer/header\n");
		return 0;
	}

	len = le32_to_cpu(hdr->OriginalCompressedSegmentSize);
	alg = le16_to_cpu(hdr->CompressionAlgorithm);
	flags = le16_to_cpu(hdr->Flags);

	if (flags == SMB2_COMPRESSION_FLAG_CHAINED) {
		if (unlikely(!chained)) {
			pr_warn("chained payload but chaining wasn't negotiated\n");
			return 0;
		}

		if (unlikely(!smb_compress_alg_valid(alg, true))) {
			pr_warn("invalid chained compression algorithm (%u)\n", alg);
			return 0;
		}

		/* This is the first payload_hdr->Length field, so it can never be 0 */
		if (unlikely(le32_to_cpu(hdr->Offset) == 0)) {
			pr_warn("invalid chained compression payload length 0\n");
			return 0;
		}
	} else if (flags == SMB2_COMPRESSION_FLAG_NONE) {
		/*
		 * When unchained, we must account for the offset (uncompressed) part as well
		 * (usually SMB2 header, but can be anything).
		 */
		u32 res, offset = le32_to_cpu(hdr->Offset);

		if (unlikely(chained)) {
			pr_warn("unchained payload but chaining negotiated\n");
			return 0;
		}

		if (unlikely(offset > pdu_size - sizeof(*hdr) ||
			     check_add_overflow(len, offset, &res))) {
			pr_warn("invalid unchained compression offset (%u, len=%u)\n", offset, len);
			return 0;
		}

		len = res;
		if (unlikely(!smb_compress_alg_valid(alg, false) || alg == SMB3_COMPRESS_PATTERN)) {
			pr_warn("invalid unchained compression algorithm (%u)\n", alg);
			return 0;
		}
	} else {
		pr_warn("invalid SMB2 compression flags (0x%x)\n", flags);
		return 0;
	}

	if (unlikely(len < dmin || len > dmax)) {
		pr_warn("decompressed size %u out of range (min=%u, max=%u)\n", len, dmin, dmax);
		return 0;
	}

	return len;
}

/**
 * smb_compress_alloc_size() - Compute total allocation size required for compressed (dst) buffer.
 * @size:		uncompressed size
 * @chained:		if chained compression is enabled
 * @use_pattern:	if Pattern_V1 is enabled
 * @lzalg:		LZ* algorithm that will be used
 *
 * For any case:
 * - SMB2 compression hdr
 * - LZ* payload
 *
 * If @chained, account for:
 * - 1x payload hdr
 *
 * If @use_pattern, also account for:
 * - 2x payload hdr for Pattern_V1
 * - 2x Pattern_V1 payloads
 *
 * (possible uncompressed leftovers are included in LZ alloc size)
 *
 * This helper assumes that the invalid combination (!@chained && @use_pattern) was previously
 * checked by the caller.
 */
static __always_inline u32 smb_compress_alloc_size(const u32 size, const bool chained,
						   const bool use_pattern, const __le16 lzalg)
{
	u32 alloc_size = sizeof(struct smb2_compression_hdr);

	if (lzalg == SMB3_COMPRESS_LZ77)
		alloc_size += smb_lz77_compressed_alloc_size(size);
	else if (lzalg == SMB3_COMPRESS_LZ77_HUFF)
		alloc_size += smb_huff_compressed_alloc_size(size);

	if (chained) {
		alloc_size += SMB2_COMPRESSION_PAYLOAD_BASE_LEN;
		if (use_pattern)
			alloc_size += (SMB2_COMPRESSION_PAYLOAD_BASE_LEN * 2) +
				(sizeof(struct smb2_compression_pattern_v1) * 2);
	}

	return alloc_size;
}

int smb_compression_decompress(__le16 alg, bool allow_chained,
			       const void *src, u32 slen, void *dst, u32 dlen);
int smb_compression_compress(__le16 alg, bool chained, bool allow_pattern,
			     const void *src, u32 slen,
			     void *dst, u32 *dlen);

#endif /* _COMMON_SMB_COMPRESS_H */
