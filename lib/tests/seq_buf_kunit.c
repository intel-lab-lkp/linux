// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the seq_buf API
 *
 * Copyright (C) 2025, Google LLC.
 */

#include <kunit/test.h>
#include <linux/console.h>
#include <linux/fs.h>
#include <linux/seq_buf.h>
#include <linux/shmem_fs.h>

static void seq_buf_init_test(struct kunit *test)
{
	char buf[32];
	struct seq_buf s;

	seq_buf_init(&s, buf, sizeof(buf));

	KUNIT_EXPECT_EQ(test, s.size, 32);
	KUNIT_EXPECT_EQ(test, s.len, 0);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_EQ(test, seq_buf_buffer_left(&s), 32);
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 0);
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "");
}

static void seq_buf_declare_test(struct kunit *test)
{
	DECLARE_SEQ_BUF(s, 24);

	KUNIT_EXPECT_EQ(test, s.size, 24);
	KUNIT_EXPECT_EQ(test, s.len, 0);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_EQ(test, seq_buf_buffer_left(&s), 24);
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 0);
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "");
}

static void seq_buf_clear_test(struct kunit *test)
{
	DECLARE_SEQ_BUF(s, 128);

	seq_buf_puts(&s, "hello");
	KUNIT_EXPECT_EQ(test, s.len, 5);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "hello");

	seq_buf_clear(&s);

	KUNIT_EXPECT_EQ(test, s.len, 0);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "");
}

static void seq_buf_puts_test(struct kunit *test)
{
	DECLARE_SEQ_BUF(s, 16);

	seq_buf_puts(&s, "hello");
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 5);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "hello");

	seq_buf_puts(&s, " world");
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 11);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "hello world");
}

static void seq_buf_puts_overflow_test(struct kunit *test)
{
	DECLARE_SEQ_BUF(s, 10);

	seq_buf_puts(&s, "123456789");
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 9);

	seq_buf_puts(&s, "0");
	KUNIT_EXPECT_TRUE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 10);
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "123456789");

	seq_buf_clear(&s);
	KUNIT_EXPECT_EQ(test, s.len, 0);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "");
}

static void seq_buf_putc_test(struct kunit *test)
{
	DECLARE_SEQ_BUF(s, 4);

	seq_buf_putc(&s, 'a');
	seq_buf_putc(&s, 'b');
	seq_buf_putc(&s, 'c');

	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 3);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "abc");

	seq_buf_putc(&s, 'd');
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 4);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "abc");

	seq_buf_putc(&s, 'e');
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 4);
	KUNIT_EXPECT_TRUE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "abc");

	seq_buf_clear(&s);
	KUNIT_EXPECT_EQ(test, s.len, 0);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "");
}

static void seq_buf_pop_test(struct kunit *test)
{
	DECLARE_SEQ_BUF(s, 8);
	struct seq_buf t;
	char *buf;

	/* Nothing to pop. */
	KUNIT_EXPECT_EQ(test, seq_buf_pop(&s), -1);
	KUNIT_EXPECT_EQ(test, s.len, 0);

	seq_buf_puts(&s, "hello");
	KUNIT_EXPECT_EQ(test, seq_buf_pop(&s), 'o');
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 4);
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "hell");

	/* A 0xff byte must not be mistaken for an empty buffer. */
	seq_buf_putc(&s, 0xff);
	KUNIT_EXPECT_EQ(test, seq_buf_pop(&s), 0xff);

	/* A full buffer pops its last byte. */
	seq_buf_puts(&s, "abc");
	seq_buf_putc(&s, 'd');
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 8);
	KUNIT_EXPECT_EQ(test, seq_buf_pop(&s), 'd');
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 7);
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "hellabc");

	/*
	 * An overflowed buffer has nothing to pop, and stays overflowed. Use
	 * a buffer allocated at its exact size, so that KASAN reports any
	 * read past its end.
	 */
	buf = kunit_kmalloc(test, 16, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	seq_buf_init(&t, buf, 16);
	KUNIT_EXPECT_EQ(test, seq_buf_printf(&t, "%s", "longer than sixteen"), -1);
	KUNIT_EXPECT_EQ(test, seq_buf_pop(&t), -1);
	KUNIT_EXPECT_TRUE(test, seq_buf_has_overflowed(&t));
}

static void seq_buf_printf_test(struct kunit *test)
{
	DECLARE_SEQ_BUF(s, 32);

	seq_buf_printf(&s, "hello %s", "world");
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 11);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "hello world");

	seq_buf_printf(&s, " %d", 123);
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 15);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "hello world 123");
}

