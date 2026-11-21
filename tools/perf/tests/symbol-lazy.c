// SPDX-License-Identifier: GPL-2.0
#include <elf.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/kernel.h>

#include "build-id.h"
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
#include "util/copyfile.h"

static int test__symbol_lazy_choice(struct test_suite *test __maybe_unused,
				    int subtest __maybe_unused)
{
	struct choice_case {
		u64 len;
		u8 type, binding;
		bool needs_names;
	} cases[] = {
		{ 0x10, STT_FUNC, STB_GLOBAL, true },
		{ 0, STT_FUNC, STB_GLOBAL, false },
		{ 0x10, STT_NOTYPE, STB_GLOBAL, false },
		{ 0x10, STT_GNU_IFUNC, STB_GLOBAL, true },
		{ 0x10, STT_FUNC, STB_WEAK, false },
		{ 0x10, STT_FUNC, STB_LOCAL, false },
	};
	struct symbol *a, *b;
	int ret = TEST_OK;
	size_t i;

	a = symbol__new(0x1000, 0x10, STB_GLOBAL, STT_FUNC, "a");
	if (!a)
		return TEST_FAIL;
	for (i = 0; i < ARRAY_SIZE(cases) && ret == TEST_OK; i++) {
		b = symbol__new(0x1000, cases[i].len, cases[i].binding,
				cases[i].type, "b");
		if (!b ||
		    symbol__choice_needs_names(a, b) != cases[i].needs_names ||
		    symbol__choice_needs_names(b, a) != cases[i].needs_names) {
			pr_debug("case %zu\n", i);
			ret = TEST_FAIL;
		}
		if (b)
			symbol__delete(b);
	}
	symbol__delete(a);
	return ret;
}

#ifdef HAVE_LIBELF_SUPPORT
#define LAZY_SYM_START	0x1000
#define LAZY_SYM_SIZE	0x100

struct lazy_fixture {
	char path[PATH_MAX];
	struct dso *dso;
	char **names;
	size_t nr;
};

static void lazy_fixture__exit(struct lazy_fixture *f)
{
	size_t i;

	dso__put(f->dso);
	for (i = 0; f->names && i < f->nr; i++)
		free(f->names[i]);
	free(f->names);
	unlink(f->path);
}

/*
 * Write @nr names to a file and add a lazy symbol for each to a new DSO,
 * whose symbol table file is that file. Name i is @len[i] characters long,
 * or has no terminating NUL before the end of the file if @len[i] is 0.
 */
static int lazy_fixture__init(struct lazy_fixture *f, const size_t *len, size_t nr)
{
	struct dso_lazy_strtab strtab = { 0 };
	u64 off = 1;
	size_t i;
	FILE *fp;
	int fd;

	memset(f, 0, sizeof(*f));
	strlcpy(f->path, "/tmp/perf-test-lazy-names-XXXXXX", sizeof(f->path));
	fd = mkstemp(f->path);
	if (fd < 0)
		return -1;
	fp = fdopen(fd, "w");
	if (!fp) {
		close(fd);
		unlink(f->path);
		return -1;
	}
	fputc('\0', fp);

	f->names = calloc(nr, sizeof(*f->names));
	f->dso = dso__new(f->path);
	if (!f->names || !f->dso)
		goto out_close;
	f->nr = nr;
	dso__set_symtab_type(f->dso, DSO_BINARY_TYPE__SYSTEM_PATH_DSO);

	for (i = 0; i < nr; i++) {
		size_t n = len[i] ?: 32;
		struct symbol *sym;

		f->names[i] = malloc(n + 1);
		if (!f->names[i])
			goto out_close;
		for (size_t j = 0; j < n; j++)
			f->names[i][j] = 'a' + (i + j) % 26;
		f->names[i][n] = '\0';
		if (fwrite(f->names[i], 1, n + !!len[i], fp) != n + !!len[i])
			goto out_close;

		sym = symbol__new_lazy(LAZY_SYM_START + i * LAZY_SYM_SIZE,
				       LAZY_SYM_SIZE, STB_GLOBAL, STT_FUNC, off);
		if (!sym)
			goto out_close;
		symbols__insert(dso__symbols(f->dso), sym);
		off += n + !!len[i];
	}
	strtab.size = off;
	if (fflush(fp) || dso__set_lazy_file(f->dso, fileno(fp), 0, &strtab))
		goto out_close;
	dso__set_loaded(f->dso);
	if (fclose(fp)) {
		lazy_fixture__exit(f);
		return -1;
	}
	return 0;

out_close:
	fclose(fp);
	lazy_fixture__exit(f);
	return -1;
}

