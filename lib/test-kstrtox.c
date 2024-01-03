#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

#define for_each_test(i, test)	\
	for (i = 0; i < ARRAY_SIZE(test); i++)

struct test_fail {
	const char *str;
	unsigned int base;
};

#define DEFINE_TEST_FAIL(test)	\
	const struct test_fail test[] __initconst

#define DECLARE_TEST_OK(type, test_type)	\
	test_type {				\
		const char *str;		\
		unsigned int base;		\
		type expected_res;		\
	}

#define DEFINE_TEST_OK(type, test)	\
	const type test[] __initconst

#define TEST_FAIL(fn, type, fmt, test)					\
{									\
	unsigned int i;							\
									\
	for_each_test(i, test) {					\
		const struct test_fail *t = &test[i];			\
		type tmp;						\
		int rv;							\
									\
		tmp = 0;						\
		rv = fn(t->str, t->base, &tmp);				\
		if (rv >= 0) {						\
			WARN(1, "str '%s', base %u, expected -E, got %d/" fmt "\n",	\
				t->str, t->base, rv, tmp);		\
			continue;					\
		}							\
	}								\
}

#define TEST_OK(fn, type, fmt, test)					\
{									\
	unsigned int i;							\
									\
	for_each_test(i, test) {					\
		const typeof(test[0]) *t = &test[i];			\
		type res;						\
		int rv;							\
									\
		rv = fn(t->str, t->base, &res);				\
		if (rv != 0) {						\
			WARN(1, "str '%s', base %u, expected 0/" fmt ", got %d\n",	\
				t->str, t->base, t->expected_res, rv);	\
			continue;					\
		}							\
		if (res != t->expected_res) {				\
			WARN(1, "str '%s', base %u, expected " fmt ", got " fmt "\n",	\
				t->str, t->base, t->expected_res, res);	\
			continue;					\
		}							\
	}								\
}

static void __init test_kstrtoull_ok(void)
{
	DECLARE_TEST_OK(unsigned long long, struct test_ull);
	static DEFINE_TEST_OK(struct test_ull, test_ull_ok) = {
		{"0",	10,	0ULL},
		{"1",	10,	1ULL},
		{"127",	10,	127ULL},
		{"128",	10,	128ULL},
		{"129",	10,	129ULL},
		{"255",	10,	255ULL},
		{"256",	10,	256ULL},
		{"257",	10,	257ULL},
		{"32767",	10,	32767ULL},
		{"32768",	10,	32768ULL},
		{"32769",	10,	32769ULL},
		{"65535",	10,	65535ULL},
		{"65536",	10,	65536ULL},
		{"65537",	10,	65537ULL},
		{"2147483647",	10,	2147483647ULL},
		{"2147483648",	10,	2147483648ULL},
		{"2147483649",	10,	2147483649ULL},
		{"4294967295",	10,	4294967295ULL},
		{"4294967296",	10,	4294967296ULL},
		{"4294967297",	10,	4294967297ULL},
		{"9223372036854775807",	10,	9223372036854775807ULL},
		{"9223372036854775808",	10,	9223372036854775808ULL},
		{"9223372036854775809",	10,	9223372036854775809ULL},
		{"18446744073709551614",	10,	18446744073709551614ULL},
		{"18446744073709551615",	10,	18446744073709551615ULL},

		{"00",		8,	00ULL},
		{"01",		8,	01ULL},
		{"0177",	8,	0177ULL},
		{"0200",	8,	0200ULL},
		{"0201",	8,	0201ULL},
		{"0377",	8,	0377ULL},
		{"0400",	8,	0400ULL},
		{"0401",	8,	0401ULL},
		{"077777",	8,	077777ULL},
		{"0100000",	8,	0100000ULL},
		{"0100001",	8,	0100001ULL},
		{"0177777",	8,	0177777ULL},
		{"0200000",	8,	0200000ULL},
		{"0200001",	8,	0200001ULL},
		{"017777777777",	8,	017777777777ULL},
		{"020000000000",	8,	020000000000ULL},
		{"020000000001",	8,	020000000001ULL},
		{"037777777777",	8,	037777777777ULL},
		{"040000000000",	8,	040000000000ULL},
		{"040000000001",	8,	040000000001ULL},
		{"0777777777777777777777",	8,	0777777777777777777777ULL},
		{"01000000000000000000000",	8,	01000000000000000000000ULL},
		{"01000000000000000000001",	8,	01000000000000000000001ULL},
		{"01777777777777777777776",	8,	01777777777777777777776ULL},
		{"01777777777777777777777",	8,	01777777777777777777777ULL},

		{"0x0",		16,	0x0ULL},
		{"0x1",		16,	0x1ULL},
		{"0x7f",	16,	0x7fULL},
		{"0x80",	16,	0x80ULL},
		{"0x81",	16,	0x81ULL},
		{"0xff",	16,	0xffULL},
		{"0x100",	16,	0x100ULL},
		{"0x101",	16,	0x101ULL},
		{"0x7fff",	16,	0x7fffULL},
		{"0x8000",	16,	0x8000ULL},
		{"0x8001",	16,	0x8001ULL},
		{"0xffff",	16,	0xffffULL},
		{"0x10000",	16,	0x10000ULL},
		{"0x10001",	16,	0x10001ULL},
		{"0x7fffffff",	16,	0x7fffffffULL},
		{"0x80000000",	16,	0x80000000ULL},
		{"0x80000001",	16,	0x80000001ULL},
		{"0xffffffff",	16,	0xffffffffULL},
		{"0x100000000",	16,	0x100000000ULL},
		{"0x100000001",	16,	0x100000001ULL},
		{"0x7fffffffffffffff",	16,	0x7fffffffffffffffULL},
		{"0x8000000000000000",	16,	0x8000000000000000ULL},
		{"0x8000000000000001",	16,	0x8000000000000001ULL},
		{"0xfffffffffffffffe",	16,	0xfffffffffffffffeULL},
		{"0xffffffffffffffff",	16,	0xffffffffffffffffULL},

		{"0\n",	0,	0ULL},
	};
	TEST_OK(kstrtoull, unsigned long long, "%llu", test_ull_ok);
}

