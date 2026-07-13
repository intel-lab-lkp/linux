// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SMB2 compression transform helpers.
 *
 * Copyright (C) 2026 Namjae Jeon <linkinjeon@kernel.org>
 */
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/string.h>

#include "compress.h"

/*
 * A NONE payload carries bytes verbatim. Keep both cursors and remaining
 * lengths together so every chained payload handler applies identical bounds
 * accounting.
 */
static int smb_decompress_none(const u8 **src, u32 *slen, u8 **dst, u32 *dlen,
			       u32 len)
{
	if (len > *slen || len > *dlen)
		return -EINVAL;

	memcpy(*dst, *src, len);
	*src += len;
	*slen -= len;
	*dst += len;
	*dlen -= len;
	return 0;
}

/*
 * Pattern_V1 represents a run of one byte. Its wire payload is always the
 * fixed-size smb2_compression_pattern_v1 structure.
 */
static int smb_decompress_pattern(const u8 **src, u32 *slen, u8 **dst,
				  u32 *dlen, u32 len)
{
	const struct smb2_compression_pattern_v1 *pattern;
	u32 repetitions;

	if (len != sizeof(*pattern) || len > *slen)
		return -EINVAL;

	pattern = (const struct smb2_compression_pattern_v1 *)*src;
	repetitions = le32_to_cpu(pattern->Repetitions);
	if (repetitions > *dlen)
		return -EINVAL;

	memset(*dst, pattern->Pattern, repetitions);
	*src += len;
	*slen -= len;
	*dst += repetitions;
	*dlen -= repetitions;
	return 0;
}

/*
 * LZ77 payload Length includes the four-byte OriginalPayloadSize field.
 * Consume that field before passing the compressed stream to the raw codec.
 */
static int smb_decompress_lz77_payload(const u8 **src, u32 *slen, u8 **dst,
				       u32 *dlen, u32 len)
{
	u32 orig_size;
	int rc;

	if (len < sizeof(__le32) || len > *slen)
		return -EINVAL;

	orig_size = mem_read32(*src);
	if (orig_size > *dlen)
		return -EINVAL;

	*src += sizeof(__le32);
	*slen -= sizeof(__le32);
	len -= sizeof(__le32);

	rc = smb_lz77_decompress(*src, len, *dst, orig_size);
	if (rc)
		return rc;

	*src += len;
	*slen -= len;
	*dst += orig_size;
	*dlen -= orig_size;
	return 0;
}

static int smb_decompress_chained(__le16 alg, bool allow_chained,
				  const struct smb2_compression_hdr *hdr,
				  u32 slen, void *dst, u32 dlen)
{
	const struct smb2_compression_payload_hdr *payload;
	const u8 *src = (const u8 *)hdr + SMB2_COMPRESSION_CHAINED_HDR_LEN;
	u32 orig_size = le32_to_cpu(hdr->OriginalCompressedSegmentSize);
	u32 remaining = slen - SMB2_COMPRESSION_CHAINED_HDR_LEN;
	u8 *out = dst;
	u32 out_remaining = dlen;
	bool first = true;
	int rc;

	if (!allow_chained || orig_size != dlen)
		return -EINVAL;

	/*
	 * The chained transform has an eight-byte top-level header. The next
	 * bytes are a sequence of payload headers whose Length fields account
	 * for payload data, including OriginalPayloadSize where applicable.
	 */
	while (remaining) {
		__le16 payload_alg;
		__le16 flags;
		u32 len;

		if (remaining < SMB2_COMPRESSION_PAYLOAD_BASE_LEN)
			return -EINVAL;

		payload = (const struct smb2_compression_payload_hdr *)src;
		payload_alg = payload->CompressionAlgorithm;
		flags = payload->Flags;
		len = le32_to_cpu(payload->Length);

		/*
		 * CHAINED marks only the first payload. Requiring NONE on every
		 * later payload rejects ambiguous or independently chained data.
		 */
		if ((first && flags != cpu_to_le16(SMB2_COMPRESSION_FLAG_CHAINED)) ||
		    (!first && flags != cpu_to_le16(SMB2_COMPRESSION_FLAG_NONE)))
			return -EINVAL;

		src += SMB2_COMPRESSION_PAYLOAD_BASE_LEN;
		remaining -= SMB2_COMPRESSION_PAYLOAD_BASE_LEN;

		if (payload_alg == SMB3_COMPRESS_NONE) {
			rc = smb_decompress_none(&src, &remaining, &out,
						 &out_remaining, len);
		} else if (payload_alg == SMB3_COMPRESS_PATTERN) {
			rc = smb_decompress_pattern(&src, &remaining, &out,
						    &out_remaining, len);
		} else if (payload_alg == alg && alg == SMB3_COMPRESS_LZ77) {
			rc = smb_decompress_lz77_payload(&src, &remaining, &out,
							 &out_remaining, len);
		} else {
			return -EINVAL;
		}
		if (rc)
			return rc;
		first = false;
	}

	return out_remaining ? -EINVAL : 0;
}

