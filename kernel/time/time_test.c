// SPDX-License-Identifier: LGPL-2.1+

#include <kunit/test.h>
#include <linux/gcd.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/time.h>

/*
 * Traditional implementation of leap year evaluation, but note that long
 * is a signed type and the tests do cover negative year values. So this
 * can't use the is_leap_year() helper from rtc.h.
 */
static bool is_leap(long year)
{
	return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

/*
 * Gets the last day of a month.
 */
static int last_day_of_month(long year, int month)
{
	if (month == 2)
		return 28 + is_leap(year);
	if (month == 4 || month == 6 || month == 9 || month == 11)
		return 30;
	return 31;
}

/*
 * Advances a date by one day.
 */
static void advance_date(long *year, int *month, int *mday, int *yday)
{
	if (*mday != last_day_of_month(*year, *month)) {
		++*mday;
		++*yday;
		return;
	}

	*mday = 1;
	if (*month != 12) {
		++*month;
		++*yday;
		return;
	}

	*month = 1;
	*yday  = 0;
	++*year;
}

/*
 * Checks every day in a 160000 years interval centered at 1970-01-01
 * against the expected result.
 */
static void time64_to_tm_test_date_range(struct kunit *test)
{
	/*
	 * 80000 years	= (80000 / 400) * 400 years
	 *		= (80000 / 400) * 146097 days
	 *		= (80000 / 400) * 146097 * 86400 seconds
	 */
	time64_t total_secs = ((time64_t) 80000) / 400 * 146097 * 86400;
	long year = 1970 - 80000;
	int month = 1;
	int mdday = 1;
	int yday = 0;

	struct tm result;
	time64_t secs;
	s64 days;

	for (secs = -total_secs; secs <= total_secs; secs += 86400) {

		time64_to_tm(secs, 0, &result);

		days = div_s64(secs, 86400);

		#define FAIL_MSG "%05ld/%02d/%02d (%2d) : %lld", \
			year, month, mdday, yday, days

		KUNIT_ASSERT_EQ_MSG(test, year - 1900, result.tm_year, FAIL_MSG);
		KUNIT_ASSERT_EQ_MSG(test, month - 1, result.tm_mon, FAIL_MSG);
		KUNIT_ASSERT_EQ_MSG(test, mdday, result.tm_mday, FAIL_MSG);
		KUNIT_ASSERT_EQ_MSG(test, yday, result.tm_yday, FAIL_MSG);

		advance_date(&year, &month, &mdday, &yday);
	}
}

/*
 * jiffies_to_clock_t() must be the inverse of clock_t_to_jiffies() for every
 * clock_t value that is a whole number of jiffies, whatever HZ is.
 */
static void jiffies_to_clock_t_test(struct kunit *test)
{
	unsigned long step = USER_HZ / gcd(HZ, USER_HZ);
	unsigned long x;

	/* Bridge STP defaults, exposed in USER_HZ through sysfs and netlink */
	KUNIT_EXPECT_EQ(test, 15 * USER_HZ, jiffies_to_clock_t(15 * HZ));
	KUNIT_EXPECT_EQ(test, 2 * USER_HZ, jiffies_to_clock_t(2 * HZ));
	KUNIT_EXPECT_EQ(test, 300 * USER_HZ, jiffies_to_clock_t(300 * HZ));
	/* Large enough to catch a rounded NSEC_PER_SEC / USER_HZ (alpha) */
	KUNIT_EXPECT_EQ(test, 10000 * USER_HZ, jiffies_to_clock_t(10000 * HZ));

	for (x = 0; x <= 100 * USER_HZ; x += step) {
		unsigned long j = clock_t_to_jiffies(x);
		clock_t c = jiffies_to_clock_t(j);
		u64 c64 = jiffies_64_to_clock_t(j);

		KUNIT_ASSERT_EQ_MSG(test, x * HZ, (u64)j * USER_HZ,
				    "clock_t %lu -> %lu jiffies", x, j);
		if (c != (long)x || c64 != x) {
			KUNIT_FAIL(test, "clock_t %lu -> %lu jiffies -> %ld / %llu",
				   x, j, (long)c, c64);
			break;
		}
	}
}

/*
 * Every unsigned int number of microseconds fits in MAX_JIFFY_OFFSET jiffies
 * for any supported HZ, so usecs_to_jiffies() must never saturate. It must
 * round up; when HZ does not divide USEC_PER_SEC, the reciprocal
 * multiplication may round up by one more jiffy for large values.
 */
static bool check_usecs_to_jiffies(struct kunit *test, unsigned int u,
				   unsigned long got)
{
	unsigned long want = DIV_ROUND_UP_ULL((u64)u * HZ, USEC_PER_SEC);

	if (got >= want && got <= want + !!(USEC_PER_SEC % HZ))
		return true;
	KUNIT_FAIL(test, "usecs_to_jiffies(%u) = %lu, want %lu%s", u, got,
		   want, USEC_PER_SEC % HZ ? " or one more" : "");
	return false;
}

static void usecs_to_jiffies_test(struct kunit *test)
{
	static const unsigned int vals[] = {
		0, 1, 999, 1000, 1001, 1000000, 1431649098, 1431649099,
		2000000000, 3000000000U, UINT_MAX - 20000, UINT_MAX - 1,
		UINT_MAX,
	};
	u64 x;
	int i;

	for (i = 0; i < ARRAY_SIZE(vals); i++)
		check_usecs_to_jiffies(test, vals[i], usecs_to_jiffies(vals[i]));

	for (x = 0; x <= UINT_MAX; x += 999983)
		if (!check_usecs_to_jiffies(test, x, usecs_to_jiffies(x)))
			break;
	for (x = UINT_MAX - 30000; x <= UINT_MAX; x++)
		if (!check_usecs_to_jiffies(test, x, usecs_to_jiffies(x)))
			break;

	/* Constant-folded variant */
	check_usecs_to_jiffies(test, 3000000000U, usecs_to_jiffies(3000000000U));
	check_usecs_to_jiffies(test, UINT_MAX, usecs_to_jiffies(UINT_MAX));
}

static struct kunit_case time_test_cases[] = {
	KUNIT_CASE_SLOW(time64_to_tm_test_date_range),
	KUNIT_CASE(jiffies_to_clock_t_test),
	KUNIT_CASE(usecs_to_jiffies_test),
	{}
};

static struct kunit_suite time_test_suite = {
	.name = "time_test_cases",
	.test_cases = time_test_cases,
};

kunit_test_suite(time_test_suite);
MODULE_DESCRIPTION("time unit test suite");
MODULE_LICENSE("GPL");
