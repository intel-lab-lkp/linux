// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sound core KUnit test
 * Author: Ivan Orlov <ivan.orlov0322@gmail.com>
 */

#include <kunit/test.h>
#include <linux/ctype.h>
#include <sound/core.h>
#include <sound/pcm.h>

#define SILENCE_BUFFER_MAX_FRAMES 260
#define SILENCE_BUFFER_SIZE (sizeof(u64) * SILENCE_BUFFER_MAX_FRAMES)
#define SILENCE(...) { __VA_ARGS__ }
#define DEFINE_FORMAT(fmt, pbits, wd, endianness, signd, silence_arr) {		\
	.format = SNDRV_PCM_FORMAT_##fmt, .physical_bits = pbits,		\
	.width = wd, .le = endianness, .sd = signd, .silence = silence_arr,	\
	.name = #fmt,								\
}

#define WRONG_FORMAT_1 (SNDRV_PCM_FORMAT_LAST + 1)
#define WRONG_FORMAT_2 -1

#define VALID_NAME "ValidName"
#define NAME_W_SPEC_CHARS "In%v@1id name"
#define NAME_W_SPACE "Test name"
#define NAME_W_SPACE_REMOVED "Testname"

#define TEST_FIRST_COMPONENT "Component1"
#define TEST_SECOND_COMPONENT "Component2"

struct snd_format_test_data {
	snd_pcm_format_t format;
	int physical_bits;
	int width;
	int le;
	int sd;
	unsigned char silence[8];
	unsigned char *name;
};

struct avail_test_data {
	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t hw_ptr;
	snd_pcm_uframes_t appl_ptr;
	snd_pcm_uframes_t expected_avail;
};

