// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for lib/sg_split.c.
 *
 * Each case sets up a deterministic input scatterlist (page + offset +
 * length per entry), calls sg_split(), and verifies the two documented
 * post-conditions:
 *
 *   1. sum of out[k] entry lengths equals split_sizes[k]   (per-split size)
 *   2. the bytes walked from out[k] match input bytes
 *      [skip + sum(prev sizes) .. +sizes[k])               (per-split content)
 *
 * Backing memory is a position-unique byte pattern, so a content mismatch
 * identifies exactly which input region got mis-exposed.
 *
 * COVERAGE
 * --------
 * Every base case T1..T16 (+ T8b) runs in two modes via sg_split_run():
 *   - unmapped : in_mapped_nents = 0  -> the CPU-view path (sg_split_phys).
 *   - mapped   : an identity DMA mapping (dma_len == length, IOVA laid out
 *                contiguously from SG_SPLIT_DMA_BASE) -> additionally the
 *                pass-2 / sg_split_mapped() path, asserting the DMA addresses
 *                and lengths as well.  This is the path the upstream callers
 *                that pass a non-zero in_mapped_nents actually hit.
 *
 * The D* cases exercise DIVERGENT geometry, where the DMA segmentation is
 * coarser than the CPU one (coalescing): dma_len != length, so pass 2
 * computes a different per-split nents than pass 1.  They require a real,
 * separate dma_length field and so are skipped on !NEED_SG_DMA_LENGTH
 * arches (where sg_dma_len() aliases ->length and divergence cannot be
 * represented).
 */

#include <kunit/test.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/string.h>

#define SG_SPLIT_MAX_IN_ENTRIES		8
#define SG_SPLIT_MAX_SPLITS		4
#define SG_SPLIT_PAGE_BYTES		4096
/* Synthetic IOVA base, fits a 32-bit dma_addr_t, clearly not a kernel VA. */
#define SG_SPLIT_DMA_BASE		((dma_addr_t)0x40000000UL)

struct sg_split_in_spec {
	int		page;
	unsigned int	off;
	unsigned int	len;
};

static u8 sg_split_pattern(int page, unsigned int off)
{
	return (u8)(((page * 31u) + off * 17u + 0x5a) & 0xffu);
}

/* Verify the DMA view of one split: addresses tile the contiguous IOVA
 * window [SG_SPLIT_DMA_BASE + cursor ..] and the DMA lengths sum to want.
 */
static void sg_split_check_dma(struct kunit *test, const char *mode, int k,
			       struct scatterlist *out, int dma_nents,
			       size_t want, unsigned long cursor)
{
	struct scatterlist *sg;
	unsigned long sum = 0, pos = cursor;
	int j;

	for_each_sg(out, sg, dma_nents, j) {
		KUNIT_EXPECT_EQ_MSG(test, (u64)sg_dma_address(sg),
				    (u64)(SG_SPLIT_DMA_BASE + pos),
				    "[%s] split[%d] dma addr[%d]=0x%llx want 0x%llx",
				    mode, k, j, (u64)sg_dma_address(sg),
				    (u64)(SG_SPLIT_DMA_BASE + pos));
		sum += sg_dma_len(sg);
		pos += sg_dma_len(sg);
	}
	KUNIT_EXPECT_EQ_MSG(test, sum, want,
			    "[%s] split[%d] DMA sum=%lu requested=%zu",
			    mode, k, sum, want);
}

/* Verify the CPU view of one split: ->length sums to want and the page
 * bytes match the linear oracle window.  nents is the number of CPU
 * entries to walk (out_nents when geometry matches, sg_nents() under
 * coalescing).
 */
static void sg_split_check_cpu(struct kunit *test, const char *mode, int k,
			       struct scatterlist *out, int nents, size_t want,
			       const u8 *oracle, unsigned long cursor)
{
	struct scatterlist *sg;
	unsigned long sum = 0;
	size_t copied = 0;
	u8 *got;
	int j;

	for_each_sg(out, sg, nents, j)
		sum += sg->length;
	KUNIT_EXPECT_EQ_MSG(test, sum, want,
			    "[%s] split[%d] CPU sum=%lu requested=%zu",
			    mode, k, sum, want);

	got = kunit_kzalloc(test, want + 1, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, got);
	for_each_sg(out, sg, nents, j) {
		size_t take = min_t(size_t, sg->length, want - copied);

		memcpy(got + copied, sg_virt(sg), take);
		copied += take;
		if (copied >= want)
			break;
	}
	KUNIT_EXPECT_EQ_MSG(test,
			    memcmp(got, oracle + cursor,
				   min_t(size_t, copied, want)),
			    0, "[%s] split[%d] CPU bytes mismatch", mode, k);
}

