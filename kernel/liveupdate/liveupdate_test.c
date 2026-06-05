// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit test for Live Update compatibility strings.
 */
#include <kunit/test.h>
#include <linux/string.h>
#include <linux/kho/abi/kexec_handover.h>
#include <linux/kho/abi/luo.h>
#include <linux/kho/abi/memfd.h>

/* Verify that compatibility sub-components are unique and sorted alphabetically */
static bool is_alphabetical_unique(const char *compat)
{
	char buf[1024];
	char *string = buf;
	char *token;
	char *prev = NULL;
	char *sub;

	strscpy(buf, compat, sizeof(buf));

	sub = strchr(string, ';');
	if (!sub)
		return true;

	sub++;

	while ((token = strsep(&sub, ";")) != NULL) {
		if (prev && strcmp(prev, token) >= 0)
			return false;
		prev = token;
	}

	return true;
}

static void test_compatibility_alphabetical(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, is_alphabetical_unique(KHO_FDT_COMPATIBLE));
	KUNIT_EXPECT_TRUE(test, is_alphabetical_unique(LUO_ABI_COMPATIBLE));
	KUNIT_EXPECT_TRUE(test, is_alphabetical_unique(MEMFD_LUO_FH_COMPATIBLE));
}

static struct kunit_case liveupdate_test_cases[] = {
	KUNIT_CASE(test_compatibility_alphabetical),
	{}
};

static struct kunit_suite liveupdate_test_suite = {
	.name = "liveupdate-compatibility",
	.test_cases = liveupdate_test_cases,
};

kunit_test_suite(liveupdate_test_suite);

MODULE_LICENSE("GPL");