static void __init test_kstrtoull_fail(void)
{
	static DEFINE_TEST_FAIL(test_ull_fail) = {
		{"",	0},
		{"",	8},
		{"",	10},
		{"",	16},
		{"\n",	0},
		{"\n",	8},
		{"\n",	10},
		{"\n",	16},
		{"\n0",	0},
		{"\n0",	8},
		{"\n0",	10},
		{"\n0",	16},
		{"+",	0},
		{"+",	8},
		{"+",	10},
		{"+",	16},
		{"-",	0},
		{"-",	8},
		{"-",	10},
		{"-",	16},
		{"0x",	0},
		{"0x",	16},
		{"0X",	0},
		{"0X",	16},
		{"0 ",	0},
		{"1+",	0},
		{"1-",	0},
		{" 2",	0},
		/* base autodetection */
		{"0x0z",	0},
		{"0z",		0},
		{"a",		0},
		/* digit >= base */
		{"2",	2},
		{"8",	8},
		{"a",	10},
		{"A",	10},
		{"g",	16},
		{"G",	16},
		/* overflow */
		{"10000000000000000000000000000000000000000000000000000000000000000",	2},
		{"2000000000000000000000",	8},
		{"18446744073709551616",	10},
		{"10000000000000000",	16},
		/* negative */
		{"-0", 0},
		{"-0", 8},
		{"-0", 10},
		{"-0", 16},
		{"-1", 0},
		{"-1", 8},
		{"-1", 10},
		{"-1", 16},
		/* sign is first character if any */
		{"-+1", 0},
		{"-+1", 8},
		{"-+1", 10},
		{"-+1", 16},
		/* nothing after \n */
		{"0\n0", 0},
		{"0\n0", 8},
		{"0\n0", 10},
		{"0\n0", 16},
		{"0\n+", 0},
		{"0\n+", 8},
		{"0\n+", 10},
		{"0\n+", 16},
		{"0\n-", 0},
		{"0\n-", 8},
		{"0\n-", 10},
		{"0\n-", 16},
		{"0\n ", 0},
		{"0\n ", 8},
		{"0\n ", 10},
		{"0\n ", 16},
	};
	TEST_FAIL(kstrtoull, unsigned long long, "%llu", test_ull_fail);
}

