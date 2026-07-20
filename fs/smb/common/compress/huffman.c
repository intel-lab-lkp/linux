// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026, SUSE LLC
 *
 * Authors: Enzo Matsumiya <ematsumiya@suse.de>
 *
 * Implementation of the LZ77+Huffman compression algorithm, as per MS-XCA spec.
 */
#include <linux/slab.h>
#include <linux/count_zeros.h>
#include <linux/sort.h>
#include <linux/list_sort.h>
#include <linux/uio.h>

#include "compress.h"

#define HUFF_MIN_MATCH_LEN	3
#define HUFF_MAX_SEQUENCES	((HUFF_BLOCK_SIZE / HUFF_MIN_MATCH_LEN) + 1)
#define HUFF_MAX_SYMS		512
#define HUFF_SYM_MARKER		256

struct huff_sym {
	union {
		u16 freq;
		u16 len;
	};

	union {
		u16 sym;
		u16 code;
	};
} __packed;

struct huff_sequence {
	const u8 *lits_start;
	const u8 *lits_end;

	u16 match_sym;
	u16 match_dist;
	u16 match_len;
};

struct bitstream {
	u32 bits;
	s32 extra;

	union {
		const void *src;
		void *dst;
	};
	const void *end;
	void *cur;
	void *next;
};

#define huff_sym_ptr(ptr)	((struct huff_sym *)(ptr))

static int cmp_freq(const void *a, const void *b)
{
	const u16 freq_a = huff_sym_ptr(a)->freq;
	const u16 freq_b = huff_sym_ptr(b)->freq;

	if (freq_a <= freq_b)
		return -1;

	/*
	 * MS-XCA says we should compare symbol values in case of equal frequencies, but we never
	 * have duplicate symbols in an array, at any time.
	 */
	return 1;
}

static __always_inline int cmp_sym(const void *a, const void *b)
{
	const u16 sym_a = huff_sym_ptr(a)->sym;
	const u16 sym_b = huff_sym_ptr(b)->sym;

	if (sym_a <= sym_b)
		return -1;

	/* There are no duplicate symbols ever, no need to check. */
	return 1;
}

static __always_inline int cmp_len(const void *a, const void *b)
{
	const u16 len_a = huff_sym_ptr(a)->len;
	const u16 len_b = huff_sym_ptr(b)->len;

	if (len_a < len_b)
		return -1;

	if (len_a > len_b)
		return 1;

	/* Same depth, choose smallest symbol. */
	return cmp_sym(a, b);
}

/*
 * Bitstream ops.
 */

/*
 * Read 1, 2, or 4 bytes directly from @st->src and advance it.
 * Store read value in *@varp.
 * Don't update bitstream state!
 */
#define bitstream_read_bytes(st, t, varp)			\
({								\
	const size_t __ts = sizeof(t);				\
	bool __ret = false;					\
	BUILD_BUG_ON(__ts == 3 || __ts > 4);			\
	if (likely((st)->src + __ts <= (st)->end)) {		\
		const t __v = MEM_UNALIGNED_READ((st)->src, t);	\
		(st)->src += sizeof(t);				\
		MEM_UNALIGNED_WRITE((varp), __v, t);		\
		__ret = true;					\
	}							\
	(__ret);						\
})

static __always_inline bool bitstream_decompress_init(struct bitstream *stream, const void *src,
						      const void *end)
{
	u16 bits;

	stream->src = src;
	stream->end = end;

	/* There must be at least 32 bits of data available at start */
	if (unlikely(!bitstream_read_bytes(stream, u16, &bits)))
		return false;

	stream->bits = bits << 16;
	if (unlikely(!bitstream_read_bytes(stream, u16, &bits)))
		return false;

	stream->bits |= bits;
	stream->extra = 16;

	/* Unused on decompress. */
	stream->cur = NULL;
	stream->next = NULL;

	return true;
}

static __always_inline bool bitstream_read_advance(struct bitstream *stream, const u8 bits)
{
	stream->bits <<= bits;
	stream->extra -= bits;
	if (stream->extra < 0) {
		u16 bits;

		if (unlikely(!bitstream_read_bytes(stream, u16, &bits)))
			return false;

		stream->bits |= bits << (-stream->extra);
		stream->extra += 16;
	}

	return true;
}

