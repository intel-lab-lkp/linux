/*
 * Test cases for lib/hexdump.c module.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/ctype.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

static const unsigned char data_b[] = {
	'\xbe', '\x32', '\xdb', '\x7b', '\x0a', '\x18', '\x93', '\xb2',	/* 00 - 07 */
	'\x70', '\xba', '\xc4', '\x24', '\x7d', '\x83', '\x34', '\x9b',	/* 08 - 0f */
	'\xa6', '\x9c', '\x31', '\xad', '\x9c', '\x0f', '\xac', '\xe9',	/* 10 - 17 */
	'\x4c', '\xd1', '\x19', '\x99', '\x43', '\xb1', '\xaf', '\x0c',	/* 18 - 1f */
};

#define FILL_CHAR	'#'

static unsigned total_tests __initdata;
static unsigned failed_tests __initdata;

static size_t __init test_hexdump_prepare_test(const unsigned char *buf, size_t len,
					       size_t rowsize, size_t groupsize,
					       char *test, size_t testlen, bool ascii)
{
	char *p;
	size_t byteswap, i, j;

	if (!len) {
		test[0] = 0;
		return 0;
	}

	if (rowsize != 16 && rowsize != 32)
		rowsize = 16;

	if (len > rowsize)
		len = rowsize;

	if (!is_power_of_2(groupsize) || groupsize > 8 || (len % groupsize != 0))
		groupsize = 1;
	byteswap = IS_ENABLED(CONFIG_CPU_BIG_ENDIAN) ? 0 : groupsize - 1;

	/* Check test passed a big enough output buffer */
	if (ascii)
		i = rowsize * 2 + rowsize / groupsize + 1 + len + 1;
	else
		i = len * 2 + len / groupsize - 1 + 1;
	if (i > testlen)
		return scnprintf(test, testlen, "buffer too short %zu < %zu", testlen, i);

	/* hex dump */
	p = test;
	for (i = 0, j = 0; i < len; i++) {
		unsigned char b = buf[i ^ byteswap];
		*p++ = "0123456789abcdef"[b >> 4];
		*p++ = "0123456789abcdef"[b & 15];
		if (++j == groupsize) {
			j = 0;
			*p++ = ' ';
		}
	}
	p--;

	/* ASCII part */
	if (ascii) {
		do {
			*p++ = ' ';
		} while (p < test + rowsize * 2 + rowsize / groupsize + 1);

		for (i = 0; i < len; i++) {
			unsigned char b = buf[i];
			*p++ = (isascii(b) && isprint(b)) ? b : '.';
		}
	}

	*p = '\0';
	return p - test;
}

#define TEST_HEXDUMP_BUF_SIZE		(32 * 3 + 2 + 32 + 1)

static void __init test_hexdump(const void *buf, size_t len, size_t rowsize,
				size_t groupsize, bool ascii)
{
	char test[TEST_HEXDUMP_BUF_SIZE];
	char real[TEST_HEXDUMP_BUF_SIZE];

	total_tests++;

	memset(real, FILL_CHAR, sizeof(real));
	hex_dump_to_buffer(buf, len, rowsize, groupsize, real, sizeof(real),
			   ascii);

	memset(test, FILL_CHAR, sizeof(test));
	test_hexdump_prepare_test(buf, len, rowsize, groupsize, test, sizeof(test),
				  ascii);

	if (memcmp(test, real, TEST_HEXDUMP_BUF_SIZE)) {
		pr_err("Len: %zu row: %zu group: %zu\n", len, rowsize, groupsize);
		pr_err("Result: '%s'\n", real);
		pr_err("Expect: '%s'\n", test);
		failed_tests++;
	}
}