static void __init test_kstrtoll_ok(void)
{
	DECLARE_TEST_OK(long long, struct test_ll);
	static DEFINE_TEST_OK(struct test_ll, test_ll_ok) = {
		{"0",	10,	0LL},
		{"1",	10,	1LL},
		{"127",	10,	127LL},
		{"128",	10,	128LL},
		{"129",	10,	129LL},
		{"255",	10,	255LL},
		{"256",	10,	256LL},
		{"257",	10,	257LL},
		{"32767",	10,	32767LL},
		{"32768",	10,	32768LL},
		{"32769",	10,	32769LL},
		{"65535",	10,	65535LL},
		{"65536",	10,	65536LL},
		{"65537",	10,	65537LL},
		{"2147483647",	10,	2147483647LL},
		{"2147483648",	10,	2147483648LL},
		{"2147483649",	10,	2147483649LL},
		{"4294967295",	10,	4294967295LL},
		{"4294967296",	10,	4294967296LL},
		{"4294967297",	10,	4294967297LL},
		{"9223372036854775807",	10,	9223372036854775807LL},

		{"-0",	10,	0LL},
		{"-1",	10,	-1LL},
		{"-2",	10,	-2LL},
		{"-9223372036854775808",	10,	LLONG_MIN},
	};
	TEST_OK(kstrtoll, long long, "%lld", test_ll_ok);
}

/*
 * The special pattern to make sure the result is not modified for error cases.
 */
#define ULL_PATTERN		(0xefefefef7a7a7a7aULL)
#define POINTER_PATTERN		((void *)(uintptr_t)(ULL_PATTERN & UINTPTR_MAX))

/* Want to include "E" suffix for full coverage. */
#define MEMPARSE_TEST_SUFFIX	(MEMPARSE_SUFFIX_K | MEMPARSE_SUFFIX_M |\
				 MEMPARSE_SUFFIX_G | MEMPARSE_SUFFIX_T |\
				 MEMPARSE_SUFFIX_P | MEMPARSE_SUFFIX_E)

static void __init test_memparse_safe_fail(void)
{
	struct memparse_test_fail {
		const char *str;
		/* Expected error number, either -EINVAL or -ERANGE. */
		unsigned int expected_ret;
	};
	static const struct memparse_test_fail tests[] __initconst = {
		/* No valid string can be found at all. */
		{"", -EINVAL},
		{"\n", -EINVAL},
		{"\n0", -EINVAL},
		{"+", -EINVAL},
		{"-", -EINVAL},

		/* Only hex prefix, but no valid string. */
		{"0x", -EINVAL},
		{"0X", -EINVAL},

		/* Only hex prefix, with suffix but still no valid string. */
		{"0xK", -EINVAL},
		{"0xM", -EINVAL},
		{"0xG", -EINVAL},

		/* Only hex prefix, with invalid chars. */
		{"0xH", -EINVAL},
		{"0xy", -EINVAL},

		/*
		 * No support for any leading "+-" chars, even followed by a valid
		 * number.
		 */
		{"-0", -EINVAL},
		{"+0", -EINVAL},
		{"-1", -EINVAL},
		{"+1", -EINVAL},

		/* Stray suffix would also be rejected. */
		{"K", -EINVAL},
		{"P", -EINVAL},

		/* Overflow in the string itself*/
		{"18446744073709551616", -ERANGE},
		{"02000000000000000000000", -ERANGE},
		{"0x10000000000000000", -ERANGE},

		/*
		 * Good string but would overflow with suffix.
		 *
		 * Note, for "E" suffix, one should not use with hex, or "0x1E"
		 * would be treated as 0x1e (30 in decimal), not 0x1 and "E" suffix.
		 * Another reason "E" suffix is cursed.
		 */
		{"16E", -ERANGE},
		{"020E", -ERANGE},
		{"16384P", -ERANGE},
		{"040000P", -ERANGE},
		{"16777216T", -ERANGE},
		{"0100000000T", -ERANGE},
		{"17179869184G", -ERANGE},
		{"0200000000000G", -ERANGE},
		{"17592186044416M", -ERANGE},
		{"0400000000000000M", -ERANGE},
		{"18014398509481984K", -ERANGE},
		{"01000000000000000000K", -ERANGE},
	};
	unsigned int i;

	for_each_test(i, tests) {
		const struct memparse_test_fail *t = &tests[i];
		unsigned long long tmp = ULL_PATTERN;
		char *retptr = POINTER_PATTERN;
		int ret;

		ret = memparse_safe(t->str, MEMPARSE_TEST_SUFFIX, &tmp, &retptr);
		if (ret != t->expected_ret) {
			WARN(1, "str '%s', expected ret %d got %d\n", t->str,
			     t->expected_ret, ret);
			continue;
		}
		if (tmp != ULL_PATTERN)
			WARN(1, "str '%s' failed as expected, but result got modified",
			     t->str);
		if (retptr != POINTER_PATTERN)
			WARN(1, "str '%s' failed as expected, but pointer got modified",
			     t->str);
	}
}