/* Add a lazy symbol at index @i whose name is past the end of the file. */
static int lazy_fixture__add_unreadable(struct lazy_fixture *f, size_t i)
{
	struct symbol *sym = symbol__new_lazy(LAZY_SYM_START + i * LAZY_SYM_SIZE,
					      LAZY_SYM_SIZE, STB_GLOBAL, STT_FUNC,
					      1 << 30);

	if (!sym)
		return -1;
	symbols__insert(dso__symbols(f->dso), sym);
	return 0;
}

static bool lazy_symbol_ok(const struct symbol *sym, const struct lazy_fixture *f,
			   size_t i)
{
	return sym && sym->start == LAZY_SYM_START + i * LAZY_SYM_SIZE &&
	       !symbol__needs_lazy_name(sym) && !strcmp(sym->name, f->names[i]) &&
	       sym->namelen == strlen(f->names[i]);
}

static bool lazy_symbol_unknown(const struct symbol *sym, size_t i)
{
	return sym && sym->start == LAZY_SYM_START + i * LAZY_SYM_SIZE &&
	       !symbol__needs_lazy_name(sym) && !strcmp(sym->name, "[unknown]");
}

static size_t lazy_nr_symbols(struct dso *dso)
{
	struct rb_node *nd;
	size_t nr = 0;

	for (nd = rb_first_cached(dso__symbols(dso)); nd; nd = rb_next(nd))
		nr++;
	return nr;
}

/*
 * Names of 10, 300 and 1000 characters, which take one, two and three
 * reads, a name with no NUL before the end of the file, and an offset past
 * the end of the file.
 */
static int test__symbol_lazy_names(struct test_suite *test __maybe_unused,
				   int subtest __maybe_unused)
{
	static const size_t len[] = { 10, 300, 1000, 0 };
	bool saved_lazy = symbol_conf.lazy_load_symbols;
	struct lazy_fixture f;
	struct symbol *sym;
	struct rb_node *nd;
	int ret = TEST_FAIL;
	size_t i;

	symbol_conf.lazy_load_symbols = true;
	if (lazy_fixture__init(&f, len, ARRAY_SIZE(len)))
		goto out;
	if (lazy_fixture__add_unreadable(&f, 4))
		goto out_exit;

	for (i = 0; i < 4; i++) {
		sym = dso__find_symbol(f.dso, LAZY_SYM_START + i * LAZY_SYM_SIZE + 1);
		if (!(i < 3 ? lazy_symbol_ok(sym, &f, i) : !sym)) {
			pr_debug("lookup of name %zu failed\n", i);
			goto out_exit;
		}
	}

	/*
	 * Sorting by name loads every name, and keeps the symbols whose name
	 * cannot be read, named "[unknown]". Lookups still do not return them.
	 */
	dso__sort_by_name(f.dso);
	i = 0;
	dso__for_each_symbol(f.dso, sym, nd) {
		if (!(i < 3 ? lazy_symbol_ok(sym, &f, i) : lazy_symbol_unknown(sym, i))) {
			pr_debug("walk of name %zu failed\n", i);
			goto out_exit;
		}
		i++;
	}
	if (i != 5)
		goto out_exit;
	for (i = 3; i < 5; i++) {
		if (dso__find_symbol_nocache(f.dso, LAZY_SYM_START + i * LAZY_SYM_SIZE)) {
			pr_debug("unreadable name %zu was found\n", i);
			goto out_exit;
		}
	}
	sym = dso__find_symbol_by_name(f.dso, f.names[1], NULL);
	if (!lazy_symbol_ok(sym, &f, 1))
		goto out_exit;
	ret = TEST_OK;
out_exit:
	lazy_fixture__exit(&f);
out:
	symbol_conf.lazy_load_symbols = saved_lazy;
	return ret;
}

/*
 * After the file is replaced, as a deploy replaces a binary, names must not
 * be read from the new file. It has no build ID, so this holds even if it has
 * the same contents. A name loaded before stays.
 */
