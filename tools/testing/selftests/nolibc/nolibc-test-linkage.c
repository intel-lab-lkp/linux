/* SPDX-License-Identifier: GPL-2.0 */

#include "nolibc-test-linkage.h"

#include <errno.h>

/*
 * Set BIT(step + 1), BIT(0) shows whether all steps ran once and in order
 *
 * Copied from nolibc-test.c.
 */
#define MARK_STEP_DONE(val, step) do {					\
	if ((val) == 0 && (step) == 0)					\
		(val) |= 0x1;						\
	else if (!(val & (1 << (step))) || (val) & (1 << ((step) + 1)))	\
		(val) &= ~0x1;						\
	(val) |= 1 << ((step) + 1);					\
	} while (0)

void *linkage_test_errno_addr(void)
{
	return &errno;
}

int linkage_test_constructor_test_value = 0;

__attribute__((constructor))
static void constructor1(void)
{
	MARK_STEP_DONE(linkage_test_constructor_test_value, 0);
}

__attribute__((constructor))
static void constructor2(void)
{
	MARK_STEP_DONE(linkage_test_constructor_test_value, 1);
}