static __always_inline void bitstream_compress_init(struct bitstream *stream, void *dst,
						    const void *end)
{
	stream->bits = 0;
	stream->extra = 16;
	stream->dst = dst;
	stream->end = end;
	stream->cur = dst + 2;
	stream->next = dst + 4;
}

static __always_inline int bitstream_write(struct bitstream *stream, const u16 bits, const u8 n)
{
	if (n <= stream->extra) {
		stream->extra -= n;
		stream->bits <<= n;
		stream->bits |= bits;
	} else {
		stream->bits <<= stream->extra;
		stream->bits |= (bits >> (n - stream->extra));
		stream->extra -= n;

		if (unlikely(stream->dst + 1 >= stream->end))
			return false;

		mem_write8(stream->dst, stream->bits & 0xff);
		mem_write8(stream->dst + 1, (stream->bits >> 8) & 0xff);

		stream->dst = stream->cur;
		stream->cur = stream->next;
		stream->next += 2;
		stream->extra += 16;
		stream->bits = bits;
	}

	return true;
}

static __always_inline bool bitstream_write_byte(struct bitstream *stream, const u8 bits)
{
	if (unlikely(stream->next >= stream->end))
		return false;

	mem_write8(stream->next, bits);
	stream->next++;
	return true;
}

static __always_inline bool bitstream_write_2bytes(struct bitstream *stream, const u16 bits)
{
	if (unlikely(stream->next + sizeof(u16) > stream->end))
		return false;

	mem_write16(stream->next, bits);
	stream->next += sizeof(u16);
	return true;
}

static __always_inline bool bitstream_flush(struct bitstream *stream)
{
	stream->bits <<= stream->extra;

	if (unlikely(stream->dst + 1 >= stream->end || stream->cur + sizeof(u16) > stream->end))
		return false;

	mem_write8(stream->dst, (stream->bits & 0xff));
	mem_write8(stream->dst + 1, ((stream->bits >> 8) & 0xff));
	mem_write16(stream->cur, 0);

	return true;
}

static int huff_build_histogram(struct huff_sequence *seqs, const int nseqs, struct huff_sym *syms)
{
	u16 nsyms = 0;
	int i;

	for (i = 0; i < nseqs; i++) {
		struct huff_sequence *seq = &seqs[i];

		if (seq->lits_start && seq->lits_end) {
			const u8 *p = seq->lits_start;

			while (p < seq->lits_end)
				if (!syms[mem_read8(p++)].freq++)
					nsyms++;
		}

		if (likely(seq->match_sym >= HUFF_SYM_MARKER))
			if (!syms[seq->match_sym].freq++)
				nsyms++;
	}

	/* Bug in huff_scan_sequences() */
	if (unlikely(nsyms == 0 || nsyms > HUFF_MAX_SYMS))
		return -EIO;

	return nsyms;
}

static __always_inline void init_nodes(struct huff_sym *syms, const int nsyms,
				       struct huff_sym *nodes, const int rebalances)
{
	u16 i, n = 0;

	for (i = 0; i < HUFF_MAX_SYMS; i++) {
		u16 freq = syms[i].freq;

		if (!freq)
			continue;
		/*
		 * When rebalancing, half symbols frequencies on each retry, which will generate a
		 * shallower "tree".
		 * Rebalancing doesn't affect original symbol count.
		 */
		if (unlikely(rebalances)) {
			freq /= (2 * rebalances);
			freq++;
		}

		nodes[n].sym = i;
		nodes[n].freq = freq;
		n++;
	}

	sort(nodes, nsyms, sizeof(*nodes), cmp_freq, NULL);
}

static __always_inline int select_node(int *ap, const int amax, int *bp, const int bmax,
				       const struct huff_sym *nodes)
{
	const int a = *ap;
	const int b = *bp;

	if (a < amax && b >= bmax)
		return (*ap)++;

	if (a >= amax && b < bmax)
		return (*bp)++;

	if (nodes[a].freq <= nodes[b].freq)
		return (*ap)++;

	return (*bp)++;
}

