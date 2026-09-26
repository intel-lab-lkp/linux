// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026 Meta Platforms, Inc. and affiliates. */
#include <test_progs.h>
#include <sys/user.h>
#ifndef PAGE_SIZE /* on some archs it comes in sys/user.h */
#include <unistd.h>
#define PAGE_SIZE getpagesize()
#endif

#include "typed_arena.skel.h"

/* The sizes progs/typed_arena.c declares */
#define OBJ_ARENA_SIZE (256 * 1024)
#define WIDE_ARENA_SIZE (8 * 1024 * 1024)
#define BIG_OBJ_SIZE 8192
#define LIST_LEN 64

/* The map's memory usage as the "memlock:" line of its fdinfo. */
static long map_memlock(int map_fd)
{
	char path[64], line[128];
	long memlock = -1;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", map_fd);
	f = fopen(path, "r");
	if (!ASSERT_OK_PTR(f, "open_fdinfo"))
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "memlock:\t%ld", &memlock) == 1)
			break;
	}
	fclose(f);
	ASSERT_NEQ(memlock, -1, "parse_memlock");
	return memlock;
}

static int run_ret(struct bpf_program *prog, const char *name)
{
	LIBBPF_OPTS(bpf_test_run_opts, opts);
	int err = bpf_prog_test_run_opts(bpf_program__fd(prog), &opts);

	if (!ASSERT_OK(err, name))
		return -1;
	return opts.retval;
}

static int run(struct bpf_program *prog, const char *name)
{
	int ret = run_ret(prog, name);

	if (!ASSERT_OK(ret, name))
		return -1;
	return 0;
}

/* Poll the module object's reference count until a deferred release has run. */
static long wait_ref_cnt(struct typed_arena *skel, long want)
{
	int i;

	for (i = 0; i < 500; i++) {
		if (run(skel->progs.read_ref_cnt, "read_ref_cnt"))
			return -1;
		if (skel->bss->ref_cnt == want)
			break;
		usleep(10000);
	}
	return skel->bss->ref_cnt;
}

/* Poll the map's memory usage until it reaches the expected value. */
static long wait_memlock(int map_fd, long want)
{
	long memlock = -1;
	int i;

	for (i = 0; i < 500; i++) {
		memlock = map_memlock(map_fd);
		if (memlock == want)
			break;
		usleep(10000);
	}
	return memlock;
}

/* Poll a fixed allocation until the release of the chunk it names has run. */
static int wait_alloc_at(struct typed_arena *skel)
{
	int i, ret = -1;

	for (i = 0; i < 500; i++) {
		ret = run_ret(skel->progs.alloc_at, "alloc_at");
		if (ret <= 0)
			break;
		usleep(10000);
	}
	return ret;
}

