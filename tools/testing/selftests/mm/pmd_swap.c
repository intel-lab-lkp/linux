// SPDX-License-Identifier: GPL-2.0
/* Test PMD-level swap entries and their users. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <pthread.h>
#include <linux/userfaultfd.h>
#include <time.h>

#include "kselftest_harness.h"
#include "vm_util.h"

#define ZSWAP_ENABLED_PATH "/sys/module/zswap/parameters/enabled"

/* pagemap: bits 0-54 hold the PFN, or type|offset for a swap entry. */
#define PM_PFRAME_MASK		((1ULL << 55) - 1)
/* Must match MAX_SWAPFILES_SHIFT in include/linux/swap.h. */
#define MAX_SWAPFILES_SHIFT	5

static bool check_swapped(int pagemap_fd, char *addr, unsigned long size)
{
	unsigned long off;

	for (off = 0; off < size; off += getpagesize())
		if (!pagemap_is_swapped(pagemap_fd, addr + off))
			return false;
	return true;
}

static bool zswap_enabled(void)
{
	char enabled = 0;
	FILE *f;

	f = fopen(ZSWAP_ENABLED_PATH, "r");
	if (!f)
		return false;

	if (fscanf(f, " %c", &enabled) != 1)
		enabled = 0;
	fclose(f);

	return enabled == 'Y' || enabled == 'y' || enabled == '1';
}

static bool swap_available(unsigned long required_bytes)
{
	unsigned long required_kb = (required_bytes + 1023) / 1024;
	unsigned long size_kb, used_kb;
	char line[256];
	bool ret = false;
	FILE *f;

	f = fopen("/proc/swaps", "r");
	if (!f)
		return false;

	/* Skip the header. */
	if (!fgets(line, sizeof(line), f))
		goto out;

	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%*s %*s %lu %lu", &size_kb, &used_kb) == 2 &&
		    size_kb >= used_kb && size_kb - used_kb >= required_kb) {
			ret = true;
			break;
		}
	}

out:
	fclose(f);
	return ret;
}

static bool same_swap_device(const struct stat *a, const struct stat *b)
{
	if (S_ISBLK(a->st_mode) && S_ISBLK(b->st_mode))
		return a->st_rdev == b->st_rdev;
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

/*
 * Returns true if @swap_dev is the one and only active swap device, and stores
 * its /proc/swaps priority in *prio so the caller can put it back the way it
 * found it.
 */
static bool swap_device_is_only_active(const char *swap_dev, int *prio)
{
	struct stat expected, active;
	char path[256], line[512];
	unsigned int nr_active = 0;
	bool matches = false;
	FILE *f;

	*prio = -1;

	if (stat(swap_dev, &expected))
		return false;

	f = fopen("/proc/swaps", "r");
	if (!f)
		return false;
	if (!fgets(line, sizeof(line), f))
		goto out;

	while (fgets(line, sizeof(line), f)) {
		int line_prio;

		if (sscanf(line, "%255s %*s %*s %*s %d", path, &line_prio) != 2)
			continue;
		nr_active++;
		if (nr_active > 1)
			goto out;
		matches = !stat(path, &active) &&
			  same_swap_device(&expected, &active);
		if (matches)
			*prio = line_prio;
	}

out:
	fclose(f);
	return nr_active == 1 && matches;
}

/* Re-enable a device swapoff()ed by this test, at its original priority. */
static int swapon_restore(const char *swap_dev, int prio)
{
	int flags = 0;

	if (prio >= 0)
		flags = SWAP_FLAG_PREFER |
			((prio << SWAP_FLAG_PRIO_SHIFT) & SWAP_FLAG_PRIO_MASK);
	return swapon(swap_dev, flags);
}

/* Locate this task's cgroup-v2 directory. An empty relative path is the root. */
static bool cgroup2_self_dir(char *buf, size_t len)
{
	char mnt[PATH_MAX] = "", type[64], rel[PATH_MAX] = "";
	char line[2 * PATH_MAX];
	size_t rel_len;
	FILE *f;

	f = fopen("/proc/self/mounts", "r");
	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%*s %4095s %63s", mnt, type) == 2 &&
		    !strcmp(type, "cgroup2"))
			goto found_mnt;
	}
	mnt[0] = '\0';