static void __init test_memparse_safe_ok(void)
{
	struct memparse_test_ok {
		const char *str;
		unsigned long long expected_value;
		/* How many bytes the @retptr pointer should be moved forward. */
		unsigned int retptr_off;

		/* If 0, falls back to MEMPARSE_TEST_SUFFIX. */
		enum memparse_suffix suffixes;
	};
	static DEFINE_TEST_OK(struct memparse_test_ok, tests) = {
		/*
		 * The same pattern of kstrtoull, just with extra @retptr
		 * verification.
		 */
		{"0",			0ULL,			1},
		{"1",			1ULL,			1},
		{"127",			127ULL,			3},
		{"128",			128ULL,			3},
		{"129",			129ULL,			3},
		{"255",			255ULL,			3},
		{"256",			256ULL,			3},
		{"257",			257ULL,			3},
		{"32767",		32767ULL,		5},
		{"32768",		32768ULL,		5},
		{"32769",		32769ULL,		5},
		{"65535",		65535ULL,		5},
		{"65536",		65536ULL,		5},
		{"65537",		65537ULL,		5},
		{"2147483647",		2147483647ULL,		10},
		{"2147483648",		2147483648ULL,		10},
		{"2147483649",		2147483649ULL,		10},
		{"4294967295",		4294967295ULL,		10},
		{"4294967296",		4294967296ULL,		10},
		{"4294967297",		4294967297ULL,		10},
		{"9223372036854775807",	9223372036854775807ULL,	19},
		{"9223372036854775808",	9223372036854775808ULL,	19},
		{"9223372036854775809",	9223372036854775809ULL,	19},
		{"18446744073709551614", 18446744073709551614ULL, 20},
		{"18446744073709551615", 18446744073709551615ULL, 20},

		{"00",				00ULL,		2},
		{"01",				01ULL,		2},
		{"0177",			0177ULL,	4},
		{"0200",			0200ULL,	4},
		{"0201",			0201ULL,	4},
		{"0377",			0377ULL,	4},
		{"0400",			0400ULL,	4},
		{"0401",			0401ULL,	4},
		{"077777",			077777ULL,	6},
		{"0100000",			0100000ULL,	7},
		{"0100001",			0100001ULL,	7},
		{"0177777",			0177777ULL,	7},
		{"0200000",			0200000ULL,	7},
		{"0200001",			0200001ULL,	7},
		{"017777777777",		017777777777ULL,	12},
		{"020000000000",		020000000000ULL,	12},
		{"020000000001",		020000000001ULL,	12},
		{"037777777777",		037777777777ULL,	12},
		{"040000000000",		040000000000ULL,	12},
		{"040000000001",		040000000001ULL,	12},
		{"0777777777777777777777",	0777777777777777777777ULL, 22},
		{"01000000000000000000000",	01000000000000000000000ULL, 23},
		{"01000000000000000000001",	01000000000000000000001ULL, 23},
		{"01777777777777777777776",	01777777777777777777776ULL, 23},
		{"01777777777777777777777",	01777777777777777777777ULL, 23},

		{"0x0",			0x0ULL,			3},
		{"0x1",			0x1ULL,			3},
		{"0x7f",		0x7fULL,		4},
		{"0x80",		0x80ULL,		4},
		{"0x81",		0x81ULL,		4},
		{"0xff",		0xffULL,		4},
		{"0x100",		0x100ULL,		5},
		{"0x101",		0x101ULL,		5},
		{"0x7fff",		0x7fffULL,		6},
		{"0x8000",		0x8000ULL,		6},
		{"0x8001",		0x8001ULL,		6},
		{"0xffff",		0xffffULL,		6},
		{"0x10000",		0x10000ULL,		7},
		{"0x10001",		0x10001ULL,		7},
		{"0x7fffffff",		0x7fffffffULL,		10},
		{"0x80000000",		0x80000000ULL,		10},
		{"0x80000001",		0x80000001ULL,		10},
		{"0xffffffff",		0xffffffffULL,		10},
		{"0x100000000",		0x100000000ULL,		11},
		{"0x100000001",		0x100000001ULL,		11},
		{"0x7fffffffffffffff",	0x7fffffffffffffffULL,	18},
		{"0x8000000000000000",	0x8000000000000000ULL,	18},
		{"0x8000000000000001",	0x8000000000000001ULL,	18},
		{"0xfffffffffffffffe",	0xfffffffffffffffeULL,	18},
		{"0xffffffffffffffff",	0xffffffffffffffffULL,	18},

		/* Now with extra non-suffix chars to test @retptr update. */
		{"1q84",		1,			1},
		{"02o45",		2,			2},
		{"0xffvii",		0xff,			4},

		/*
		 * Valid suffix then tailing chars, to test the @retptr
		 * behavior.
		 */
		{"68k ",		69632,			3},
		{"8MS",			8388608,		2},
		{"0xaeGis",		0x2b80000000,		5},
		{"0xaTx",		0xa0000000000,		4},
		{"3E8",			0x3000000000000000,	2},

		/* Valid suffixes but not enabled. */
		{"68k ",	68,	2, MEMPARSE_SUFFIX_M},
		{"8MS",		8,	1, MEMPARSE_SUFFIX_K},
		{"0xaeGis",	0xae,	4, MEMPARSE_SUFFIX_K},
		{"0xaTx",	0xa,	3, MEMPARSE_SUFFIX_K},
		{"3E8",		3,	1, MEMPARSE_SUFFIX_K},
	};
	unsigned int i;

	for_each_test(i, tests) {
		const struct memparse_test_ok *t = &tests[i];
		unsigned long long tmp;
		char *retptr;
		int ret;
		enum memparse_suffix suffixes = MEMPARSE_TEST_SUFFIX;

		if (t->suffixes)
			suffixes = t->suffixes;

		ret = memparse_safe(t->str, suffixes, &tmp, &retptr);
		if (ret != 0) {
			WARN(1, "str '%s', expected ret 0 got %d\n", t->str, ret);
			continue;
		}
		if (tmp != t->expected_value)
			WARN(1, "str '%s' incorrect result, expected %llu got %llu",
			     t->str, t->expected_value, tmp);
		if (retptr != t->str + t->retptr_off)
			WARN(1, "str '%s' incorrect endptr, expected %u got %zu",
			     t->str, t->retptr_off, retptr - t->str);
	}
}
static void __init test_kstrtoll_fail(void)
{
	static DEFINE_TEST_FAIL(test_ll_fail) = {
		{"9223372036854775808",	10},
		{"9223372036854775809",	10},
		{"18446744073709551614",	10},
		{"18446744073709551615",	10},
		{"-9223372036854775809",	10},
		{"-18446744073709551614",	10},
		{"-18446744073709551615",	10},
		/* sign is first character if any */
		{"-+1", 0},
		{"-+1", 8},
		{"-+1", 10},
		{"-+1", 16},
	};
	TEST_FAIL(kstrtoll, long long, "%lld", test_ll_fail);
}

