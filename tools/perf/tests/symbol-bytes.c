// SPDX-License-Identifier: GPL-2.0
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "symbol.h"
#include "symbol_conf.h"
#include "tests.h"

static int test__symbol_bytes_long_name(struct test_suite *test __maybe_unused,
					int subtest __maybe_unused)
{
	const size_t name_len = 65536;
	unsigned long saved_max = symbol_conf.max_symbol_bytes;
	size_t baseline = symbol__bytes_used();
	struct symbol *sym = NULL;
	size_t expected = symbol_conf.priv_size + sizeof(*sym) + name_len + 1;
	char *name;
	int ret = TEST_FAIL;

	symbol_conf.max_symbol_bytes = 0;
	name = malloc(name_len + 1);
	if (!name)
		goto out;
	memset(name, 'a', name_len);
	name[name_len] = '\0';

	sym = symbol__new(0, 1, 0, 0, name);
	if (!sym)
		goto out_free_name;
	if (symbol__bytes_used() != baseline + expected) {
		pr_debug("long symbol name accounting mismatch\n");
		goto out_delete;
	}

	/* Kallsyms splitting can shorten the stored name in place. */
	sym->name[10] = '\0';
	symbol__delete(sym);
	sym = NULL;
	if (symbol__bytes_used() != baseline) {
		pr_debug("long symbol name was not fully unaccounted\n");
		goto out_free_name;
	}
	ret = TEST_OK;

out_delete:
	if (sym)
		symbol__delete(sym);
out_free_name:
	free(name);
out:
	symbol_conf.max_symbol_bytes = saved_max;
	return ret;
}

struct reserve_arg {
	size_t bytes;
	bool success;
};

static void *reserve_bytes(void *data)
{
	struct reserve_arg *arg = data;

	arg->success = symbol__try_account_bytes(arg->bytes);
	return NULL;
}

static int test__symbol_bytes_reservation(struct test_suite *test __maybe_unused,
					  int subtest __maybe_unused)
{
	enum { NR_THREADS = 8, NR_ALLOWED = 4 };
	const size_t reservation = 1024;
	unsigned long saved_max = symbol_conf.max_symbol_bytes;
	size_t baseline = symbol__bytes_used();
	struct reserve_arg args[NR_THREADS];
	pthread_t threads[NR_THREADS];
	int created = 0, successful = 0;
	int ret = TEST_FAIL;
	int i;

	if (baseline > ULONG_MAX - NR_ALLOWED * reservation)
		return TEST_SKIP;

	symbol_conf.max_symbol_bytes = baseline + NR_ALLOWED * reservation;
	if (symbol__try_account_bytes(SIZE_MAX)) {
		pr_debug("overflowing symbol reservation succeeded\n");
		symbol__unaccount_bytes(SIZE_MAX);
		goto out;
	}

	for (i = 0; i < NR_THREADS; i++) {
		args[i].bytes = reservation;
		args[i].success = false;
		if (pthread_create(&threads[i], NULL, reserve_bytes, &args[i]))
			goto out_join;
		created++;
	}

out_join:
	for (i = 0; i < created; i++)
		pthread_join(threads[i], NULL);
	for (i = 0; i < created; i++) {
		if (args[i].success)
			successful++;
	}

	if (created != NR_THREADS || successful != NR_ALLOWED) {
		pr_debug("symbol reservation count: created %d, successful %d\n",
			 created, successful);
		goto out_release;
	}
	if (symbol__bytes_used() != baseline + NR_ALLOWED * reservation) {
		pr_debug("symbol reservation exceeded configured budget\n");
		goto out_release;
	}
	ret = TEST_OK;

out_release:
	for (i = 0; i < created; i++) {
		if (args[i].success)
			symbol__unaccount_bytes(args[i].bytes);
	}
out:
	symbol_conf.max_symbol_bytes = saved_max;
	if (symbol__bytes_used() != baseline)
		ret = TEST_FAIL;
	return ret;
}

static struct test_case tests__symbol_bytes[] = {
	TEST_CASE("Long name accounting", symbol_bytes_long_name),
	TEST_CASE("Concurrent strict reservations", symbol_bytes_reservation),
	{ .name = NULL, }
};

struct test_suite suite__symbol_bytes = {
	.desc = "Symbol memory accounting",
	.test_cases = tests__symbol_bytes,
};