found_mnt:
	fclose(f);
	if (!mnt[0])
		return false;

	f = fopen("/proc/self/cgroup", "r");
	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "0::", 3))
			continue;
		rel_len = strcspn(line + 3, "\n");
		if (rel_len && rel_len < sizeof(rel)) {
			memcpy(rel, line + 3, rel_len);
			rel[rel_len] = '\0';
		}
		break;
	}
	fclose(f);
	if (!rel[0])
		return false;
	if (!strcmp(rel, "/"))
		rel[0] = '\0';
	return snprintf(buf, len, "%s%s/memory.reclaim", mnt, rel) < (int)len;
}

static bool cgroup_reclaim(unsigned long bytes)
{
	char path[PATH_MAX], val[32];
	ssize_t written;
	int fd, len, err;

	if (!cgroup2_self_dir(path, sizeof(path)))
		return false;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return false;
	len = snprintf(val, sizeof(val), "%lu", bytes);
	written = write(fd, val, len);
	err = errno;
	close(fd);
	/* -EAGAIN means it reclaimed something but fell short, which is fine. */
	return written == len || err == EAGAIN;
}

/*
 * mincore() reports 1 over a PMD swap entry iff the swap cache still holds it.
 * This samples the first slot only: it is a hint used to decide whether the
 * swap cache was evicted, not an assertion about the whole range.
 */
static bool swapcache_resident(char *mem)
{
	unsigned char vec[1];

	if (mincore(mem, getpagesize(), vec))
		return true;
	return vec[0] & 1;
}

/*
 * MADV_PAGEOUT leaves the folio in the swap cache on asynchronous swap
 * devices, so faulting would just remap the cached folio and never reach
 * swapin_sync(). Push it out so the PMD-order swap-in path is exercised.
 */
static bool drop_swapcache(char *mem, unsigned long pmd_size)
{
	unsigned long want = pmd_size;
	int i;

	for (i = 0; i < 3 && swapcache_resident(mem); i++, want *= 4)
		if (!cgroup_reclaim(want))
			break;
	return !swapcache_resident(mem);
}

static unsigned int random_seed(void)
{
	unsigned int seed;

	if (getrandom(&seed, sizeof(seed), 0) != sizeof(seed))
		seed = (unsigned int)time(NULL);
	return seed;
}

static unsigned long test_page_size(void)
{
	static unsigned long page_size;

	if (!page_size)
		page_size = getpagesize();
	return page_size;
}

/*
 * Two base pages of the same PMD must never hold identical bytes, or the split
 * tests cannot tell that the slots came back in the wrong order. A single byte
 * cannot encode a page index on its own - HPAGE_PMD_NR is 8192 on arm64 with
 * 64K pages - so spell the index out in the first two bytes of every page.
 */
static unsigned char pattern_byte(unsigned int seed, unsigned long off)
{
	unsigned long page_size = test_page_size();
	unsigned long idx = off & (page_size - 1);

	if (idx < 2)
		return (unsigned char)(seed + ((off / page_size) >> (idx * 8)));

	return (unsigned char)(seed + off + (off >> 8) + (off >> 16));
}

static void fill_pattern(char *buf, unsigned long size, unsigned int seed)
{
	unsigned long i;

	for (i = 0; i < size; i++)
		buf[i] = (char)pattern_byte(seed, i);
}

static bool verify_pattern_range(char *buf, unsigned long size,
				 unsigned int seed, unsigned long offset)
{
	unsigned long i;

	for (i = 0; i < size; i++)
		if ((unsigned char)buf[i] != pattern_byte(seed, offset + i))
			return false;
	return true;
}

static bool verify_pattern(char *buf, unsigned long size, unsigned int seed)
{
	return verify_pattern_range(buf, size, seed, 0);
}

static bool verify_zero(char *buf, unsigned long size)
{
	unsigned long i;

	for (i = 0; i < size; i++)
		if (buf[i])
			return false;
	return true;
}

