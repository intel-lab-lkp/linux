// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the textsearch infrastructure.
 *
 * The cases below are run against every string-pattern algorithm registered
 * with lib/textsearch.c, so that all implementations are held to the same
 * interface contract.
 *
 * ts_fsm is deliberately not covered: fsm_init() consumes an array of
 * struct ts_fsm_token rather than a plain byte string, so it cannot share
 * these test vectors.
 */

#include <kunit/test.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/textsearch.h>

static const char * const ts_algo_names[] = { "kmp", "bm" };

static void ts_algo_desc(const char * const *algo, char *desc)
{
	strscpy(desc, *algo, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(ts_algo, ts_algo_names, ts_algo_desc);

/*
 * Build a configuration for the algorithm under test. Skips the case rather
 * than failing it when the algorithm is not registered, so that a kernel
 * built without, say, CONFIG_TEXTSEARCH_BM still reports cleanly.
 */
static struct ts_config *ts_conf_get(struct kunit *test, const char *pattern)
{
	const char *algo = *(const char * const *)test->param_value;
	struct ts_config *conf;

	conf = textsearch_prepare(algo, pattern, strlen(pattern),
				  GFP_KERNEL, TS_AUTOLOAD);
	if (IS_ERR(conf))
		kunit_skip(test, "algorithm \"%s\" not registered (%pe)",
			   algo, conf);

	return conf;
}

static void ts_find_middle(struct kunit *test)
{
	static const char text[] = "We dance the funky chicken";
	static const char pattern[] = "chicken";
	struct ts_config *conf = ts_conf_get(test, pattern);
	struct ts_state state;

	KUNIT_EXPECT_EQ(test,
			textsearch_find_continuous(conf, &state, text,
						   strlen(text)),
			strlen(text) - strlen(pattern));

	textsearch_destroy(conf);
}

static void ts_find_at_start(struct kunit *test)
{
	static const char text[] = "abcdefg";
	static const char pattern[] = "abc";
	struct ts_config *conf = ts_conf_get(test, pattern);
	struct ts_state state;

	KUNIT_EXPECT_EQ(test,
			textsearch_find_continuous(conf, &state, text,
						   strlen(text)),
			0);

	textsearch_destroy(conf);
}

static void ts_find_at_end(struct kunit *test)
{
	static const char text[] = "abcdefg";
	static const char pattern[] = "efg";
	struct ts_config *conf = ts_conf_get(test, pattern);
	struct ts_state state;

	KUNIT_EXPECT_EQ(test,
			textsearch_find_continuous(conf, &state, text,
						   strlen(text)),
			4);

	textsearch_destroy(conf);
}

static void ts_find_no_match(struct kunit *test)
{
	static const char text[] = "abcdefg";
	static const char pattern[] = "xyz";
	struct ts_config *conf = ts_conf_get(test, pattern);
	struct ts_state state;

	KUNIT_EXPECT_EQ(test,
			textsearch_find_continuous(conf, &state, text,
						   strlen(text)),
			UINT_MAX);

	textsearch_destroy(conf);
}

/*
 * textsearch_find() resets state->offset and textsearch_next() relies on the
 * algorithm having advanced it past the match it just reported. An algorithm
 * that leaves state->offset alone reports the same position forever.
 */
static void ts_next_advances(struct kunit *test)
{
	static const char text[] = "aaaa";
	static const char pattern[] = "aa";
	struct ts_config *conf = ts_conf_get(test, pattern);
	unsigned int pos, prev;
	struct ts_state state;
	int i;

	pos = textsearch_find_continuous(conf, &state, text, strlen(text));
	KUNIT_ASSERT_EQ(test, pos, 0);

	/* Bounded so that a non-advancing algorithm fails instead of hanging. */
	for (i = 0; i < 8; i++) {
		prev = pos;

		pos = textsearch_next(conf, &state);
		if (pos == UINT_MAX)
			break;

		KUNIT_ASSERT_GT_MSG(test, pos, prev,
				    "textsearch_next() reported %u after %u; it must advance past the previous match",
				    pos, prev);
	}

	KUNIT_EXPECT_EQ_MSG(test, pos, UINT_MAX,
			    "search did not terminate within 8 iterations");

	textsearch_destroy(conf);
}

/* The full set of matches must be reported exactly once, in order. */
static void ts_next_finds_all(struct kunit *test)
{
	static const char text[] = "xxABxxABxx";
	static const char pattern[] = "AB";
	static const unsigned int expect[] = { 2, 6 };
	struct ts_config *conf = ts_conf_get(test, pattern);
	struct ts_state state;
	unsigned int pos;
	int i;

	pos = textsearch_find_continuous(conf, &state, text, strlen(text));

	for (i = 0; i < ARRAY_SIZE(expect); i++) {
		KUNIT_ASSERT_EQ_MSG(test, pos, expect[i],
				    "match %d: expected offset %u, got %u",
				    i, expect[i], pos);
		pos = textsearch_next(conf, &state);
	}

	KUNIT_EXPECT_EQ_MSG(test, pos, UINT_MAX,
			    "expected exactly %zu matches", ARRAY_SIZE(expect));

	textsearch_destroy(conf);
}

/*
 * A block source that hands the text out in fixed-size chunks, so that the
 * algorithms are driven the way a non-linear skb drives them. Boundaries sit
 * at multiples of @chunk, mirroring skb_seq_read().
 */
struct ts_chunk_state {
	const char	*data;
	unsigned int	len;
	unsigned int	chunk;
};

static unsigned int ts_get_chunk(unsigned int consumed, const u8 **dst,
				 struct ts_config *conf,
				 struct ts_state *state)
{
	struct ts_chunk_state *cs = (struct ts_chunk_state *)state->cb;
	unsigned int end;

	if (consumed >= cs->len)
		return 0;

	end = (consumed / cs->chunk + 1) * cs->chunk;
	if (end > cs->len)
		end = cs->len;

	*dst = (const u8 *)cs->data + consumed;
	return end - consumed;
}

static unsigned int ts_find_chunked(struct ts_config *conf,
				    struct ts_state *state, const char *text,
				    unsigned int len, unsigned int chunk)
{
	struct ts_chunk_state *cs = (struct ts_chunk_state *)state->cb;

	BUILD_BUG_ON(sizeof(struct ts_chunk_state) > sizeof(state->cb));

	conf->get_next_block = ts_get_chunk;
	cs->data = text;
	cs->len = len;
	cs->chunk = chunk;

	return textsearch_find(conf, state);
}

/*
 * A match that lies entirely inside one block must be found no matter how the
 * text is split up. Matches spanning a block boundary are deliberately not
 * covered: ts_bm documents those as missed, ts_kmp finds them.
 */
static void ts_blocks_match_within_block(struct kunit *test)
{
	static const char text[] = "xxxxABCDxxxx";
	static const char pattern[] = "ABCD";
	struct ts_config *conf = ts_conf_get(test, pattern);
	struct ts_state state;

	/* chunk 4 puts "ABCD" exactly in the second block */
	KUNIT_EXPECT_EQ_MSG(test,
			    ts_find_chunked(conf, &state, text,
					    strlen(text), 4),
			    4, "match inside a single block must be found");

	/* one block for the whole text must agree with the chunked run */
	KUNIT_EXPECT_EQ(test,
			ts_find_chunked(conf, &state, text, strlen(text),
					strlen(text)),
			4);

	textsearch_destroy(conf);
}

/* Iterating over a chunked buffer must terminate and must make progress. */
static void ts_blocks_iteration_terminates(struct kunit *test)
{
	static const char text[] = "abababababab";
	static const char pattern[] = "ab";
	struct ts_config *conf = ts_conf_get(test, pattern);
	unsigned int chunk, pos, prev;
	struct ts_state state;
	int i;

	for (chunk = 1; chunk <= strlen(text); chunk++) {
		pos = ts_find_chunked(conf, &state, text, strlen(text), chunk);

		for (i = 0; i < 32 && pos != UINT_MAX; i++) {
			KUNIT_ASSERT_LE_MSG(test, pos + strlen(pattern),
					    strlen(text),
					    "chunk %u: reported match at %u runs past the text",
					    chunk, pos);
			KUNIT_ASSERT_MEMEQ_MSG(test, text + pos, pattern,
					       strlen(pattern),
					       "chunk %u: offset %u is not a real match",
					       chunk, pos);
			prev = pos;
			pos = textsearch_next(conf, &state);
			if (pos == UINT_MAX)
				break;
			KUNIT_ASSERT_GT_MSG(test, pos, prev,
					    "chunk %u: reported %u after %u",
					    chunk, pos, prev);
		}

		KUNIT_EXPECT_EQ_MSG(test, pos, UINT_MAX,
				    "chunk %u: search did not terminate", chunk);
	}

	textsearch_destroy(conf);
}

static void ts_get_pattern(struct kunit *test)
{
	static const char pattern[] = "chicken";
	struct ts_config *conf = ts_conf_get(test, pattern);

	KUNIT_EXPECT_EQ(test, textsearch_get_pattern_len(conf),
			strlen(pattern));
	KUNIT_EXPECT_MEMEQ(test, textsearch_get_pattern(conf), pattern,
			   strlen(pattern));

	textsearch_destroy(conf);
}

/* textsearch_prepare() documents -EINVAL for a zero-length pattern. */
static void ts_prepare_zero_len(struct kunit *test)
{
	const char *algo = *(const char * const *)test->param_value;
	struct ts_config *conf;

	conf = textsearch_prepare(algo, "", 0, GFP_KERNEL, TS_AUTOLOAD);
	KUNIT_ASSERT_TRUE(test, IS_ERR(conf));
	KUNIT_EXPECT_EQ(test, PTR_ERR(conf), -EINVAL);
}

static struct kunit_case textsearch_test_cases[] = {
	KUNIT_CASE_PARAM(ts_find_middle, ts_algo_gen_params),
	KUNIT_CASE_PARAM(ts_find_at_start, ts_algo_gen_params),
	KUNIT_CASE_PARAM(ts_find_at_end, ts_algo_gen_params),
	KUNIT_CASE_PARAM(ts_find_no_match, ts_algo_gen_params),
	KUNIT_CASE_PARAM(ts_next_advances, ts_algo_gen_params),
	KUNIT_CASE_PARAM(ts_next_finds_all, ts_algo_gen_params),
	KUNIT_CASE_PARAM(ts_blocks_match_within_block, ts_algo_gen_params),
	KUNIT_CASE_PARAM(ts_blocks_iteration_terminates, ts_algo_gen_params),
	KUNIT_CASE_PARAM(ts_get_pattern, ts_algo_gen_params),
	KUNIT_CASE_PARAM(ts_prepare_zero_len, ts_algo_gen_params),
	{}
};

static struct kunit_suite textsearch_test_suite = {
	.name = "textsearch",
	.test_cases = textsearch_test_cases,
};

kunit_test_suite(textsearch_test_suite);

MODULE_DESCRIPTION("KUnit tests for the textsearch infrastructure");
MODULE_LICENSE("GPL");