/* Objects persist across invocations, and chunks come and go around them. */
static void test_pages(void)
{
	long ps = PAGE_SIZE, base, base_cnt;
	struct typed_arena *skel;
	void *p;
	int fd;

	skel = typed_arena__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_load"))
		return;
	fd = bpf_map__fd(skel->maps.arena);

	/* Registration at load accounts the page tables and the scratch chunks. */
	base = map_memlock(fd);
	ASSERT_GE(base, ps, "registered");
	if (run(skel->progs.read_ref_cnt, "base_ref_cnt"))
		goto out;
	base_cnt = skel->bss->ref_cnt;

	if (run(skel->progs.alloc, "alloc"))
		goto out;
	ASSERT_EQ(skel->data->page_cnt, 1, "granted");
	p = skel->bss->ptr;
	ASSERT_EQ(map_memlock(fd), base + ps, "after_alloc");

	skel->bss->value = 42;
	if (run(skel->progs.write_value, "write_value"))
		goto out;
	skel->bss->value = 0;
	if (run(skel->progs.read_value, "read_value"))
		goto out;
	ASSERT_EQ(skel->bss->value, 42, "persisted");

	/* The chunk is taken until released. */
	ASSERT_EQ(run_ret(skel->progs.alloc_at, "alloc_at"), 1, "taken");

	if (run(skel->progs.stash_ref, "stash_ref"))
		goto out;
	ASSERT_EQ(wait_ref_cnt(skel, base_cnt + 1), base_cnt + 1, "ref_stashed");

	/* Release drops the reference after the grace period and returns the memory. */
	if (run(skel->progs.free_pages, "free_pages"))
		goto out;
	ASSERT_EQ(wait_ref_cnt(skel, base_cnt), base_cnt, "ref_dropped");
	ASSERT_EQ(map_memlock(fd), base, "after_free");

	/* The same chunk can be claimed again, and comes back zeroed. */
	ASSERT_EQ(wait_alloc_at(skel), 0, "reclaimed");
	ASSERT_EQ(skel->bss->ptr, p, "same_object");
	if (run(skel->progs.read_value, "read_fresh"))
		goto out;
	ASSERT_EQ(skel->bss->value, 0, "fresh");
	if (run(skel->progs.free_pages, "free_again"))
		goto out;

	/*
	 * An object nobody allocated reads as the dummy object, and the chunk
	 * it faults in is taken until released in turn. Any value casts to an
	 * object, so a small offset names one as well as a pointer does.
	 */
	skel->bss->ptr = (void *)(7 * ps);
	skel->bss->value = 1;
	if (run(skel->progs.read_value, "read_unallocated"))
		goto out;
	ASSERT_EQ(skel->bss->value, 0, "dummy");
	ASSERT_EQ(run_ret(skel->progs.alloc_at, "alloc_scratch_chunk"), 1, "scratch_taken");
	if (run(skel->progs.free_pages, "free_scratch_chunk"))
		goto out;
	ASSERT_EQ(wait_alloc_at(skel), 0, "scratch_released");
out:
	typed_arena__destroy(skel);
}

/*
 * A typed arena belongs to the program BTF that registered it. Two loads of
 * the same object sharing one arena map carry two BTF objects, so a pointer
 * of one names an object in a different typed arena when the other casts it.
 */
static void test_identity(void)
{
	struct typed_arena *skel1, *skel2 = NULL;

	skel1 = typed_arena__open_and_load();
	if (!ASSERT_OK_PTR(skel1, "open_load1"))
		return;
	skel2 = typed_arena__open();
	if (!ASSERT_OK_PTR(skel2, "open2"))
		goto out;
	if (!ASSERT_OK(bpf_map__reuse_fd(skel2->maps.arena, bpf_map__fd(skel1->maps.arena)),
		       "reuse_fd"))
		goto out;
	if (!ASSERT_OK(typed_arena__load(skel2), "load2"))
		goto out;

	if (run(skel1->progs.alloc, "alloc1"))
		goto out;
	skel1->bss->value = 7;
	if (run(skel1->progs.write_value, "write1"))
		goto out;
	skel2->bss->ptr = skel1->bss->ptr;
	if (run(skel2->progs.read_value, "read2"))
		goto out;
	ASSERT_EQ(skel2->bss->value, 0, "distinct_typed_arena");
	if (run(skel1->progs.read_value, "read1"))
		goto out;
	ASSERT_EQ(skel1->bss->value, 7, "own_typed_arena");
out:
	typed_arena__destroy(skel2);
	typed_arena__destroy(skel1);
}

/* Every span of a batch of releases is processed, not only the first one with real pages. */
static void test_release_spans(void)
{
	struct typed_arena *skel;
	long ps = PAGE_SIZE, base;
	void *p;
	int fd, i;

	skel = typed_arena__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_load"))
		return;
	fd = bpf_map__fd(skel->maps.arena);
	base = map_memlock(fd);

	skel->data->page_cnt = 8;
	if (run(skel->progs.alloc, "alloc"))
		goto out;
	ASSERT_EQ(skel->data->page_cnt, 8, "granted");
	p = skel->bss->ptr;
	ASSERT_EQ(map_memlock(fd), base + 8 * ps, "after_alloc");

	/* Release the pages one at a time, so that the worker sees a span per page. */
	skel->data->page_cnt = 1;
	for (i = 0; i < 8; i++) {
		skel->bss->ptr = p + i * ps;
		if (run(skel->progs.free_pages, "free_pages"))
			goto out;
	}
	ASSERT_EQ(wait_memlock(fd, base), base, "all_released");
out:
	typed_arena__destroy(skel);
}