/*
 * Compute the depth (code length) of each Huffman symbol in @nodes.
 *
 * A full-fledged Huffman tree would be built something like:
 *	qa = syms
 *	qb = nodes
 *	while (qa not empty || qb not singular) {
 *		left = select_node(qa, qb)
 *		right = select_node(qa, qb)
 *		new_node->freq = left->freq + right->freq
 *		queue_node(new_node, qb)
 *	}
 *	root = queue_first(qb)
 *
 * Then traverse the tree from root to compute each leaf (i.e. syms elements) depth.
 *
 * This implementation instead simulates those merges, and store frequencies and parents in a
 * separate array, and traverse the "tree" by chasing parents in a more linear manner.
 *
 * Also, the Huffman tree is a full binary tree, so, given its properties, we know exactly how many
 * merges should be done, and not rely on more complex checks.
 *
 * This saves memory and computing resources.
 */
static bool compute_code_lengths(struct huff_sym *nodes, const int nsyms, int *parents)
{
	int root, l = 0, n = nsyms, next = nsyms, merges = nsyms - 1;

	do {
		const int a = select_node(&l, nsyms, &n, next, nodes);
		const int b = select_node(&l, nsyms, &n, next, nodes);

		nodes[next].freq = nodes[a].freq + nodes[b].freq;
		parents[a] = next;
		parents[b] = next;

		next++;
	} while (--merges);

	root = next - 1;

	for (l = 0; l < nsyms; l++) {
		int d = 0, p = l;

		/* Chase parent's indices up until root to compute this symbol code length. */
		while (p != root) {
			p = parents[p];
			d++;
		}

		if (unlikely(d > 14))
			return false;

		nodes[l].len = d;
	}

	return true;
}

static __always_inline int write_codes(struct huff_sym *nodes, struct huff_sym *syms,
				       const int nsyms, u8 *dst)
{
	u16 nextcode = 0, clen = 0;
	int i;

	MEM_PREFETCH(nodes);

	memset(dst, 0, HUFF_TABLE_SIZE);
	sort(nodes, nsyms, sizeof(*nodes), cmp_len, NULL);

	for (i = 0; i < nsyms; i++) {
		struct huff_sym *node = &nodes[i];
		const u16 len = node->len;
		const u16 s = node->sym;
		const u16 pos = (s >> 1);
		const u8 shift = (s & 1) ? 4 : 0;

		/* Generate symbol code. */
		nextcode <<= len - clen;
		clen = len;

		/* Make @syms indexed by symbol value, so we can access it directly later. */
		syms[s].code = nextcode++;
		syms[s].len = len;

		/*
		 * Write symbol depth/code length of Huffman symbols to the table header.
		 * There can be up to 512 Huffman symbols, but code lengths are stored on nibs
		 * (4 bits) so the final table will be 256 bytes long.
		 * (odd symbols go in the upper 4 bits, even symbols on lower 4 bits)
		 *
		 * @pos >= HUFF_TABLE_SIZE means there's a bug in huff_encode_syms().
		 */
		if (WARN_ON_ONCE(pos >= HUFF_TABLE_SIZE))
			return -EFAULT;

		dst[pos] |= (len << shift);
	}

	return 0;
}

static __always_inline int write_single_code(struct huff_sym *syms, u8 *dst)
{
	u16 i;

	memset(dst, 0, HUFF_TABLE_SIZE);

	/* @syms is unsorted here, so @i == symbol */
	for (i = 0; i < HUFF_MAX_SYMS; i++) {
		u16 pos;
		u8 shift;

		if (!syms[i].freq)
			continue;

		pos = (i >> 1);
		shift = (i & 1) ? 4 : 0;
		if (WARN_ON_ONCE(pos >= HUFF_TABLE_SIZE))
			return -EFAULT;

		syms[i].code = 0;
		/* len is 1 as it would be the only child in the tree */
		syms[i].len = 1;
		dst[pos] |= (1 << shift);

		return 0;
	}

	return -EFAULT;
}