static void __init test_kstrtou64_ok(void)
{
	DECLARE_TEST_OK(u64, struct test_u64);
	static DEFINE_TEST_OK(struct test_u64, test_u64_ok) = {
		{"0",	10,	0},
		{"1",	10,	1},
		{"126",	10,	126},
		{"127",	10,	127},
		{"128",	10,	128},
		{"129",	10,	129},
		{"254",	10,	254},
		{"255",	10,	255},
		{"256",	10,	256},
		{"257",	10,	257},
		{"32766",	10,	32766},
		{"32767",	10,	32767},
		{"32768",	10,	32768},
		{"32769",	10,	32769},
		{"65534",	10,	65534},
		{"65535",	10,	65535},
		{"65536",	10,	65536},
		{"65537",	10,	65537},
		{"2147483646",	10,	2147483646},
		{"2147483647",	10,	2147483647},
		{"2147483648",	10,	2147483648ULL},
		{"2147483649",	10,	2147483649ULL},
		{"4294967294",	10,	4294967294ULL},
		{"4294967295",	10,	4294967295ULL},
		{"4294967296",	10,	4294967296ULL},
		{"4294967297",	10,	4294967297ULL},
		{"9223372036854775806",	10,	9223372036854775806ULL},
		{"9223372036854775807",	10,	9223372036854775807ULL},
		{"9223372036854775808",	10,	9223372036854775808ULL},
		{"9223372036854775809",	10,	9223372036854775809ULL},
		{"18446744073709551614",	10,	18446744073709551614ULL},
		{"18446744073709551615",	10,	18446744073709551615ULL},
	};
	TEST_OK(kstrtou64, u64, "%llu", test_u64_ok);
}