/* A release of more pages than the typed arena holds past the address is refused. */
static void test_release_bounds(void)
{
	long ps = PAGE_SIZE, base, nr = OBJ_ARENA_SIZE / PAGE_SIZE;
	struct typed_arena *skel;
	int fd;

	skel = typed_arena__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_load"))
		return;
	fd = bpf_map__fd(skel->maps.arena);
	base = map_memlock(fd);

	/* Fill the typed arena, so that a scan of its bitmap for a free chunk runs to the end. */
	skel->bss->ptr = NULL;
	skel->data->page_cnt = nr;
	if (run(skel->progs.alloc_at, "fill"))
		goto out;
	ASSERT_EQ(map_memlock(fd), base + nr * ps, "full");

	skel->data->page_cnt = nr + 1;
	if (run(skel->progs.free_pages, "free_oversized"))
		goto out;

	/*
	 * A release of the last chunk alone goes through. Once it has run,
	 * anything queued before it has run too.
	 */
	skel->bss->ptr = (void *)((nr - 1) * ps);
	skel->data->page_cnt = 1;
	if (run(skel->progs.free_pages, "free_last"))
		goto out;
	ASSERT_EQ(wait_alloc_at(skel), 0, "last_released");
	ASSERT_EQ(map_memlock(fd), base + nr * ps, "still_full");
	skel->bss->ptr = NULL;
	ASSERT_EQ(run_ret(skel->progs.alloc_at, "alloc_at"), 1, "first_still_taken");

	skel->data->page_cnt = nr;
	if (run(skel->progs.free_pages, "free_all"))
		goto out;
	ASSERT_EQ(wait_memlock(fd, base), base, "released");
out:
	typed_arena__destroy(skel);
}

/*
 * A chunk is released once: a second release of a chunk whose release is
 * queued is refused, so that it cannot take away whatever claims the chunk
 * after the first release has run. The repeated request is made while the
 * worker is waiting out the grace period of the first, which puts it in a
 * later batch; a request that arrives before the worker starts joins the
 * first batch instead and is harmless either way.
 */
static void test_release_once(void)
{
	struct typed_arena *skel;
	void *p1, *p2;
	long base;
	int fd;

	skel = typed_arena__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_load"))
		return;
	fd = bpf_map__fd(skel->maps.arena);
	base = map_memlock(fd);

	if (run(skel->progs.alloc, "alloc1"))
		goto out;
	p1 = skel->bss->ptr;
	if (run(skel->progs.alloc, "alloc2"))
		goto out;
	p2 = skel->bss->ptr;

	skel->bss->ptr = p1;
	if (run(skel->progs.free_pages, "free1"))
		goto out;
	usleep(1000);
	if (run(skel->progs.free_pages, "free1_again"))
		goto out;
	skel->bss->ptr = p2;
	if (run(skel->progs.free_pages, "free2"))
		goto out;

	/* Claim the first chunk again as soon as its release has run, and write to it. */
	skel->bss->ptr = p1;
	ASSERT_EQ(wait_alloc_at(skel), 0, "reclaimed");
	skel->bss->value = 42;
	if (run(skel->progs.write_value, "write_value"))
		goto out;

	/*
	 * The second chunk's release was queued after the repeated one. Once it
	 * has run, so has the repeated one, if it was accepted.
	 */
	skel->bss->ptr = p2;
	ASSERT_EQ(wait_alloc_at(skel), 0, "marker_released");

	skel->bss->ptr = p1;
	skel->bss->value = 0;
	if (run(skel->progs.read_value, "read_value"))
		goto out;
	ASSERT_EQ(skel->bss->value, 42, "kept");

	if (run(skel->progs.free_pages, "free1_final"))
		goto out;
	skel->bss->ptr = p2;
	if (run(skel->progs.free_pages, "free2_final"))
		goto out;
	ASSERT_EQ(wait_memlock(fd, base), base, "released");
out:
	typed_arena__destroy(skel);
}

