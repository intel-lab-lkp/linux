// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for lib/sg_split.c.
 *
 * Each case sets up a deterministic input scatterlist (page + offset +
 * length per entry), calls sg_split, and verifies the two documented
 * post-conditions:
 *
 *   1. sum of out[k] entry lengths equals split_sizes[k]   (per-split size)
 *   2. the bytes walked from out[k] match input bytes
 *      [skip + sum(prev sizes) .. +sizes[k])               (per-split content)
 *
 * Backing memory is a position-unique byte pattern, so a content
 * mismatch identifies exactly which input region got mis-exposed.
 */

#include <kunit/test.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/string.h>

#define SG_SPLIT_MAX_IN_ENTRIES		8
#define SG_SPLIT_MAX_SPLITS		4
#define SG_SPLIT_PAGE_BYTES		4096

struct sg_split_in_spec {
	int		page;
	unsigned int	off;
	unsigned int	len;
};

static u8 sg_split_pattern(int page, unsigned int off)
{
	return (u8)(((page * 31u) + off * 17u + 0x5a) & 0xffu);
}

/*
 * Build the input SG, run sg_split, and verify both properties.
 * KUnit handles cleanup of kunit_kzalloc'd memory automatically;
 * we kfree the per-split output arrays ourselves.
 */
static void sg_split_run(struct kunit *test,
			 const struct sg_split_in_spec *in_spec, int n_in,
			 off_t skip,
			 const size_t *sizes, int nb_splits)
{
	u8 *backing, *oracle_buf;
	struct scatterlist *in_sgl;
	struct scatterlist *out[SG_SPLIT_MAX_SPLITS] = { NULL };
	int out_nents[SG_SPLIT_MAX_SPLITS] = { 0 };
	unsigned long oracle_cursor = (unsigned long)skip;
	size_t oracle_total = 0;
	int rc, k, i;

	KUNIT_ASSERT_LE(test, n_in, SG_SPLIT_MAX_IN_ENTRIES);
	KUNIT_ASSERT_LE(test, nb_splits, SG_SPLIT_MAX_SPLITS);

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
		oracle_total += in_spec[i].len;
	}

	/* Linear concatenation oracle for property 2. */
	oracle_buf = kunit_kzalloc(test, oracle_total, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, oracle_buf);
	{
		size_t op = 0;

		for (i = 0; i < n_in; i++) {
			memcpy(oracle_buf + op,
			       backing
				+ in_spec[i].page * SG_SPLIT_PAGE_BYTES
				+ in_spec[i].off,
			       in_spec[i].len);
			op += in_spec[i].len;
		}
	}

	rc = sg_split(in_sgl, n_in, skip, nb_splits, sizes,
		      out, out_nents, GFP_KERNEL);
	KUNIT_ASSERT_EQ_MSG(test, rc, 0, "sg_split returned %d", rc);

	for (k = 0; k < nb_splits; k++) {
		unsigned long sum = 0;
		struct scatterlist *sg;
		int j;
		u8 *got;
		size_t copied = 0;

		KUNIT_ASSERT_NOT_NULL_MSG(test, out[k],
					  "split[%d] is NULL", k);

		/* Property 1: per-split size. */
		for_each_sg(out[k], sg, out_nents[k], j)
			sum += sg->length;
		KUNIT_EXPECT_EQ_MSG(test, sum, sizes[k],
				    "P1 split[%d]: sum=%lu requested=%zu",
				    k, sum, sizes[k]);

		/* Property 2: per-split content. */
		got = kunit_kzalloc(test, sizes[k] + 1, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, got);
		for_each_sg(out[k], sg, out_nents[k], j) {
			size_t take = min_t(size_t, sg->length,
					    sizes[k] - copied);

			memcpy(got + copied, sg_virt(sg), take);
			copied += take;
			if (copied >= sizes[k])
				break;
		}
		KUNIT_EXPECT_EQ_MSG(test,
				    memcmp(got, oracle_buf + oracle_cursor,
					   min_t(size_t, copied, sizes[k])),
				    0,
				    "P2 split[%d]: bytes mismatch", k);

		oracle_cursor += sizes[k];
	}

	for (k = 0; k < nb_splits; k++)
		kfree(out[k]);
}

/* ========================================================================
 * Group A — full or edge-aligned coverage.  These must pass even on the
 * pre-patch kernel; they exercise the path the existing five upstream
 * callers all hit.
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
	/* Boundary at end of in[0]; partial coverage but edge-aligned. */
	const struct sg_split_in_spec in[] = { { 0, 0, 60 }, { 1, 0, 40 } };
	const size_t sizes[] = { 60 };

	sg_split_run(test, in, 2, 0, sizes, 1);
}

/* ========================================================================
 * Group B — partial coverage with mid-entry boundary.  These exercise
 * the contract clause "union of spans is a subrange of the original"
 * from f8bcbe62acd0.  They fail on the pre-patch kernel.
 */

static void sg_split_t5_minimal_repro(struct kunit *test)
{
	const struct sg_split_in_spec in[] = { { 0, 0, 2 }, { 1, 0, 1 } };
	const size_t sizes[] = { 1 };

	sg_split_run(test, in, 2, 0, sizes, 1);
}