static noinline void *huff_encode_syms(struct huff_sym *syms, const int nsyms, void *dst)
{
	const int max_nodes = 2 * nsyms - 1;
	int ret, rebalances = 0, *parents = NULL;
	struct huff_sym *nodes = NULL;

	/*
	 * Skip the whole allocations and computations if @nsyms == 1 (very unlikely case).
	 * (assumes @nsyms == 0 was previously discarded (cf. huff_build_histogram()))
	 */
	if (unlikely(nsyms == 1)) {
		ret = write_single_code(syms, dst);
		goto out;
	}

	nodes = kzalloc_objs(*nodes, max_nodes);
	parents = kzalloc_objs(*parents, max_nodes);
	if (unlikely(!nodes || !parents)) {
		ret = -ENOMEM;
		goto out;
	}

	ret = -EOVERFLOW;
	do {
		MEM_PREFETCH(nodes);

		init_nodes(syms, nsyms, nodes, rebalances++);

		if (likely(compute_code_lengths(nodes, nsyms, parents))) {
			ret = write_codes(nodes, syms, nsyms, dst);
			break;
		}

		/* Max code length is too large ("tree" too deep); reset, rebalance, and retry. */
		memset(parents, 0, max_nodes * sizeof(parents[0]));
		memset(nodes, 0, max_nodes * sizeof(*nodes));

		 /* XXX: should this be increased/decreased? */
	} while (rebalances < 5);
out:
	kfree(nodes);
	kfree(parents);

	return (!ret ? dst + HUFF_TABLE_SIZE : ERR_PTR(ret));
}

static noinline void *huff_encode_final(const void *src, void *dst, const void *dst_end,
					struct huff_sequence *seqs, const int max_seqs,
					struct huff_sym *syms)
{
	const struct huff_sequence *seq;
	struct bitstream stream;
	int i = 0;

	bitstream_compress_init(&stream, dst, dst_end);

	do {
		u16 len, sym;
		u8 distbit;

		/* Assumes @max_seqs >= 1 was checked by caller. */
		seq = &seqs[i++];
		if (seq->lits_start && seq->lits_end) {
			const u8 *p = seq->lits_start;

			while (p < seq->lits_end) {
				sym = *p++;
				if (unlikely(!bitstream_write(&stream, syms[sym].code,
							      syms[sym].len)))
					return ERR_PTR(-EFAULT);
			}
		}

		/* Done, we just wrote leftover literals */
		if (unlikely(!seq->match_len))
			break;

		sym = seq->match_sym;
		len = seq->match_len;
		distbit = (sym - HUFF_SYM_MARKER) / 16;

		if (unlikely(!bitstream_write(&stream, syms[sym].code, syms[sym].len)))
			return ERR_PTR(-EFAULT);

		len -= 3;
		if (len >= 15) {
			if (unlikely(!bitstream_write_byte(&stream, (u8)umin(len - 15, 255))))
				return ERR_PTR(-EFAULT);

			if (len - 15 >= 255)
				/*
				 * Match length is < 64k.
				 * No current support for longer matches.
				 */
				if (unlikely(!bitstream_write_2bytes(&stream, (u16)len)))
					return ERR_PTR(-EFAULT);
		}

		if (unlikely(!bitstream_write(&stream, seq->match_dist - (1U << distbit),
					      (u8)distbit)))
			return ERR_PTR(-EFAULT);
	} while (i < max_seqs);

	if (unlikely(!bitstream_flush(&stream)))
		return ERR_PTR(-EFAULT);

	return stream.next;
}

static __always_inline u16 huff_encode_match(const u16 dist, const u16 len)
{
	const u8 distbit = dist < 256 ? __fls(dist) : 8 + __fls(dist >> 8);

	return (u16)(HUFF_SYM_MARKER + umin(len - 3, 15) + (16 * distbit));
}

static __always_inline u32 hash3(const void *ptr)
{
	return smb_compress_hash(mem_read32(ptr) & 0xffffff);
}