static int smb_decompress_unchained(__le16 alg,
				    const struct smb2_compression_hdr *hdr,
				    u32 slen, void *dst, u32 dlen)
{
	u32 orig_size, offset, comp_size;

	if (hdr->CompressionAlgorithm != alg ||
	    !smb_compress_alg_valid(hdr->CompressionAlgorithm, false))
		return -EINVAL;

	orig_size = le32_to_cpu(hdr->OriginalCompressedSegmentSize);
	offset = le32_to_cpu(hdr->Offset);
	if (offset > slen - sizeof(*hdr) || offset > dlen ||
	    orig_size > dlen - offset || orig_size + offset != dlen)
		return -EINVAL;

	memcpy(dst, (const u8 *)hdr + sizeof(*hdr), offset);
	comp_size = slen - sizeof(*hdr) - offset;
	return smb_lz77_decompress((const u8 *)hdr + sizeof(*hdr) + offset,
				   comp_size, (u8 *)dst + offset, orig_size);
}

/**
 * smb_compression_decompress() - decode an SMB2 compression transform
 * @alg: negotiated general-purpose compression algorithm
 * @allow_chained: whether chained transforms were negotiated
 * @src: transform header followed by compressed payload data
 * @slen: total number of bytes available at @src
 * @dst: output buffer for the reconstructed SMB2 message
 * @dlen: exact expected size of the reconstructed SMB2 message
 *
 * Validate the transform type and negotiated capabilities before dispatching
 * to the chained or unchained decoder. The caller supplies the expected output
 * size after applying its transport-specific message size limits.
 *
 * Return: 0 on success, otherwise a negative errno.
 */
int smb_compression_decompress(__le16 alg, bool allow_chained,
			       const void *src, u32 slen, void *dst, u32 dlen)
{
	const struct smb2_compression_hdr *hdr = src;

	if (!src || !dst || slen < sizeof(*hdr) ||
	    hdr->ProtocolId != SMB2_COMPRESSION_TRANSFORM_ID ||
	    alg == SMB3_COMPRESS_NONE)
		return -EINVAL;

	if (hdr->Flags == cpu_to_le16(SMB2_COMPRESSION_FLAG_CHAINED))
		return smb_decompress_chained(alg, allow_chained, hdr, slen,
					      dst, dlen);

	if (hdr->Flags != cpu_to_le16(SMB2_COMPRESSION_FLAG_NONE))
		return -EINVAL;

	return smb_decompress_unchained(alg, hdr, slen, dst, dlen);
}
EXPORT_SYMBOL_GPL(smb_compression_decompress);

struct smb_compression_builder {
	u8 *pos;
	u32 remaining;
};

/*
 * Reserve one chained payload header and initialize its common fields.
 * OriginalPayloadSize is present only for LZNT1/LZ77/LZ77+Huffman payloads.
 */
