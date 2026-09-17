/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LIB_TESTS_LINEINFO_KUNIT_H
#define _LIB_TESTS_LINEINFO_KUNIT_H

#include <linux/compiler.h>

int lineinfo_target_second_file(int x);

/*
 * Inlined into lineinfo_cross_file_caller() in lineinfo_kunit.c, so the
 * addresses it covers must resolve to this header rather than to the
 * caller's file.  __always_inline because the test object is built with
 * -fno-inline-functions-called-once.
 */
static __always_inline int lineinfo_header_helper(int x)
{
	barrier();
	return x + 1;
}

#endif /* _LIB_TESTS_LINEINFO_KUNIT_H */