static void sg_split_t6_dthev2_aead_shape(struct kunit *test)
{
	/* AEAD shape: [AAD || ciphertext || TAG]; skip=AAD, take cryptlen,
	 * leave TAG as trailing tail.  Edge-aligned in this layout, so
	 * even pre-patch the bug doesn't fire — included to pin that.
	 */
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 64 }, { 1, 0, 100 }, { 2, 0, 16 }
	};
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
 * Group C — sizes-containing-zero edge.  Pre-patch behaviour is to
 * produce a zero-length split and return success.  The break-condition
 * fix preserves this; a (rejected) earlier attempt at fixing the bug
 * via `if (!size) continue;` regressed this case to a crash.
 */

static void sg_split_t8_sizes_trailing_zero(struct kunit *test)
{
	const struct sg_split_in_spec in[] = { { 0, 0, 10 }, { 1, 0, 5 } };
	const size_t sizes[] = { 10, 0 };

	sg_split_run(test, in, 2, 0, sizes, 2);
}

/* ========================================================================
 * Group D — boundary-position matrix.  Using 3 input entries of 10 bytes
 * each, walk the (skip + sum(sizes)) boundary through each position to
 * confirm the trigger condition is "mid-entry AND has trailing input".
 */

static void sg_split_t9_mid_first_two_trailing(struct kunit *test)
{
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 }
	};
	const size_t sizes[] = { 5 };

	sg_split_run(test, in, 3, 0, sizes, 1);
}

static void sg_split_t10_edge_first_two_trailing(struct kunit *test)
{
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 }
	};
	const size_t sizes[] = { 10 };

	sg_split_run(test, in, 3, 0, sizes, 1);
}

static void sg_split_t11_mid_middle_one_trailing(struct kunit *test)
{
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 }
	};
	const size_t sizes[] = { 15 };

	sg_split_run(test, in, 3, 0, sizes, 1);
}

static void sg_split_t12_mid_last_no_trailing(struct kunit *test)
{
	/* Boundary mid-in[2] with NO trailing entries.  Even with the
	 * pre-patch dual-decrement overshoot, the for_each_sg loop ends
	 * before iter N+1 can corrupt state.  Confirms the trigger
	 * narrowness.
	 */
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 }
	};
	const size_t sizes[] = { 25 };

	sg_split_run(test, in, 3, 0, sizes, 1);
}

/* ========================================================================
 * Group E — skip-position variations.
 */

static void sg_split_t13_skip_at_entry_boundary(struct kunit *test)
{
	/* skip = exactly len(in[0]).  `skip > sglen` is strict, so the
	 * main body runs with len=0 in iter 1.  Iter 2 takes 5 from in[1];
	 * boundary mid-in[1], in[2] trailing.
	 */
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 }
	};
	const size_t sizes[] = { 5 };

	sg_split_run(test, in, 3, 10, sizes, 1);
}

static void sg_split_t14_skip_fastforward_partial(struct kunit *test)
{
	/* skip=12 → iter 1 fast-forwards (skip -= 10, continue); iter 2
	 * has effective skip=2, takes 5 from in[1] starting at byte 2;
	 * boundary at byte 17 = mid-in[1] with in[2] trailing.
	 */
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 }
	};
	const size_t sizes[] = { 5 };

	sg_split_run(test, in, 3, 12, sizes, 1);
}

/* ========================================================================
 * Group F — multi-split variations.
 */

static void sg_split_t15_two_splits_last_mid_trailing(struct kunit *test)
{
	/* sizes=[5,10] over [10,10,10]: first split mid-in[0], second
	 * split ends mid-in[1] with in[2] trailing.  Trigger on the LAST
	 * split.
	 */
	const struct sg_split_in_spec in[] = {
		{ 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 }
	};
	const size_t sizes[] = { 5, 10 };

	sg_split_run(test, in, 3, 0, sizes, 2);
}

static void sg_split_t16_two_splits_full_coverage_mid_first(struct kunit *test)
{
	/* sizes=[3,17] over [10,10]: first-split boundary mid-in[0],
	 * second-split boundary at end of in[1].  Full coverage.
	 */
	const struct sg_split_in_spec in[] = { { 0, 0, 10 }, { 1, 0, 10 } };
	const size_t sizes[] = { 3, 17 };

	sg_split_run(test, in, 2, 0, sizes, 2);
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
	KUNIT_CASE(sg_split_t9_mid_first_two_trailing),
	KUNIT_CASE(sg_split_t10_edge_first_two_trailing),
	KUNIT_CASE(sg_split_t11_mid_middle_one_trailing),
	KUNIT_CASE(sg_split_t12_mid_last_no_trailing),
	KUNIT_CASE(sg_split_t13_skip_at_entry_boundary),
	KUNIT_CASE(sg_split_t14_skip_fastforward_partial),
	KUNIT_CASE(sg_split_t15_two_splits_last_mid_trailing),
	KUNIT_CASE(sg_split_t16_two_splits_full_coverage_mid_first),
	{}
};

static struct kunit_suite sg_split_test_suite = {
	.name = "sg_split",
	.test_cases = sg_split_test_cases,
};

kunit_test_suite(sg_split_test_suite);

MODULE_DESCRIPTION("KUnit tests for lib/sg_split.c");
MODULE_LICENSE("GPL");