static const struct snd_format_test_data valid_fmt[] = {
	DEFINE_FORMAT(S8, 8, 8, -1, 1, SILENCE()),
	DEFINE_FORMAT(U8, 8, 8, -1, 0, SILENCE(0x80)),
	DEFINE_FORMAT(S16_LE, 16, 16, 1, 1, SILENCE()),
	DEFINE_FORMAT(S16_BE, 16, 16, 0, 1, SILENCE()),
	DEFINE_FORMAT(U16_LE, 16, 16, 1, 0, SILENCE(0x00, 0x80)),
	DEFINE_FORMAT(U16_BE, 16, 16, 0, 0, SILENCE(0x80, 0x00)),
	DEFINE_FORMAT(S24_LE, 32, 24, 1, 1, SILENCE()),
	DEFINE_FORMAT(S24_BE, 32, 24, 0, 1, SILENCE()),
	DEFINE_FORMAT(U24_LE, 32, 24, 1, 0, SILENCE(0x00, 0x00, 0x80)),
	DEFINE_FORMAT(U24_BE, 32, 24, 0, 0, SILENCE(0x00, 0x80, 0x00, 0x00)),
	DEFINE_FORMAT(S32_LE, 32, 32, 1, 1, SILENCE()),
	DEFINE_FORMAT(S32_BE, 32, 32, 0, 1, SILENCE()),
	DEFINE_FORMAT(U32_LE, 32, 32, 1, 0, SILENCE(0x00, 0x00, 0x00, 0x80)),
	DEFINE_FORMAT(U32_BE, 32, 32, 0, 0, SILENCE(0x80, 0x00, 0x00, 0x00)),
	DEFINE_FORMAT(FLOAT_LE, 32, 32, 1, -1, SILENCE()),
	DEFINE_FORMAT(FLOAT_BE, 32, 32, 0, -1, SILENCE()),
	DEFINE_FORMAT(FLOAT64_LE, 64, 64, 1, -1, SILENCE()),
	DEFINE_FORMAT(FLOAT64_BE, 64, 64, 0, -1, SILENCE()),
	DEFINE_FORMAT(IEC958_SUBFRAME_LE, 32, 32, 1, -1, SILENCE()),
	DEFINE_FORMAT(IEC958_SUBFRAME_BE, 32, 32, 0, -1, SILENCE()),
	DEFINE_FORMAT(MU_LAW, 8, 8, -1, -1, SILENCE(0x7f)),
	DEFINE_FORMAT(A_LAW, 8, 8, -1, -1, SILENCE(0x55)),
	DEFINE_FORMAT(IMA_ADPCM, 4, 4, -1, -1, SILENCE()),
	DEFINE_FORMAT(G723_24, 3, 3, -1, -1, SILENCE()),
	DEFINE_FORMAT(G723_40, 5, 5, -1, -1, SILENCE()),
	DEFINE_FORMAT(DSD_U8, 8, 8, 1, 0, SILENCE(0x69)),
	DEFINE_FORMAT(DSD_U16_LE, 16, 16, 1, 0, SILENCE(0x69, 0x69)),
	DEFINE_FORMAT(DSD_U32_LE, 32, 32, 1, 0, SILENCE(0x69, 0x69, 0x69, 0x69)),
	DEFINE_FORMAT(DSD_U16_BE, 16, 16, 0, 0, SILENCE(0x69, 0x69)),
	DEFINE_FORMAT(DSD_U32_BE, 32, 32, 0, 0, SILENCE(0x69, 0x69, 0x69, 0x69)),
	DEFINE_FORMAT(S20_LE, 32, 20, 1, 1, SILENCE()),
	DEFINE_FORMAT(S20_BE, 32, 20, 0, 1, SILENCE()),
	DEFINE_FORMAT(U20_LE, 32, 20, 1, 0, SILENCE(0x00, 0x00, 0x08, 0x00)),
	DEFINE_FORMAT(U20_BE, 32, 20, 0, 0, SILENCE(0x00, 0x08, 0x00, 0x00)),
	DEFINE_FORMAT(S24_3LE, 24, 24, 1, 1, SILENCE()),
	DEFINE_FORMAT(S24_3BE, 24, 24, 0, 1, SILENCE()),
	DEFINE_FORMAT(U24_3LE, 24, 24, 1, 0, SILENCE(0x00, 0x00, 0x80)),
	DEFINE_FORMAT(U24_3BE, 24, 24, 0, 0, SILENCE(0x80, 0x00, 0x00)),
	DEFINE_FORMAT(S20_3LE, 24, 20, 1, 1, SILENCE()),
	DEFINE_FORMAT(S20_3BE, 24, 20, 0, 1, SILENCE()),
	DEFINE_FORMAT(U20_3LE, 24, 20, 1, 0, SILENCE(0x00, 0x00, 0x08)),
	DEFINE_FORMAT(U20_3BE, 24, 20, 0, 0, SILENCE(0x08, 0x00, 0x00)),
	DEFINE_FORMAT(S18_3LE, 24, 18, 1, 1, SILENCE()),
	DEFINE_FORMAT(S18_3BE, 24, 18, 0, 1, SILENCE()),
	DEFINE_FORMAT(U18_3LE, 24, 18, 1, 0, SILENCE(0x00, 0x00, 0x02)),
	DEFINE_FORMAT(U18_3BE, 24, 18, 0, 0, SILENCE(0x02, 0x00, 0x00)),
	DEFINE_FORMAT(G723_24_1B, 8, 3, -1, -1, SILENCE()),
	DEFINE_FORMAT(G723_40_1B, 8, 5, -1, -1, SILENCE()),
};

static void test_phys_format_size(struct kunit *test)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(valid_fmt); i++) {
		KUNIT_EXPECT_EQ(test, snd_pcm_format_physical_width(valid_fmt[i].format),
				valid_fmt[i].physical_bits);
	}

	KUNIT_EXPECT_EQ(test, snd_pcm_format_physical_width(WRONG_FORMAT_1), -EINVAL);
	KUNIT_EXPECT_EQ(test, snd_pcm_format_physical_width(WRONG_FORMAT_2), -EINVAL);
}

static void test_format_width(struct kunit *test)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(valid_fmt); i++) {
		KUNIT_EXPECT_EQ(test, snd_pcm_format_width(valid_fmt[i].format),
				valid_fmt[i].width);
	}

	KUNIT_EXPECT_EQ(test, snd_pcm_format_width(WRONG_FORMAT_1), -EINVAL);
	KUNIT_EXPECT_EQ(test, snd_pcm_format_width(WRONG_FORMAT_2), -EINVAL);
}