static struct smb2_compression_payload_hdr *
smb_compression_add_payload(struct smb_compression_builder *builder,
			    __le16 alg, u32 payload_len, bool orig_size)
{
	struct smb2_compression_payload_hdr *payload;
	u32 hdr_len = SMB2_COMPRESSION_PAYLOAD_BASE_LEN;
	u32 total_len;

	if (orig_size)
		hdr_len += sizeof(payload->OriginalPayloadSize);
	if (check_add_overflow(hdr_len, payload_len, &total_len) ||
	    total_len > builder->remaining)
		return NULL;

	payload = (struct smb2_compression_payload_hdr *)builder->pos;
	payload->CompressionAlgorithm = alg;
	payload->Flags = cpu_to_le16(SMB2_COMPRESSION_FLAG_NONE);
	payload->Length = cpu_to_le32(payload_len +
		(orig_size ? sizeof(payload->OriginalPayloadSize) : 0));

	builder->pos += hdr_len;
	builder->remaining -= hdr_len;
	return payload;
}

static int smb_compression_add_pattern(struct smb_compression_builder *builder,
				       u8 pattern, u32 repetitions)
{
	struct smb2_compression_pattern_v1 *payload;

	if (!smb_compression_add_payload(builder, SMB3_COMPRESS_PATTERN,
					 sizeof(*payload), false))
		return -ENOSPC;

	payload = (struct smb2_compression_pattern_v1 *)builder->pos;
	payload->Pattern = pattern;
	payload->Reserved1 = 0;
	payload->Reserved2 = 0;
	payload->Repetitions = cpu_to_le32(repetitions);
	builder->pos += sizeof(*payload);
	builder->remaining -= sizeof(*payload);
	return 0;
}

static int smb_compression_add_none(struct smb_compression_builder *builder,
				    const u8 *src, u32 len)
{
	if (!smb_compression_add_payload(builder, SMB3_COMPRESS_NONE, len, false))
		return -ENOSPC;

	memcpy(builder->pos, src, len);
	builder->pos += len;
	builder->remaining -= len;
	return 0;
}

static int smb_compression_add_lz77(struct smb_compression_builder *builder,
				    const u8 *src, u32 len, bool chained)
{
	struct smb2_compression_payload_hdr *payload;
	u32 comp_len;
	int rc;

	if (builder->remaining <= sizeof(*payload))
		return -ENOSPC;

	comp_len = builder->remaining;
	if (chained) {
		comp_len -= sizeof(*payload);
		payload = smb_compression_add_payload(builder, SMB3_COMPRESS_LZ77,
						      comp_len, true);
		if (!payload)
			return -ENOSPC;
	}

	rc = smb_lz77_compress(src, len, builder->pos, &comp_len);
	if (rc)
		return rc;

	if (chained) {
		payload->Length = cpu_to_le32(comp_len + sizeof(payload->OriginalPayloadSize));
		payload->OriginalPayloadSize = cpu_to_le32(len);
	}

	builder->pos += comp_len;
	builder->remaining -= comp_len;
	return 0;
}

/**
 * smb_compression_compress() - build a SMB2 compression transform (chained optional)
 * @alg: negotiated general-purpose compression algorithm
 * @chained: is chained compression was negotiated
 * @allow_pattern: whether Pattern_V1 was negotiated
 * @src: complete uncompressed SMB2 message
 * @slen: size of @src
 * @dst: output buffer for the transform
 * @dlen: input capacity of @dst and output transform size
 * @uncomp: (optional) uncompressed data to be included in the compressed payload
 * @uncomp_len: (optional) @uncomp size, or amount of bytes to copy uncompressed from @src
 *
 * Following the algorithm in [MS-SMB2] 3.1.4.4, encode sufficiently long
 * repeated runs at the front and back as Pattern_V1 payloads. Compress a
 * middle region larger than 1 KiB with LZ77; smaller middle regions are
 * represented by a chained NONE payload.
 *
 * This helper does not decide whether the final transform is smaller than the
 * original message. The transport caller owns that policy decision.
 *
 * If @uncomp is not NULL, copy @uncomp_len from it, as is, to right after the compression header.
 * Otherwise, if @uncomp_len is >0, copy that many bytes from @src to right after compression
 * header.
 *
 * Return: 0 on success, otherwise a negative errno.
 */