/* A request larger than one batch of the allocator is served whole. */
static void test_batch_alloc(void)
{
	long ps = PAGE_SIZE, base, nr = WIDE_ARENA_SIZE / PAGE_SIZE;
	struct typed_arena *skel;
	void *p;
	int fd;

	skel = typed_arena__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_load"))
		return;
	fd = bpf_map__fd(skel->maps.arena);
	base = map_memlock(fd);

	skel->data->page_cnt = nr;
	if (run(skel->progs.wide_alloc, "wide_alloc"))
		goto out;
	ASSERT_EQ(skel->data->page_cnt, nr, "granted");
	p = skel->bss->ptr;
	ASSERT_EQ(map_memlock(fd), base + nr * ps, "after_alloc");

	/* The last page of the range is usable. */
	skel->bss->ptr = p + (nr - 1) * ps;
	skel->bss->value = 42;
	if (run(skel->progs.wide_touch, "wide_touch"))
		goto out;

	skel->bss->ptr = p;
	if (run(skel->progs.wide_free, "wide_free"))
		goto out;
	ASSERT_EQ(wait_memlock(fd, base), base, "released");
out:
	typed_arena__destroy(skel);
}

/* An object of more than a page is backed and released as one chunk. */
static void test_multipage(void)
{
	long ps = PAGE_SIZE, base, granted;
	struct typed_arena *skel;
	int fd;

	granted = BIG_OBJ_SIZE > ps ? BIG_OBJ_SIZE / ps : 1;

	skel = typed_arena__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_load"))
		return;
	fd = bpf_map__fd(skel->maps.arena);
	base = map_memlock(fd);

	/* One page requested, the whole object granted. */
	skel->data->page_cnt = 1;
	if (run(skel->progs.big_alloc, "big_alloc"))
		goto out;
	ASSERT_EQ(skel->data->page_cnt, granted, "granted");
	ASSERT_EQ(map_memlock(fd), base + granted * ps, "after_alloc");

	skel->bss->value = 42;
	if (run(skel->progs.big_touch, "big_touch"))
		goto out;

	if (run(skel->progs.big_free, "big_free"))
		goto out;
	ASSERT_EQ(wait_memlock(fd, base), base, "released");
out:
	typed_arena__destroy(skel);
}

/*
 * A list linked through typed pointer fields is walked with no cast, read
 * and written, and its NULL end dereferences object 0 of the slice.
 */
static void test_fields(void)
{
	struct typed_arena *skel;

	skel = typed_arena__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_load"))
		return;
	if (run(skel->progs.list_build, "list_build"))
		goto out;
	if (run(skel->progs.list_sum_values, "list_sum"))
		goto out;
	ASSERT_EQ(skel->bss->list_sum, LIST_LEN * (LIST_LEN + 1) / 2, "sum");
	if (run(skel->progs.list_bump_values, "list_bump"))
		goto out;
	if (run(skel->progs.list_sum_values, "list_sum_bumped"))
		goto out;
	ASSERT_EQ(skel->bss->list_sum, LIST_LEN * (LIST_LEN + 1) / 2 + LIST_LEN, "sum_bumped");

	skel->bss->value = 4242;
	if (run(skel->progs.list_deref_end, "list_deref_end"))
		goto out;
	ASSERT_EQ(skel->bss->list_sum, 4242, "null_is_object_zero");
out:
	typed_arena__destroy(skel);
}

void test_typed_arena(void)
{
	struct typed_arena *skel;
	bool supported;

	/* The programs need a compiler that emits the cast. */
	skel = typed_arena__open();
	if (!ASSERT_OK_PTR(skel, "open"))
		return;
	supported = skel->rodata->typed_arena_supported;
	typed_arena__destroy(skel);
	if (!supported) {
		test__skip();
		return;
	}

	if (test__start_subtest("pages"))
		test_pages();
	if (test__start_subtest("identity"))
		test_identity();
	if (test__start_subtest("release_spans"))
		test_release_spans();
	if (test__start_subtest("release_bounds"))
		test_release_bounds();
	if (test__start_subtest("release_once"))
		test_release_once();
	if (test__start_subtest("batch_alloc"))
		test_batch_alloc();
	if (test__start_subtest("multipage"))
		test_multipage();
	if (test__start_subtest("fields"))
		test_fields();
}