static void __init test_kstrtou64_fail(void)
{
	static DEFINE_TEST_FAIL(test_u64_fail) = {
		{"-2",	10},
		{"-1",	10},
		{"18446744073709551616",	10},
		{"18446744073709551617",	10},
	};
	TEST_FAIL(kstrtou64, u64, "%llu", test_u64_fail);
}

static void __init test_kstrtos64_ok(void)
{
	DECLARE_TEST_OK(s64, struct test_s64);
	static DEFINE_TEST_OK(struct test_s64, test_s64_ok) = {
		{"-128",	10,	-128},
		{"-127",	10,	-127},
		{"-1",	10,	-1},
		{"0",	10,	0},
		{"1",	10,	1},
		{"126",	10,	126},
		{"127",	10,	127},
		{"128",	10,	128},
		{"129",	10,	129},
		{"254",	10,	254},
		{"255",	10,	255},
		{"256",	10,	256},
		{"257",	10,	257},
		{"32766",	10,	32766},
		{"32767",	10,	32767},
		{"32768",	10,	32768},
		{"32769",	10,	32769},
		{"65534",	10,	65534},
		{"65535",	10,	65535},
		{"65536",	10,	65536},
		{"65537",	10,	65537},
		{"2147483646",	10,	2147483646},
		{"2147483647",	10,	2147483647},
		{"2147483648",	10,	2147483648LL},
		{"2147483649",	10,	2147483649LL},
		{"4294967294",	10,	4294967294LL},
		{"4294967295",	10,	4294967295LL},
		{"4294967296",	10,	4294967296LL},
		{"4294967297",	10,	4294967297LL},
		{"9223372036854775806",	10,	9223372036854775806LL},
		{"9223372036854775807",	10,	9223372036854775807LL},
	};
	TEST_OK(kstrtos64, s64, "%lld", test_s64_ok);
}

static void __init test_kstrtos64_fail(void)
{
	static DEFINE_TEST_FAIL(test_s64_fail) = {
		{"9223372036854775808",	10},
		{"9223372036854775809",	10},
		{"18446744073709551614",	10},
		{"18446744073709551615",	10},
		{"18446744073709551616",	10},
		{"18446744073709551617",	10},
	};
	TEST_FAIL(kstrtos64, s64, "%lld", test_s64_fail);
}

static void __init test_kstrtou32_ok(void)
{
	DECLARE_TEST_OK(u32, struct test_u32);
	static DEFINE_TEST_OK(struct test_u32, test_u32_ok) = {
		{"0",	10,	0},
		{"1",	10,	1},
		{"126",	10,	126},
		{"127",	10,	127},
		{"128",	10,	128},
		{"129",	10,	129},
		{"254",	10,	254},
		{"255",	10,	255},
		{"256",	10,	256},
		{"257",	10,	257},
		{"32766",	10,	32766},
		{"32767",	10,	32767},
		{"32768",	10,	32768},
		{"32769",	10,	32769},
		{"65534",	10,	65534},
		{"65535",	10,	65535},
		{"65536",	10,	65536},
		{"65537",	10,	65537},
		{"2147483646",	10,	2147483646},
		{"2147483647",	10,	2147483647},
		{"2147483648",	10,	2147483648U},
		{"2147483649",	10,	2147483649U},
		{"4294967294",	10,	4294967294U},
		{"4294967295",	10,	4294967295U},
	};
	TEST_OK(kstrtou32, u32, "%u", test_u32_ok);
}

static void __init test_kstrtou32_fail(void)
{
	static DEFINE_TEST_FAIL(test_u32_fail) = {
		{"-2",	10},
		{"-1",	10},
		{"4294967296",	10},
		{"4294967297",	10},
		{"9223372036854775806",	10},
		{"9223372036854775807",	10},
		{"9223372036854775808",	10},
		{"9223372036854775809",	10},
		{"18446744073709551614",	10},
		{"18446744073709551615",	10},
		{"18446744073709551616",	10},
		{"18446744073709551617",	10},
	};
	TEST_FAIL(kstrtou32, u32, "%u", test_u32_fail);
}

