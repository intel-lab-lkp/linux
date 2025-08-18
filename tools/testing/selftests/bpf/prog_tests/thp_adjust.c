// SPDX-License-Identifier: GPL-2.0

#include <math.h>
#include <sys/mman.h>
#include <test_progs.h>
#include "cgroup_helpers.h"
#include "test_thp_adjust.skel.h"
#include "test_thp_adjust_failure.skel.h"

#define LEN (16 * 1024 * 1024) /* 16MB */
#define THP_ENABLED_FILE "/sys/kernel/mm/transparent_hugepage/enabled"
#define PMD_SIZE_FILE "/sys/kernel/mm/transparent_hugepage/hpage_pmd_size"

static char *thp_addr;
static char old_mode[32];

static int thp_mode_save(void)
{
	const char *start, *end;
	char buf[128];
	int fd, err;
	size_t len;

	fd = open(THP_ENABLED_FILE, O_RDONLY);
	if (fd == -1)
		return -1;

	err = read(fd, buf, sizeof(buf) - 1);
	if (err == -1)
		goto close;

	start = strchr(buf, '[');
	end = start ? strchr(start, ']') : NULL;
	if (!start || !end || end <= start) {
		err = -1;
		goto close;
	}

	len = end - start - 1;
	if (len >= sizeof(old_mode))
		len = sizeof(old_mode) - 1;
	strncpy(old_mode, start + 1, len);
	old_mode[len] = '\0';

close:
	close(fd);
	return err;
}

static int thp_mode_set(const char *desired_mode)
{
	int fd, err;

	fd = open(THP_ENABLED_FILE, O_RDWR);
	if (fd == -1)
		return -1;

	err = write(fd, desired_mode, strlen(desired_mode));
	close(fd);
	return err;
}

static int thp_mode_reset(void)
{
	int fd, err;

	fd = open(THP_ENABLED_FILE, O_WRONLY);
	if (fd == -1)
		return -1;

	err = write(fd, old_mode, strlen(old_mode));
	close(fd);
	return err;
}

int thp_alloc(long pagesize)
{
	int err, i;

	thp_addr = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (thp_addr == MAP_FAILED)
		return -1;

	err = madvise(thp_addr, LEN, MADV_HUGEPAGE);
	if (err == -1)
		goto unmap;

	/* Accessing a single byte within a page is sufficient to trigger a page fault. */
	for (i = 0; i < LEN; i += pagesize)
		thp_addr[i] = 1;
	return 0;

unmap:
	munmap(thp_addr, LEN);
	return -1;
}

static void thp_free(void)
{
	if (!thp_addr)
		return;
	munmap(thp_addr, LEN);
}

static int get_pmd_order(long pagesize)
{
	ssize_t bytes_read, size;
	char buf[64], *endptr;
	int fd, ret = -1;

	fd = open(PMD_SIZE_FILE, O_RDONLY);
	if (fd < 0)
		return -1;

	bytes_read = read(fd, buf, sizeof(buf) - 1);
	if (bytes_read <= 0)
		goto close_fd;

	/* Remove potential newline character */
	if (buf[bytes_read - 1] == '\n')
		buf[bytes_read - 1] = '\0';

	size = strtoul(buf, &endptr, 10);
	if (endptr == buf || *endptr != '\0')
		goto close_fd;
	if (size % pagesize != 0)
		goto close_fd;
	ret = size / pagesize;
	if ((ret & (ret - 1)) == 0)
		ret = log2(ret);

close_fd:
	close(fd);
	return ret;
}

static void subtest_thp_adjust(void)
{
	struct bpf_link *fentry_link, *ops_link;
	int err, cgrp_fd, cgrp_id, pmd_order;
	struct test_thp_adjust *skel;
	long pagesize;

	pagesize = sysconf(_SC_PAGESIZE);
	pmd_order = get_pmd_order(pagesize);
	if (!ASSERT_NEQ(pmd_order, -1, "get_pmd_order"))
		return;

	err = setup_cgroup_environment();
	if (!ASSERT_OK(err, "cgrp_env_setup"))
		return;

	cgrp_fd = create_and_get_cgroup("thp_adjust");
	if (!ASSERT_GE(cgrp_fd, 0, "create_and_get_cgroup"))
		goto cleanup;

	err = join_cgroup("thp_adjust");
	if (!ASSERT_OK(err, "join_cgroup"))
		goto close_fd;

	cgrp_id = get_cgroup_id("thp_adjust");
	if (!ASSERT_GE(cgrp_id, 0, "create_and_get_cgroup"))
		goto join_root;

	if (!ASSERT_NEQ(thp_mode_save(), -1, "THP mode save"))
		goto join_root;
	if (!ASSERT_GE(thp_mode_set("madvise"), 0, "THP mode set"))
		goto join_root;

	skel = test_thp_adjust__open();
	if (!ASSERT_OK_PTR(skel, "open"))
		goto thp_reset;

	skel->bss->cgrp_id = cgrp_id;
	skel->bss->pmd_order = pmd_order;

	err = test_thp_adjust__load(skel);
	if (!ASSERT_OK(err, "load"))
		goto destroy;

	fentry_link = bpf_program__attach_trace(skel->progs.thp_run);
	if (!ASSERT_OK_PTR(fentry_link, "attach fentry"))
		goto destroy;

	ops_link = bpf_map__attach_struct_ops(skel->maps.thp);
	if (!ASSERT_OK_PTR(ops_link, "attach struct_ops"))
		goto destroy;

	if (!ASSERT_NEQ(thp_alloc(pagesize), -1, "THP alloc"))
		goto destroy;

	/* After attaching struct_ops, THP will be allocated only in khugepaged . */
	if (!ASSERT_EQ(skel->bss->pf_alloc, 0, "alloc_in_pf"))
		goto thp_free;
	if (!ASSERT_GT(skel->bss->pf_disallow, 0, "disallow_in_pf"))
		goto thp_free;

	if (!ASSERT_GT(skel->bss->khugepaged_alloc, 0, "alloc_in_khugepaged"))
		goto thp_free;
	ASSERT_EQ(skel->bss->khugepaged_disallow, 0, "disallow_in_khugepaged");

thp_free:
	thp_free();
destroy:
	test_thp_adjust__destroy(skel);
thp_reset:
	ASSERT_GE(thp_mode_reset(), 0, "THP mode reset");
join_root:
	/* We must join the root cgroup before removing the created cgroup. */
	err = join_root_cgroup();
	ASSERT_OK(err, "join_cgroup to root");
close_fd:
	close(cgrp_fd);
	remove_cgroup("thp_adjust");
cleanup:
	cleanup_cgroup_environment();
}

void test_thp_adjust(void)
{
	if (test__start_subtest("thp_adjust"))
		subtest_thp_adjust();
	RUN_TESTS(test_thp_adjust_failure);
}