static int test__symbol_lazy_replaced(struct test_suite *test __maybe_unused,
				      int subtest __maybe_unused)
{
	static const size_t len[] = { 10, 20, 30 };
	bool saved_lazy = symbol_conf.lazy_load_symbols;
	struct lazy_fixture f, new;
	struct symbol *sym;
	int ret = TEST_FAIL;
	size_t i;
	int err;

	symbol_conf.lazy_load_symbols = true;
	if (lazy_fixture__init(&f, len, ARRAY_SIZE(len)))
		goto out;
	sym = dso__find_symbol(f.dso, LAZY_SYM_START);
	if (!lazy_symbol_ok(sym, &f, 0) || lazy_fixture__init(&new, len, ARRAY_SIZE(len)))
		goto out_exit;
	err = rename(new.path, f.path);
	lazy_fixture__exit(&new);
	if (err)
		goto out_exit;
	for (i = 1; i < ARRAY_SIZE(len); i++) {
		sym = dso__find_symbol_nocache(f.dso, LAZY_SYM_START + i * LAZY_SYM_SIZE);
		if (sym) {
			pr_debug("name %zu read from the replaced file: %s\n", i, sym->name);
			goto out_exit;
		}
	}
	sym = dso__find_symbol_nocache(f.dso, LAZY_SYM_START);
	if (lazy_symbol_ok(sym, &f, 0))
		ret = TEST_OK;
out_exit:
	lazy_fixture__exit(&f);
out:
	symbol_conf.lazy_load_symbols = saved_lazy;
	return ret;
}

/*
 * A file overwritten in place with another of the same size, keeping its
 * mtime, as cp -p does, must not give names either. Only its ctime changes.
 */
static int test__symbol_lazy_overwritten(struct test_suite *test __maybe_unused,
					 int subtest __maybe_unused)
{
	static const size_t len[] = { 10, 20, 30 };
	bool saved_lazy = symbol_conf.lazy_load_symbols;
	struct timespec times[2];
	struct lazy_fixture f;
	struct stat st, now;
	struct symbol *sym;
	int ret = TEST_FAIL;
	int tries = 0;
	u64 off = 1;
	int fd = -1;
	size_t i;

	symbol_conf.lazy_load_symbols = true;
	if (lazy_fixture__init(&f, len, ARRAY_SIZE(len)))
		goto out;
	sym = dso__find_symbol_nocache(f.dso, LAZY_SYM_START);
	fd = open(f.path, O_WRONLY);
	if (!lazy_symbol_ok(sym, &f, 0) || fd < 0 || fstat(fd, &st))
		goto out_exit;
	for (i = 0; i < ARRAY_SIZE(len); i++) {
		char name[32];

		for (size_t j = 0; j < len[i]; j++)
			name[j] = f.names[i][j] - 'a' + 'A';
		if (pwrite(fd, name, len[i], off) != (ssize_t)len[i])
			goto out_exit;
		off += len[i] + 1;
	}
	times[0] = st.st_atim;
	times[1] = st.st_mtim;
	/* The ctime can stay the same within one tick of the clock. */
	do {
		usleep(1000);
		if (futimens(fd, times) || fstat(fd, &now))
			goto out_exit;
	} while (now.st_ctim.tv_sec == st.st_ctim.tv_sec &&
		 now.st_ctim.tv_nsec == st.st_ctim.tv_nsec && ++tries < 1000);
	if (tries == 1000) {
		ret = TEST_SKIP;
		goto out_exit;
	}
	for (i = 1; i < ARRAY_SIZE(len); i++) {
		sym = dso__find_symbol_nocache(f.dso, LAZY_SYM_START + i * LAZY_SYM_SIZE);
		if (sym) {
			pr_debug("name %zu read from the overwritten file: %s\n", i, sym->name);
			goto out_exit;
		}
	}
	ret = TEST_OK;
out_exit:
	if (fd >= 0)
		close(fd);
	lazy_fixture__exit(&f);
out:
	symbol_conf.lazy_load_symbols = saved_lazy;
	return ret;
}

/*
 * A file that cannot be opened for one name, as when perf runs out of file
 * descriptors, must still give the names read after it can be opened again.
 */
