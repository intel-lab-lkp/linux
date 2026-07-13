// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/swap.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"
#include "cgroup_util.h"

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif

#define TIERS_PATH "/sys/kernel/mm/swap/tiers"
#define TIERS_MAX "memory.swap.tiers.max"

static int tiers_write(const char *cmd)
{
	int fd, ret = 0;

	fd = open(TIERS_PATH, O_WRONLY);
	if (fd < 0)
		return -errno;
	if (write(fd, cmd, strlen(cmd)) < 0)
		ret = -errno;
	close(fd);
	return ret;
}

static int tier_count(void)
{
	char buf[4096], *line, *save;
	int fd, count = 0;
	ssize_t n;

	fd = open(TIERS_PATH, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';

	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char name[64];
		int idx, s, e;

		if (sscanf(line, "%63s %d %d %d", name, &idx, &s, &e) == 4)
			count++;
	}
	return count;
}

static long swap_used_kb(const char *dev)
{
	char line[256];
	long used = -1;
	FILE *f;

	f = fopen("/proc/swaps", "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char name[128], type[64];
		long size, u, prio;

		if (sscanf(line, "%127s %63s %ld %ld %ld",
			   name, type, &size, &u, &prio) >= 4 &&
		    !strcmp(name, dev)) {
			used = u;
			break;
		}
	}
	fclose(f);
	return used;
}

static int swap_active_count(void)
{
	char line[256];
	int n = 0;
	FILE *f;

	f = fopen("/proc/swaps", "r");
	if (!f)
		return -1;
	if (fgets(line, sizeof(line), f))
		while (fgets(line, sizeof(line), f))
			n++;
	fclose(f);
	return n;
}

