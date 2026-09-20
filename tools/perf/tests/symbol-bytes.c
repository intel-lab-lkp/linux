// SPDX-License-Identifier: GPL-2.0
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/kernel.h>
#include <linux/zalloc.h>

#include "debug.h"
#include "dso.h"
#include "map.h"
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

static int test__symbol_bytes_duplicate_selection(struct test_suite *test __maybe_unused,
						  int subtest __maybe_unused)
{
	struct duplicate_case {
		u64 a_size;
		u8 a_type;
		u8 a_binding;
		const char *a_name;
		u64 b_size;
		u8 b_type;
		u8 b_binding;
		const char *b_name;
		int expected;
	} cases[] = {
		{ 1, STT_FUNC, STB_GLOBAL, "a", 0, STT_FUNC, STB_GLOBAL, "b", SYMBOL_A },
		{ 1, STT_NOTYPE, STB_GLOBAL, "a", 1, STT_FUNC, STB_GLOBAL, "b", SYMBOL_B },
		{ 1, STT_FUNC, STB_WEAK, "a", 1, STT_FUNC, STB_GLOBAL, "b", SYMBOL_B },
		{ 1, STT_FUNC, STB_GLOBAL, "a", 1, STT_FUNC, STB_LOCAL, "b", SYMBOL_A },
		{ 1, STT_FUNC, STB_GLOBAL, "name", 1, STT_FUNC, STB_GLOBAL, "_name", SYMBOL_A },
		{ 1, STT_FUNC, STB_GLOBAL, "a", 1, STT_FUNC, STB_GLOBAL, "long", SYMBOL_B },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct duplicate_case *c = &cases[i];

		if (symbol__choose_best(c->a_size, c->a_type, c->a_binding, c->a_name,
					c->b_size, c->b_type, c->b_binding, c->b_name) !=
		    c->expected)
			return TEST_FAIL;
	}
	return TEST_OK;
}

#ifdef HAVE_LIBELF_SUPPORT
static int truncated_name_case(size_t file_size, unsigned int expected_reads)
{
	char path[] = "/tmp/perf-lazy-truncated-XXXXXX";
	struct dso *data_dso = NULL;
	char *contents = NULL;
	char *name_heap = NULL;
	char namebuf[1024];
	const char *name;
	unsigned int nr_reads;
	int ret = TEST_FAIL;
	int fd = -1;

	contents = malloc(file_size);
	if (!contents)
		goto out;
	memset(contents, 'a', file_size);

	fd = mkstemp(path);
	if (fd < 0 || write(fd, contents, file_size) != (ssize_t)file_size)
		goto out;
	close(fd);
	fd = -1;

	data_dso = dso__new(path);
	if (!data_dso || dso__data_set_path(data_dso, path) < 0)
		goto out;
	dso__set_binary_type(data_dso, DSO_BINARY_TYPE__SYSTEM_PATH_DSO);
	name = dso__read_ondemand_symbol_name(data_dso, 0, 8192, 0,
					      namebuf, sizeof(namebuf),
					      &name_heap, &nr_reads);
	if (name || name_heap || nr_reads != expected_reads)
		goto out;
	ret = TEST_OK;
out:
	if (fd >= 0)
		close(fd);
	if (data_dso)
		dso__put(data_dso);
	unlink(path);
	free(name_heap);
	free(contents);
	return ret;
}

static int test__symbol_bytes_truncated_name(struct test_suite *test __maybe_unused,
					     int subtest __maybe_unused)
{
	/*
	 * One byte is short in the stack-buffer read.  1023 bytes fills it
	 * exactly, so the following read exercises the heap-buffer path.
	 */
	if (truncated_name_case(1, 1) != TEST_OK ||
	    truncated_name_case(1023, 2) != TEST_OK)
		return TEST_FAIL;
	return TEST_OK;
}