/* Allocate + pattern-fill backing, build the input SG, and build the
 * linear concatenation oracle.  Returns the input SG; *oracle_out and
 * *backing_out get the two kunit-managed buffers.
 */
static struct scatterlist *
sg_split_build_input(struct kunit *test,
		     const struct sg_split_in_spec *in_spec, int n_in,
		     u8 **oracle_out)
{
	u8 *backing, *oracle;
	struct scatterlist *in_sgl;
	size_t total = 0, op = 0;
	int i;

	backing = kunit_kzalloc(test,
				SG_SPLIT_MAX_IN_ENTRIES * SG_SPLIT_PAGE_BYTES,
				GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, backing);
	for (int p = 0; p < SG_SPLIT_MAX_IN_ENTRIES; p++)
		for (unsigned int o = 0; o < SG_SPLIT_PAGE_BYTES; o++)
			backing[p * SG_SPLIT_PAGE_BYTES + o] =
				sg_split_pattern(p, o);

	in_sgl = kunit_kzalloc(test, sizeof(*in_sgl) * n_in, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, in_sgl);
	sg_init_table(in_sgl, n_in);
	for (i = 0; i < n_in; i++) {
		u8 *buf = backing + in_spec[i].page * SG_SPLIT_PAGE_BYTES
			+ in_spec[i].off;

		sg_set_buf(&in_sgl[i], buf, in_spec[i].len);
		total += in_spec[i].len;
	}

	oracle = kunit_kzalloc(test, total ? total : 1, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, oracle);
	for (i = 0; i < n_in; i++) {
		memcpy(oracle + op,
		       backing + in_spec[i].page * SG_SPLIT_PAGE_BYTES
			+ in_spec[i].off,
		       in_spec[i].len);
		op += in_spec[i].len;
	}

	*oracle_out = oracle;
	return in_sgl;
}

/* Run one case in one mode (unmapped or identity-mapped). */
static void sg_split_run_mode(struct kunit *test, const char *mode,
			      const struct sg_split_in_spec *in_spec, int n_in,
			      off_t skip, const size_t *sizes, int nb_splits,
			      bool dma_mapped)
{
	struct scatterlist *in_sgl;
	struct scatterlist *out[SG_SPLIT_MAX_SPLITS] = { NULL };
	int out_nents[SG_SPLIT_MAX_SPLITS] = { 0 };
	unsigned long cursor = (unsigned long)skip;
	u8 *oracle;
	int rc, k;

	KUNIT_ASSERT_LE(test, n_in, SG_SPLIT_MAX_IN_ENTRIES);
	KUNIT_ASSERT_LE(test, nb_splits, SG_SPLIT_MAX_SPLITS);

	in_sgl = sg_split_build_input(test, in_spec, n_in, &oracle);

	if (dma_mapped) {
		/* Identity mapping: dma_len == length, contiguous IOVA. */
		dma_addr_t a = SG_SPLIT_DMA_BASE;
		int i;

		for (i = 0; i < n_in; i++) {
			sg_dma_address(&in_sgl[i]) = a;
			sg_dma_len(&in_sgl[i]) = in_spec[i].len;
			a += in_spec[i].len;
		}
	}

	rc = sg_split(in_sgl, dma_mapped ? n_in : 0, skip, nb_splits, sizes,
		      out, out_nents, GFP_KERNEL);
	KUNIT_ASSERT_EQ_MSG(test, rc, 0, "[%s] sg_split returned %d", mode, rc);

	for (k = 0; k < nb_splits; k++) {
		KUNIT_ASSERT_NOT_NULL_MSG(test, out[k], "[%s] split[%d] NULL",
					  mode, k);
		/* Exactly one end marker, on the final entry: each entry is
		 * end-marked iff it is the last.  out_nents == 0 (the
		 * degenerate zero-size split, out[k] == ZERO_SIZE_PTR) walks
		 * zero times, so we never index out[k][-1].
		 */
		for (int j = 0; j < out_nents[k]; j++)
			KUNIT_EXPECT_EQ_MSG(test, !!sg_is_last(&out[k][j]),
					    j == out_nents[k] - 1,
					    "[%s] split[%d] entry[%d] end-mark wrong",
					    mode, k, j);
		/* Identity mapping keeps CPU and DMA geometry equal, so
		 * out_nents[k] is both counts; it is also 0 for a degenerate
		 * zero-size split, keeping the walk safe.
		 */
		sg_split_check_cpu(test, mode, k, out[k], out_nents[k],
				   sizes[k], oracle, cursor);
		if (dma_mapped)
			sg_split_check_dma(test, mode, k, out[k], out_nents[k],
					   sizes[k], cursor);
		cursor += sizes[k];
	}

	for (k = 0; k < nb_splits; k++)
		kfree(out[k]);
}

