/* SPDX-License-Identifier: GPL-2.0+ */
/* devmem test tests.h
 *
 * Copyright (C) 2025 Red Hat, Inc. All Rights Reserved.
 * Written by Alessandro Carminati (acarmina@redhat.com)
 */

#ifndef TESTS_H
#define TESTS_H

#include "utils.h"

#define EXPECTED_LINEAR_LIMIT 0x377fe000
#define PASS 0
#define FAIL -1
#define SKIPPED 1
#define OK_STR "[\e[1;32mPASS\e[0m]"
#define KO_STR "[\e[1;31mFAIL\e[0m]"
#define SKP_STR "[\e[1;33mSKIP\e[0m]"
#define DC_STR "[\e[1;33mDON'T CARE\e[0m]"
#define WARN_STR "\e[1;31mThis shouldn't have happen. Memory is probably corrupted!\e[0m"
#define NO_WARN_STR ""

int test_read_at_addr_32bit_ge(struct test_context *t);
int test_read_outside_linear_map(struct test_context *t);
int test_strict_devmem(struct test_context *t);
int test_devmem_access(struct test_context *t);
int test_read_secret_area(struct test_context *t);
int test_read_allowed_area(struct test_context *t);
int test_read_reserved_area(struct test_context *t);
int test_read_allowed_area(struct test_context *t);
int test_read_allowed_area_ppos_advance(struct test_context *t);
int test_read_restricted_area(struct test_context *t);
int test_write_outside_area(struct test_context *t);
int test_seek_seek_cur(struct test_context *t);
int test_seek_seek_set(struct test_context *t);
int test_seek_seek_other(struct test_context *t);
int test_open_devnum(struct test_context *t);

static inline bool is_64bit_arch(void)
{
	return sizeof(void *) == 8;
}

#endif