static void zram_remove(int idx);
static int zram_add(long size)
{
	char path[128], val[64];
	ssize_t n;
	int idx, fd;

	fd = open("/sys/class/zram-control/hot_add", O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, val, sizeof(val) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	val[n] = '\0';
	idx = atoi(val);

	snprintf(path, sizeof(path), "/sys/block/zram%d/disksize", idx);
	fd = open(path, O_WRONLY);
	if (fd < 0) {
		zram_remove(idx);
		return -1;
	}

	snprintf(val, sizeof(val), "%ld", size);
	n = write(fd, val, strlen(val));
	close(fd);

	if (n != strlen(val)) {
		zram_remove(idx);
		return -1;
	}

	return idx;
}

static void zram_remove(int idx)
{
	char val[16];
	int fd;

	fd = open("/sys/class/zram-control/hot_remove", O_WRONLY);
	if (fd < 0)
		return;
	snprintf(val, sizeof(val), "%d", idx);
	write(fd, val, strlen(val)); /* ignore: best-effort cleanup */
	close(fd);
}

static int swap_setup(const char *dev, int prio)
{
	char cmd[128];

	snprintf(cmd, sizeof(cmd), "mkswap %s >/dev/null 2>&1", dev);
	if (system(cmd))
		return -1;
	return swapon(dev, SWAP_FLAG_PREFER | (prio & SWAP_FLAG_PRIO_MASK));
}

static int test_default(const char *root)
{
	char *cg = cg_name(root, "swaptier_default");
	int ret = KSFT_FAIL;

	if (!cg || cg_create(cg))
		goto out;
	if (!cg_read_strstr(cg, TIERS_MAX, "fast max") &&
	    !cg_read_strstr(cg, TIERS_MAX, "slow max"))
		ret = KSFT_PASS;
out:
	if (cg) {
		cg_destroy(cg);
		free(cg);
	}
	return ret;
}

static int test_toggle(const char *root)
{
	char *cg = cg_name(root, "swaptier_toggle");
	int ret = KSFT_FAIL;

	if (!cg || cg_create(cg))
		goto out;
	if (cg_write(cg, TIERS_MAX, "fast 0"))
		goto out;
	if (cg_read_strstr(cg, TIERS_MAX, "fast 0"))
		goto out;
	if (cg_write(cg, TIERS_MAX, "fast max"))
		goto out;
	if (cg_read_strstr(cg, TIERS_MAX, "fast max"))
		goto out;
	ret = KSFT_PASS;
out:
	if (cg) {
		cg_destroy(cg);
		free(cg);
	}
	return ret;
}

static int test_invalid(const char *root)
{
	char *cg = cg_name(root, "swaptier_invalid");
	int ret = KSFT_FAIL;

	if (!cg || cg_create(cg))
		goto out;
	if (!cg_write(cg, TIERS_MAX, "nosuchtier 0"))
		goto out;
	if (!cg_write(cg, TIERS_MAX, "fast bogus"))
		goto out;
	ret = KSFT_PASS;
out:
	if (cg) {
		cg_destroy(cg);
		free(cg);
	}
	return ret;
}

static int test_recreate(const char *root)
{
	char *cg = cg_name(root, "swaptier_recreate");
	int ret = KSFT_FAIL;

	if (!cg || cg_create(cg))
		goto out;
	if (cg_write(cg, TIERS_MAX, "fast 0"))
		goto out;
	if (cg_read_strstr(cg, TIERS_MAX, "fast 0"))
		goto out;
	if (tiers_write("-fast") || tiers_write("+fast:10"))
		goto out;
	if (cg_read_strstr(cg, TIERS_MAX, "fast max"))
		goto out;
	ret = KSFT_PASS;
out:
	if (cg) {
		cg_destroy(cg);
		free(cg);
	}
	return ret;
}

static int swapout_child(const char *cgroup, void *arg)
{
	size_t size = (size_t)arg;
	char *mem;
	size_t i;
	int page_size;

	mem = mmap(NULL, size, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED)
		return -1;

	page_size = sysconf(_SC_PAGE_SIZE);
	for (i = 0; i < size; i += page_size)
		mem[i] = 'x';
	if (madvise(mem, size, MADV_PAGEOUT))
		return -1;
	/* Hold the swap entries while the parent inspects /proc/swaps. */
	pause();
	return 0;
}

static int wait_for_dev(const char *dev, const char *dev2)
{
	int i;

	for (i = 0; i < 50; i++) {
		if (access(dev, F_OK) == 0 && access(dev2, F_OK) == 0)
			return 0;

		usleep(100000);
	}

	return -1;
}

static int run_routing_case(const char *cg)
{
	char fast_dev[32], slow_dev[32];
	int zfast = -1, zslow = -1;
	long used_fast, used_slow;
	int ret = KSFT_SKIP;
	pid_t pid = -1;
	int i;

	/* Only our devices must be present, so usage is unambiguous. */
	if (swap_active_count() != 0)
		return KSFT_SKIP;

	zfast = zram_add(MB(128));
	if (zfast < 0)
		goto out;
	snprintf(fast_dev, sizeof(fast_dev), "/dev/zram%d", zfast);

	zslow = zram_add(MB(128));
	if (zslow < 0)
		goto out;
	snprintf(slow_dev, sizeof(slow_dev), "/dev/zram%d", zslow);

	if (wait_for_dev(fast_dev, slow_dev))
		goto out;

	/* prio 10 -> 'fast' tier [10, MAX]; prio 0 -> 'slow' tier [-1, 9]. */
	if (swap_setup(fast_dev, 10) || swap_setup(slow_dev, 0))
		goto out;

	ret = KSFT_FAIL;

	pid = cg_run_nowait(cg, swapout_child, (void *)MB(64));
	if (pid < 0)
		goto out;

	for (i = 0; i < 300; i++) {		/* up to ~30s for pageout */
		if (swap_used_kb(slow_dev) > 0)
			break;
		usleep(100000);
	}

	used_fast = swap_used_kb(fast_dev);
	used_slow = swap_used_kb(slow_dev);
	if (used_slow > 0 && used_fast == 0)
		ret = KSFT_PASS;
	else
		ksft_print_msg("routing[%s]: fast=%ldKB slow=%ldKB (want fast=0, slow>0)\n",
			       cg, used_fast, used_slow);
out:
	if (pid > 0) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
	}
	if (zfast >= 0) {
		swapoff(fast_dev);
		zram_remove(zfast);
	}
	if (zslow >= 0) {
		swapoff(slow_dev);
		zram_remove(zslow);
	}
	return ret;
}

static int test_routing(const char *root)
{
	char *cg = cg_name(root, "swaptier_routing");
	int ret = KSFT_FAIL;

	if (!cg || cg_create(cg))
		goto out;
	if (cg_write(cg, TIERS_MAX, "fast 0"))
		goto out;
	ret = run_routing_case(cg);
out:
	if (cg) {
		cg_destroy(cg);
		free(cg);
	}
	return ret;
}