/* Run a case both unmapped and identity-mapped. */
static void sg_split_run(struct kunit *test,
			 const struct sg_split_in_spec *in_spec, int n_in,
			 off_t skip, const size_t *sizes, int nb_splits)
{
	sg_split_run_mode(test, "unmapped", in_spec, n_in, skip, sizes,
			  nb_splits, false);
	sg_split_run_mode(test, "mapped", in_spec, n_in, skip, sizes,
			  nb_splits, true);
}

/* ========================================================================
 * Group A — full or edge-aligned coverage.
 */
static void sg_split_t1_single_entry_full(struct kunit *test)
{
	const struct sg_split_in_spec in[] = { { 0, 0, 100 } };
	const size_t sizes[] = { 100 };

	sg_split_run(test, in, 1, 0, sizes, 1);
}

static void sg_split_t2_two_entries_full_coverage(struct kunit *test)
{
	const struct sg_split_in_spec in[] = { { 0, 0, 60 }, { 1, 0, 40 } };
	const size_t sizes[] = { 100 };

	sg_split_run(test, in, 2, 0, sizes, 1);
}

static void sg_split_t3_two_entries_two_splits_full(struct kunit *test)
{
	const struct sg_split_in_spec in[] = { { 0, 0, 60 }, { 1, 0, 40 } };
	const size_t sizes[] = { 50, 50 };

	sg_split_run(test, in, 2, 0, sizes, 2);
}

static void sg_split_t4_edge_aligned_partial(struct kunit *test)
{
	const struct sg_split_in_spec in[] = { { 0, 0, 60 }, { 1, 0, 40 } };
	const size_t sizes[] = { 60 };

	sg_split_run(test, in, 2, 0, sizes, 1);
}

/* ========================================================================
 * Group B — partial coverage with mid-entry boundary.
 */
static void sg_split_t5_minimal_repro(struct kunit *test)
{
	const struct sg_split_in_spec in[] = { { 0, 0, 2 }, { 1, 0, 1 } };
	const size_t sizes[] = { 1 };

	sg_split_run(test, in, 2, 0, sizes, 1);
}

static void sg_split_t6_dthev2_aead_shape(struct kunit *test)
{
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 64 }, { 1, 0, 100 }, { 2, 0, 16 } };
	const size_t sizes[] = { 100 };

	sg_split_run(test, in, 3, 64, sizes, 1);
}

static void sg_split_t7_skip_plus_partial_midentry(struct kunit *test)
{
	const struct sg_split_in_spec in[] = { { 0, 0, 100 }, { 1, 0, 50 } };
	const size_t sizes[] = { 30 };

	sg_split_run(test, in, 2, 20, sizes, 1);
}

/* ========================================================================
 * Group C — trailing zero-size split.
 */
static void sg_split_t8_sizes_trailing_zero(struct kunit *test)
{
	const struct sg_split_in_spec in[] = { { 0, 0, 10 }, { 1, 0, 5 } };
	const size_t sizes[] = { 10, 0 };

	sg_split_run(test, in, 2, 0, sizes, 2);
}