static void test_format_signed(struct kunit *test)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(valid_fmt); i++) {
		KUNIT_EXPECT_EQ(test, snd_pcm_format_signed(valid_fmt[i].format),
				valid_fmt[i].sd < 0 ? -EINVAL : valid_fmt[i].sd);
		KUNIT_EXPECT_EQ(test, snd_pcm_format_unsigned(valid_fmt[i].format),
				valid_fmt[i].sd < 0 ? -EINVAL : 1 - valid_fmt[i].sd);
	}

	KUNIT_EXPECT_EQ(test, snd_pcm_format_width(WRONG_FORMAT_1), -EINVAL);
	KUNIT_EXPECT_EQ(test, snd_pcm_format_width(WRONG_FORMAT_2), -EINVAL);
}

static void test_format_endianness(struct kunit *test)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(valid_fmt); i++) {
		KUNIT_EXPECT_EQ(test, snd_pcm_format_little_endian(valid_fmt[i].format),
				valid_fmt[i].le < 0 ? -EINVAL : valid_fmt[i].le);
		KUNIT_EXPECT_EQ(test, snd_pcm_format_big_endian(valid_fmt[i].format),
				valid_fmt[i].le < 0 ? -EINVAL : 1 - valid_fmt[i].le);
	}

	KUNIT_EXPECT_EQ(test, snd_pcm_format_little_endian(WRONG_FORMAT_1), -EINVAL);
	KUNIT_EXPECT_EQ(test, snd_pcm_format_little_endian(WRONG_FORMAT_2), -EINVAL);
	KUNIT_EXPECT_EQ(test, snd_pcm_format_big_endian(WRONG_FORMAT_1), -EINVAL);
	KUNIT_EXPECT_EQ(test, snd_pcm_format_big_endian(WRONG_FORMAT_2), -EINVAL);
}

static void _test_fill_silence(struct kunit *test, const struct snd_format_test_data *data,
			       u8 *buffer, size_t samples_count)
{
	size_t sample_bytes = data->physical_bits >> 3;
	u32 i;

	KUNIT_ASSERT_EQ(test, snd_pcm_format_set_silence(data->format, buffer, samples_count), 0);
	for (i = 0; i < samples_count * sample_bytes; i++)
		KUNIT_EXPECT_EQ(test, buffer[i], data->silence[i % sample_bytes]);
}

static void test_format_fill_silence(struct kunit *test)
{
	static const u32 buf_samples[] = { 10, 20, 32, 64, 129, SILENCE_BUFFER_MAX_FRAMES };
	u8 *buffer;
	u32 i, j;

	buffer = kunit_kzalloc(test, SILENCE_BUFFER_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buffer);

	for (i = 0; i < ARRAY_SIZE(buf_samples); i++) {
		for (j = 0; j < ARRAY_SIZE(valid_fmt); j++)
			_test_fill_silence(test, &valid_fmt[j], buffer, buf_samples[i]);
	}

	KUNIT_EXPECT_EQ(test, snd_pcm_format_set_silence(WRONG_FORMAT_1, buffer, 20), -EINVAL);
	KUNIT_EXPECT_EQ(test, snd_pcm_format_set_silence(SNDRV_PCM_FORMAT_LAST, buffer, 0), 0);
}

static snd_pcm_uframes_t calculate_boundary(snd_pcm_uframes_t buffer_size)
{
	snd_pcm_uframes_t boundary = buffer_size;

	while (boundary * 2 <= 0x7fffffffUL - buffer_size)
		boundary *= 2;
	return boundary;
}

static const struct avail_test_data p_avail_data[] = {
	/* buf_size + hw_ptr < appl_ptr => avail = buf_size + hw_ptr - appl_ptr + boundary */
	{ 128, 1000, 1129, 1073741824UL - 1 },
	/*
	 * buf_size + hw_ptr - appl_ptr >= boundary =>
	 * => avail = buf_size + hw_ptr - appl_ptr - boundary
	 */
	{ 128, 1073741824UL, 10, 118 },
	/* standard case: avail = buf_size + hw_ptr - appl_ptr */
	{ 128, 1000, 1001, 127 },
};

static void test_playback_avail(struct kunit *test)
{
	struct snd_pcm_runtime *r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
	u32 i;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, r);

	r->status = kunit_kzalloc(test, sizeof(*r->status), GFP_KERNEL);
	r->control = kunit_kzalloc(test, sizeof(*r->control), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, r->status);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, r->control);

	for (i = 0; i < ARRAY_SIZE(p_avail_data); i++) {
		r->buffer_size = p_avail_data[i].buffer_size;
		r->boundary = calculate_boundary(r->buffer_size);
		r->status->hw_ptr = p_avail_data[i].hw_ptr;
		r->control->appl_ptr = p_avail_data[i].appl_ptr;
		KUNIT_EXPECT_EQ(test, snd_pcm_playback_avail(r), p_avail_data[i].expected_avail);
	}
}