static char *make_parent(const char *root, const char *name)
{
	char *cg = cg_name(root, name);

	if (cg && !cg_create(cg) &&
	    !cg_write(cg, "cgroup.subtree_control", "+memory"))
		return cg;

	if (cg) {
		cg_destroy(cg);
		free(cg);
	}
	return NULL;
}

static int test_routing_parent_wins(const char *root)
{
	char *parent = make_parent(root, "swaptier_pwins");
	char *child = NULL;
	int ret = KSFT_FAIL;

	if (!parent)
		goto out;
	if (cg_write(parent, TIERS_MAX, "fast 0"))
		goto out;

	child = cg_name(parent, "child");
	if (!child || cg_create(child))
		goto out;
	if (cg_write(child, TIERS_MAX, "fast max"))	/* child tries to re-enable */
		goto out;

	ret = run_routing_case(child);
out:
	if (child) {
		cg_destroy(child);
		free(child);
	}
	if (parent) {
		cg_destroy(parent);
		free(parent);
	}
	return ret;
}

static int test_routing_child_restricts(const char *root)
{
	char *parent = make_parent(root, "swaptier_crestr");
	char *child = NULL;
	int ret = KSFT_FAIL;

	if (!parent)
		goto out;

	child = cg_name(parent, "child");
	if (!child || cg_create(child))
		goto out;
	if (cg_write(child, TIERS_MAX, "fast 0"))
		goto out;

	ret = run_routing_case(child);
out:
	if (child) {
		cg_destroy(child);
		free(child);
	}
	if (parent) {
		cg_destroy(parent);
		free(parent);
	}
	return ret;
}

static void tiers_clear(void)
{
	char buf[4096], *line, *save;
	int fd;
	ssize_t n;

	fd = open(TIERS_PATH, O_RDONLY);
	if (fd < 0)
		return;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 0)
		return;
	buf[n] = '\0';

	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char name[64], cmd[80];
		int idx, s, e;

		if (sscanf(line, "%63s %d %d %d", name, &idx, &s, &e) != 4)
			continue;
		snprintf(cmd, sizeof(cmd), "-%s", name);
		tiers_write(cmd);
	}
}

int main(void)
{
	char root[PATH_MAX];

	ksft_print_header();
	ksft_set_plan(7);

	if (geteuid() != 0)
		ksft_exit_skip("test requires root\n");
	if (cg_find_unified_root(root, sizeof(root), NULL))
		ksft_exit_skip("cgroup v2 isn't mounted\n");
	if (cg_read_strstr(root, "cgroup.controllers", "memory"))
		ksft_exit_skip("memory controller isn't available\n");
	if (cg_read_strstr(root, "cgroup.subtree_control", "memory"))
		if (cg_write(root, "cgroup.subtree_control", "+memory"))
			ksft_exit_skip("failed to enable memory controller\n");
	if (access(TIERS_PATH, F_OK))
		ksft_exit_skip("swap tiers interface not present\n");
	if (tier_count() != 0)
		ksft_exit_skip("swap tiers already configured; run on a clean system\n");

	/* Two tiers: fast = [10, MAX], slow = [-1, 9]. */
	if (tiers_write("+slow:-1 +fast:10"))
		ksft_exit_skip("failed to configure swap tiers\n");

	ksft_test_result(test_default(root) == KSFT_PASS, "default mask is max\n");
	ksft_test_result(test_toggle(root) == KSFT_PASS, "enable/disable tier\n");
	ksft_test_result(test_invalid(root) == KSFT_PASS, "invalid input rejected\n");
	ksft_test_result(test_recreate(root) == KSFT_PASS,
			 "recreated tier resets cgroup mask\n");

	ksft_test_result_code(test_routing(root),
			      "swapout honors tier mask", NULL);
	ksft_test_result_code(test_routing_parent_wins(root),
			      "child cannot re-enable a parent-disabled tier", NULL);
	ksft_test_result_code(test_routing_child_restricts(root),
			      "child can restrict tiers below its parent", NULL);

	tiers_clear();

	ksft_finished();
}
