// SPDX-License-Identifier: GPL-2.0
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fcntl.h>
#include <linux/kernel.h>
#include <linux/zalloc.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

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

static int test__symbol_lazy_duplicate_selection(struct test_suite *test __maybe_unused,
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

static int test__symbol_lazy_truncated_name(struct test_suite *test __maybe_unused,
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
 * functions named lazy_sym_NNN, with names @stride bytes apart in a
 * string-table file read through a private data DSO.
 */
static int lazy_fixture__init(struct lazy_fixture *f, u32 nr, u32 stride)
{
	struct dso_ondemand *od = NULL;
	size_t strtab_size = (size_t)nr * stride;
	char *strtab;
	int fd, ret = -1;
	u32 i;

	memset(f, 0, sizeof(*f));
	f->nr = nr;
	strcpy(f->path, "/tmp/perf-lazy-names-XXXXXX");

	strtab = calloc(1, strtab_size);
	if (!strtab)
		return -1;
	for (i = 0; i < nr; i++)
		lazy_name(strtab + (size_t)i * stride, i);

	fd = mkstemp(f->path);
	if (fd < 0) {
		f->path[0] = '\0';
		goto out;
	}
	if (write(fd, strtab, strtab_size) != (ssize_t)strtab_size) {
		close(fd);
		goto out;
	}
	close(fd);

	f->dso = dso__new("/not/the/symbol/source");
	od = dso_ondemand__new();
	if (!f->dso || !od)
		goto out;
	od->sorted = calloc(nr, sizeof(*od->sorted));
	od->data_dso = dso__new(f->path);
	if (!od->sorted || !od->data_dso ||
	    dso__data_set_path(od->data_dso, f->path) < 0)
		goto out;
	dso__set_binary_type(od->data_dso, DSO_BINARY_TYPE__SYSTEM_PATH_DSO);
	od->strtab[0].size = strtab_size;
	od->nr_sorted = nr;
	for (i = 0; i < nr; i++) {
		od->sorted[i] = (struct sym_idx) {
			.start = LAZY_SYM_START + i * LAZY_SYM_SIZE,
			.end = LAZY_SYM_START + (i + 1) * LAZY_SYM_SIZE,
			.name_off = i * stride,
			.binding = STB_GLOBAL,
			.type = STT_FUNC,
		};
	}
	dso__set_ondemand(f->dso, od);
	od = NULL;
	dso__set_loaded(f->dso);
	f->map = map__new2(0, f->dso);
	if (f->map)
		ret = 0;
out:
	dso_ondemand__free(od);
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

static int test__symbol_lazy_name_lookup(struct test_suite *test __maybe_unused,
					 int subtest __maybe_unused)
{
	bool saved_lazy = symbol_conf.lazy_load_symbols;
	struct lazy_fixture f;
	struct symbol *sym;
	int ret = TEST_FAIL;

	symbol_conf.lazy_load_symbols = true;
	if (lazy_fixture__init(&f, 2, LAZY_NAME_LEN))
		goto out;

	/* Not via map__find_symbol(): dso__find_symbol() must dispatch. */
	sym = dso__find_symbol(f.dso, LAZY_SYM_START + 1);
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
	symbol_conf.lazy_load_symbols = saved_lazy;
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
			if (!sym || !lazy_symbol_ok(sym, i))
				arg->failed = true;
		}
	}
	return NULL;
}

/*
 * Address lookups race with name lookups, which materialize the whole index
 * and free it. Every symbol must be materialized exactly once, and the DSO
 * must not change once its name array exists.
 */
static int test__symbol_lazy_lookup_race(struct test_suite *test __maybe_unused,
					 int subtest __maybe_unused)
{
	enum { NR_SYMS = 256, NR_ADDR_THREADS = 4, NR_THREADS = NR_ADDR_THREADS + 2 };
	bool saved_lazy = symbol_conf.lazy_load_symbols;
	struct lazy_lookup_arg args[NR_THREADS];
	pthread_t threads[NR_THREADS];
	struct lazy_fixture f;
	int created = 0, nr_symbols, i;
	int ret = TEST_FAIL;

	symbol_conf.lazy_load_symbols = true;
	if (lazy_fixture__init(&f, NR_SYMS, LAZY_NAME_LEN))
		goto out;

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
	if (dso__ondemand(f.dso) || nr_symbols != NR_SYMS) {
		pr_debug("unexpected lazy state: index %p, %d symbols\n",
			 dso__ondemand(f.dso), nr_symbols);
		goto out;
	}

	for (i = 0; i < NR_SYMS; i++)
		map__find_symbol(f.map, LAZY_SYM_START + i * LAZY_SYM_SIZE + 1);
	if (lazy_nr_symbols(f.dso) != nr_symbols) {
		pr_debug("DSO changed after its name array was built\n");
		goto out;
	}
	ret = TEST_OK;
out:
	lazy_fixture__exit(&f);
	symbol_conf.lazy_load_symbols = saved_lazy;
	return ret;
}

struct lazy_io_arg {
	struct lazy_fixture	*f;
	pthread_mutex_t		*done_lock;
	pthread_cond_t		*done_cond;
	int			*nr_done;
	bool			by_name;
	bool			data_reader;
	bool			failed;
};

static void *lazy_io(void *data)
{
	struct lazy_io_arg *arg = data;
	struct lazy_fixture *f = arg->f;
	u32 i;

	for (i = 0; i < f->nr; i++) {
		struct symbol *sym;
		int fd;

		if (arg->data_reader) {
			/* Reopening the parent takes its lock under the data-open lock. */
			dso__data_close(f->dso);
			if (dso__data_get_fd(f->dso, NULL, &fd))
				dso__data_put_fd(f->dso);
			else
				arg->failed = true;
			continue;
		}
		if (arg->by_name) {
			char name[LAZY_NAME_LEN];

			lazy_name(name, i);
			sym = map__find_symbol_by_name(f->map, name);
		} else {
			sym = map__find_symbol(f->map, LAZY_SYM_START +
					       i * LAZY_SYM_SIZE + 1);
		}
		if (!sym || !lazy_symbol_ok(sym, i))
			arg->failed = true;
	}

	pthread_mutex_lock(arg->done_lock);
	(*arg->nr_done)++;
	pthread_cond_signal(arg->done_cond);
	pthread_mutex_unlock(arg->done_lock);
	return NULL;
}

/*
 * Lazy lookups read symbol names through the DSO data cache, which takes the
 * global data-open lock and then the lock of the DSO being opened. They must
 * not hold the lock of the looked-up DSO while doing so, or they deadlock
 * against threads reading that DSO's own data. Each name sits on its own
 * cache page so that every materialization misses the data cache.
 */
static int test__symbol_lazy_lookup_vs_data_read(struct test_suite *test __maybe_unused,
						 int subtest __maybe_unused)
{
	enum { NR_SYMS = 1000, NR_THREADS = 4 };
	pthread_mutex_t done_lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t done_cond = PTHREAD_COND_INITIALIZER;
	bool saved_lazy = symbol_conf.lazy_load_symbols;
	struct lazy_io_arg args[NR_THREADS];
	pthread_t threads[NR_THREADS];
	struct lazy_fixture f;
	struct timespec deadline;
	int created = 0, nr_done = 0, i;
	int ret = TEST_FAIL;

	symbol_conf.lazy_load_symbols = true;
	if (lazy_fixture__init(&f, NR_SYMS, 4096) ||
	    dso__data_set_path(f.dso, f.path) < 0)
		goto out;

	for (i = 0; i < NR_THREADS; i++) {
		args[i] = (struct lazy_io_arg) {
			.f = &f,
			.done_lock = &done_lock,
			.done_cond = &done_cond,
			.nr_done = &nr_done,
			.by_name = i == 2,
			.data_reader = i == 3,
		};
		if (pthread_create(&threads[i], NULL, lazy_io, &args[i]))
			break;
		created++;
	}

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 60;
	pthread_mutex_lock(&done_lock);
	while (nr_done < created) {
		if (pthread_cond_timedwait(&done_cond, &done_lock, &deadline))
			break;
	}
	pthread_mutex_unlock(&done_lock);
	if (nr_done < created) {
		/*
		 * The stuck threads still use this stack frame and hold the
		 * global data-open lock, so returning is not safe.
		 */
		pr_err("lazy lookups deadlocked against DSO data reads\n");
		fflush(NULL);
		_exit(-TEST_FAIL);
	}

	for (i = 0; i < created; i++)
		pthread_join(threads[i], NULL);
	if (created != NR_THREADS)
		goto out;
	for (i = 0; i < NR_THREADS; i++) {
		if (args[i].failed) {
			pr_debug("thread %d: wrong lookup or data read failure\n", i);
			goto out;
		}
	}
	if (dso__ondemand(f.dso) || lazy_nr_symbols(f.dso) != NR_SYMS) {
		pr_debug("unexpected lazy state: index %p, %d symbols\n",
			 dso__ondemand(f.dso), lazy_nr_symbols(f.dso));
		goto out;
	}
	ret = TEST_OK;
out:
	lazy_fixture__exit(&f);
	symbol_conf.lazy_load_symbols = saved_lazy;
	return ret;
}

struct sym_entry {
	u64	start;
	u64	end;
	char	*name;
};

static int cmp_sym_entry(const void *a, const void *b)
{
	const struct sym_entry *sa = a, *sb = b;

	if (sa->start != sb->start)
		return sa->start < sb->start ? -1 : 1;
	if (sa->end != sb->end)
		return sa->end < sb->end ? -1 : 1;
	return strcmp(sa->name, sb->name);
}

/* Return the last-starting entry of sorted @entries that contains @addr. */
static const struct sym_entry *innermost_sym_entry(const struct sym_entry *entries,
						   size_t nr, u64 addr)
{
	size_t lo = 0, hi = nr;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (entries[mid].start <= addr)
			lo = mid + 1;
		else
			hi = mid;
	}
	while (lo-- > 0) {
		if (entries[lo].end > addr)
			return &entries[lo];
	}
	return NULL;
}