/*
 * mmap an anonymous PMD-aligned region of pmd_size bytes. Over-allocates
 * by one PMD and trims the unaligned head/tail so the returned address is
 * PMD-aligned (required for whole-PMD UFFDIO_MOVE).
 */
static char *mmap_pmd_aligned(unsigned long pmd_size)
{
	unsigned long pad = pmd_size;
	char *raw, *aligned;

	raw = mmap(NULL, pmd_size + pad, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED)
		return MAP_FAILED;

	aligned = (char *)(((uintptr_t)raw + pmd_size - 1) & ~(pmd_size - 1));
	if (aligned != raw)
		munmap(raw, aligned - raw);
	if (aligned + pmd_size != raw + pmd_size + pad)
		munmap(aligned + pmd_size,
		       (raw + pmd_size + pad) - (aligned + pmd_size));
	return aligned;
}

/* Per-process swapped size in bytes, from /proc/self/status VmSwap. */
static unsigned long read_vmswap(void)
{
	char line[256];
	unsigned long kb = 0;
	FILE *f;

	f = fopen("/proc/self/status", "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "VmSwap:", 7)) {
			kb = strtoul(line + 7, NULL, 10);
			break;
		}
	}
	fclose(f);
	return kb * 1024;
}

/*
 * Swap the PMD range out. Returns true if a PMD swap entry was installed.
 * On failure *swap_failed is set unless this environment simply cannot swap
 * at PMD granularity, in which case the caller should skip rather than fail.
 */
static bool swap_out_pmd(char *mem, unsigned long pmd_size, int pagemap_fd,
			 bool *swap_failed)
{
	long pmd_before = read_vmstat("thp_swpout_pmd");
	long fallback_before = read_vmstat("thp_swpout_fallback");
	long pmd_after, fallback_after;
	bool swapped;

	/*
	 * A kernel without PMD swap entry support has no thp_swpout_pmd at
	 * all. Skip rather than report a failure the kernel never promised.
	 */
	if (pmd_before < 0 || fallback_before < 0) {
		ksft_print_msg("no thp_swpout_pmd counter; PMD swap entries unsupported\n");
		return false;
	}

	if (madvise(mem, pmd_size, MADV_PAGEOUT)) {
		ksft_print_msg("MADV_PAGEOUT failed: %s\n", strerror(errno));
		*swap_failed = true;
		return false;
	}

	swapped = check_swapped(pagemap_fd, mem, pmd_size);
	pmd_after = read_vmstat("thp_swpout_pmd");
	fallback_after = read_vmstat("thp_swpout_fallback");
	ksft_print_msg("thp_swpout_pmd: %ld -> %ld, fallback: %ld -> %ld\n",
		       pmd_before, pmd_after, fallback_before, fallback_after);

	if (!swapped) {
		ksft_print_msg("MADV_PAGEOUT did not swap the whole PMD range\n");
		*swap_failed = true;
		return false;
	}
	/*
	 * Both counters are system-wide, so another task swapping a THP during
	 * the window above can move either of them. Test the fallback counter
	 * first: if reclaim split *any* THP we cannot be sure ours was not the
	 * one, and skipping is the safe direction. That also covers a kernel
	 * built without CONFIG_THP_SWAP, where folio_alloc_swap() returns
	 * -E2BIG for every PMD-order folio.
	 */
	if (fallback_after > fallback_before) {
		ksft_print_msg("PMD swap unavailable; reclaim used PTE fallback\n");
		return false;
	}
	if (pmd_after > pmd_before)
		return true;

	*swap_failed = true;
	return false;
}

