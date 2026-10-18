// SPDX-License-Identifier: GPL-2.0
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tests.h"
#include "debug.h"
#include "util/ftrace.h"

static int test_parse_retaddr(void)
{
	char sym[128];
	u64 offset = 0;

	/* Standard format */
	TEST_ASSERT_VAL("parse standard retaddr",
			ftrace_parse_retaddr("kernel_read() { /* <-load_elf_phdrs+0x6c/0xb0 */",
					     sym, sizeof(sym), &offset));
	TEST_ASSERT_EQUAL("sym name", strcmp(sym, "load_elf_phdrs"), 0);
	TEST_ASSERT_VAL("offset", offset == 0x6c);

	/* With return value */
	TEST_ASSERT_VAL("parse retaddr with ret",
			ftrace_parse_retaddr(
				"__cond_resched(); /* <-load_elf_phdrs+0x6c/0xb0 ret=0x0 */",
				sym, sizeof(sym), &offset));
	TEST_ASSERT_EQUAL("sym name", strcmp(sym, "load_elf_phdrs"), 0);
	TEST_ASSERT_VAL("offset", offset == 0x6c);

	/* Bracketed address */
	TEST_ASSERT_VAL("parse bracketed retaddr",
			ftrace_parse_retaddr("foo() { /* <-[0xffffffff818534c8] bar+0x20/0x40 */",
					     sym, sizeof(sym), &offset));
	TEST_ASSERT_EQUAL("sym name", strcmp(sym, "bar"), 0);
	TEST_ASSERT_VAL("offset", offset == 0x20);

	/* No return address */
	TEST_ASSERT_VAL("no retaddr",
			!ftrace_parse_retaddr("do_filp_open();", sym, sizeof(sym), &offset));

	return TEST_OK;
}

static int test_filter_retaddr(void)
{
	char line1[] = "kernel_read() { /* <-load_elf_phdrs+0x6c/0xb0 */";
	char line2[] = "__cond_resched(); /* <-load_elf_phdrs+0x6c/0xb0 ret=0x0 */";
	char line3[] = "do_filp_open();";

	ftrace_filter_retaddr(line1);
	TEST_ASSERT_EQUAL("filter standard retaddr", strcmp(line1, "kernel_read() {"), 0);

	ftrace_filter_retaddr(line2);
	TEST_ASSERT_EQUAL("filter retaddr with ret", strcmp(line2, "__cond_resched();"), 0);

	ftrace_filter_retaddr(line3);
	TEST_ASSERT_EQUAL("filter line without retaddr unchanged",
			  strcmp(line3, "do_filp_open();"), 0);

	return TEST_OK;
}

static int test__ftrace(struct test_suite *test __maybe_unused, int subtest __maybe_unused)
{
	int ret;

	ret = test_parse_retaddr();
	if (ret != TEST_OK)
		return ret;

	ret = test_filter_retaddr();
	if (ret != TEST_OK)
		return ret;

	return TEST_OK;
}

DEFINE_SUITE("Ftrace return address processing", ftrace);