static void seq_buf_printf_overflow_test(struct kunit *test)
{
	DECLARE_SEQ_BUF(s, 16);

	seq_buf_printf(&s, "%lu", 1234567890UL);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 10);
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "1234567890");

	seq_buf_printf(&s, "%s", "abcdefghij");
	KUNIT_EXPECT_TRUE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 16);
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "1234567890abcde");

	seq_buf_clear(&s);
	KUNIT_EXPECT_EQ(test, s.len, 0);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "");
}

static void seq_buf_get_buf_commit_test(struct kunit *test)
{
	DECLARE_SEQ_BUF(s, 16);
	char *buf;
	size_t len;

	len = seq_buf_get_buf(&s, &buf);
	KUNIT_EXPECT_EQ(test, len, 16);
	KUNIT_EXPECT_PTR_NE(test, buf, NULL);

	memcpy(buf, "hello", 5);
	seq_buf_commit(&s, 5);

	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 5);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "hello");

	len = seq_buf_get_buf(&s, &buf);
	KUNIT_EXPECT_EQ(test, len, 11);
	KUNIT_EXPECT_PTR_NE(test, buf, NULL);

	memcpy(buf, " worlds!", 8);
	seq_buf_commit(&s, 6);

	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 11);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "hello world");

	len = seq_buf_get_buf(&s, &buf);
	KUNIT_EXPECT_EQ(test, len, 5);
	KUNIT_EXPECT_PTR_NE(test, buf, NULL);

	seq_buf_commit(&s, -1);
	KUNIT_EXPECT_TRUE(test, seq_buf_has_overflowed(&s));
}

static void seq_buf_putmem_hex_test(struct kunit *test)
{
	DECLARE_SEQ_BUF(s, 24);
	const u8 data[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
#ifdef __BIG_ENDIAN
	const char *expected = "0001020304050607 0809 ";
#else
	const char *expected = "0706050403020100 0908 ";
#endif

	KUNIT_EXPECT_EQ(test, seq_buf_putmem_hex(&s, data, sizeof(data)), 0);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), strlen(expected));
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), expected);
}

static void seq_buf_putmem_hex_overflow_test(struct kunit *test)
{
	DECLARE_SEQ_BUF(s, 20);
	const u8 data[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
#ifdef __BIG_ENDIAN
	const char *expected = "0001020304050607 08";
#else
	const char *expected = "0706050403020100 09";
#endif

	KUNIT_EXPECT_EQ(test, seq_buf_putmem_hex(&s, data, sizeof(data)), -1);
	KUNIT_EXPECT_TRUE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 20);
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), expected);
}

static void seq_buf_puts_partial_overflow_test(struct kunit *test)
{
	DECLARE_SEQ_BUF(s, 16);
	struct seq_buf t;
	char buf[8];

	/* As much of the string as fits is written, like seq_buf_printf(). */
	seq_buf_puts(&s, "hello");
	KUNIT_EXPECT_EQ(test, seq_buf_puts(&s, " world, again"), -1);
	KUNIT_EXPECT_TRUE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 16);
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "hello world, ag");

	/* Stale bytes after the data must not show up in the string. */
	memset(buf, 'X', sizeof(buf));
	seq_buf_init(&t, buf, sizeof(buf));
	seq_buf_putc(&t, 'a');
	KUNIT_EXPECT_EQ(test, seq_buf_puts(&t, "bcdefghij"), -1);
	KUNIT_EXPECT_TRUE(test, seq_buf_has_overflowed(&t));
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&t), "abcdefg");
}

static void seq_buf_putmem_hex_partial_overflow_test(struct kunit *test)
{
	const u8 data[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
#ifdef __BIG_ENDIAN
	const char *expected = "0001020304050607 08";
#else
	const char *expected = "0706050403020100 09";
#endif
	struct seq_buf s;
	char buf[20];

	/* Stale bytes after the data must not show up in the string. */
	memset(buf, 'X', sizeof(buf));
	seq_buf_init(&s, buf, sizeof(buf));
	KUNIT_EXPECT_EQ(test, seq_buf_putmem_hex(&s, data, sizeof(data)), -1);
	KUNIT_EXPECT_TRUE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), expected);
}

/* Long enough that it cannot fit in the room the test leaves for it. */
#define SEQ_BUF_TEST_PATH	"/seq_buf_kunit_path_name"