/*
 * Look up the first and last address of every eager symbol lazily, before
 * and after the symbols are materialized. Each must resolve to the innermost
 * eager symbol containing it, which is the one an unambiguous lookup has to
 * return.
 */
static int check_lazy_lookups(struct map *map, const struct sym_entry *eager,
			      size_t nr_eager)
{
	size_t i;
	int pass, k;

	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < nr_eager; i++) {
			if (eager[i].end <= eager[i].start)
				continue;
			for (k = 0; k < 2; k++) {
				u64 addr = k ? eager[i].end - 1 : eager[i].start;
				const struct sym_entry *want;
				struct symbol *sym;

				want = innermost_sym_entry(eager, nr_eager, addr);
				sym = map__find_symbol(map, addr);
				if (!sym || sym->start != want->start ||
				    strcmp(sym->name, want->name) ||
				    addr < sym->start || addr >= sym->end) {
					pr_debug("lookup %#" PRIx64 ": eager %#" PRIx64
						 "-%#" PRIx64 " %s, lazy %s\n", addr,
						 want->start, want->end, want->name,
						 sym ? sym->name : "[none]");
					return TEST_FAIL;
				}
			}
		}
	}
	return TEST_OK;
}

static void free_sym_entries(struct sym_entry *entries, size_t nr)
{
	size_t i;

	for (i = 0; i < nr; i++)
		free(entries[i].name);
	free(entries);
}