static const struct avail_test_data c_avail_data[] = {
	/* hw_ptr - appl_ptr < 0 => avail = hw_ptr - appl_ptr + boundary */
	{ 128, 1000, 1001, 1073741824UL - 1 },
	/* standard case: avail = hw_ptr - appl_ptr */
	{ 128, 1001, 1000, 1 },
};

static void test_capture_avail(struct kunit *test)
{
	struct snd_pcm_runtime *r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
	u32 i;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, r);

	r->status = kunit_kzalloc(test, sizeof(*r->status), GFP_KERNEL);
	r->control = kunit_kzalloc(test, sizeof(*r->control), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, r->status);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, r->control);

	for (i = 0; i < ARRAY_SIZE(c_avail_data); i++) {
		r->buffer_size = c_avail_data[i].buffer_size;
		r->boundary = calculate_boundary(r->buffer_size);
		r->status->hw_ptr = c_avail_data[i].hw_ptr;
		r->control->appl_ptr = c_avail_data[i].appl_ptr;
		KUNIT_EXPECT_EQ(test, snd_pcm_capture_avail(r), c_avail_data[i].expected_avail);
	}
}

static void test_card_set_id(struct kunit *test)
{
	struct snd_card *card = kunit_kzalloc(test, sizeof(*card), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, card);

	snd_card_set_id(card, VALID_NAME);
	KUNIT_EXPECT_STREQ(test, card->id, VALID_NAME);

	/* clear the first id character so we can set it again */
	card->id[0] = '\0';
	snd_card_set_id(card, NAME_W_SPEC_CHARS);
	KUNIT_EXPECT_STRNEQ(test, card->id, NAME_W_SPEC_CHARS);

	card->id[0] = '\0';
	snd_card_set_id(card, NAME_W_SPACE);
	kunit_info(test, "%s", card->id);
	KUNIT_EXPECT_STREQ(test, card->id, NAME_W_SPACE_REMOVED);
}

static void test_pcm_format_name(struct kunit *test)
{
	u32 i;
	const char *name;

	for (i = 0; i < ARRAY_SIZE(valid_fmt); i++) {
		name = snd_pcm_format_name(valid_fmt[i].format);
		KUNIT_ASSERT_NOT_NULL_MSG(test, name, "Don't have name for %s", valid_fmt[i].name);
		KUNIT_EXPECT_STREQ(test, name, valid_fmt[i].name);
	}

	KUNIT_ASSERT_STREQ(test, snd_pcm_format_name(WRONG_FORMAT_1), "Unknown");
	KUNIT_ASSERT_STREQ(test, snd_pcm_format_name(WRONG_FORMAT_2), "Unknown");
}

static void test_card_add_component(struct kunit *test)
{
	struct snd_card *card = kunit_kzalloc(test, sizeof(*card), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, card);

	snd_component_add(card, TEST_FIRST_COMPONENT);
	KUNIT_ASSERT_STREQ(test, card->components, TEST_FIRST_COMPONENT);

	snd_component_add(card, TEST_SECOND_COMPONENT);
	KUNIT_ASSERT_STREQ(test, card->components, TEST_FIRST_COMPONENT " " TEST_SECOND_COMPONENT);
}

/* snd_utf8_strscpy() has to behave exactly like strscpy() for ASCII and only
 * differ where a multi-byte sequence would be cut.
 */
#define STRSCPY_UMLAUT		"\xc3\xa4"	/* 2 bytes */
#define STRSCPY_HIRAGANA	"\xe3\x81\x82"	/* 3 bytes */