static char *alloc_fill_swap_thp(unsigned long pmd_size, int pagemap_fd,
				 unsigned int seed, bool *swap_failed)
{
	char *mem;

	*swap_failed = false;

	mem = mmap_pmd_aligned(pmd_size);
	if (mem == MAP_FAILED)
		return MAP_FAILED;

	if (madvise(mem, pmd_size, MADV_HUGEPAGE)) {
		ksft_print_msg("MADV_HUGEPAGE failed: %s\n", strerror(errno));
		munmap(mem, pmd_size);
		return MAP_FAILED;
	}
	fill_pattern(mem, pmd_size, seed);

	if (!check_huge_anon(mem, pmd_size, 1, pmd_size)) {
		munmap(mem, pmd_size);
		return MAP_FAILED;
	}
	if (!swap_out_pmd(mem, pmd_size, pagemap_fd, swap_failed)) {
		munmap(mem, pmd_size);
		return MAP_FAILED;
	}

	return mem;
}

struct rwp_access_args {
	unsigned char *addr;
	unsigned char expected;
	bool write;
	bool ok;
};

static void *rwp_access_thread(void *data)
{
	struct rwp_access_args *args = data;

	if (args->write)
		*args->addr = args->expected;
	args->ok = *args->addr == args->expected;
	return NULL;
}

static int register_rwp(char *addr, unsigned long size, bool protect)
{
	struct uffdio_register reg = {};
	struct uffdio_rwprotect rwp = {};
	struct uffdio_api api = {};
	int uffd;

	uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (uffd < 0)
		return -1;

	api.api = UFFD_API;
	api.features = UFFD_FEATURE_RWP;
	if (ioctl(uffd, UFFDIO_API, &api) ||
	    !(api.features & UFFD_FEATURE_RWP))
		goto error;

	reg.range.start = (unsigned long)addr;
	reg.range.len = size;
	reg.mode = UFFDIO_REGISTER_MODE_RWP;
	if (ioctl(uffd, UFFDIO_REGISTER, &reg))
		goto error;

	if (!protect)
		return uffd;

	rwp.range.start = (unsigned long)addr;
	rwp.range.len = size;
	rwp.mode = UFFDIO_RWPROTECT_MODE_RWP;
	if (!ioctl(uffd, UFFDIO_RWPROTECT, &rwp))
		return uffd;

error:
	close(uffd);
	return -1;
}

static bool expect_rwp_fault(int uffd, char *addr, unsigned long size,
			     unsigned char expected, bool write)
{
	struct rwp_access_args args = {
		.addr = (unsigned char *)addr,
		.expected = expected,
		.write = write,
	};
	struct uffdio_rwprotect rwp = {
		.range = {
			.start = (unsigned long)addr,
			.len = size,
		},
	};
	struct pollfd pollfd = {
		.fd = uffd,
		.events = POLLIN,
	};
	struct uffd_msg msg = {};
	pthread_t thread;
	bool saw_rwp = false;
	int ret;

	if (pthread_create(&thread, NULL, rwp_access_thread, &args))
		return false;

	ret = poll(&pollfd, 1, 5000);
	if (ret == 1 && (pollfd.revents & POLLIN) &&
	    read(uffd, &msg, sizeof(msg)) == (ssize_t)sizeof(msg)) {
		saw_rwp = msg.event == UFFD_EVENT_PAGEFAULT &&
			  (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_RWP);
	}

	/* Resolve the access even on failure so the worker cannot remain blocked. */
	ioctl(uffd, UFFDIO_RWPROTECT, &rwp);
	if (pthread_join(thread, NULL))
		return false;
	return saw_rwp && args.ok;
}

FIXTURE(pmd_swap)
{
	unsigned long pmd_size;
	unsigned long mem_len;
	int pagemap_fd;
	int uffd;
	unsigned int seed;
	bool zswap_enabled;
	bool swap_disabled;
	int swap_prio;
	bool swapcache_dropped;
	const char *swap_dev;
	char *mem;
	char *aux;
};