int smb_compression_compress(__le16 alg, bool chained, bool allow_pattern,
			     const void *src, u32 slen, void *dst, u32 *dlen,
			     const void *uncomp, u32 uncomp_len)
{
	struct smb_compression_builder builder;
	struct smb2_compression_hdr *hdr;
	const u8 *input = src;
	u32 forward = 0, backward = 0, middle_len;
	int rc;

	if (!src || !dst || !dlen || alg != SMB3_COMPRESS_LZ77 ||
	    *dlen <= SMB2_COMPRESSION_CHAINED_HDR_LEN || !slen)
		return -EINVAL;

	/* Note that the below is a bug, but (chained && !allow_pattern) is a valid combination */
	if (WARN_ON_ONCE(!chained && allow_pattern))
		return -EINVAL;

	/*
	 * Offset @dst for both unchained and chained cases, header is setup at the end.
	 *
	 * Note the layouts:
	 *
	 * smb2_compression_hdr			| smb2_compression_payload_hdr
	 * -------------------------------------|-------------------------------
	 *   ProtocolId				|
	 *   OriginalCompressedSegmentSize	|
	 *   CompressionAlgorithm		| CompressionAlgorithm
	 *   Flags				| Flags
	 *   Offset				| Length
	 *
	 * By aligning @dst to start at the "first payload header", @uncomp_len fits nicely for
	 * both unchained and chained cases as well.
	 */
	builder.pos = (u8 *)dst + SMB2_COMPRESSION_CHAINED_HDR_LEN;
	builder.remaining = *dlen - SMB2_COMPRESSION_CHAINED_HDR_LEN;

	if (uncomp_len) {
		smb_compression_add_none(&builder, uncomp ?: input, uncomp_len);

		if (!uncomp)
			input += uncomp_len;
	}

	if (!chained)
		goto do_lz;

	if (allow_pattern && slen > 32) {
		for (forward = 1; forward < slen; forward++) {
			if (input[forward] != input[0])
				break;
		}
		if (forward <= 32)
			forward = 0;

		for (backward = 1; backward < slen - forward; backward++) {
			if (input[slen - backward - 1] != input[slen - 1])
				break;
		}
		if (backward <= 32)
			backward = 0;
	}

	if (forward) {
		rc = smb_compression_add_pattern(&builder, input[0], forward);
		if (rc)
			return rc;
	}
do_lz:
	rc = 0;
	middle_len = slen - forward - backward;
	if (middle_len > 1024 || !chained)
		rc = smb_compression_add_lz77(&builder, input + forward, middle_len, chained);
	else if (middle_len && chained)
		rc = smb_compression_add_none(&builder,
					      input + forward, middle_len);
	if (rc)
		return rc;

	if (backward) {
		rc = smb_compression_add_pattern(&builder, input[slen - 1],
						 backward);
		if (rc)
			return rc;
	}

	*dlen = builder.pos - (u8 *)dst;

	/*
	 * Set header at the end so we overwrite the flag of first payload (when chained).
	 * Also when chained, if @uncomp_len > 0, that uncompressed chunk is also part of the whole
	 * payload, so we must account for that.
	 */
	hdr = dst;
	hdr->ProtocolId = SMB2_COMPRESSION_TRANSFORM_ID;
	hdr->OriginalCompressedSegmentSize = cpu_to_le32(slen);
	hdr->Flags = cpu_to_le16(SMB2_COMPRESSION_FLAG_NONE);

	if (chained) {
		hdr->OriginalCompressedSegmentSize += cpu_to_le32(uncomp_len);
		hdr->Flags = cpu_to_le16(SMB2_COMPRESSION_FLAG_CHAINED);
	} else if (uncomp_len) {
		/*
		 * If we copied an uncompressed chunk, it was added with a "NONE" algorithm,
		 * so set the correct one when unchained.
		 */
		hdr->CompressionAlgorithm = alg;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(smb_compression_compress);