/*
 * Load @filename on a fresh host machine and return the range and name of
 * every symbol, sorted. If lazy mode built an index, first check lookups
 * against @eager, then build the name-sorted array, which materializes the
 * whole index.
 */
static int load_sym_entries(const char *filename, bool lazy,
			    const struct sym_entry *eager, size_t nr_eager,
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
	if (lazy && !dso__ondemand(dso))
		pr_debug("%s: no lazy index was built\n", filename);
	else if (lazy && check_lazy_lookups(map, eager, nr_eager) != TEST_OK)
		goto out;
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
		entries[nr].end = sym->end;
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

/* outer is [start, start + 0x100) and inner is [start + 0x20, start + 0x40). */
static int test__symbol_lazy_nested_lookup(struct test_suite *test __maybe_unused,
					   int subtest __maybe_unused)
{
	bool saved_lazy = symbol_conf.lazy_load_symbols;
	const u64 outer = LAZY_SYM_START;
	const u64 inner = LAZY_SYM_START + 0x20;
	struct dso_ondemand *od;
	struct lazy_fixture f;
	struct symbol *sym;
	char name[LAZY_NAME_LEN];
	int ret = TEST_FAIL;

	symbol_conf.lazy_load_symbols = true;
	if (lazy_fixture__init(&f, 2, LAZY_NAME_LEN))
		goto out;

	od = dso__ondemand(f.dso);
	od->sorted[0].end = outer + 0x100;
	od->sorted[1].start = inner;
	od->sorted[1].end = inner + 0x20;
	od->outer = calloc(2, sizeof(*od->outer));
	if (!od->outer)
		goto out;
	od->outer[0] = SYM_IDX_NONE;
	od->outer[1] = 0;

	lazy_name(name, 1);
	sym = dso__find_symbol(f.dso, inner);
	if (!sym || sym->start != inner || strcmp(sym->name, name))
		goto out;
	sym = dso__find_symbol(f.dso, inner + 1);
	if (!sym || sym->start != inner || strcmp(sym->name, name))
		goto out;
	/* Only the index finds outer here; the post-sort rb-tree does not. */
	sym = map__find_symbol(f.map, inner + 0x20);
	if (!sym || !lazy_symbol_ok(sym, 0))
		goto out;

	dso__sort_by_name(f.dso);
	if (dso__ondemand(f.dso) || lazy_nr_symbols(f.dso) != 2)
		goto out;
	sym = dso__find_symbol_by_name(f.dso, name, NULL);
	if (!sym || sym->start != inner || sym->end != inner + 0x20)
		goto out;
	lazy_name(name, 0);
	sym = dso__find_symbol_by_name(f.dso, name, NULL);
	if (!sym || sym->start != outer || sym->end != outer + 0x100)
		goto out;
	sym = dso__find_symbol(f.dso, inner);
	if (!sym || sym->start != inner || strcmp(sym->name, "lazy_sym_001"))
		goto out;
	ret = TEST_OK;
out:
	lazy_fixture__exit(&f);
	symbol_conf.lazy_load_symbols = saved_lazy;
	return ret;
}

/*
 * Compare the symbols of a DSO (perf itself, or --dso) loaded eagerly and
 * lazily. Lazy address lookups must agree with the eager symbols, and the
 * fully materialized ranges and names must match.
 */
static int test__symbol_lazy_parity(struct test_suite *test __maybe_unused,
				    int subtest __maybe_unused)
{
	bool saved_lazy = symbol_conf.lazy_load_symbols;
	struct sym_entry *eager = NULL, *lazy = NULL;
	size_t nr_eager = 0, nr_lazy = 0, i;
	char filename[PATH_MAX];
	int ret;

	if (dso_to_test)
		strlcpy(filename, dso_to_test, sizeof(filename));
	else
		perf_exe(filename, sizeof(filename));

	ret = load_sym_entries(filename, false, NULL, 0, &eager, &nr_eager);
	if (ret == TEST_OK)
		ret = load_sym_entries(filename, true, eager, nr_eager, &lazy, &nr_lazy);
	if (ret != TEST_OK)
		goto out;

	pr_debug("%s: %zu eager and %zu lazy symbols\n", filename, nr_eager, nr_lazy);
	for (i = 0; i < nr_eager && i < nr_lazy; i++) {
		if (cmp_sym_entry(&eager[i], &lazy[i])) {
			pr_debug("mismatch: eager %#" PRIx64 "-%#" PRIx64 " %s, lazy %#"
				 PRIx64 "-%#" PRIx64 " %s\n",
				 eager[i].start, eager[i].end, eager[i].name,
				 lazy[i].start, lazy[i].end, lazy[i].name);
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
	symbol_conf.lazy_load_symbols = saved_lazy;
	return ret;
}
#endif

static struct test_case tests__symbol_lazy[] = {
	TEST_CASE("Shared duplicate selection", symbol_lazy_duplicate_selection),
#ifdef HAVE_LIBELF_SUPPORT
	TEST_CASE("Truncated lazy symbol names", symbol_lazy_truncated_name),
	TEST_CASE("Lazy address and name lookup", symbol_lazy_name_lookup),
	TEST_CASE("Lazy nested range lookup", symbol_lazy_nested_lookup),
	TEST_CASE("Lazy address lookups racing name lookups", symbol_lazy_lookup_race),
	TEST_CASE("Lazy lookups racing DSO data reads", symbol_lazy_lookup_vs_data_read),
	TEST_CASE("Lazy and eager symbol parity", symbol_lazy_parity),
#endif
	{ .name = NULL, }
};

struct test_suite suite__symbol_lazy = {
	.desc = "Lazy symbol loading",
	.test_cases = tests__symbol_lazy,
};