static void test_utf8_strscpy(struct kunit *test)
{
	char dst[8], ref[8];
	size_t len;

	/* fits: same result as strscpy(), NUL terminated */
	len = snd_utf8_strscpy(dst, "abc", sizeof(dst));
	KUNIT_EXPECT_EQ(test, len, 3);
	KUNIT_EXPECT_STREQ(test, dst, "abc");

	/* exactly fills the buffer */
	len = snd_utf8_strscpy(dst, "abcdefg", sizeof(dst));
	KUNIT_EXPECT_EQ(test, len, sizeof(dst) - 1);
	KUNIT_EXPECT_STREQ(test, dst, "abcdefg");

	/* too long and pure ASCII: identical to strscpy() */
	len = snd_utf8_strscpy(dst, "abcdefghij", sizeof(dst));
	strscpy(ref, "abcdefghij", sizeof(ref));
	KUNIT_EXPECT_EQ(test, len, sizeof(dst) - 1);
	KUNIT_EXPECT_STREQ(test, dst, ref);

	/* 2-byte characters: three fit in seven bytes, the fourth has to be
	 * dropped whole rather than halved
	 */
	len = snd_utf8_strscpy(dst, STRSCPY_UMLAUT STRSCPY_UMLAUT
				    STRSCPY_UMLAUT STRSCPY_UMLAUT,
			       sizeof(dst));
	KUNIT_EXPECT_EQ(test, len, 6);
	KUNIT_EXPECT_STREQ(test, dst, STRSCPY_UMLAUT STRSCPY_UMLAUT
				      STRSCPY_UMLAUT);

	/* 3-byte characters: two fit, the third would be cut */
	len = snd_utf8_strscpy(dst, STRSCPY_HIRAGANA STRSCPY_HIRAGANA
				    STRSCPY_HIRAGANA,
			       sizeof(dst));
	KUNIT_EXPECT_EQ(test, len, 6);
	KUNIT_EXPECT_STREQ(test, dst, STRSCPY_HIRAGANA STRSCPY_HIRAGANA);

	/* not UTF-8 at all: a run of continuation bytes longer than any
	 * sequence, so the plain strscpy() cut stands rather than eating the
	 * whole string
	 */
	len = snd_utf8_strscpy(dst, "\x80\x80\x80\x80\x80\x80\x80\x80\x80",
			       sizeof(dst));
	KUNIT_EXPECT_EQ(test, len, sizeof(dst) - 1);

	/* zero-sized destination must not be written to */
	KUNIT_EXPECT_EQ(test, snd_utf8_strscpy(dst, "abc", 0), 0);
}

/* The UTF-8 test names are spelled out as escapes on purpose: the bytes are
 * what is under test, and the file itself stays plain ASCII.  Literals are
 * split where a hex escape is followed by a hex digit, since \x escapes are
 * greedy: "\xe2\x80\xaedef" would parse as \xe2 \x80 \xaed.
 */
#define UTF8_LATIN_NAME		"M\xc3\xbcller Audio"		/* Mueller with umlaut */
#define UTF8_CJK_NAME		"\xe3\x81\x82\xe9\x9f\xb3"	/* hiragana a + kanji */
#define UTF8_ASTRAL_NAME	"\xf0\x9f\x8e\xb5 Audio"	/* musical note + text */
#define UTF8_UNTRIMMED_NAME	" \t" UTF8_LATIN_NAME " \n"
#define UTF8_ONLY_SPACES	"  \t\r\n "
#define UTF8_REPLACEMENT	"\xef\xbf\xbd"			/* U+FFFD */

#define UTF8_UMLAUT		"\xc3\xa4"			/* 2 bytes */
#define UTF8_HIRAGANA		"\xe3\x81\x82"			/* 3 bytes */

/* Malformed input.  None of it is rejected any more; each bad byte becomes one
 * U+FFFD, so that a caller is never pushed back to the unfiltered ASCII field.
 */