static void __init test_hexdump_set(size_t rowsize, bool ascii)
{
	size_t len;

	for (len = 1; len <= rowsize; len++) {
		test_hexdump(data_b, len, rowsize, 4, ascii);
		test_hexdump(data_b, len, rowsize, 2, ascii);
		test_hexdump(data_b, len, rowsize, 8, ascii);
		test_hexdump(data_b, len, rowsize, 1, ascii);
	}
}

static bool __init test_hexdump_overflow(size_t buflen, size_t len,
					   size_t rowsize, size_t groupsize,
					   bool ascii)
{
	char test[TEST_HEXDUMP_BUF_SIZE];
	char buf[TEST_HEXDUMP_BUF_SIZE];
	size_t expected;
	size_t f, result;
	bool ok;

	total_tests++;

	memset(buf, FILL_CHAR, sizeof(buf));

	result = hex_dump_to_buffer(data_b, len, rowsize, groupsize, buf,
				    buflen, ascii);

	/* Test output is generated into a 'long enough' buffer */
	expected = test_hexdump_prepare_test(data_b, len, rowsize, groupsize, test,
					     sizeof(test), ascii);

	f = min(expected + 1, buflen);
	if (f)
		test[f - 1] = '\0';
	memset(test + f, FILL_CHAR, sizeof(test) - f);

	ok = result == expected && !memcmp(test, buf, TEST_HEXDUMP_BUF_SIZE);

	if (!ok) {
		pr_err("Rowsize %zu, groupsize %zu, ascii %s, len %zu, buflen %zu, strlen %zu\n",
			rowsize, groupsize, ascii ? "yes" : "no",
			len, buflen, strnlen(buf, sizeof(buf)));
		pr_err("Result: %zd '%.*s'\n", result, TEST_HEXDUMP_BUF_SIZE, buf);
		pr_err("Expect: %zd '%.*s'\n", expected, TEST_HEXDUMP_BUF_SIZE, test);
		failed_tests++;
	}

	return expected < buflen;
}

static void __init test_hexdump_overflow_set(size_t rowsize, bool ascii)
{
	size_t groupsize, len, buflen;

	for (groupsize = 1; groupsize <= 8; groupsize *= 2) {
		len = 0;
		buflen = 0;
		for (; len <= rowsize && buflen <= TEST_HEXDUMP_BUF_SIZE;) {
			if (test_hexdump_overflow(buflen, len, rowsize,
						  groupsize, ascii))
				len += groupsize;
			else
				buflen++;
		}
	}
}

static void __init test_hexdump_bytes(void)
{
	unsigned char buf[16];
	size_t i, j;

	for (i = 0; i < 256; i += 16) {
		for (j = 0; j < 16; j++)
			buf[j] = i + j;
		test_hexdump(buf, 16, 16, 1, true);
		test_hexdump(buf, 16, 16, 2, true);
		test_hexdump(buf, 16, 16, 4, true);
		test_hexdump(buf, 16, 16, 8, true);
	}
}


static int __init test_hexdump_init(void)
{
	size_t rowsize;

	for (rowsize = 16; rowsize <= 32; rowsize += 16) {
		test_hexdump_set(rowsize, false);
		test_hexdump_set(rowsize, true);
	}

	test_hexdump_overflow_set(16, false);
	test_hexdump_overflow_set(16, true);
	test_hexdump_overflow_set(32, false);
	test_hexdump_overflow_set(32, true);

	test_hexdump_bytes();

	if (failed_tests == 0)
		pr_info("all %u tests passed\n", total_tests);
	else
		pr_err("failed %u out of %u tests\n", failed_tests, total_tests);

	return failed_tests ? -EINVAL : 0;
}
module_init(test_hexdump_init);

static void __exit test_hexdump_exit(void)
{
	/* do nothing */
}
module_exit(test_hexdump_exit);

MODULE_AUTHOR("Andy Shevchenko <andriy.shevchenko@linux.intel.com>");
MODULE_DESCRIPTION("Test cases for lib/hexdump.c module");
MODULE_LICENSE("Dual BSD/GPL");
