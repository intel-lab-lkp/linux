// SPDX-License-Identifier: GPL-2.0
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/kernel.h>
#include <linux/zalloc.h>

#include "debug.h"
#include "dso.h"
#include "env.h"
#include "machine.h"
#include "map.h"
#include "symbol.h"
#include "symbol_conf.h"
#include "tests.h"
#include "thread.h"
#include "util.h"

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
		struct symbol_candidate a, b;
		int expected;
	} cases[] = {
		{ { 1, "a", STT_FUNC, STB_GLOBAL }, { 0, "b", STT_FUNC, STB_GLOBAL }, SYMBOL_A },
		{ { 1, "a", STT_NOTYPE, STB_GLOBAL }, { 1, "b", STT_FUNC, STB_GLOBAL }, SYMBOL_B },
		{ { 1, "a", STT_FUNC, STB_WEAK }, { 1, "b", STT_FUNC, STB_GLOBAL }, SYMBOL_B },
		{ { 1, "a", STT_FUNC, STB_GLOBAL }, { 1, "b", STT_FUNC, STB_LOCAL }, SYMBOL_A },
		{ { 1, "name", STT_FUNC, STB_GLOBAL },
		  { 1, "_name", STT_FUNC, STB_GLOBAL }, SYMBOL_A },
		{ { 1, "a", STT_FUNC, STB_GLOBAL }, { 1, "long", STT_FUNC, STB_GLOBAL }, SYMBOL_B },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		if (symbol__choose_best(&cases[i].a, &cases[i].b) != cases[i].expected)
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

#define LAZY_SYM_START	0x1000
#define LAZY_SYM_SIZE	0x10
#define LAZY_NAME_FMT	"lazy_sym_%03u"
#define LAZY_NAME_LEN	sizeof("lazy_sym_000")

static void lazy_name(char *buf, u32 i)
{
	snprintf(buf, LAZY_NAME_LEN, LAZY_NAME_FMT, i % 1000);
}

struct lazy_fixture {
	char		path[32];
	struct dso	*dso;
	struct map	*map;
	u32		nr;
};

/*
 * Build a DSO whose symbols exist only in a lazy index: @nr adjacent
 * functions named lazy_sym_NNN, with names in a string-table file read
 * through a private data DSO.
 */
static int lazy_fixture__init(struct lazy_fixture *f, u32 nr)
{
	struct dso_ondemand *od = NULL;
	char *strtab;
	int fd, ret = -1;
	u32 i;

	memset(f, 0, sizeof(*f));
	f->nr = nr;
	strcpy(f->path, "/tmp/perf-lazy-names-XXXXXX");

	strtab = malloc(nr * LAZY_NAME_LEN);
	if (!strtab)
		return -1;
	for (i = 0; i < nr; i++)
		lazy_name(strtab + i * LAZY_NAME_LEN, i);

	fd = mkstemp(f->path);
	if (fd < 0) {
		f->path[0] = '\0';
		goto out;
	}
	if (write(fd, strtab, nr * LAZY_NAME_LEN) != (ssize_t)(nr * LAZY_NAME_LEN)) {
		close(fd);
		goto out;
	}
	close(fd);

	f->dso = dso__new("/not/the/symbol/source");
	od = zalloc(sizeof(*od));
	if (!f->dso || !od)
		goto out;
	od->sorted = calloc(nr, sizeof(*od->sorted));
	od->data_dso = dso__new(f->path);
	if (!od->sorted || !od->data_dso ||
	    dso__data_set_path(od->data_dso, f->path) < 0)
		goto out;
	dso__set_binary_type(od->data_dso, DSO_BINARY_TYPE__SYSTEM_PATH_DSO);
	od->strtab_size = nr * LAZY_NAME_LEN;
	od->nr_sorted = nr;
	od->nr_alloc = nr;
	for (i = 0; i < nr; i++) {
		od->sorted[i] = (struct sym_idx) {
			.start = LAZY_SYM_START + i * LAZY_SYM_SIZE,
			.end = LAZY_SYM_START + (i + 1) * LAZY_SYM_SIZE,
			.name_off = i * LAZY_NAME_LEN,
			.binding = STB_GLOBAL,
			.type = STT_FUNC,
		};
	}
	if (!symbol__try_account_bytes(od->nr_alloc * sizeof(*od->sorted)))
		goto out;
	dso__set_ondemand(f->dso, od);
	od = NULL;
	dso__set_loaded(f->dso);
	f->map = map__new2(0, f->dso);
	if (f->map)
		ret = 0;
out:
	if (od) {
		if (od->data_dso)
			dso__put(od->data_dso);
		free(od->sorted);
		free(od);
	}
	free(strtab);
	return ret;
}

static void lazy_fixture__exit(struct lazy_fixture *f)
{
	map__put(f->map);
	dso__put(f->dso);
	if (f->path[0])
		unlink(f->path);
}

static bool lazy_symbol_ok(const struct symbol *sym, u32 i)
{
	char name[LAZY_NAME_LEN];

	lazy_name(name, i);
	return sym->start == LAZY_SYM_START + i * LAZY_SYM_SIZE && !strcmp(sym->name, name);
}

static int lazy_nr_symbols(struct dso *dso)
{
	struct rb_node *node;
	int nr = 0;

	for (node = rb_first_cached(dso__symbols(dso)); node; node = rb_next(node))
		nr++;
	return nr;
}

static int test__symbol_bytes_lazy_name_lookup(struct test_suite *test __maybe_unused,
					       int subtest __maybe_unused)
{
	unsigned long saved_max = symbol_conf.max_symbol_bytes;
	bool saved_lazy = symbol_conf.lazy_load_symbols;
	size_t baseline = symbol__bytes_used();
	struct lazy_fixture f;
	struct symbol *sym;
	int ret = TEST_FAIL;

	symbol_conf.max_symbol_bytes = 0;
	symbol_conf.lazy_load_symbols = true;
	if (lazy_fixture__init(&f, 2))
		goto out;

	sym = map__find_symbol(f.map, LAZY_SYM_START + 1);
	if (!sym || !lazy_symbol_ok(sym, 0))
		goto out;

	/* Name lookup must still work after the data descriptor is closed. */
	dso__data_close(dso__ondemand(f.dso)->data_dso);
	sym = map__find_symbol_by_name(f.map, "lazy_sym_001");
	if (!sym || !lazy_symbol_ok(sym, 1) || dso__ondemand(f.dso))
		goto out;
	if (lazy_nr_symbols(f.dso) != 2)
		goto out;
	ret = TEST_OK;
out:
	lazy_fixture__exit(&f);
	symbol_conf.max_symbol_bytes = saved_max;
	symbol_conf.lazy_load_symbols = saved_lazy;
	if (symbol__bytes_used() != baseline)
		ret = TEST_FAIL;
	return ret;
}

struct lazy_lookup_arg {
	struct lazy_fixture	*f;
	bool			by_name;
	bool			failed;
};

static void *lazy_lookup(void *data)
{
	struct lazy_lookup_arg *arg = data;
	struct lazy_fixture *f = arg->f;
	u32 i, round;

	for (round = 0; round < 16; round++) {
		for (i = 0; i < f->nr; i++) {
			struct symbol *sym;

			if (arg->by_name) {
				char name[LAZY_NAME_LEN];

				lazy_name(name, i);
				sym = map__find_symbol_by_name(f->map, name);
			} else {
				sym = map__find_symbol(f->map, LAZY_SYM_START +
						       i * LAZY_SYM_SIZE + 1);
			}
			if (sym && !lazy_symbol_ok(sym, i))
				arg->failed = true;
		}
	}
	return NULL;
}

/*
 * Address lookups race with name lookups while the symbol budget stops
 * materialization part way. Once a name lookup has run, the DSO must no
 * longer grow, even after budget becomes available again.
 */
static int test__symbol_bytes_lazy_budget_race(struct test_suite *test __maybe_unused,
					       int subtest __maybe_unused)
{
	enum { NR_SYMS = 256, NR_ADDR_THREADS = 4, NR_THREADS = NR_ADDR_THREADS + 2 };
	unsigned long saved_max = symbol_conf.max_symbol_bytes;
	bool saved_lazy = symbol_conf.lazy_load_symbols;
	size_t baseline = symbol__bytes_used();
	size_t charge = symbol_conf.priv_size + sizeof(struct symbol) + LAZY_NAME_LEN;
	struct lazy_lookup_arg args[NR_THREADS];
	pthread_t threads[NR_THREADS];
	struct lazy_fixture f;
	int created = 0, nr_symbols, i;
	int ret = TEST_FAIL;

	symbol_conf.max_symbol_bytes = 0;
	symbol_conf.lazy_load_symbols = true;
	if (lazy_fixture__init(&f, NR_SYMS))
		goto out;
	symbol_conf.max_symbol_bytes = symbol__bytes_used() + NR_SYMS / 4 * charge;

	for (i = 0; i < NR_THREADS; i++) {
		args[i] = (struct lazy_lookup_arg) {
			.f = &f,
			.by_name = i >= NR_ADDR_THREADS,
		};
		if (pthread_create(&threads[i], NULL, lazy_lookup, &args[i]))
			break;
		created++;
	}
	for (i = 0; i < created; i++)
		pthread_join(threads[i], NULL);
	if (created != NR_THREADS)
		goto out;
	for (i = 0; i < NR_THREADS; i++) {
		if (args[i].failed) {
			pr_debug("lazy lookup returned a wrong symbol\n");
			goto out;
		}
	}

	nr_symbols = lazy_nr_symbols(f.dso);
	if (dso__ondemand(f.dso) || nr_symbols == 0 || nr_symbols > NR_SYMS / 4) {
		pr_debug("unexpected lazy state: index %p, %d symbols\n",
			 dso__ondemand(f.dso), nr_symbols);
		goto out;
	}

	symbol_conf.max_symbol_bytes = 0;
	for (i = 0; i < NR_SYMS; i++)
		map__find_symbol(f.map, LAZY_SYM_START + i * LAZY_SYM_SIZE + 1);
	if (lazy_nr_symbols(f.dso) != nr_symbols) {
		pr_debug("DSO changed after its name array was built\n");
		goto out;
	}
	ret = TEST_OK;
out:
	lazy_fixture__exit(&f);
	symbol_conf.max_symbol_bytes = saved_max;
	symbol_conf.lazy_load_symbols = saved_lazy;
	if (symbol__bytes_used() != baseline)
		ret = TEST_FAIL;
	return ret;
}
struct sym_entry {
	u64	start;
	char	*name;
};

static int cmp_sym_entry(const void *a, const void *b)
{
	const struct sym_entry *sa = a, *sb = b;

	if (sa->start != sb->start)
		return sa->start < sb->start ? -1 : 1;
	return strcmp(sa->name, sb->name);
}

static void free_sym_entries(struct sym_entry *entries, size_t nr)
{
	size_t i;

	for (i = 0; i < nr; i++)
		free(entries[i].name);
	free(entries);
}

/*
 * Load @filename on a fresh host machine and return the start and name of
 * every symbol, sorted. In lazy mode, building the name-sorted array
 * materializes the whole index.
 */
static int load_sym_entries(const char *filename, bool lazy,
			    struct sym_entry **entries_p, size_t *nr_p)
{
	struct sym_entry *entries = NULL;
	struct machine *machine = NULL;
	struct thread *thread = NULL;
	struct map *map = NULL;
	struct perf_env env;
	struct rb_node *nd;
	struct dso *dso;
	size_t nr = 0, alloc = 0;
	int ret = TEST_FAIL;

	perf_env__init(&env);
	symbol_conf.lazy_load_symbols = lazy;
	machine = machine__new_host(&env);
	if (!machine)
		goto out;
	thread = machine__findnew_thread(machine, 100, 100);
	if (!thread)
		goto out;
	map = map__new(machine, 0x100000, 0xffffffff, 0, &dso_id_empty,
		       PROT_EXEC, /*flags=*/0, (char *)filename, thread);
	if (!map)
		goto out;

	dso = map__dso(map);
	if (dso__load(dso, map) <= 0) {
		pr_debug("%s: no symbols loaded\n", filename);
		ret = TEST_SKIP;
		goto out;
	}
	if (lazy && !dso__ondemand(dso)) {
		pr_debug("%s: no lazy index was built\n", filename);
		ret = TEST_SKIP;
		goto out;
	}
	dso__sort_by_name(dso);

	for (nd = rb_first_cached(dso__symbols(dso)); nd; nd = rb_next(nd)) {
		struct symbol *sym = rb_entry(nd, struct symbol, rb_node);

		if (nr == alloc) {
			struct sym_entry *tmp;

			alloc = alloc ? alloc * 2 : 1024;
			tmp = realloc(entries, alloc * sizeof(*entries));
			if (!tmp)
				goto out;
			entries = tmp;
		}
		entries[nr].start = sym->start;
		entries[nr].name = strdup(sym->name);
		if (!entries[nr].name)
			goto out;
		nr++;
	}
	qsort(entries, nr, sizeof(*entries), cmp_sym_entry);
	*entries_p = entries;
	*nr_p = nr;
	entries = NULL;
	ret = TEST_OK;
out:
	if (entries)
		free_sym_entries(entries, nr);
	map__put(map);
	thread__put(thread);
	machine__delete(machine);
	perf_env__exit(&env);
	return ret;
}

/*
 * Compare the symbols of a DSO (perf itself, or --dso) loaded eagerly and
 * lazily. Addresses and names must match.
 */
static int test__symbol_bytes_lazy_parity(struct test_suite *test __maybe_unused,
					  int subtest __maybe_unused)
{
	unsigned long saved_max = symbol_conf.max_symbol_bytes;
	bool saved_lazy = symbol_conf.lazy_load_symbols;
	struct sym_entry *eager = NULL, *lazy = NULL;
	size_t nr_eager = 0, nr_lazy = 0, i;
	char filename[PATH_MAX];
	int ret;

	if (dso_to_test)
		strlcpy(filename, dso_to_test, sizeof(filename));
	else
		perf_exe(filename, sizeof(filename));

	symbol_conf.max_symbol_bytes = 0;
	ret = load_sym_entries(filename, false, &eager, &nr_eager);
	if (ret == TEST_OK)
		ret = load_sym_entries(filename, true, &lazy, &nr_lazy);
	if (ret != TEST_OK)
		goto out;

	pr_debug("%s: %zu eager and %zu lazy symbols\n", filename, nr_eager, nr_lazy);
	for (i = 0; i < nr_eager && i < nr_lazy; i++) {
		if (cmp_sym_entry(&eager[i], &lazy[i])) {
			pr_debug("mismatch: eager %#" PRIx64 " %s, lazy %#" PRIx64 " %s\n",
				 eager[i].start, eager[i].name, lazy[i].start, lazy[i].name);
			ret = TEST_FAIL;
			goto out;
		}
	}
	if (nr_eager != nr_lazy)
		ret = TEST_FAIL;
out:
	if (eager)
		free_sym_entries(eager, nr_eager);
	if (lazy)
		free_sym_entries(lazy, nr_lazy);
	symbol_conf.max_symbol_bytes = saved_max;
	symbol_conf.lazy_load_symbols = saved_lazy;
	return ret;
}
#endif

static struct test_case tests__symbol_bytes[] = {
	TEST_CASE("Concurrent strict reservations", symbol_bytes_reservation),
	TEST_CASE("Shared duplicate selection", symbol_bytes_duplicate_selection),
#ifdef HAVE_LIBELF_SUPPORT
	TEST_CASE("Truncated lazy symbol names", symbol_bytes_truncated_name),
	TEST_CASE("Lazy address and name lookup", symbol_bytes_lazy_name_lookup),
	TEST_CASE("Lazy lookups racing a truncated name lookup", symbol_bytes_lazy_budget_race),
	TEST_CASE("Lazy and eager symbol parity", symbol_bytes_lazy_parity),
#endif
	{ .name = NULL, }
};

struct test_suite suite__symbol_bytes = {
	.desc = "Symbol memory accounting",
	.test_cases = tests__symbol_bytes,
};