static const struct {
	const char *in;
	const char *out;
} sanitised_names[] = {
	/* truncated 2-byte start */
	{ "abc\xc3",			"abc" UTF8_REPLACEMENT },
	/* stray continuation byte */
	{ "abc\x80",			"abc" UTF8_REPLACEMENT },
	/* overlong encoding of '/': both bytes are bad */
	{ "abc\xc0\xaf",		"abc" UTF8_REPLACEMENT UTF8_REPLACEMENT },
	/* U+D800, a surrogate half: three bad bytes */
	{ "abc\xed\xa0\x80",		"abc" UTF8_REPLACEMENT UTF8_REPLACEMENT
					UTF8_REPLACEMENT },
	/* beyond U+10FFFF */
	{ "abc\xf5\x80\x80\x80",	"abc" UTF8_REPLACEMENT UTF8_REPLACEMENT
					UTF8_REPLACEMENT UTF8_REPLACEMENT },
	/* broken continuation: the 'A' after it is valid and survives */
	{ "abc\xe3\x81" "A",		"abc" UTF8_REPLACEMENT UTF8_REPLACEMENT "A" },
	/* C0 control in the middle becomes a space */
	{ "abc\ndef",			"abc def" },
	/* U+0085 NEL, a C1 control */
	{ "abc\xc2\x85" "def",		"abc def" },
	/* ANSI escape: the ESC is what makes it dangerous */
	{ "abc\x1b[31mdef",		"abc [31mdef" },
	/* U+2028 LINE SEPARATOR */
	{ "abc\xe2\x80\xa8" "def",	"abc def" },
};

/* Code points the kernel deliberately does NOT touch.  They are a real
 * spoofing vector, but catching them needs the Unicode character properties,
 * which live in user space.  This test pins the documented behaviour so that
 * nobody mistakes it for an oversight.
 */
static const char * const passthrough_names[] = {
	"Audio\xd8\x9c" "Device",	/* U+061C ARABIC LETTER MARK */
	"Audio\xe2\x80\xae" "Device",	/* U+202E RIGHT-TO-LEFT OVERRIDE */
	"Audio\xe2\x81\xaa" "Device",	/* U+206A INHIBIT SYMMETRIC SWAPPING */
	"Au\xc2\xad" "dio",		/* U+00AD SOFT HYPHEN */
	"Audio\xef\xb8\x8f",		/* U+FE0F VARIATION SELECTOR-16 */
	"Audio\xf3\xa0\x80\x81",	/* U+E0001 LANGUAGE TAG */
	"Audio\xef\xbf\xbf",		/* U+FFFF non-character */
};

/* Build @repeat copies of @seq, NUL terminated. */
static char *utf8_repeat(struct kunit *test, const char *seq, size_t repeat)
{
	size_t seq_len = strlen(seq);
	char *buf;
	size_t i;

	buf = kunit_kzalloc(test, seq_len * repeat + 1, GFP_KERNEL);
	if (!buf)
		return NULL;
	for (i = 0; i < repeat; i++)
		memcpy(buf + i * seq_len, seq, seq_len);
	return buf;
}

static void test_card_full_names(struct kunit *test)
{
	struct snd_card *card = kunit_kzalloc(test, sizeof(*card), GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, card);

	/* an unset field stays NULL, which means "fall back to ASCII" */
	KUNIT_EXPECT_NULL(test, card->full_shortname);
	KUNIT_EXPECT_NULL(test, card->full_longname);
	KUNIT_EXPECT_NULL(test, card->full_mixername);

	KUNIT_EXPECT_EQ(test, 0, snd_card_set_full_shortname(card, UTF8_LATIN_NAME));
	KUNIT_EXPECT_STREQ(test, card->full_shortname, UTF8_LATIN_NAME);

	KUNIT_EXPECT_EQ(test, 0, snd_card_set_full_longname(card, UTF8_CJK_NAME));
	KUNIT_EXPECT_STREQ(test, card->full_longname, UTF8_CJK_NAME);

	KUNIT_EXPECT_EQ(test, 0, snd_card_set_full_mixername(card, UTF8_ASTRAL_NAME));
	KUNIT_EXPECT_STREQ(test, card->full_mixername, UTF8_ASTRAL_NAME);

	/* setting it again replaces the previous value */
	KUNIT_EXPECT_EQ(test, 0, snd_card_set_full_shortname(card, UTF8_CJK_NAME));
	KUNIT_EXPECT_STREQ(test, card->full_shortname, UTF8_CJK_NAME);

	/* NULL clears the field back to "unset" */
	KUNIT_EXPECT_EQ(test, 0, snd_card_set_full_shortname(card, NULL));
	KUNIT_EXPECT_NULL(test, card->full_shortname);

	snd_card_set_full_longname(card, NULL);
	snd_card_set_full_mixername(card, NULL);
}

/* A UTF-8 field is only worth an allocation when it carries more than the
 * ASCII field next to it; otherwise the documented fallback says the same.
 */