static const void *store_seq(struct huff_sequence *seq, const void *literals,
			     const void *match, const void *cur, const void *end)
{
	if (cur > literals) {
		seq->lits_start = literals;
		seq->lits_end = cur;
	}

	if (likely(match)) {
		seq->match_len = mem_match_len(match, cur, end);
		seq->match_dist = cur - match;
		seq->match_sym = huff_encode_match(seq->match_dist, seq->match_len);

		cur += seq->match_len;
	}

	return cur;
}

/*
 * Scan sequences on @src.
 *
 * Sequences are defined as:
 * - literals chunk (start and end)
 * - match data (symbol, distance, and length)
 *
 * A sequence is stored when a match is found, or at the end, if there are literal leftovers.
 *
 * Aside from that, the whole function structure and match finding algorithm are the same as the
 * one found in lz77.c::smb_lz77_compress(), the only difference is that here we don't do adaptive
 * skipping, but actually parse every @src byte (for better compression).
 */
static noinline int huff_scan_sequences(const void *src, const u32 slen,
					struct huff_sequence *seqs, int *nseqs)
{
	const void *srcp, *rlim, *end, *anchor;
	const int max_seqs = *nseqs;
	u32 *htable, hash, s = 0;
	int ret = 0;

	*nseqs = 0;

	srcp = anchor = src;
	end = src + slen;
	rlim = end - SMB_COMPRESS_MSTEP_SIZE; /* read limit for match finding */

	htable = kvcalloc(SMB_COMPRESS_HASH_SIZE, sizeof(*htable), GFP_KERNEL);
	if (!htable)
		return -ENOMEM;

	MEM_PREFETCH(srcp + SMB_COMPRESS_RSTEP_SIZE);

	hash = hash3(srcp++);
	htable[hash] = 0;
	hash = hash3(srcp);

	do {
		const void *match, *next = srcp;

		do {
			const u32 cur_hash = hash;

			srcp = next;
			next++;
			if (unlikely(next >= rlim))
				goto out;

			hash = hash3(next);
			match = src + htable[cur_hash];
			htable[cur_hash] = srcp - src;

			/*
			 * Scans are done in blocks up to HUFF_BLOCK_SIZE (64k) bytes long.
			 * Due to encoding limitations, Huffman can only find matches that are
			 * (64k - 1) bytes back, which is impossible, because:
			 * - we're only reading up to 'rlim' (i.e. end - 8)
			 * - even if going further, that would mean a match len of 1 (min is 3)
			 *
			 * So, IOW, with our window < block size and window < max distance, we're
			 * always within our window, so all we need to check is 'match' == 'srcp'
			 * (i.e. htable entry was not filled yet).
			 */
		} while (match == srcp || memcmp(match, srcp, HUFF_MIN_MATCH_LEN));

		if (unlikely(s >= max_seqs)) {
			ret = -EIO;
			break;
		}

		srcp = store_seq(&seqs[s++], anchor, match, srcp, end);
		anchor = srcp;
		MEM_PREFETCH(srcp);

		if (unlikely(srcp >= rlim))
			break;

		hash = hash3(srcp);
	} while (srcp < end);
out:
	kvfree(htable);

	if (!ret) {
		if (unlikely(s >= max_seqs))
			return -EIO;

		/* Add sequence for leftover literals */
		end = store_seq(&seqs[s++], anchor, NULL, end, NULL);
		if (IS_ERR(end))
			ret = PTR_ERR(end);
	}

	*nseqs = s;

	return ret;
}

/*
 * Huffman encoding performs extra steps on top of a LZ77-style encoded buffer so it can further
 * compress the symbols at bit level, offering a much better compression (vs. e.g. LZ77 plain).
 *
 * The uncompressed buffer @src is parsed in 64k blocks, and each block goes through 4 main steps
 * as described below.
 *
 * Expectations (compared to LZ77 plain):
 * - compression ratio should be about 10-20% better
 * - performance should be around 2-3x worse
 */