static void seq_buf_path_overflow_test(struct kunit *test)
{
	DECLARE_SEQ_BUF(s, 32);
	const char *expected = "keep:xxxxxxxxxxxxxxxxxxxxxxx";
	struct file *file;
	size_t len;
	int i;

	/*
	 * The tests run before anything writable is mounted, so take the file
	 * whose path gets printed from shmem, which needs no mount of its own.
	 */
	file = shmem_file_setup(SEQ_BUF_TEST_PATH, 0, EMPTY_VMA_FLAGS);
	if (IS_ERR(file))
		kunit_skip(test, "cannot create a file to print the path of");

	/* Leave less room than the path needs, so d_path() cannot fit it. */
	seq_buf_puts(&s, "keep:");
	len = seq_buf_used(&s);
	for (i = len; i < 28; i++)
		seq_buf_putc(&s, 'x');

	KUNIT_EXPECT_EQ(test, seq_buf_path(&s, &file->f_path, "\n"), -1);
	fput(file);

	KUNIT_EXPECT_TRUE(test, seq_buf_has_overflowed(&s));

	/*
	 * d_path() keeps as much of the path as fits when it does not fit
	 * whole, and seq_buf_str() would hand out that fragment, as it ends
	 * the string at the last byte of an overflowed buffer.
	 */
	for (i = 28; i < 32; i++)
		KUNIT_EXPECT_EQ_MSG(test, s.buffer[i], '\0',
				    "byte %d past the data is not cleared", i);
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), expected);
}

static void seq_buf_strlen_test(struct kunit *test)
{
	DECLARE_SEQ_BUF(s, 16);

	KUNIT_EXPECT_EQ(test, seq_buf_strlen(&s), 0);
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "");

	seq_buf_puts(&s, "hello");
	KUNIT_EXPECT_EQ(test, seq_buf_strlen(&s), 5);
	KUNIT_EXPECT_EQ(test, seq_buf_strlen(&s), strlen(seq_buf_str(&s)));

	seq_buf_printf(&s, " %s", "world");
	KUNIT_EXPECT_EQ(test, seq_buf_strlen(&s), 11);
	KUNIT_EXPECT_EQ(test, seq_buf_strlen(&s), strlen(seq_buf_str(&s)));
}

static void seq_buf_strlen_printf_overflow_test(struct kunit *test)
{
	DECLARE_SEQ_BUF(s, 16);
	DECLARE_SEQ_BUF(t, 8);

	seq_buf_printf(&s, "%s", "1234567890abcdefghij");
	KUNIT_EXPECT_TRUE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 16);
	KUNIT_EXPECT_EQ(test, seq_buf_strlen(&s), 15);
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "1234567890abcde");
	KUNIT_EXPECT_EQ(test, seq_buf_strlen(&s), strlen(seq_buf_str(&s)));

	/* Output one byte too long for the NUL. */
	seq_buf_printf(&t, "%s", "12345678");
	KUNIT_EXPECT_TRUE(test, seq_buf_has_overflowed(&t));
	KUNIT_EXPECT_EQ(test, seq_buf_used(&t), 8);
	KUNIT_EXPECT_EQ(test, seq_buf_strlen(&t), 7);
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&t), "1234567");
	KUNIT_EXPECT_EQ(test, seq_buf_strlen(&t), strlen(seq_buf_str(&t)));
}

static void seq_buf_strlen_full_test(struct kunit *test)
{
	DECLARE_SEQ_BUF(s, 4);
	DECLARE_SEQ_BUF(t, 8);
	char *buf;
	size_t len;

	/* Filled exactly, with no room left for a NUL, but not overflowed. */
	seq_buf_putc(&s, 'a');
	seq_buf_putc(&s, 'b');
	seq_buf_putc(&s, 'c');
	seq_buf_putc(&s, 'd');
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_EQ(test, seq_buf_used(&s), 4);
	KUNIT_EXPECT_EQ(test, seq_buf_strlen(&s), 3);
	/* seq_buf_strlen() terminates the buffer by itself. */
	KUNIT_EXPECT_EQ(test, s.buffer[3], '\0');
	KUNIT_EXPECT_EQ(test, strnlen(s.buffer, s.size), 3);
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "abc");
	KUNIT_EXPECT_EQ(test, seq_buf_strlen(&s), strlen(seq_buf_str(&s)));

	/* A printf into a full buffer writes nothing. */
	KUNIT_EXPECT_EQ(test, seq_buf_printf(&s, "%s", "x"), -1);
	KUNIT_EXPECT_TRUE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_EQ(test, seq_buf_strlen(&s), 3);
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "abc");
	KUNIT_EXPECT_EQ(test, seq_buf_strlen(&s), strlen(seq_buf_str(&s)));

	len = seq_buf_get_buf(&t, &buf);
	KUNIT_ASSERT_EQ(test, len, 8);
	memset(buf, 'z', len);
	seq_buf_commit(&t, len);
	KUNIT_EXPECT_FALSE(test, seq_buf_has_overflowed(&t));
	KUNIT_EXPECT_EQ(test, seq_buf_strlen(&t), 7);
	KUNIT_EXPECT_EQ(test, t.buffer[7], '\0');
	KUNIT_EXPECT_EQ(test, strnlen(t.buffer, t.size), 7);
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&t), "zzzzzzz");
	KUNIT_EXPECT_EQ(test, seq_buf_strlen(&t), strlen(seq_buf_str(&t)));
}