static int test__symbol_bytes_lazy_name_lookup(struct test_suite *test __maybe_unused,
					       int subtest __maybe_unused)
{
	static const char names[] = "first\0second\0";
	unsigned long saved_max = symbol_conf.max_symbol_bytes;
	size_t baseline = symbol__bytes_used();
	char path[] = "/tmp/perf-lazy-names-XXXXXX";
	struct dso_ondemand *od = NULL;
	struct symbol *sym;
	struct dso *dso = NULL;
	struct map *map = NULL;
	struct rb_node *node;
	int nr_symbols = 0;
	int ret = TEST_FAIL;
	int fd = -1;

	symbol_conf.max_symbol_bytes = 0;
	fd = mkstemp(path);
	if (fd < 0 || write(fd, names, sizeof(names)) != (ssize_t)sizeof(names))
		goto out;
	close(fd);
	fd = -1;

	dso = dso__new("/not/the/symbol/source");
	od = zalloc(sizeof(*od));
	if (!dso || !od)
		goto out;
	od->sorted = zalloc(2 * sizeof(*od->sorted));
	od->data_dso = dso__new(path);
	if (!od->sorted || !od->data_dso ||
	    dso__data_set_path(od->data_dso, path) < 0)
		goto out;
	dso__set_binary_type(od->data_dso, DSO_BINARY_TYPE__SYSTEM_PATH_DSO);
	od->strtab_size = sizeof(names);
	od->nr_sorted = 2;
	od->nr_alloc = 2;
	od->sorted[0] = (struct sym_idx) {
		.start = 0x10,
		.end = 0x20,
		.name_off = 0,
		.binding = STB_GLOBAL,
		.type = STT_FUNC,
	};
	od->sorted[1] = (struct sym_idx) {
		.start = 0x20,
		.end = 0x30,
		.name_off = sizeof("first"),
		.binding = STB_GLOBAL,
		.type = STT_FUNC,
	};
	if (!symbol__try_account_bytes(od->nr_alloc * sizeof(*od->sorted)))
		goto out;
	dso__set_ondemand(dso, od);
	od = NULL;
	dso__set_loaded(dso);
	map = map__new2(0, dso);
	if (!map)
		goto out;

	sym = map__find_symbol(map, 0x11);
	if (!sym || strcmp(sym->name, "first"))
		goto out;

	dso__data_close(dso__ondemand(dso)->data_dso);
	sym = map__find_symbol_by_name(map, "second");
	if (!sym || strcmp(sym->name, "second") || dso__ondemand(dso))
		goto out;

	for (node = rb_first_cached(dso__symbols(dso)); node; node = rb_next(node))
		nr_symbols++;
	if (nr_symbols != 2)
		goto out;
	ret = TEST_OK;
out:
	if (fd >= 0)
		close(fd);
	if (map)
		map__put(map);
	if (dso)
		dso__put(dso);
	if (od) {
		if (od->data_dso)
			dso__put(od->data_dso);
		free(od->sorted);
		free(od);
	}
	unlink(path);
	symbol_conf.max_symbol_bytes = saved_max;
	if (symbol__bytes_used() != baseline)
		ret = TEST_FAIL;
	return ret;
}
#endif

static struct test_case tests__symbol_bytes[] = {
	TEST_CASE("Long name accounting", symbol_bytes_long_name),
	TEST_CASE("Concurrent strict reservations", symbol_bytes_reservation),
	TEST_CASE("Shared duplicate selection", symbol_bytes_duplicate_selection),
#ifdef HAVE_LIBELF_SUPPORT
	TEST_CASE("Truncated lazy symbol names", symbol_bytes_truncated_name),
	TEST_CASE("Lazy address and name lookup", symbol_bytes_lazy_name_lookup),
#endif
	{ .name = NULL, }
};

struct test_suite suite__symbol_bytes = {
	.desc = "Symbol memory accounting",
	.test_cases = tests__symbol_bytes,
};
