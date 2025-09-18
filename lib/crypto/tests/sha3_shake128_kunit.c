// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */
#include <crypto/sha3.h>
#include "sha3_shake128_testvecs.h"

static int test_shake128_128(const u8 *in, size_t in_len, u8 *out)
{
	return shake128(in, in_len, out, SHAKE128_DIGEST_SIZE);
}

static void test_shake128_init_128(struct sha3_ctx *ctx)
{
	return shake128_init(ctx, SHAKE128_DIGEST_SIZE);
}

#define HASH		test_shake128_128
#define HASH_CTX	sha3_ctx
#define HASH_SIZE	SHAKE128_DIGEST_SIZE
#define HASH_INIT	test_shake128_init_128
#define HASH_UPDATE	sha3_update
#define HASH_FINAL	sha3_final
#include "hash-test-template.h"

static struct kunit_case hash_test_cases[] = {
	HASH_KUNIT_CASES,
	KUNIT_CASE(benchmark_hash),
	{},
};

static struct kunit_suite hash_test_suite = {
	.name = "shake128",
	.test_cases = hash_test_cases,
	.suite_init = hash_suite_init,
	.suite_exit = hash_suite_exit,
};
kunit_test_suite(hash_test_suite);

MODULE_DESCRIPTION("KUnit tests and benchmark for SHAKE-128 with 128-bit digest");
MODULE_LICENSE("GPL");