static void __init test_kstrtos32_ok(void)
{
	DECLARE_TEST_OK(s32, struct test_s32);
	static DEFINE_TEST_OK(struct test_s32, test_s32_ok) = {
		{"-128",	10,	-128},
		{"-127",	10,	-127},
		{"-1",	10,	-1},
		{"0",	10,	0},
		{"1",	10,	1},
		{"126",	10,	126},
		{"127",	10,	127},
		{"128",	10,	128},
		{"129",	10,	129},
		{"254",	10,	254},
		{"255",	10,	255},
		{"256",	10,	256},
		{"257",	10,	257},
		{"32766",	10,	32766},
		{"32767",	10,	32767},
		{"32768",	10,	32768},
		{"32769",	10,	32769},
		{"65534",	10,	65534},
		{"65535",	10,	65535},
		{"65536",	10,	65536},
		{"65537",	10,	65537},
		{"2147483646",	10,	2147483646},
		{"2147483647",	10,	2147483647},
	};
	TEST_OK(kstrtos32, s32, "%d", test_s32_ok);
}

static void __init test_kstrtos32_fail(void)
{
	static DEFINE_TEST_FAIL(test_s32_fail) = {
		{"2147483648",	10},
		{"2147483649",	10},
		{"4294967294",	10},
		{"4294967295",	10},
		{"4294967296",	10},
		{"4294967297",	10},
		{"9223372036854775806",	10},
		{"9223372036854775807",	10},
		{"9223372036854775808",	10},
		{"9223372036854775809",	10},
		{"18446744073709551614",	10},
		{"18446744073709551615",	10},
		{"18446744073709551616",	10},
		{"18446744073709551617",	10},
	};
	TEST_FAIL(kstrtos32, s32, "%d", test_s32_fail);
}

static void __init test_kstrtou16_ok(void)
{
	DECLARE_TEST_OK(u16, struct test_u16);
	static DEFINE_TEST_OK(struct test_u16, test_u16_ok) = {
		{"0",	10,	0},
		{"1",	10,	1},
		{"126",	10,	126},
		{"127",	10,	127},
		{"128",	10,	128},
		{"129",	10,	129},
		{"254",	10,	254},
		{"255",	10,	255},
		{"256",	10,	256},
		{"257",	10,	257},
		{"32766",	10,	32766},
		{"32767",	10,	32767},
		{"32768",	10,	32768},
		{"32769",	10,	32769},
		{"65534",	10,	65534},
		{"65535",	10,	65535},
	};
	TEST_OK(kstrtou16, u16, "%hu", test_u16_ok);
}

static void __init test_kstrtou16_fail(void)
{
	static DEFINE_TEST_FAIL(test_u16_fail) = {
		{"-2",	10},
		{"-1",	10},
		{"65536",	10},
		{"65537",	10},
		{"2147483646",	10},
		{"2147483647",	10},
		{"2147483648",	10},
		{"2147483649",	10},
		{"4294967294",	10},
		{"4294967295",	10},
		{"4294967296",	10},
		{"4294967297",	10},
		{"9223372036854775806",	10},
		{"9223372036854775807",	10},
		{"9223372036854775808",	10},
		{"9223372036854775809",	10},
		{"18446744073709551614",	10},
		{"18446744073709551615",	10},
		{"18446744073709551616",	10},
		{"18446744073709551617",	10},
	};
	TEST_FAIL(kstrtou16, u16, "%hu", test_u16_fail);
}

static void __init test_kstrtos16_ok(void)
{
	DECLARE_TEST_OK(s16, struct test_s16);
	static DEFINE_TEST_OK(struct test_s16, test_s16_ok) = {
		{"-130",	10,	-130},
		{"-129",	10,	-129},
		{"-128",	10,	-128},
		{"-127",	10,	-127},
		{"-1",	10,	-1},
		{"0",	10,	0},
		{"1",	10,	1},
		{"126",	10,	126},
		{"127",	10,	127},
		{"128",	10,	128},
		{"129",	10,	129},
		{"254",	10,	254},
		{"255",	10,	255},
		{"256",	10,	256},
		{"257",	10,	257},
		{"32766",	10,	32766},
		{"32767",	10,	32767},
	};
	TEST_OK(kstrtos16, s16, "%hd", test_s16_ok);
}

static void __init test_kstrtos16_fail(void)
{
	static DEFINE_TEST_FAIL(test_s16_fail) = {
		{"32768",	10},
		{"32769",	10},
		{"65534",	10},
		{"65535",	10},
		{"65536",	10},
		{"65537",	10},
		{"2147483646",	10},
		{"2147483647",	10},
		{"2147483648",	10},
		{"2147483649",	10},
		{"4294967294",	10},
		{"4294967295",	10},
		{"4294967296",	10},
		{"4294967297",	10},
		{"9223372036854775806",	10},
		{"9223372036854775807",	10},
		{"9223372036854775808",	10},
		{"9223372036854775809",	10},
		{"18446744073709551614",	10},
		{"18446744073709551615",	10},
		{"18446744073709551616",	10},
		{"18446744073709551617",	10},
	};
	TEST_FAIL(kstrtos16, s16, "%hd", test_s16_fail);
}

