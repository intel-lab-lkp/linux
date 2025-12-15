// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2025 Rusydi H. Makarim <rusydi.makarim@kriptograf.id>
 */

#include <crypto/ascon_hash.h>
#include "ascon_hash-testvecs.h"

#define HASH		ascon_hash256
#define HASH_CTX	ascon_hash256_ctx
#define HASH_SIZE	ASCON_HASH256_DIGEST_SIZE
#define HASH_INIT	ascon_hash256_init
#define HASH_UPDATE	ascon_hash256_update
#define HASH_FINAL	ascon_hash256_final

#include "hash-test-template.h"

static struct kunit_case hash_test_cases[] = {
	HASH_KUNIT_CASES,
	KUNIT_CASE(benchmark_hash),
	{},
};

static struct kunit_suite hash_test_suite = {
	.name = "ascon_hash256",
	.test_cases = hash_test_cases,
	.suite_init = hash_suite_init,
	.suite_exit = hash_suite_exit,
};
kunit_test_suite(hash_test_suite);

MODULE_DESCRIPTION("KUnit tests and benchmark for Ascon-Hash256");
MODULE_LICENSE("GPL");