static void seq_buf_strlen_puts_overflow_test(struct kunit *test)
{
	DECLARE_SEQ_BUF(s, 16);

	/* A puts that does not fit copies as much as fits. */
	seq_buf_puts(&s, "hello");
	KUNIT_EXPECT_EQ(test, seq_buf_puts(&s, " this does not fit"), -1);
	KUNIT_EXPECT_TRUE(test, seq_buf_has_overflowed(&s));
	KUNIT_EXPECT_EQ(test, seq_buf_strlen(&s), 15);
	KUNIT_EXPECT_STREQ(test, seq_buf_str(&s), "hello this does");
	KUNIT_EXPECT_EQ(test, seq_buf_strlen(&s), strlen(seq_buf_str(&s)));
}

/*
 * Counters for the console that seq_buf_do_printk_test() registers while it
 * runs. Only records carrying the marker, and records holding nothing but a
 * line feed, are counted, so unrelated kernel messages do not disturb them.
 */
#define SEQ_BUF_PRINTK_MARKER	"sbdpkx"

static unsigned int seq_buf_printk_marked;
static unsigned int seq_buf_printk_empty;

static void seq_buf_printk_capture(struct console *con, const char *s,
				   unsigned int count)
{
	const char *text = s;
	const char *prefix;

	/* Skip the "[   12.345678] " timestamp, when there is one. */
	prefix = memchr(s, ']', count);
	if (prefix && prefix + 2 <= s + count && prefix[1] == ' ')
		text = prefix + 2;
	count -= text - s;

	if (count == 0 || (count == 1 && text[0] == '\n'))
		seq_buf_printk_empty++;
	else if (strnstr(text, SEQ_BUF_PRINTK_MARKER, count))
		seq_buf_printk_marked++;
}

static void seq_buf_do_printk_test(struct kunit *test)
{
	static struct console capture = {
		.name = "sbufcap",
		.write = seq_buf_printk_capture,
		.flags = CON_ENABLED,
		.index = -1,
	};
	DECLARE_SEQ_BUF(s, 8);

	/*
	 * Fill the buffer exactly, so that the NUL takes the place of the
	 * last byte and the string ends with the line feed before it.
	 */
	seq_buf_puts(&s, SEQ_BUF_PRINTK_MARKER);
	seq_buf_putc(&s, '\n');
	seq_buf_putc(&s, '!');
	KUNIT_ASSERT_FALSE(test, seq_buf_has_overflowed(&s));
	KUNIT_ASSERT_EQ(test, seq_buf_used(&s), 8);
	KUNIT_ASSERT_EQ(test, seq_buf_strlen(&s), 7);

	seq_buf_printk_marked = 0;
	seq_buf_printk_empty = 0;

	register_console(&capture);
	seq_buf_do_printk(&s, KERN_INFO);
	unregister_console(&capture);

	/* The one line that was written, and nothing after it. */
	KUNIT_EXPECT_EQ(test, seq_buf_printk_marked, 1);
	KUNIT_EXPECT_EQ(test, seq_buf_printk_empty, 0);
}

static struct kunit_case seq_buf_test_cases[] = {
	KUNIT_CASE(seq_buf_init_test),
	KUNIT_CASE(seq_buf_declare_test),
	KUNIT_CASE(seq_buf_clear_test),
	KUNIT_CASE(seq_buf_puts_test),
	KUNIT_CASE(seq_buf_puts_overflow_test),
	KUNIT_CASE(seq_buf_putc_test),
	KUNIT_CASE(seq_buf_pop_test),
	KUNIT_CASE(seq_buf_printf_test),
	KUNIT_CASE(seq_buf_printf_overflow_test),
	KUNIT_CASE(seq_buf_get_buf_commit_test),
	KUNIT_CASE(seq_buf_putmem_hex_test),
	KUNIT_CASE(seq_buf_putmem_hex_overflow_test),
	KUNIT_CASE(seq_buf_puts_partial_overflow_test),
	KUNIT_CASE(seq_buf_putmem_hex_partial_overflow_test),
	KUNIT_CASE(seq_buf_path_overflow_test),
	KUNIT_CASE(seq_buf_strlen_test),
	KUNIT_CASE(seq_buf_strlen_printf_overflow_test),
	KUNIT_CASE(seq_buf_strlen_full_test),
	KUNIT_CASE(seq_buf_strlen_puts_overflow_test),
	KUNIT_CASE(seq_buf_do_printk_test),
	{}
};

static struct kunit_suite seq_buf_test_suite = {
	.name = "seq_buf",
	.test_cases = seq_buf_test_cases,
};

kunit_test_suite(seq_buf_test_suite);

MODULE_DESCRIPTION("Runtime test cases for seq_buf string API");
MODULE_LICENSE("GPL");