static void __init test_kstrtou8_ok(void)
{
	DECLARE_TEST_OK(u8, struct test_u8);
	static DEFINE_TEST_OK(struct test_u8, test_u8_ok) = {
		{"0",	10,	0},
		{"1",	10,	1},
		{"126",	10,	126},
		{"127",	10,	127},
		{"128",	10,	128},
		{"129",	10,	129},
		{"254",	10,	254},
		{"255",	10,	255},
	};
	TEST_OK(kstrtou8, u8, "%hhu", test_u8_ok);
}

static void __init test_kstrtou8_fail(void)
{
	static DEFINE_TEST_FAIL(test_u8_fail) = {
		{"-2",	10},
		{"-1",	10},
		{"256",	10},
		{"257",	10},
		{"32766",	10},
		{"32767",	10},
		{"32768",	10},
		{"32769",	10},
		{"65534",	10},
		{"65535",	10},
		{"65536",	10},
		{"65537",	10},
		{"2147483646",	10},
		{"2147483647",	10},
		{"2147483648",	10},
		{"2147483649",	10},
		{"4294967294",	10},
		{"4294967295",	10},
		{"4294967296",	10},
		{"4294967297",	10},
		{"9223372036854775806",	10},
		{"9223372036854775807",	10},
		{"9223372036854775808",	10},
		{"9223372036854775809",	10},
		{"18446744073709551614",	10},
		{"18446744073709551615",	10},
		{"18446744073709551616",	10},
		{"18446744073709551617",	10},
	};
	TEST_FAIL(kstrtou8, u8, "%hhu", test_u8_fail);
}

static void __init test_kstrtos8_ok(void)
{
	DECLARE_TEST_OK(s8, struct test_s8);
	static DEFINE_TEST_OK(struct test_s8, test_s8_ok) = {
		{"-128",	10,	-128},
		{"-127",	10,	-127},
		{"-1",	10,	-1},
		{"0",	10,	0},
		{"1",	10,	1},
		{"126",	10,	126},
		{"127",	10,	127},
	};
	TEST_OK(kstrtos8, s8, "%hhd", test_s8_ok);
}

static void __init test_kstrtos8_fail(void)
{
	static DEFINE_TEST_FAIL(test_s8_fail) = {
		{"-130",	10},
		{"-129",	10},
		{"128",	10},
		{"129",	10},
		{"254",	10},
		{"255",	10},
		{"256",	10},
		{"257",	10},
		{"32766",	10},
		{"32767",	10},
		{"32768",	10},
		{"32769",	10},
		{"65534",	10},
		{"65535",	10},
		{"65536",	10},
		{"65537",	10},
		{"2147483646",	10},
		{"2147483647",	10},
		{"2147483648",	10},
		{"2147483649",	10},
		{"4294967294",	10},
		{"4294967295",	10},
		{"4294967296",	10},
		{"4294967297",	10},
		{"9223372036854775806",	10},
		{"9223372036854775807",	10},
		{"9223372036854775808",	10},
		{"9223372036854775809",	10},
		{"18446744073709551614",	10},
		{"18446744073709551615",	10},
		{"18446744073709551616",	10},
		{"18446744073709551617",	10},
	};
	TEST_FAIL(kstrtos8, s8, "%hhd", test_s8_fail);
}

static int __init test_kstrtox_init(void)
{
	test_kstrtoull_ok();
	test_kstrtoull_fail();
	test_kstrtoll_ok();
	test_kstrtoll_fail();

	test_memparse_safe_ok();
	test_memparse_safe_fail();

	test_kstrtou64_ok();
	test_kstrtou64_fail();
	test_kstrtos64_ok();
	test_kstrtos64_fail();

	test_kstrtou32_ok();
	test_kstrtou32_fail();
	test_kstrtos32_ok();
	test_kstrtos32_fail();

	test_kstrtou16_ok();
	test_kstrtou16_fail();
	test_kstrtos16_ok();
	test_kstrtos16_fail();

	test_kstrtou8_ok();
	test_kstrtou8_fail();
	test_kstrtos8_ok();
	test_kstrtos8_fail();
	return -EINVAL;
}
module_init(test_kstrtox_init);
MODULE_LICENSE("Dual BSD/GPL");