int smb_huff_compress(const void *src, const u32 slen, void *dst, u32 *dlen)
{
	struct huff_sequence *seqs;
	ssize_t ret, remaining = slen;
	struct huff_sym *syms;
	const void *srcp = src, *dst_end = dst + *dlen;
	int nseqs, nsyms;
	void *dstp = dst;

	seqs = kvzalloc_objs(*seqs, HUFF_MAX_SEQUENCES);
	if (unlikely(!seqs))
		return -ENOMEM;

	syms = kzalloc_objs(*syms, HUFF_MAX_SYMS);
	if (unlikely(!syms)) {
		kvfree(seqs);
		return -ENOMEM;
	}

	do {
		const u32 block_slen = umin(HUFF_BLOCK_SIZE, remaining);

		/*
		 * Step 1. LZ77 encoding
		 *
		 * As per the spec, we should first compress @src with smb_lz77_compress() and then
		 * use @dst as our input for step 2.
		 * This implementation decided to NOT do the full LZ77 encoding in order to save
		 * processing time, as it would be required to implement a smb_lz77_decompress-like
		 * function to proceed.
		 * Instead it stores "sequence" tokens to aggregate literals and matches data
		 * (which are then used on step 2).
		 *
		 * This step is implementation specific anyway, as it only builds up intermediate
		 * data that won't affect the final format.
		 *
		 * Even though the bit- vs byte-level compression does an amazing job, match
		 * finding here still counts a lot.
		 */
		nseqs = HUFF_MAX_SEQUENCES;
		ret = huff_scan_sequences(srcp, block_slen, seqs, &nseqs);
		if (unlikely(ret))
			break;

		/*
		 * Step 2. Symbol histogram
		 *
		 * Build a histogram of Huffman symbols; each literal (individual byte) is a symbol
		 * (i.e. 0 - 255), and each match is encoded as a symbol too (256 - 511).
		 *
		 * Count the frequencies of each symbol occurrence, along with the number of unique
		 * symbols in the block.
		 */
		nsyms = huff_build_histogram(seqs, nseqs, syms);
		if (unlikely(nsyms < 0)) {
			ret = nsyms;
			break;
		}

		/*
		 * Step 3. Encode symbols
		 *
		 * Canonically, this step builds up the Huffman tree in order to compute the code
		 * and depth (or length) of each symbol.
		 * This implementation also decided to not do it this way, but instead use a faster
		 * approach (cf. compute_code_lengths()).
		 *
		 * After computing those, code lengths are written to the Huffman table (the first
		 * 256 bytes of the compressed block).
		 */
		dstp = huff_encode_syms(syms, nsyms, dstp);
		if (IS_ERR(dstp)) {
			ret = PTR_ERR(dstp);
			break;
		}

		/*
		 * Step 4. Final encoding
		 *
		 * Now we have all symbols' codes and lengths, write those out to @dstp as a
		 * bitstream (for bit-level compression).
		 */
		dstp = huff_encode_final(srcp, dstp, dst_end, seqs, nseqs, syms);
		if (IS_ERR(dstp)) {
			ret = PTR_ERR(dstp);
			break;
		}

		srcp += block_slen;
		remaining -= block_slen;

		if (likely(remaining > 0)) {
			int i;

			memset(syms, 0, sizeof(*syms) * HUFF_MAX_SYMS);

			for (i = 0; i < HUFF_MAX_SEQUENCES; i++)
				memset(&seqs[i], 0, sizeof(*seqs));
		}
	} while (remaining > 0);

	kvfree(seqs);
	kfree(syms);

	if (!ret)
		*dlen = dstp - dst;

	return ret;
}

static __always_inline const void *fill_table(const u8 *header, u16 *table, u8 *clens)
{
	const u8 *end = header + HUFF_TABLE_SIZE;
	int len, i = 0;
	u8 *plens = clens;
	u16 sym;

	/* read code lenghts from Huffman table on compressed buffer */
	while (header < end) {
		const u8 b = mem_read8(header++);

		mem_write8(plens++, (b & 0x0f));
		mem_write8(plens++, (b & 0xf0) >> 4);
	}

	for (len = 1; len < 16; len++) {
		for (sym = 0; sym < HUFF_MAX_SYMS; sym++) {
			if (clens[sym] == len) {
				int n = (1U << (SMB_COMPRESS_HASH_LOG - len));

				while (n-- > 0) {
					if (unlikely(i >= SMB_COMPRESS_HASH_SIZE))
						return ERR_PTR(-EIO);

					table[i++] = sym;
				}
			}
		}
	}

	if (unlikely(i != SMB_COMPRESS_HASH_SIZE))
		return ERR_PTR(-EIO);

	return header;
}