static void sg_split_t8b_trailing_zero_no_input_left(struct kunit *test)
{
	/* First split consumes the entire input; the trailing zero-size split
	 * gets no input entry (nents == 0).  sg_split_phys()/sg_split_mapped()
	 * must not write out_sg[-1] for that empty split (out_sg would be
	 * ZERO_SIZE_PTR).  Distinct from T8, where a trailing input entry
	 * leaves the zero split with nents > 0.
	 */
	const struct sg_split_in_spec in[] = { { 0, 0, 10 } };
	const size_t sizes[] = { 10, 0 };

	sg_split_run(test, in, 1, 0, sizes, 2);
}

/* ========================================================================
 * Group D — boundary-position matrix (3 x 10-byte entries).
 */
static void sg_split_t9_mid_first_two_trailing(struct kunit *test)
{
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 } };
	const size_t sizes[] = { 5 };

	sg_split_run(test, in, 3, 0, sizes, 1);
}

static void sg_split_t10_edge_first_two_trailing(struct kunit *test)
{
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 } };
	const size_t sizes[] = { 10 };

	sg_split_run(test, in, 3, 0, sizes, 1);
}

static void sg_split_t11_mid_middle_one_trailing(struct kunit *test)
{
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 } };
	const size_t sizes[] = { 15 };

	sg_split_run(test, in, 3, 0, sizes, 1);
}

static void sg_split_t12_mid_last_no_trailing(struct kunit *test)
{
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 } };
	const size_t sizes[] = { 25 };

	sg_split_run(test, in, 3, 0, sizes, 1);
}

/* ========================================================================
 * Group E — skip-position variations.
 */
static void sg_split_t13_skip_at_entry_boundary(struct kunit *test)
{
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 } };
	const size_t sizes[] = { 5 };

	sg_split_run(test, in, 3, 10, sizes, 1);
}

static void sg_split_t14_skip_fastforward_partial(struct kunit *test)
{
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 } };
	const size_t sizes[] = { 5 };

	sg_split_run(test, in, 3, 12, sizes, 1);
}

/* ========================================================================
 * Group F — multi-split, mid-entry internal transition.
 */
static void sg_split_t15_two_splits_last_mid_trailing(struct kunit *test)
{
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 } };
	const size_t sizes[] = { 5, 10 };

	sg_split_run(test, in, 3, 0, sizes, 2);
}

static void sg_split_t16_two_splits_full_coverage_mid_first(struct kunit *test)
{
	const struct sg_split_in_spec in[] = { { 0, 0, 10 }, { 1, 0, 10 } };
	const size_t sizes[] = { 3, 17 };

	sg_split_run(test, in, 2, 0, sizes, 2);
}

/* ========================================================================
 * Group G — divergent CPU/DMA geometry (coalescing).  NEED arches only:
 * needs a separate dma_length field so dma_len != length is representable.
 */
static void sg_split_run_divergent(struct kunit *test,
				   const struct sg_split_in_spec *in_spec,
				   int n_in, off_t skip,
				   const size_t *sizes, int nb_splits,
				   const unsigned int *dma_seg, int n_dma)
{
#ifndef CONFIG_NEED_SG_DMA_LENGTH
	kunit_skip(test,
		   "divergent CPU/DMA geometry needs a separate dma_length field (NEED_SG_DMA_LENGTH)");
#else
	struct scatterlist *in_sgl;
	struct scatterlist *out[SG_SPLIT_MAX_SPLITS] = { NULL };
	int out_nents[SG_SPLIT_MAX_SPLITS] = { 0 };
	unsigned long cursor = (unsigned long)skip;
	dma_addr_t a = SG_SPLIT_DMA_BASE;
	u8 *oracle;
	int rc, k, i;

	KUNIT_ASSERT_LE(test, n_in, SG_SPLIT_MAX_IN_ENTRIES);
	KUNIT_ASSERT_LE(test, nb_splits, SG_SPLIT_MAX_SPLITS);

	in_sgl = sg_split_build_input(test, in_spec, n_in, &oracle);

	/* Coalesced DMA view on the first n_dma entries, contiguous IOVA. */
	for (i = 0; i < n_dma; i++) {
		sg_dma_address(&in_sgl[i]) = a;
		sg_dma_len(&in_sgl[i]) = dma_seg[i];
		a += dma_seg[i];
	}

	rc = sg_split(in_sgl, n_dma, skip, nb_splits, sizes,
		      out, out_nents, GFP_KERNEL);
	KUNIT_ASSERT_EQ_MSG(test, rc, 0, "divergent sg_split returned %d", rc);

	for (k = 0; k < nb_splits; k++) {
		KUNIT_ASSERT_NOT_NULL_MSG(test, out[k], "split[%d] NULL", k);
		/* CPU view describes the scattered pages (pass 1): its entry
		 * count is the end-marker count sg_nents(), not the (smaller)
		 * DMA count out_nents[k].
		 */
		sg_split_check_cpu(test, "divergent", k, out[k],
				   sg_nents(out[k]), sizes[k], oracle, cursor);
		/* DMA view describes the coalesced contiguous IOVA (pass 2). */
		sg_split_check_dma(test, "divergent", k, out[k], out_nents[k],
				   sizes[k], cursor);
		cursor += sizes[k];
	}

	for (k = 0; k < nb_splits; k++)
		kfree(out[k]);
#endif
}