static void test_card_utf8_names_same_as_ascii(struct kunit *test)
{
	struct snd_card *card = kunit_kzalloc(test, sizeof(*card), GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, card);

	strscpy(card->shortname, VALID_NAME);

	KUNIT_EXPECT_EQ(test, 0, snd_card_set_full_shortname(card, VALID_NAME));
	KUNIT_EXPECT_NULL(test, card->full_shortname);

	/* the same name with padding sanitises to the ASCII one, so still unset */
	KUNIT_EXPECT_EQ(test, 0, snd_card_set_full_shortname(card, " " VALID_NAME "\t"));
	KUNIT_EXPECT_NULL(test, card->full_shortname);

	/* one that differs is kept */
	KUNIT_EXPECT_EQ(test, 0, snd_card_set_full_shortname(card, UTF8_LATIN_NAME));
	KUNIT_EXPECT_STREQ(test, card->full_shortname, UTF8_LATIN_NAME);

	snd_card_set_full_shortname(card, NULL);
}

static void test_card_utf8_names_sanitised(struct kunit *test)
{
	struct snd_card *card = kunit_kzalloc(test, sizeof(*card), GFP_KERNEL);
	size_t i;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, card);

	for (i = 0; i < ARRAY_SIZE(sanitised_names); i++) {
		KUNIT_EXPECT_EQ_MSG(test, 0,
				    snd_card_set_full_shortname(card, sanitised_names[i].in),
				    "name %zu was rejected", i);
		KUNIT_EXPECT_STREQ_MSG(test, card->full_shortname,
				       sanitised_names[i].out,
				       "name %zu came out wrong", i);
	}

	snd_card_set_full_shortname(card, NULL);
}

static void test_card_utf8_names_passthrough(struct kunit *test)
{
	struct snd_card *card = kunit_kzalloc(test, sizeof(*card), GFP_KERNEL);
	size_t i;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, card);

	for (i = 0; i < ARRAY_SIZE(passthrough_names); i++) {
		KUNIT_EXPECT_EQ(test, 0,
				snd_card_set_full_shortname(card, passthrough_names[i]));
		KUNIT_EXPECT_STREQ_MSG(test, card->full_shortname,
				       passthrough_names[i],
				       "name %zu was altered; the kernel does not filter these",
				       i);
	}

	snd_card_set_full_shortname(card, NULL);
}

static void test_card_utf8_names_trimmed(struct kunit *test)
{
	struct snd_card *card = kunit_kzalloc(test, sizeof(*card), GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, card);

	KUNIT_EXPECT_EQ(test, 0, snd_card_set_full_shortname(card, UTF8_UNTRIMMED_NAME));
	KUNIT_EXPECT_STREQ(test, card->full_shortname, UTF8_LATIN_NAME);

	/* a name that is only whitespace carries no information: treat it as
	 * unset rather than storing an empty string
	 */
	KUNIT_EXPECT_EQ(test, 0, snd_card_set_full_shortname(card, UTF8_ONLY_SPACES));
	KUNIT_EXPECT_NULL(test, card->full_shortname);

	KUNIT_EXPECT_EQ(test, 0, snd_card_set_full_shortname(card, ""));
	KUNIT_EXPECT_NULL(test, card->full_shortname);

	/* control characters at the ends become spaces and are then trimmed */
	KUNIT_EXPECT_EQ(test, 0, snd_card_set_full_shortname(card, "\x01" VALID_NAME "\x7f"));
	KUNIT_EXPECT_STREQ(test, card->full_shortname, VALID_NAME);

	snd_card_set_full_shortname(card, NULL);
}