static __always_inline int huff_decode_match(struct bitstream *stream, u32 *len)
{
	u32 mlen = *len & 15;

	if (mlen == 15) {
		if (unlikely(!bitstream_read_bytes(stream, u8, &mlen)))
			return -EFAULT;

		if (mlen == 255) {
			if (unlikely(!bitstream_read_bytes(stream, u16, &mlen)))
				return -EFAULT;

			if (mlen == 0) {
				if (unlikely(!bitstream_read_bytes(stream, u32, &mlen)))
					return -EFAULT;

				if (unlikely(mlen + 15 < HUFF_BLOCK_SIZE))
					return -EIO;
			} else if (unlikely(mlen < 15)) {
				return -EIO;
			}
			mlen -= 15;
		}
		mlen += 15;
	}
	mlen += 3;
	*len = mlen;

	return 0;
}

int smb_huff_decompress(const void *src, const u32 slen, void *dst, const u32 dlen)
{
	const void *srcp = src, *end = src + slen;
	void *dstp = dst, *dst_end = dst + dlen;
	int ret = 0;

	do {
		struct bitstream stream;
		const void *block_end = dstp + umin(SZ_64K, dst_end - dstp);
		u16 *table = NULL;
		u8 *clens = NULL;

		ret = -ENOMEM;
		table = kvzalloc_objs(*table, SMB_COMPRESS_HASH_SIZE);
		if (unlikely(!table))
			goto err_free;

		clens = kzalloc_objs(*clens, HUFF_MAX_SYMS);
		if (unlikely(!clens))
			goto err_free;

		if (unlikely(srcp + HUFF_TABLE_SIZE > end)) {
			ret = -EFAULT;
			goto err_free;
		}

		srcp = fill_table(srcp, table, clens);
		if (IS_ERR(srcp)) {
			ret = PTR_ERR(srcp);
			goto err_free;
		}

		if (unlikely(!bitstream_decompress_init(&stream, srcp, end))) {
			ret = -EFAULT;
			goto err_free;
		}

		do {
			const u16 sym = table[stream.bits >> (32 - 15)];
			const u32 clen = clens[sym];
			u16 dist, distbit;
			const void *match;
			u32 len;

			ret = -EIO;
			if (unlikely(sym >= HUFF_MAX_SYMS))
				break;

			if (unlikely(clen >= 16))
				break;

			/* Advance bitstream before checking sym */
			if (unlikely(!bitstream_read_advance(&stream, clen))) {
				ret = -EFAULT;
				break;
			}

			if (sym < HUFF_SYM_MARKER) {
				mem_write8(dstp++, sym);
				ret = 0;
				continue;
			}

			if (unlikely(sym == HUFF_SYM_MARKER && stream.src >= end)) {
				ret = 0;
				break;
			}

			len = sym - HUFF_SYM_MARKER;
			distbit = len >> 4;
			if (unlikely(distbit >= 16))
				break;

			ret = huff_decode_match(&stream, &len);
			if (unlikely(ret))
				break;

			ret = -EFAULT;
			dist = (u32)((u64)stream.bits >> (32 - distbit));
			dist += (1U << distbit);
			if (unlikely(dist > dstp - dst))
				break;

			if (unlikely(len > dst_end - dstp))
				break;

			/* Advance bitstream only after checking match len */
			if (unlikely(!bitstream_read_advance(&stream, distbit)))
				break;

			match = dstp - dist;
			if (len < dist) {
				memcpy(dstp, match, len);
				dstp += len;
			} else {
				const void *match_end = dstp + len;

				while (dstp < match_end)
					mem_write8(dstp++, mem_read8(match++));
			}

			ret = 0;
		} while (dstp < block_end);
err_free:
		kvfree(table);
		kfree(clens);

		if (unlikely(ret))
			break;

		srcp = stream.src;
	} while (srcp < end && dstp < dst_end);

	return ret;
}