static int test__symbol_lazy_open_failed(struct test_suite *test __maybe_unused,
					 int subtest __maybe_unused)
{
	static const size_t len[] = { 10, 20, 30 };
	bool saved_lazy = symbol_conf.lazy_load_symbols;
	struct rlimit saved_limit, limit;
	struct lazy_fixture f;
	struct symbol *sym;
	int ret = TEST_FAIL;
	int fd;

	symbol_conf.lazy_load_symbols = true;
	if (lazy_fixture__init(&f, len, ARRAY_SIZE(len)))
		goto out;
	sym = dso__find_symbol_nocache(f.dso, LAZY_SYM_START);
	if (!lazy_symbol_ok(sym, &f, 0) || getrlimit(RLIMIT_NOFILE, &saved_limit))
		goto out_exit;
	/* Lower the limit to the lowest free file descriptor, so open() fails. */
	fd = open("/dev/null", O_RDONLY);
	if (fd < 0)
		goto out_exit;
	close(fd);
	limit = saved_limit;
	limit.rlim_cur = fd;
	if (setrlimit(RLIMIT_NOFILE, &limit))
		goto out_exit;
	sym = dso__find_symbol_nocache(f.dso, LAZY_SYM_START + LAZY_SYM_SIZE);
	if (setrlimit(RLIMIT_NOFILE, &saved_limit))
		goto out_exit;
	if (sym) {
		pr_debug("name 1 read while no file could be opened: %s\n", sym->name);
		goto out_exit;
	}
	sym = dso__find_symbol_nocache(f.dso, LAZY_SYM_START + 2 * LAZY_SYM_SIZE);
	if (lazy_symbol_ok(sym, &f, 2))
		ret = TEST_OK;
	else
		pr_debug("name 2 not read after the file could be opened again\n");
out_exit:
	lazy_fixture__exit(&f);
out:
	symbol_conf.lazy_load_symbols = saved_lazy;
	return ret;
}

#define NR_RACE_SYMS		256
#define NR_RACE_UNREADABLE	16
#define NR_RACE_ALL		(NR_RACE_SYMS + NR_RACE_UNREADABLE)
#define NR_RACE_THREADS		4

struct lazy_race {
	struct lazy_fixture *f;
	int id;
	bool failed;
};

/*
 * Look the symbols up in a different order in each thread. The symbols
 * after the readable ones have names that cannot be read.
 */
static void *lazy_race_lookups(void *data)
{
	struct lazy_race *race = data;
	struct lazy_fixture *f = race->f;
	size_t i;

	if (race->id == NR_RACE_THREADS - 1) {
		struct symbol *pos;
		struct rb_node *nd;

		i = 0;
		dso__for_each_symbol(f->dso, pos, nd) {
			if (!(i < f->nr ? lazy_symbol_ok(pos, f, i) : lazy_symbol_unknown(pos, i)))
				race->failed = true;
			i++;
		}
		return NULL;
	}
	for (i = 0; i < NR_RACE_ALL; i++) {
		size_t k = (i * (2 * race->id + 1) + race->id * 17) % NR_RACE_ALL;
		struct symbol *sym;

		sym = dso__find_symbol_nocache(f->dso, LAZY_SYM_START + k * LAZY_SYM_SIZE);
		if (!(k < f->nr ? lazy_symbol_ok(sym, f, k) : !sym))
			race->failed = true;
	}
	return NULL;
}

