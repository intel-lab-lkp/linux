// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024-2026, SUSE LLC
 *
 * Authors: Enzo Matsumiya <ematsumiya@suse.de>
 *
 * This file implements I/O compression support for SMB2 messages (SMB 3.1.1 only).
 * See compress/ for implementation details of each algorithm.
 *
 * References:
 * MS-SMB2 "3.1.4.4 Compressing the Message"
 * MS-SMB2 "3.1.5.3 Decompressing the Chained Message"
 * MS-XCA - for details of the supported algorithms
 */
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/uio.h>
#include <linux/sort.h>

#include "cifsglob.h"
#include "cifsproto.h"
#include "smb2proto.h"

#include "../common/compress/compress.h"
#include "compress.h"

/**
 * freq_count() - Count symbols (bytes) frequencies in a buffer.
 * @buf:	uncompressed buffer
 * @len:	size of @buf
 * @mem:	(caller-allocated) auxiliary memory for parallel accumulators
 * @count:	on exit, set to unique byte count (alphabet size) in @buf
 *
 * This function counts all bytes' unique count and frequencies.
 * Expect @mem to be 1024 * sizeof(u32) long and zeroed.
 * Final frequencies will be stored on first 256 elements of @mem.
 *
 * Function is optimized for OoO CPUs (straightforward iterative count is about 2-3x slower).
 *
 * Return: max frequency found in @buf.
 *
 * (*) This is an adapted version of FSE's https://github.com/Cyan4973/FiniteStateEntropy
 */
static noinline u32 freq_count(const u8 *buf, const u32 len, u32 *mem, u32 *count)
{
	const u8 *p = buf, *end = p + len;
	u32 c = 0, max_freq = 0;
	u32 v = mem_read32(p);
	u32 *acc1 = mem;
	u32 *acc2 = acc1 + 256;
	u32 *acc3 = acc2 + 256;
	u32 *acc4 = acc3 + 256;

	p += sizeof(u32);

	/* Count by stripes of 16 bytes. */
	while (p < end - 15) {
		u32 cached = v;

		v = mem_read32(p);
		p += 4;
		acc1[(u8)cached]++;
		acc2[(u8)(cached>>8)]++;
		acc3[(u8)(cached>>16)]++;
		acc4[(u8)(cached>>24)]++;

		cached = v;
		v = mem_read32(p);
		p += 4;
		acc1[(u8)cached]++;
		acc2[(u8)(cached>>8)]++;
		acc3[(u8)(cached>>16)]++;
		acc4[(u8)(cached>>24)]++;

		cached = v;
		v = mem_read32(p);
		p += 4;
		acc1[(u8)cached]++;
		acc2[(u8)(cached>>8)]++;
		acc3[(u8)(cached>>16)]++;
		acc4[(u8)(cached>>24)]++;

		cached = v;
		v = mem_read32(p);
		p += 4;
		acc1[(u8)cached]++;
		acc2[(u8)(cached>>8)]++;
		acc3[(u8)(cached>>16)]++;
		acc4[(u8)(cached>>24)]++;
	}

	p -= 4;

	/* Count lefotver symbols. */
	while (p < end)
		acc1[*p++]++;

	/*
	 * Combine accumulators + store.
	 *
	 * Avoid dereferencing @count here because performance.
	 */
	for (v = 0; v < 256; v++) {
		acc1[v] += acc2[v] + acc3[v] + acc4[v];

		if (acc1[v]) {
			c++;
			if (acc1[v] > max_freq)
				max_freq = acc1[v];
		}
	}

	if (count)
		*count = c;

	return max_freq;
}

/**
 * calc_entropy() - Compute Shannon entropy of a sample symbols' frequencies.
 * @freqs:	frequency counts of the sample
 * @len:	size of the sample
 * @scale:	scale for final result
 *
 * @freqs may include zero freq symbols.
 * The final sum is multiplied by @factor to fit callers' precision requirements.
 *
 * Valid results interpretation for @factor == 1:
 *   < 6: definitely compressible
 * 6 - 7: probably compressible, but needs other parameters/heuristics for decisive result
 *   > 7: probably uncompressible -- other parameters/heuristics _might_ evaluate to "somewhat
 *	  compressible", but usually not much
 *
 * Return: approximate number of bits (scaled to @scale) per byte that would be required to
 * compress the data.
 */
static __always_inline u32 calc_entropy(const u32 *freqs, const u32 len, const u32 scale)
{
	const u32 len2 = ilog2(len);
	size_t i, p, sum = 0;

	for (i = 0; i < 256; i++) {
		/* 0 freq won't skew the results (ilog2(n) also returns 0 if n < 2) */
		p = freqs[i];
		sum += p * (len2 - ilog2(p));
	}

	return (sum * scale / len);
}

