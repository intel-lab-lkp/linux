// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */
#include <crypto/sha3.h>
#include "sha3_384_testvecs.h"

#define HASH		sha3_384
#define HASH_CTX	sha3_ctx
#define HASH_SIZE	SHA3_384_DIGEST_SIZE
#define HASH_INIT	sha3_384_init
#define HASH_UPDATE	sha3_update
#define HASH_FINAL	sha3_final
#include "hash-test-template.h"

static struct kunit_case hash_test_cases[] = {
	HASH_KUNIT_CASES,
	KUNIT_CASE(benchmark_hash),
	{},
};

static struct kunit_suite hash_test_suite = {
	.name = "sha3_384",
	.test_cases = hash_test_cases,
	.suite_init = hash_suite_init,
	.suite_exit = hash_suite_exit,
};
kunit_test_suite(hash_test_suite);

MODULE_DESCRIPTION("KUnit tests and benchmark for SHA3-384");
MODULE_LICENSE("GPL");