static int test__symbol_lazy_race(struct test_suite *test __maybe_unused,
				  int subtest __maybe_unused)
{
	bool saved_lazy = symbol_conf.lazy_load_symbols;
	struct lazy_race race[NR_RACE_THREADS];
	pthread_t threads[NR_RACE_THREADS];
	size_t len[NR_RACE_SYMS];
	struct lazy_fixture f;
	int ret = TEST_FAIL;
	int i, nr_threads = 0;

	for (i = 0; i < NR_RACE_SYMS; i++)
		len[i] = 8 + i % 300;
	symbol_conf.lazy_load_symbols = true;
	if (lazy_fixture__init(&f, len, NR_RACE_SYMS))
		goto out;
	for (i = NR_RACE_SYMS; i < NR_RACE_ALL; i++) {
		if (lazy_fixture__add_unreadable(&f, i))
			goto out_exit;
	}

	for (i = 0; i < NR_RACE_THREADS; i++) {
		race[i] = (struct lazy_race){ .f = &f, .id = i };
		if (pthread_create(&threads[i], NULL, lazy_race_lookups, &race[i]))
			break;
		nr_threads++;
	}
	for (i = 0; i < nr_threads; i++)
		pthread_join(threads[i], NULL);
	if (nr_threads != NR_RACE_THREADS)
		goto out_exit;
	for (i = 0; i < NR_RACE_THREADS; i++) {
		if (race[i].failed) {
			pr_debug("thread %d failed\n", i);
			goto out_exit;
		}
	}
	if (lazy_nr_symbols(f.dso) != NR_RACE_ALL)
		goto out_exit;
	ret = TEST_OK;
out_exit:
	lazy_fixture__exit(&f);
out:
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

static int check_lazy_lookups(struct map *map, struct map *eager_map,
			      const struct sym_entry *eager, size_t nr_eager)
{
	size_t i;
	int k;

	for (i = 0; i < nr_eager; i++) {
		if (eager[i].end <= eager[i].start)
			continue;
		for (k = 0; k < 2; k++) {
			u64 addr = k ? eager[i].end - 1 : eager[i].start;
			struct symbol *want = map__find_symbol(eager_map, addr);
			struct symbol *sym = map__find_symbol(map, addr);

			if (!want != !sym ||
			    (sym && (sym->start != want->start || sym->end != want->end ||
				     strcmp(sym->name, want->name)))) {
				pr_debug("lookup %#" PRIx64 ": eager %s, lazy %s\n", addr,
					 want ? want->name : "[none]",
					 sym ? sym->name : "[none]");
				return TEST_FAIL;
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

struct sym_loader {
	struct perf_env env;
	struct machine *machine;
	struct thread *thread;
	struct map *map;
};

static void sym_loader__exit(struct sym_loader *l)
{
	map__put(l->map);
	thread__put(l->thread);
	machine__delete(l->machine);
	perf_env__exit(&l->env);
}

static int sym_loader__init(struct sym_loader *l, const char *filename, bool lazy)
{
	struct dso *dso = NULL;
	bool kernel;

	memset(l, 0, sizeof(*l));
	perf_env__init(&l->env);
	symbol_conf.lazy_load_symbols = lazy;
	l->machine = machine__new_host(&l->env);
	if (l->machine)
		l->thread = machine__findnew_thread(l->machine, 100, 100);
	if (l->thread)
		dso = machine__findnew_dso(l->machine, filename);
	/* map__new() cannot map a loaded kernel module, which has no lazy names. */
	kernel = dso && dso__kernel(dso) != DSO_SPACE__USER;
	dso__put(dso);
	if (kernel) {
		pr_debug("%s: kernel DSO\n", filename);
		sym_loader__exit(l);
		return TEST_SKIP;
	}
	if (l->thread)
		l->map = map__new(l->machine, 0x100000, 0xffffffff, 0, &dso_id_empty,
				  PROT_EXEC, /*flags=*/0, (char *)filename, l->thread);
	if (!l->map) {
		sym_loader__exit(l);
		return TEST_FAIL;
	}
	if (dso__load(map__dso(l->map), l->map) <= 0) {
		pr_debug("%s: no symbols loaded\n", filename);
		sym_loader__exit(l);
		return TEST_SKIP;
	}
	return TEST_OK;
}

static int get_sym_entries(struct dso *dso, struct sym_entry **entries_p, size_t *nr_p)
{
	struct sym_entry *entries = NULL;
	size_t nr = 0, alloc = 0;
	struct symbol *pos;
	struct rb_node *nd;

	dso__for_each_symbol(dso, pos, nd) {
		if (nr == alloc) {
			struct sym_entry *tmp;

			alloc = alloc ? alloc * 2 : 1024;
			tmp = realloc(entries, alloc * sizeof(*entries));
			if (!tmp)
				goto out_free;
			entries = tmp;
		}
		entries[nr].start = pos->start;
		entries[nr].end = pos->end;
		entries[nr].name = strdup(pos->name);
		if (!entries[nr].name)
			goto out_free;
		nr++;
	}
	qsort(entries, nr, sizeof(*entries), cmp_sym_entry);
	*entries_p = entries;
	*nr_p = nr;
	return 0;
out_free:
	free_sym_entries(entries, nr);
	return -1;
}

/*
 * Compare the symbols of a DSO (perf itself, or --dso) loaded eagerly and
 * lazily. Lazy address lookups must agree with eager ones, and once every
 * name is loaded, the ranges and names must match.
 */
static int test__symbol_lazy_parity(struct test_suite *test __maybe_unused,
				    int subtest __maybe_unused)
{
	bool saved_lazy = symbol_conf.lazy_load_symbols;
	struct sym_entry *eager = NULL, *lazy = NULL;
	size_t nr_eager = 0, nr_lazy = 0, nr_names = 0, i;
	struct sym_loader eager_l, lazy_l;
	char filename[PATH_MAX];
	struct symbol *pos;
	struct rb_node *nd;
	int ret;

	if (dso_to_test)
		strlcpy(filename, dso_to_test, sizeof(filename));
	else
		perf_exe(filename, sizeof(filename));

	ret = sym_loader__init(&eager_l, filename, false);
	if (ret != TEST_OK)
		goto out;
	ret = sym_loader__init(&lazy_l, filename, true);
	if (ret != TEST_OK)
		goto out_eager;

	ret = TEST_FAIL;
	symbols__for_each_entry(dso__symbols(map__dso(lazy_l.map)), pos, nd)
		nr_names += symbol__needs_lazy_name(pos);
	if (nr_names)
		pr_debug("%s: %zu symbols with lazy names\n", filename, nr_names);
	else
		pr_debug("%s: no lazy names\n", filename);

	/* Look up before and after dso__for_each_symbol() loads every name. */
	if (get_sym_entries(map__dso(eager_l.map), &eager, &nr_eager) ||
	    check_lazy_lookups(lazy_l.map, eager_l.map, eager, nr_eager) != TEST_OK ||
	    get_sym_entries(map__dso(lazy_l.map), &lazy, &nr_lazy) ||
	    check_lazy_lookups(lazy_l.map, eager_l.map, eager, nr_eager) != TEST_OK)
		goto out_lazy;

	pr_debug("%s: %zu eager and %zu lazy symbols\n", filename, nr_eager, nr_lazy);
	for (i = 0; i < nr_eager && i < nr_lazy; i++) {
		if (cmp_sym_entry(&eager[i], &lazy[i])) {
			pr_debug("mismatch: eager %#" PRIx64 "-%#" PRIx64 " %s, lazy %#"
				 PRIx64 "-%#" PRIx64 " %s\n",
				 eager[i].start, eager[i].end, eager[i].name,
				 lazy[i].start, lazy[i].end, lazy[i].name);
			goto out_lazy;
		}
	}
	if (nr_eager == nr_lazy)
		ret = TEST_OK;
out_lazy:
	sym_loader__exit(&lazy_l);
out_eager:
	sym_loader__exit(&eager_l);
out:
	if (eager)
		free_sym_entries(eager, nr_eager);
	if (lazy)
		free_sym_entries(lazy, nr_lazy);
	symbol_conf.lazy_load_symbols = saved_lazy;
	return ret;
}

enum lazy_copy {
	LAZY_COPY_SAME,
	/* The string tables are not where they were, as after objcopy. */
	LAZY_COPY_MOVED,
	/* The copy is of another build. */
	LAZY_COPY_OTHER_BUILD,
	/* The copy has fewer sections, as after strip --strip-debug. */
	LAZY_COPY_FEWER_SECTIONS,
};

/*
 * Load @path lazily, replace it with a copy, count the lazy names and the
 * names that become "[unknown]", and tell if perf rejected the copy.
 */
static int lazy_copy__load(const char *path, const char *copy, enum lazy_copy how,
			   size_t *nr_names, size_t *nr_unknown, bool *rejected)
{
	size_t names = 0, unknown = 0;
	struct dso_lazy_file *lf;
	struct sym_loader l;
	struct build_id bid;
	struct symbol *pos;
	struct rb_node *nd;
	struct dso *dso;
	int ret;

	ret = sym_loader__init(&l, path, true);
	if (ret != TEST_OK)
		return ret;
	dso = map__dso(l.map);
	symbols__for_each_entry(dso__symbols(dso), pos, nd)
		names += symbol__needs_lazy_name(pos);
	if (!names || !dso__has_build_id(dso) ||
	    dso__symtab_type(dso) != DSO_BINARY_TYPE__SYSTEM_PATH_DSO) {
		pr_debug("%s: no lazy names read from the file\n", path);
		ret = TEST_SKIP;
		goto out;
	}

	lf = dso__lazy_file(dso);
	switch (how) {
	case LAZY_COPY_MOVED:
		for (size_t i = 0; i < ARRAY_SIZE(lf->strtab); i++)
			lf->strtab[i].off += !!lf->strtab[i].size;
		break;
	case LAZY_COPY_OTHER_BUILD:
		bid = *dso__bid(dso);
		bid.data[0] ^= 1;
		dso__set_build_id(dso, &bid);
		break;
	case LAZY_COPY_FEWER_SECTIONS:
		for (size_t i = 0; i < ARRAY_SIZE(lf->strtab); i++)
			lf->strtab[i].shndx += 1 << 16;
		break;
	case LAZY_COPY_SAME:
	default:
		break;
	}

	ret = TEST_FAIL;
	if (copyfile(path, copy) || rename(copy, path))
		goto out;
	dso__for_each_symbol(dso, pos, nd)
		unknown += !strcmp(pos->name, "[unknown]");
	pr_debug("%s: %zu lazy names, %zu unknown\n", path, names, unknown);
	*nr_names = names;
	*nr_unknown = unknown;
	*rejected = lf->rejected;
	ret = TEST_OK;
out:
	sym_loader__exit(&l);
	return ret;
}

/*
 * A copy of the file with the same build ID and string tables must give
 * every name. A copy whose string tables moved or are missing, or one of
 * another build, must give none, and perf must not try it again.
 */
static int test__symbol_lazy_copy(struct test_suite *test __maybe_unused,
				  int subtest __maybe_unused)
{
	static const enum lazy_copy cases[] = {
		LAZY_COPY_SAME, LAZY_COPY_MOVED, LAZY_COPY_OTHER_BUILD,
		LAZY_COPY_FEWER_SECTIONS,
	};
	bool saved_lazy = symbol_conf.lazy_load_symbols;
	char dir[] = "/tmp/perf-test-lazy-copy-XXXXXX";
	char exe[PATH_MAX], path[PATH_MAX], copy[PATH_MAX], saved_buildid_dir[PATH_MAX];
	size_t nr_names, nr_unknown;
	int ret = TEST_FAIL;
	bool rejected;

	perf_exe(exe, sizeof(exe));
	if (!mkdtemp(dir))
		return TEST_FAIL;
	/* Names must be read from the copy, not from a cached perf binary. */
	strlcpy(saved_buildid_dir, buildid_dir, sizeof(saved_buildid_dir));
	set_buildid_dir(dir);
	snprintf(path, sizeof(path), "%s/perf", dir);
	snprintf(copy, sizeof(copy), "%s/perf-new", dir);
	if (copyfile(exe, path)) {
		pr_debug("cannot copy %s\n", exe);
		goto out_unlink;
	}

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		bool same;

		ret = lazy_copy__load(path, copy, cases[i], &nr_names, &nr_unknown,
				      &rejected);
		if (ret != TEST_OK)
			break;
		same = cases[i] == LAZY_COPY_SAME;
		if ((same && (nr_unknown || rejected)) ||
		    (!same && (nr_unknown != nr_names || !rejected))) {
			pr_debug("case %zu: %zu of %zu names unknown%s\n", i, nr_unknown,
				 nr_names, rejected ? ", rejected" : "");
			ret = TEST_FAIL;
			break;
		}
	}
out_unlink:
	unlink(copy);
	unlink(path);
	rmdir(dir);
	set_buildid_dir(saved_buildid_dir);
	symbol_conf.lazy_load_symbols = saved_lazy;
	return ret;
}

/* Rename the owner of the build ID note of @dso in @path, so it is not found. */
static int lazy_break_build_id(const char *path, struct dso *dso)
{
	const struct build_id *bid = dso__bid(dso);
	char note[4 + BUILD_ID_SIZE] = "GNU";
	int fd = open(path, O_RDWR);
	struct stat st;
	char *p, *map;
	int ret = -1;

	if (fd < 0)
		return -1;
	memcpy(note + 4, bid->data, bid->size);
	if (fstat(fd, &st))
		goto out_close;
	map = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		goto out_close;
	p = memmem(map, st.st_size, note, 4 + bid->size);
	if (p) {
		p[0] = 'X';
		ret = 0;
	}
	munmap(map, st.st_size);
out_close:
	close(fd);
	return ret;
}

/*
 * A copy whose build ID cannot be read, as after a failed read, must give no
 * names, but must not stop a later copy that can be checked from giving them.
 */
static int test__symbol_lazy_check_failed(struct test_suite *test __maybe_unused,
					  int subtest __maybe_unused)
{
	bool saved_lazy = symbol_conf.lazy_load_symbols;
	char dir[] = "/tmp/perf-test-lazy-check-XXXXXX";
	char exe[PATH_MAX], path[PATH_MAX], copy[PATH_MAX], saved_buildid_dir[PATH_MAX];
	struct symbol *pos, *syms[2] = { NULL, NULL };
	struct sym_loader l;
	struct rb_node *nd;
	struct dso *dso;
	int ret = TEST_FAIL;
	size_t nr = 0;

	perf_exe(exe, sizeof(exe));
	if (!mkdtemp(dir))
		return TEST_FAIL;
	/* Names must be read from the copy, not from a cached perf binary. */
	strlcpy(saved_buildid_dir, buildid_dir, sizeof(saved_buildid_dir));
	set_buildid_dir(dir);
	snprintf(path, sizeof(path), "%s/perf", dir);
	snprintf(copy, sizeof(copy), "%s/perf-new", dir);
	if (copyfile(exe, path)) {
		pr_debug("cannot copy %s\n", exe);
		goto out_unlink;
	}

	ret = sym_loader__init(&l, path, true);
	if (ret != TEST_OK)
		goto out_unlink;
	dso = map__dso(l.map);
	symbols__for_each_entry(dso__symbols(dso), pos, nd) {
		if (symbol__needs_lazy_name(pos) && pos->end > pos->start)
			syms[nr++] = pos;
		if (nr == ARRAY_SIZE(syms))
			break;
	}
	if (nr < ARRAY_SIZE(syms) || !dso__has_build_id(dso) ||
	    dso__symtab_type(dso) != DSO_BINARY_TYPE__SYSTEM_PATH_DSO) {
		pr_debug("%s: no lazy names read from the file\n", path);
		ret = TEST_SKIP;
		goto out;
	}

	ret = TEST_FAIL;
	if (copyfile(path, copy) || lazy_break_build_id(copy, dso) || rename(copy, path))
		goto out;
	if (dso__find_symbol_nocache(dso, syms[0]->start)) {
		pr_debug("name read from a file that was not checked\n");
		goto out;
	}
	if (copyfile(exe, copy) || rename(copy, path))
		goto out;
	pos = dso__find_symbol_nocache(dso, syms[1]->start);
	if (pos == syms[1] && !symbol__needs_lazy_name(pos) && strcmp(pos->name, "[unknown]"))
		ret = TEST_OK;
	else
		pr_debug("name not read after the file could be checked\n");
out:
	sym_loader__exit(&l);
out_unlink:
	unlink(copy);
	unlink(path);
	rmdir(dir);
	set_buildid_dir(saved_buildid_dir);
	symbol_conf.lazy_load_symbols = saved_lazy;
	return ret;
}
#endif

static struct test_case tests__symbol_lazy[] = {
	TEST_CASE("Duplicate selection without names", symbol_lazy_choice),
#ifdef HAVE_LIBELF_SUPPORT
	TEST_CASE("Lazy symbol names", symbol_lazy_names),
	TEST_CASE("Lazy symbol names loaded concurrently", symbol_lazy_race),
	TEST_CASE("Lazy and eager symbol parity", symbol_lazy_parity),
	TEST_CASE("Lazy symbol names from a replaced file", symbol_lazy_replaced),
	TEST_CASE("Lazy symbol names from a file overwritten in place", symbol_lazy_overwritten),
	TEST_CASE("Lazy symbol names after a failed open", symbol_lazy_open_failed),
	TEST_CASE("Lazy symbol names from a copy of the file", symbol_lazy_copy),
	TEST_CASE("Lazy symbol names after a failed check", symbol_lazy_check_failed),
#endif
	{ .name = NULL, }
};

struct test_suite suite__symbol_lazy = {
	.desc = "Lazy symbol loading",
	.test_cases = tests__symbol_lazy,
};