/*
 * Heuristic limit values.
 * These are based on 64k blocks.
 */
static const u32 heuristic_freq_hi	= 8000;
static const u32 heuristic_freq_lo	= 5000;
static const u32 heuristic_entropy_hi	= 800;
static const u32 heuristic_entropy_mid	= 700;
static const u32 heuristic_entropy_lo	= 600;
static const u32 heuristic_alphabet_hi	= 256;
static const u32 heuristic_alphabet_lo	= 128;

enum {
	UNCOMPRESSIBLE = 0,
	COMPRESSIBLE,
	MAYBE_COMPRESSIBLE,
};

/*
 * Computes entropy and byte dominance of @buf, on a 64k block basis.
 *
 * Return: one of the *COMPRESSIBLE values
 */
static int check_compressible_chunks(const u8 *buf, const u32 len, u32 *freqs)
{
	int score = 0, boost = 0, n = 0;
	const u32 freqs_size = 1024 * sizeof(u32);
	const u32 max_samples = 64;
	const u32 max_sample_len = (len / max_samples);
	const u32 nsamples = umin(max_samples, len / max_sample_len);
	const s32 half_samples = nsamples / 2;
	const u8 *end = buf + len;

	while (buf < end) {
		u32 alphabet = 0;
		const u32 remaining = end - buf;
		const u32 slen = umin(max_sample_len, remaining);
		const u32 max_freq = freq_count(buf, slen, freqs, &alphabet);
		const u32 entropy = calc_entropy(freqs, slen, 100);

		if (entropy < heuristic_entropy_mid && max_freq > heuristic_freq_hi)
			score++;
		else if (entropy >= heuristic_entropy_mid && max_freq < heuristic_freq_lo)
			score--;

		if (alphabet < heuristic_alphabet_hi) {
			boost++;
			if (alphabet < heuristic_alphabet_lo)
				boost++;
		}

		/* Reward/penalty for really good/bad entropy. */
		if (entropy > heuristic_entropy_hi)
			boost -= 2;
		else if (entropy < heuristic_entropy_lo)
			boost += 2;

		memset(freqs, 0, freqs_size);
		buf += slen;
		n++;
	}

	/* Check how much the heuristics above impacted (at least) half of @buf. */
	if (score + boost > 0 && score + boost < half_samples)
		return MAYBE_COMPRESSIBLE;

	if (score + boost > -half_samples) {
		if (score > half_samples || score + boost > half_samples)
			return COMPRESSIBLE;

		return MAYBE_COMPRESSIBLE;
	}

	return UNCOMPRESSIBLE;
}

/*
 * Check @buf heuristics (entropy/distribution) to determine its compressibility level.
 *
 * Tests shows that this function is quite reliable in predicting data compressibility, matching
 * very close with the behaviour of LZ77 compression success and failures.
 *
 * This function allocates memory, callers must check for -ENOMEM.
 *
 * Return: one of the *COMPRESSIBLE values on success, -errno otherwise.
 */
static __must_check int check_compressible(const u8 *buf, u32 len)
{
	u32 entropy, *freqs, rle = 0, rle_boost = 0;
	const u32 min_reps = (len / 100); /* ~1% of @len */
	const u8 *p;
	int ret;

	if (unlikely(!buf || !len))
		return -EINVAL;

	/* Stage 1: RLE (@buf is filled with same byte, a.k.a Run-Length Encoding). */
	p = memchr_inv(buf, *buf, len);
	if (!p)
		/* Full RLE */
		return 1;

	/* Stage 2: partial/starting RLE (repeating bytes cover are at least @min_reps long). */
	if (p - buf >= min_reps) {
		rle = (p - buf);

		buf += rle;
		len -= rle;

		rle_boost = rle >> (ilog2(len) - 1);
	}

	freqs = kcalloc(1024, sizeof(*freqs), GFP_KERNEL);
	if (!freqs)
		return -ENOMEM;

	/*
	 * Stage 3: whole buffer entropy check (decisive results if too high or too low).
	 *
	 * Don't care about max freq or alphabet size here.
	 */
	(void)freq_count(buf, len, freqs, NULL);
	entropy = calc_entropy(freqs, len, 1);
	ret = MAYBE_COMPRESSIBLE;

	if (entropy >= 8)
		ret = UNCOMPRESSIBLE;
	else if (entropy < 6 || (rle_boost && entropy - rle_boost <= 6) || len <= SZ_64K)
		ret = COMPRESSIBLE;

	if (ret == MAYBE_COMPRESSIBLE) {
		memset(freqs, 0, sizeof(u32) * 1024);

		/*
		 * Stage 4: break down buffer into 64k chunks for more detailed analysis.
		 *
		 * Note that it's quite common to go from MAYBE_COMPRESSIBLE -> COMPRESSIBLE after
		 * chunks analysis.
		 *
		 * OTOH, getting again MAYBE_COMPRESSIBLE really means 'uncompressible' for
		 * compressors that focus on speed (e.g. LZ77).
		 * Compressors focused on compression ratio might yield a good compression.
		 */
		ret = check_compressible_chunks(buf, len, freqs);
	}

	kfree(freqs);

	return ret;
}