static void test_card_utf8_names_truncated(struct kunit *test)
{
	struct snd_card *card = kunit_kzalloc(test, sizeof(*card), GFP_KERNEL);
	size_t len;
	char *name;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, card);

	/* 2-byte characters divide the limit evenly */
	name = utf8_repeat(test, UTF8_UMLAUT, SNDRV_CARD_FULL_SHORTNAME_MAX);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, name);
	KUNIT_EXPECT_EQ(test, 0, snd_card_set_full_shortname(card, name));
	KUNIT_ASSERT_NOT_NULL(test, card->full_shortname);
	KUNIT_EXPECT_EQ(test, strlen(card->full_shortname),
			SNDRV_CARD_FULL_SHORTNAME_MAX);

	/* 3-byte characters do not: the last whole character has to win, so the
	 * result stays below the limit instead of ending inside a sequence
	 */
	name = utf8_repeat(test, UTF8_HIRAGANA, SNDRV_CARD_FULL_SHORTNAME_MAX);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, name);
	KUNIT_EXPECT_EQ(test, 0, snd_card_set_full_shortname(card, name));
	KUNIT_ASSERT_NOT_NULL(test, card->full_shortname);
	len = strlen(card->full_shortname);
	KUNIT_EXPECT_LE(test, len, (size_t)SNDRV_CARD_FULL_SHORTNAME_MAX);
	KUNIT_EXPECT_EQ(test, len % strlen(UTF8_HIRAGANA), 0);

	/* replacing a bad byte grows it from one byte to three, so the limit
	 * has to be enforced on the output, not on the input
	 */
	name = utf8_repeat(test, "\x80", SNDRV_CARD_FULL_SHORTNAME_MAX);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, name);
	KUNIT_EXPECT_EQ(test, 0, snd_card_set_full_shortname(card, name));
	KUNIT_ASSERT_NOT_NULL(test, card->full_shortname);
	len = strlen(card->full_shortname);
	KUNIT_EXPECT_LE(test, len, (size_t)SNDRV_CARD_FULL_SHORTNAME_MAX);
	KUNIT_EXPECT_EQ(test, len % strlen(UTF8_REPLACEMENT), 0);

	/* the longname field has its own, larger limit */
	name = utf8_repeat(test, UTF8_UMLAUT, SNDRV_CARD_FULL_LONGNAME_MAX);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, name);
	KUNIT_EXPECT_EQ(test, 0, snd_card_set_full_longname(card, name));
	KUNIT_ASSERT_NOT_NULL(test, card->full_longname);
	KUNIT_EXPECT_EQ(test, strlen(card->full_longname),
			SNDRV_CARD_FULL_LONGNAME_MAX);

	snd_card_set_full_shortname(card, NULL);
	snd_card_set_full_longname(card, NULL);
}

/* card->id is an identifier, not a display name: it has to stay alphanumeric
 * ASCII even when the name it is derived from is not.  isalpha() alone does
 * not ensure that, since the ctype table treats the Latin-1 high bytes as
 * letters.
 */
static void test_card_id_stays_ascii(struct kunit *test)
{
	struct snd_card *card = kunit_kzalloc(test, sizeof(*card), GFP_KERNEL);
	size_t i;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, card);

	/* the digit has to be the first ASCII-safe byte to reach the path that
	 * copies the source byte unfiltered
	 */
	snd_card_set_id(card, "\xc3\x84" "3000");

	for (i = 0; card->id[i]; i++)
		KUNIT_EXPECT_TRUE_MSG(test, isascii(card->id[i]),
				      "card->id[%zu] = 0x%02x is not ASCII",
				      i, (unsigned char)card->id[i]);
}

static struct kunit_case sound_utils_cases[] = {
	KUNIT_CASE(test_phys_format_size),
	KUNIT_CASE(test_format_width),
	KUNIT_CASE(test_format_endianness),
	KUNIT_CASE(test_format_signed),
	KUNIT_CASE(test_format_fill_silence),
	KUNIT_CASE(test_playback_avail),
	KUNIT_CASE(test_capture_avail),
	KUNIT_CASE(test_card_set_id),
	KUNIT_CASE(test_pcm_format_name),
	KUNIT_CASE(test_card_add_component),
	KUNIT_CASE(test_utf8_strscpy),
	KUNIT_CASE(test_card_full_names),
	KUNIT_CASE(test_card_utf8_names_same_as_ascii),
	KUNIT_CASE(test_card_utf8_names_sanitised),
	KUNIT_CASE(test_card_utf8_names_passthrough),
	KUNIT_CASE(test_card_utf8_names_trimmed),
	KUNIT_CASE(test_card_utf8_names_truncated),
	KUNIT_CASE(test_card_id_stays_ascii),
	{},
};

static struct kunit_suite sound_utils_suite = {
	.name = "sound-core-test",
	.test_cases = sound_utils_cases,
};

kunit_test_suite(sound_utils_suite);
MODULE_DESCRIPTION("Sound core KUnit test");
MODULE_AUTHOR("Ivan Orlov");
MODULE_LICENSE("GPL");
