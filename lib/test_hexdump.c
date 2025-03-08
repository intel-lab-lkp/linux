/*
 * Test cases for lib/hexdump.c module.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/ctype.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/random.h>
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

static void __init test_hexdump_prepare_test(size_t len, size_t rowsize,
					     size_t groupsize, char *test,
					     size_t testlen, bool ascii)
{
	char *p;
	size_t l = len;
	size_t gs = groupsize, rs = rowsize;
	size_t bs, i, j;

	if (rs != 16 && rs != 32)
		rs = 16;

	if (l > rs)
		l = rs;

	if (!is_power_of_2(gs) || gs > 8 || (len % gs != 0))
		gs = 1;
	bs = IS_ENABLED(CONFIG_CPU_BIG_ENDIAN) ? 0 : gs - 1;

	/* hex dump */
	p = test;
	for (i = 0, j = 0; i < l; i++) {
		unsigned char b = data_b[i ^ bs];
		*p++ = "0123456789abcdef"[b >> 4];
		*p++ = "0123456789abcdef"[b & 15];
		if (++j == gs) {
			j = 0;
			*p++ = ' ';
		}
	}
	if (i)
		p--;

	/* ASCII part */
	if (ascii) {
		do {
			*p++ = ' ';
		} while (p < test + rs * 2 + rs / gs + 1);

		for (i = 0; i < l; i++) {
			unsigned char b = data_b[i];
			*p++ = (isascii(b) && isprint(b)) ? b : '.';
		}
	}

	*p = '\0';
}

#define TEST_HEXDUMP_BUF_SIZE		(32 * 3 + 2 + 32 + 1)

static void __init test_hexdump(size_t len, size_t rowsize, size_t groupsize,
				bool ascii)
{
	char test[TEST_HEXDUMP_BUF_SIZE];
	char real[TEST_HEXDUMP_BUF_SIZE];

	total_tests++;

	memset(real, FILL_CHAR, sizeof(real));
	hex_dump_to_buffer(data_b, len, rowsize, groupsize, real, sizeof(real),
			   ascii);

	memset(test, FILL_CHAR, sizeof(test));
	test_hexdump_prepare_test(len, rowsize, groupsize, test, sizeof(test),
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
	size_t d = min(sizeof(data_b), rowsize);
	size_t len = get_random_u32_inclusive(1, d);

	test_hexdump(len, rowsize, 4, ascii);
	test_hexdump(len, rowsize, 2, ascii);
	test_hexdump(len, rowsize, 8, ascii);
	test_hexdump(len, rowsize, 1, ascii);
}

static void __init test_hexdump_overflow(size_t buflen, size_t len,
					 size_t rowsize, size_t groupsize,
					 bool ascii)
{
	char test[TEST_HEXDUMP_BUF_SIZE];
	char buf[TEST_HEXDUMP_BUF_SIZE];
	size_t rs = rowsize, gs = groupsize;
	size_t ae, he, e, f, r;
	bool a;

	total_tests++;

	memset(buf, FILL_CHAR, sizeof(buf));

	r = hex_dump_to_buffer(data_b, len, rs, gs, buf, buflen, ascii);

	/*
	 * Caller must provide the data length multiple of groupsize. The
	 * calculations below are made with that assumption in mind.
	 */
	ae = rs * 2 /* hex */ + rs / gs /* spaces */ + 1 /* space */ + len /* ascii */;
	he = (gs * 2 /* hex */ + 1 /* space */) * len / gs - 1 /* no trailing space */;

	if (ascii)
		e = ae;
	else
		e = he;

	f = min(e + 1, buflen);
	if (buflen) {
		test_hexdump_prepare_test(len, rs, gs, test, sizeof(test), ascii);
		test[f - 1] = '\0';
	}
	memset(test + f, FILL_CHAR, sizeof(test) - f);

	a = r == e && !memcmp(test, buf, TEST_HEXDUMP_BUF_SIZE);

	buf[sizeof(buf) - 1] = '\0';

	if (!a) {
		pr_err("Len: %zu buflen: %zu strlen: %zu\n",
			len, buflen, strnlen(buf, sizeof(buf)));
		pr_err("Result: %zu '%s'\n", r, buf);
		pr_err("Expect: %zu '%s'\n", e, test);
		failed_tests++;
	}
}

static void __init test_hexdump_overflow_set(size_t buflen, bool ascii)
{
	unsigned int i = 0;
	size_t rs = get_random_u32_inclusive(1, 2) * 16;

	do {
		size_t gs = 1 << i;
		size_t len = get_random_u32_below(rs) + gs;

		test_hexdump_overflow(buflen, rounddown(len, gs), rs, gs, ascii);
	} while (i++ < 3);
}

static int __init test_hexdump_init(void)
{
	unsigned int i;
	size_t rowsize;

	rowsize = get_random_u32_inclusive(1, 2) * 16;
	for (i = 0; i < 16; i++)
		test_hexdump_set(rowsize, false);

	rowsize = get_random_u32_inclusive(1, 2) * 16;
	for (i = 0; i < 16; i++)
		test_hexdump_set(rowsize, true);

	for (i = 0; i <= TEST_HEXDUMP_BUF_SIZE; i++)
		test_hexdump_overflow_set(i, false);

	for (i = 0; i <= TEST_HEXDUMP_BUF_SIZE; i++)
		test_hexdump_overflow_set(i, true);

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