/*
 * should_compress() - Determines if a request (write) or the response to a
 *		       request (read) should be compressed.
 * @tcon: tcon of the request is being sent to
 * @rqst: request to evaluate
 *
 * Return: true iff:
 * - compression was successfully negotiated with server
 * - server has enabled compression for the share
 * - it's a read or write request
 * - (write only) request length is >= SMB_COMPRESS_MIN_LEN
 *
 * Return false otherwise.
 */
bool should_compress(const struct cifs_tcon *tcon, const struct smb_rqst *rq)
{
	const struct smb2_hdr *shdr = rq->rq_iov->iov_base;

	if (unlikely(!tcon || !tcon->ses || !tcon->ses->server))
		return false;

	if (!tcon->ses->server->compression.enabled)
		return false;

	if (!(tcon->share_flags & SMB2_SHAREFLAG_COMPRESS_DATA))
		return false;

	if (shdr->Command == SMB2_WRITE) {
		const struct smb2_write_req *wreq = rq->rq_iov->iov_base;

		return (le32_to_cpu(wreq->Length) >= SMB_COMPRESS_MIN_LEN);
	}

	return (shdr->Command == SMB2_READ);
}

int smb_compress(struct TCP_Server_Info *server, struct smb_rqst *rq, compress_send_fn send_fn)
{
	struct iov_iter iter;
	u32 slen, dlen, shdr_len;
	void *src, *dst = NULL;
	bool use_pattern;
	int ret;

	if (!server || !rq || !rq->rq_iov || !rq->rq_iov->iov_base)
		return -EINVAL;

	if (rq->rq_iov->iov_len != sizeof(struct smb2_write_req))
		return -EINVAL;

	slen = iov_iter_count(&rq->rq_iter);
	src = kvzalloc(slen, GFP_KERNEL);
	if (!src) {
		ret = -ENOMEM;
		goto err_free;
	}

	/* Keep the original iter intact. */
	iter = rq->rq_iter;

	if (!copy_from_iter_full(src, slen, &iter)) {
		ret = smb_EIO(smb_eio_trace_compress_copy);
		goto err_free;
	}

	/*
	 * Even though smb_lz77_compress() runs quite fast on uncompressible data (it ends up
	 * usually just parsing the input buffer), this is much faster.
	 *
	 * So to keep things balanced (compress performance vs prediction accuracy), let's drop the
	 * uncompressible low-hanging fruits here and let smb_lz77_compress() handle the
	 * exceptions/rare cases.
	 */
	ret = check_compressible(src, slen);

	/* XXX: do something with MAYBE_COMPRESSIBLE */
	if (ret != COMPRESSIBLE) {
		if (ret >= 0)
			ret = send_fn(server, 1, rq);
		goto err_free;
	}

	use_pattern = server->compression.pattern;
	shdr_len = rq->rq_iov[0].iov_len;
	dlen = smb_compress_alloc_size(slen, use_pattern) + shdr_len;
	dst = kvzalloc(dlen, GFP_KERNEL);
	if (!dst) {
		ret = -ENOMEM;
		goto err_free;
	}

	ret = smb_compression_compress(SMB3_COMPRESS_LZ77, server->compression.chained, use_pattern,
				       src, slen, dst, &dlen, rq->rq_iov[0].iov_base, shdr_len);
	if (!ret) {
		struct smb_rqst comp_rq = { .rq_nvec = 1, };
		struct kvec iov = {
			.iov_base = dst,
			.iov_len = dlen,
		};

		iov.iov_base = dst;
		iov.iov_len = dlen;
		comp_rq.rq_iov = &iov;

		ret = send_fn(server, 1, &comp_rq);
	} else if (ret == -EMSGSIZE || dlen >= slen) {
		ret = send_fn(server, 1, rq);
	}
err_free:
	kvfree(dst);
	kvfree(src);

	return ret;
}
