// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/swap.h>
#include <unistd.h>

#include "kselftest.h"

#define TIERS_PATH "/sys/kernel/mm/swap/tiers"

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

static int tier_range(const char *name, int *start, int *end)
{
	char buf[4096], *line, *save;
	int fd;
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
		char tname[64];
		int idx, s, e;

		if (sscanf(line, "%63s %d %d %d", tname, &idx, &s, &e) != 4)
			continue;
		if (!strcmp(tname, name)) {
			*start = s;
			*end = e;
			return 0;
		}
	}
	return -1;
}

static bool tier_exists(const char *name)
{
	int s, e;

	return tier_range(name, &s, &e) == 0;
}

static bool range_is(const char *name, int start, int end)
{
	int s, e;

	if (tier_range(name, &s, &e))
		return false;
	return s == start && e == end;
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
		char tname[64];
		int idx, s, e;

		if (sscanf(line, "%63s %d %d %d", tname, &idx, &s, &e) == 4)
			count++;
	}
	return count;
}

static int test_coverage(void)
{
	if (tiers_write("+orphan:100") != -EINVAL)
		return KSFT_FAIL;
	if (tier_exists("orphan"))
		return KSFT_FAIL;
	return KSFT_PASS;
}

static int test_add(void)
{
	if (tiers_write("+lo:-1 +hi:50"))
		return KSFT_FAIL;
	if (!range_is("hi", 50, SHRT_MAX) || !range_is("lo", -1, 49))
		return KSFT_FAIL;
	return KSFT_PASS;
}

static int test_split(void)
{
	if (tiers_write("+mid:100"))
		return KSFT_FAIL;
	if (!range_is("mid", 100, SHRT_MAX) ||
	    !range_is("hi", 50, 99) ||
	    !range_is("lo", -1, 49))
		return KSFT_FAIL;
	return KSFT_PASS;
}

static int test_remove(void)
{
	/* Remove the top tier: 'hi' re-expands upward to SHRT_MAX. */
	if (tiers_write("-mid"))
		return KSFT_FAIL;
	if (tier_exists("mid") || !range_is("hi", 50, SHRT_MAX))
		return KSFT_FAIL;

	/* Remove the lowest tier: 'hi' shifts its start down to -1. */
	if (tiers_write("-lo"))
		return KSFT_FAIL;
	if (tier_exists("lo") || !range_is("hi", -1, SHRT_MAX))
		return KSFT_FAIL;

	return KSFT_PASS;
}

static int test_errors(void)
{
	if (tiers_write("+hi:100") != -EEXIST)		/* duplicate name */
		return KSFT_FAIL;
	if (tiers_write("+bad.name:100") != -EINVAL)	/* illegal name */
		return KSFT_FAIL;
	if (tiers_write("+dup:-1") != -EBUSY)		/* priority in use */
		return KSFT_FAIL;
	if (tiers_write("+low:-2") != -EINVAL)		/* prio < DEF_SWAP_PRIO */
		return KSFT_FAIL;
	return KSFT_PASS;
}

/*
 * A write carrying several operations is atomic: if any operation fails, the
 * whole batch is rolled back.
 */
static int test_atomic(void)
{
	if (tiers_write("+a:50 +a:60") != -EEXIST)
		return KSFT_FAIL;
	if (tier_exists("a") || !range_is("hi", -1, SHRT_MAX))
		return KSFT_FAIL;
	return KSFT_PASS;
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
	write(fd, val, strlen(val)); /* ignore error. best-effort cleanup */
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

static int wait_for_dev(const char *dev)
{
	int i;

	for (i = 0; i < 50; i++) {
		if (access(dev, F_OK) == 0)
			return 0;

		usleep(100000);
	}

	return -1;
}

static int test_device_pins_tier(void)
{
	char dev[32];
	int zidx, ret = KSFT_FAIL;

	if (tiers_write("+top:50"))
		return KSFT_FAIL;

	zidx = zram_add(64 << 20);
	if (zidx < 0) {
		ret = KSFT_SKIP;
		goto out_tier;
	}
	snprintf(dev, sizeof(dev), "/dev/zram%d", zidx);

	if (wait_for_dev(dev)) {
		ret = KSFT_SKIP;
		goto out_zram;
	}

	if (swap_setup(dev, 50)) {
		ret = KSFT_SKIP;
		goto out_zram;
	}

	if (tiers_write("-top") == -EBUSY) {		/* blocked while active */
		swapoff(dev);
		if (!tiers_write("-top"))		/* removable after swapoff */
			ret = KSFT_PASS;
	} else {
		swapoff(dev);
	}
out_zram:
	zram_remove(zidx);
out_tier:
	tiers_write("-top");
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
	ksft_print_header();
	ksft_set_plan(7);

	if (geteuid() != 0)
		ksft_exit_skip("test requires root\n");
	if (access(TIERS_PATH, F_OK))
		ksft_exit_skip("%s not present (CONFIG_SWAP/tiers)\n", TIERS_PATH);
	if (tier_count() != 0)
		ksft_exit_skip("swap tiers already configured; run on a clean system\n");

	ksft_test_result(test_coverage() == KSFT_PASS, "coverage rule\n");
	ksft_test_result(test_add() == KSFT_PASS, "add tiers\n");
	ksft_test_result(test_split() == KSFT_PASS, "split tier\n");
	ksft_test_result(test_remove() == KSFT_PASS, "remove and merge\n");
	ksft_test_result(test_errors() == KSFT_PASS, "invalid operations\n");
	ksft_test_result(test_atomic() == KSFT_PASS, "batch atomicity\n");

	ksft_test_result_code(test_device_pins_tier(), "device pins its tier", NULL);

	tiers_clear();

	ksft_finished();
}