FIXTURE_SETUP(pmd_swap)
{
	bool swap_failed;

	self->pagemap_fd = -1;
	self->uffd = -1;
	self->mem = MAP_FAILED;
	self->aux = MAP_FAILED;
	self->mem_len = 0;
	self->swap_disabled = false;
	self->swap_prio = -1;
	self->swapcache_dropped = false;
	self->swap_dev = NULL;

	self->pmd_size = read_pmd_pagesize();
	if (!self->pmd_size)
		SKIP(return, "Cannot determine PMD size\n");

	self->pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	if (self->pagemap_fd < 0)
		SKIP(return, "Cannot open /proc/self/pagemap\n");

	if (!swap_available(self->pmd_size))
		SKIP(return, "No active swap device has enough free space\n");

	self->seed = random_seed();
	self->zswap_enabled = zswap_enabled();
	self->mem = alloc_fill_swap_thp(self->pmd_size, self->pagemap_fd,
					self->seed, &swap_failed);
	if (self->mem == MAP_FAILED) {
		ASSERT_FALSE(swap_failed);
		SKIP(return, "Could not create swapped THP\n");
	}
	self->mem_len = self->pmd_size;
	self->swapcache_dropped = drop_swapcache(self->mem, self->pmd_size);
	ksft_print_msg("swap cache %s the swapped-out THP\n",
		       self->swapcache_dropped ? "no longer holds"
					       : "still holds");
}

FIXTURE_TEARDOWN(pmd_swap)
{
	int swap_err = 0;
	int swap_ret = 0;

	if (self->swap_disabled) {
		swap_ret = swapon_restore(self->swap_dev, self->swap_prio);
		swap_err = errno;
	}
	if (self->uffd >= 0)
		close(self->uffd);
	if (self->aux != MAP_FAILED)
		munmap(self->aux, self->pmd_size);
	if (self->mem != MAP_FAILED)
		munmap(self->mem, self->mem_len);
	if (self->pagemap_fd >= 0)
		close(self->pagemap_fd);

	EXPECT_EQ(swap_ret, 0) {
		TH_LOG("swapon(%s) failed: %s", self->swap_dev,
		       strerror(swap_err));
	}
}

TEST_F(pmd_swap, basic)
{
	ASSERT_TRUE(verify_pattern(self->mem, self->pmd_size, self->seed));
}

/*
 * With the swap cache evicted, the fault cannot be served by remapping a
 * cached folio, so this covers the PMD-order swapin_sync() read.
 */
TEST_F(pmd_swap, swapin_sync)
{
	if (!self->swapcache_dropped)
		SKIP(return, "Could not evict the folio from the swap cache\n");

	ASSERT_TRUE(verify_pattern(self->mem, self->pmd_size, self->seed));
	if (self->zswap_enabled)
		ksft_print_msg("zswap enabled: PMD restoration not checked\n");
	else
		ASSERT_TRUE(check_huge_anon(self->mem, self->pmd_size, 1,
					    self->pmd_size));
}

