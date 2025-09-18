// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */
#include <crypto/sha3.h>
#include "sha3_shake256_testvecs.h"

static int test_shake256_256(const u8 *in, size_t in_len, u8 *out)
{
	return shake256(in, in_len, out, SHAKE256_DIGEST_SIZE);
}

static void test_shake256_init_256(struct sha3_ctx *ctx)
{
	return shake256_init(ctx, SHAKE256_DIGEST_SIZE);
}

#define HASH		test_shake256_256
#define HASH_CTX	sha3_ctx
#define HASH_SIZE	SHAKE256_DIGEST_SIZE
#define HASH_INIT	test_shake256_init_256
#define HASH_UPDATE	sha3_update
#define HASH_FINAL	sha3_final
#include "hash-test-template.h"

static struct kunit_case hash_test_cases[] = {
	HASH_KUNIT_CASES,
	KUNIT_CASE(benchmark_hash),
	{},
};

static struct kunit_suite hash_test_suite = {
	.name = "shake256",
	.test_cases = hash_test_cases,
	.suite_init = hash_suite_init,
	.suite_exit = hash_suite_exit,
};
kunit_test_suite(hash_test_suite);

MODULE_DESCRIPTION("KUnit tests and benchmark for SHAKE-256 with 256-bit digest");
MODULE_LICENSE("GPL");