static void sg_split_d1_coalesce_3to1(struct kunit *test)
{
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 4096 }, { 1, 0, 4096 }, { 2, 0, 4096 } };
	const size_t sizes[] = { 5000, 7288 };
	const unsigned int dma[] = { 12288 };

	sg_split_run_divergent(test, in, 3, 0, sizes, 2, dma, 1);
}

static void sg_split_d2_coalesce_3to2(struct kunit *test)
{
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 4096 }, { 1, 0, 4096 }, { 2, 0, 4096 } };
	const size_t sizes[] = { 6000, 6288 };
	const unsigned int dma[] = { 8192, 4096 };

	sg_split_run_divergent(test, in, 3, 0, sizes, 2, dma, 2);
}

static void sg_split_d3_coalesce_3to1_skip(struct kunit *test)
{
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 4096 }, { 1, 0, 4096 }, { 2, 0, 4096 } };
	const size_t sizes[] = { 5000 };
	const unsigned int dma[] = { 12288 };

	sg_split_run_divergent(test, in, 3, 1000, sizes, 1, dma, 1);
}

static struct kunit_case sg_split_test_cases[] = {
	KUNIT_CASE(sg_split_t1_single_entry_full),
	KUNIT_CASE(sg_split_t2_two_entries_full_coverage),
	KUNIT_CASE(sg_split_t3_two_entries_two_splits_full),
	KUNIT_CASE(sg_split_t4_edge_aligned_partial),
	KUNIT_CASE(sg_split_t5_minimal_repro),
	KUNIT_CASE(sg_split_t6_dthev2_aead_shape),
	KUNIT_CASE(sg_split_t7_skip_plus_partial_midentry),
	KUNIT_CASE(sg_split_t8_sizes_trailing_zero),
	KUNIT_CASE(sg_split_t8b_trailing_zero_no_input_left),
	KUNIT_CASE(sg_split_t9_mid_first_two_trailing),
	KUNIT_CASE(sg_split_t10_edge_first_two_trailing),
	KUNIT_CASE(sg_split_t11_mid_middle_one_trailing),
	KUNIT_CASE(sg_split_t12_mid_last_no_trailing),
	KUNIT_CASE(sg_split_t13_skip_at_entry_boundary),
	KUNIT_CASE(sg_split_t14_skip_fastforward_partial),
	KUNIT_CASE(sg_split_t15_two_splits_last_mid_trailing),
	KUNIT_CASE(sg_split_t16_two_splits_full_coverage_mid_first),
	KUNIT_CASE(sg_split_d1_coalesce_3to1),
	KUNIT_CASE(sg_split_d2_coalesce_3to2),
	KUNIT_CASE(sg_split_d3_coalesce_3to1_skip),
	{}
};

static struct kunit_suite sg_split_test_suite = {
	.name = "sg_split",
	.test_cases = sg_split_test_cases,
};

kunit_test_suite(sg_split_test_suite);
MODULE_DESCRIPTION("KUnit tests for lib/sg_split.c");
MODULE_LICENSE("GPL");