TEST_F(pmd_swap, fork)
{
	pid_t pid;
	int status;

	pid = fork();
	ASSERT_GE(pid, 0);

	if (pid == 0)
		_exit(verify_pattern(self->mem, self->pmd_size,
				     self->seed) ? 0 : 1);

	ASSERT_TRUE(verify_pattern(self->mem, self->pmd_size, self->seed));

	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	ASSERT_TRUE(WIFEXITED(status));
	ASSERT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(pmd_swap, fork_cow)
{
	unsigned int parent_seed = self->seed;
	unsigned int child_seed = ~self->seed;
	unsigned int new_seed = self->seed ^ 0xa5a5a5a5;
	int release_child[2];
	bool parent_ok;
	char c = 0;
	pid_t pid;
	int status, ret;

	ASSERT_EQ(pipe(release_child), 0);

	pid = fork();
	ASSERT_GE(pid, 0);

	if (pid == 0) {
		close(release_child[1]);
		if (read(release_child[0], &c, 1) != 1)
			_exit(1);
		if (!verify_pattern(self->mem, self->pmd_size, parent_seed))
			_exit(2);
		fill_pattern(self->mem, self->pmd_size, child_seed);
		if (!verify_pattern(self->mem, self->pmd_size, child_seed))
			_exit(3);
		_exit(0);
	}

	close(release_child[0]);
	fill_pattern(self->mem, self->pmd_size, new_seed);
	parent_ok = verify_pattern(self->mem, self->pmd_size, new_seed);
	ret = write(release_child[1], &c, 1);
	close(release_child[1]);
	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	ASSERT_EQ(ret, 1);
	ASSERT_TRUE(parent_ok);
	ASSERT_TRUE(WIFEXITED(status));
	ASSERT_EQ(WEXITSTATUS(status), 0);
	ASSERT_TRUE(verify_pattern(self->mem, self->pmd_size, new_seed));
}

TEST_F(pmd_swap, write)
{
	self->mem[0] = 0xbb;
	ASSERT_EQ(self->mem[0], (char)0xbb);
	ASSERT_TRUE(verify_pattern_range(self->mem + 1, self->pmd_size - 1,
					 self->seed, 1));
	if (self->zswap_enabled)
		ksft_print_msg("zswap enabled: PMD restoration not checked\n");
	else
		ASSERT_TRUE(check_huge_anon(self->mem, self->pmd_size, 1,
					    self->pmd_size));
}

TEST_F(pmd_swap, rwp_swapin)
{
	self->uffd = register_rwp(self->mem, self->pmd_size, true);
	if (self->uffd < 0)
		SKIP(return, "Userfaultfd RWP unsupported\n");

	ASSERT_TRUE(expect_rwp_fault(self->uffd, self->mem, self->pmd_size,
				     pattern_byte(self->seed, 0), false));
	ASSERT_TRUE(verify_pattern(self->mem, self->pmd_size, self->seed));
}

TEST_F(pmd_swap, munmap)
{
	unsigned long swap_before, swap_after;
	int ret;

	swap_before = read_vmswap();
	ASSERT_GE(swap_before, self->pmd_size);

	ret = munmap(self->mem, self->pmd_size);
	if (!ret) {
		self->mem = MAP_FAILED;
		self->mem_len = 0;
	}
	ASSERT_EQ(ret, 0);

	swap_after = read_vmswap();
	ASSERT_LE(swap_after, swap_before - self->pmd_size);
}

TEST_F(pmd_swap, mprotect)
{
	ASSERT_EQ(mprotect(self->mem, self->pmd_size, PROT_READ), 0);
	ASSERT_TRUE(check_swapped(self->pagemap_fd, self->mem,
				  self->pmd_size));
	ASSERT_EQ(mprotect(self->mem, self->pmd_size,
			   PROT_READ | PROT_WRITE), 0);
	ASSERT_TRUE(check_swapped(self->pagemap_fd, self->mem,
				  self->pmd_size));
	ASSERT_TRUE(verify_pattern(self->mem, self->pmd_size, self->seed));
}

TEST_F(pmd_swap, split_mprotect)
{
	unsigned long half = self->pmd_size / 2;

	ASSERT_EQ(mprotect(self->mem, half, PROT_READ), 0);
	ASSERT_TRUE(check_swapped(self->pagemap_fd, self->mem,
				  self->pmd_size));
	ASSERT_EQ(mprotect(self->mem, half, PROT_READ | PROT_WRITE), 0);
	ASSERT_TRUE(verify_pattern(self->mem, self->pmd_size, self->seed));
}

TEST_F(pmd_swap, split_munmap)
{
	unsigned long half = self->pmd_size / 2;
	unsigned long swap_before = read_vmswap();
	unsigned long i;
	char *base = self->mem;
	int ret;

	ASSERT_GE(swap_before, half);
	ret = munmap(base, half);
	if (!ret) {
		self->mem = base + half;
		self->mem_len = half;
	}
	ASSERT_EQ(ret, 0);
	ASSERT_LE(read_vmswap(), swap_before - half);

	for (i = 0; i < half; i += getpagesize())
		ASSERT_TRUE(pagemap_is_swapped(self->pagemap_fd,
					       self->mem + i));
	ASSERT_TRUE(verify_pattern_range(self->mem, half, self->seed, half));
}

TEST_F(pmd_swap, uffdio_move)
{
	struct uffdio_register reg = {};
	struct uffdio_move move = {};
	struct uffdio_api api = {};
	bool rwp;

	self->aux = mmap_pmd_aligned(self->pmd_size);
	if (self->aux == MAP_FAILED)
		SKIP(return, "Could not mmap aligned dst\n");
	ASSERT_EQ(madvise(self->aux, self->pmd_size, MADV_HUGEPAGE), 0);

	self->uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (self->uffd < 0)
		SKIP(return, "userfaultfd unavailable\n");

	api.api = UFFD_API;
	api.features = UFFD_FEATURE_MOVE | UFFD_FEATURE_RWP;
	if (ioctl(self->uffd, UFFDIO_API, &api) ||
	    !(api.features & UFFD_FEATURE_MOVE))
		SKIP(return, "UFFD_FEATURE_MOVE unsupported\n");
	rwp = api.features & UFFD_FEATURE_RWP;

	reg.range.start = (unsigned long)self->aux;
	reg.range.len = self->pmd_size;
	reg.mode = UFFDIO_REGISTER_MODE_MISSING |
		   (rwp ? UFFDIO_REGISTER_MODE_RWP : 0);
	ASSERT_EQ(ioctl(self->uffd, UFFDIO_REGISTER, &reg), 0);

	move.dst = (unsigned long)self->aux;
	move.src = (unsigned long)self->mem;
	move.len = self->pmd_size;
	ASSERT_EQ(ioctl(self->uffd, UFFDIO_MOVE, &move), 0);
	ASSERT_EQ(move.move, self->pmd_size);

	ASSERT_TRUE(check_swapped(self->pagemap_fd, self->aux,
				  self->pmd_size));
	if (rwp)
		ASSERT_TRUE(expect_rwp_fault(self->uffd, self->aux,
					     self->pmd_size,
					     pattern_byte(self->seed, 0), false));
	ASSERT_TRUE(verify_pattern(self->aux, self->pmd_size, self->seed));
	if (self->zswap_enabled)
		ksft_print_msg("zswap enabled: PMD restoration not checked\n");
	else
		ASSERT_TRUE(check_huge_anon(self->aux, self->pmd_size, 1,
					    self->pmd_size));
}

TEST_F(pmd_swap, mremap)
{
	char *new_mem, *dst;

	self->aux = mmap_pmd_aligned(self->pmd_size);
	if (self->aux == MAP_FAILED)
		SKIP(return, "Could not mmap aligned dst\n");
	dst = self->aux;

	new_mem = mremap(self->mem, self->pmd_size, self->pmd_size,
			 MREMAP_MAYMOVE | MREMAP_FIXED, dst);
	if (new_mem != MAP_FAILED) {
		self->mem = new_mem;
		self->aux = MAP_FAILED;
	}
	ASSERT_NE(new_mem, MAP_FAILED);
	ASSERT_EQ(new_mem, dst);

	ASSERT_TRUE(check_swapped(self->pagemap_fd, new_mem, self->pmd_size));
	ASSERT_TRUE(verify_pattern(new_mem, self->pmd_size, self->seed));
}

TEST_F(pmd_swap, pagemap)
{
	uint64_t entry, first;
	unsigned long off;

	entry = pagemap_get_entry(self->pagemap_fd, self->mem);
	ASSERT_TRUE(entry & (1ULL << 62));
	ASSERT_FALSE(entry & (1ULL << 63));
	first = entry & PM_PFRAME_MASK;

	/*
	 * The kernel zeroes the swap type/offset payload for readers without
	 * CAP_SYS_ADMIN, so the slot-ordering check below would silently test
	 * nothing. Skip instead of passing vacuously.
	 */
	if (!first)
		SKIP(return, "pagemap swap offsets need CAP_SYS_ADMIN\n");

	for (off = getpagesize(); off < self->pmd_size; off += getpagesize()) {
		uint64_t idx = off / getpagesize();

		entry = pagemap_get_entry(self->pagemap_fd, self->mem + off);
		ASSERT_TRUE(entry & (1ULL << 62));
		ASSERT_FALSE(entry & (1ULL << 63));
		ASSERT_EQ(entry & PM_PFRAME_MASK,
			  first + (idx << MAX_SWAPFILES_SHIFT));
	}
}

TEST_F(pmd_swap, mincore)
{
	unsigned long pages = self->pmd_size / getpagesize();
	unsigned char vec[pages];
	unsigned long i;

	ASSERT_EQ(mincore(self->mem, self->pmd_size, vec), 0);
	/*
	 * Nothing in this test faults the range, and the fixture already
	 * evicted the swap cache, so every slot must report not-resident. The
	 * other direction is not stable - reclaim can drop the cached folio
	 * between fixture setup and here, and a split cache reports per slot -
	 * so only check that mincore() answered.
	 */
	if (self->swapcache_dropped) {
		for (i = 0; i < pages; i++)
			ASSERT_EQ(vec[i] & 1, 0);
	}
	ASSERT_TRUE(check_swapped(self->pagemap_fd, self->mem,
				  self->pmd_size));
}

TEST_F(pmd_swap, madvise_free)
{
	unsigned long swap_before = read_vmswap();
	unsigned long i;

	ASSERT_TRUE(check_swapped(self->pagemap_fd, self->mem,
				  self->pmd_size));
	ASSERT_GE(swap_before, self->pmd_size);
	ASSERT_EQ(madvise(self->mem, self->pmd_size, MADV_FREE), 0);
	for (i = 0; i < self->pmd_size; i += getpagesize())
		ASSERT_FALSE(pagemap_is_swapped(self->pagemap_fd,
						self->mem + i));
	ASSERT_LE(read_vmswap(), swap_before - self->pmd_size);
	ASSERT_TRUE(verify_zero(self->mem, self->pmd_size));
}

TEST_F(pmd_swap, madvise_willneed)
{
	ASSERT_EQ(madvise(self->mem, self->pmd_size, MADV_WILLNEED), 0);
	ASSERT_TRUE(check_swapped(self->pagemap_fd, self->mem,
				  self->pmd_size));
	ASSERT_TRUE(verify_pattern(self->mem, self->pmd_size, self->seed));
	if (self->zswap_enabled)
		ksft_print_msg("zswap enabled: PMD restoration not checked\n");
	else
		ASSERT_TRUE(check_huge_anon(self->mem, self->pmd_size, 1,
					    self->pmd_size));
}

TEST_F(pmd_swap, swapoff)
{
	int ret, err;

	self->swap_dev = getenv("PMD_SWAP_DEVICE");
	if (!self->swap_dev)
		SKIP(return, "PMD_SWAP_DEVICE env var not set\n");
	/*
	 * Otherwise a higher-priority device may have taken the PMD swap
	 * entry and swapoff() would operate on the wrong backend.
	 */
	if (!swap_device_is_only_active(self->swap_dev, &self->swap_prio))
		SKIP(return, "PMD_SWAP_DEVICE must be the only active swap device\n");

	self->uffd = register_rwp(self->mem, self->pmd_size, true);

	ret = swapoff(self->swap_dev);
	err = errno;
	if (!ret)
		self->swap_disabled = true;
	ASSERT_EQ(ret, 0) {
		TH_LOG("swapoff(%s) failed: %s", self->swap_dev, strerror(err));
	}

	/*
	 * Check residency before touching the memory. If we read
	 * first, a bug that left a PMD swap entry in place after swapoff
	 * would silently trigger do_huge_pmd_swap_page() and reinstall a
	 * PMD mapping, masking the regression.
	 */
	if (self->zswap_enabled)
		ksft_print_msg("zswap enabled: PMD restoration not checked\n");
	else
		ASSERT_TRUE(check_huge_anon(self->mem, self->pmd_size, 1,
					    self->pmd_size));
	if (self->uffd >= 0)
		ASSERT_TRUE(expect_rwp_fault(self->uffd, self->mem,
					     self->pmd_size,
					     pattern_byte(self->seed, 0), false));
	ASSERT_TRUE(verify_pattern(self->mem, self->pmd_size, self->seed));

	ret = swapon_restore(self->swap_dev, self->swap_prio);
	err = errno;
	if (!ret)
		self->swap_disabled = false;
	ASSERT_EQ(ret, 0) {
		TH_LOG("swapon(%s) failed: %s", self->swap_dev, strerror(err));
	}
}

TEST_HARNESS_MAIN
