// SPDX-License-Identifier: GPL-2.0-only
/*
 * Verify that the slow GUP path (pin_user_pages -> follow_page_mask ->
 * follow_pte_batch) returns the correct pages for a PTE-mapped large folio
 * (mTHP), including that COW copies produce the right content.
 *
 * Uses the CONFIG_GUP_TEST PIN_LONGTERM interface: START pins a range on the
 * slow path, READ copies the pinned pages' bytes back so we can compare them
 * against the pattern we wrote.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <linux/types.h>

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

#define GUP_DEV "/sys/kernel/debug/gup_test"

#define PIN_LONGTERM_TEST_START	_IOW('g', 7, struct pin_longterm_test)
#define PIN_LONGTERM_TEST_STOP	_IO('g', 8)
#define PIN_LONGTERM_TEST_READ	_IOW('g', 9, __u64)
#define USE_WRITE	1
#define USE_FAST	2

struct pin_longterm_test {
	__u64 addr;
	__u64 size;
	__u32 flags;
};

#define ORDER_KB	64
#define NR_FOLIOS	16
#define REGION		((size_t)ORDER_KB * 1024 * NR_FOLIOS)

static long PS;
static int fails;
static int tap;

static void ok(int cond, const char *desc)
{
	printf("%s %d %s\n", cond ? "ok" : "not ok", ++tap, desc);
	if (!cond)
		fails++;
}

/* Deterministic, per-page-distinct pattern so any mis-order or leak shows. */
static void fill(char *base, size_t sz, uint32_t salt)
{
	for (size_t off = 0; off < sz; off += PS) {
		uint32_t k = off / PS;
		uint64_t v = ((uint64_t)salt << 32) ^ (k * 0x9E3779B1u + 0x1234);

		for (size_t i = 0; i < PS; i += sizeof(v))
			memcpy(base + off + i, &v, sizeof(v));
	}
}

static int wsysfs(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);

	if (fd < 0)
		return -1;
	int r = write(fd, val, strlen(val));

	close(fd);
	return r < 0 ? -1 : 0;
}

/* Force sub-PMD 64kB mTHP only, so faults produce PTE-mapped large folios. */
static void setup_mthp(void)
{
	const char *thp = "/sys/kernel/mm/transparent_hugepage";
	char p[256];
	static const int kb[] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };

	wsysfs("/sys/kernel/mm/transparent_hugepage/enabled", "never");
	for (unsigned int i = 0; i < ARRAY_SIZE(kb); i++) {
		snprintf(p, sizeof(p), "%s/hugepages-%dkB/enabled", thp, kb[i]);
		wsysfs(p, kb[i] == ORDER_KB ? "always" : "never");
	}
}

/* Count how many pages sit in a contiguous >=ORDER_KB PFN run (via pagemap). */
static int count_large_pages(char *base, size_t sz)
{
	int pm = open("/proc/self/pagemap", O_RDONLY);
	size_t n = sz / PS, large = 0;
	uint64_t *pfn = calloc(n, sizeof(*pfn));

	if (pm < 0)
		return -1;
	for (size_t k = 0; k < n; k++) {
		uint64_t ent;
		off_t idx = ((uintptr_t)base + k * PS) / PS * sizeof(ent);

		if (pread(pm, &ent, sizeof(ent), idx) != sizeof(ent) ||
		    !(ent & (1ULL << 63)))
			pfn[k] = 0;
		else
			pfn[k] = ent & ((1ULL << 55) - 1);
	}
	close(pm);
	for (size_t k = 0; k < n; k++)
		if (k + 1 < n && pfn[k] && pfn[k + 1] == pfn[k] + 1)
			large++;
	free(pfn);
	return large;
}

/* Pin @base..@sz on the slow path, read the pinned bytes back, compare to exp. */
static int pin_verify(int fd, char *base, size_t sz, uint32_t wr, char *exp)
{
	struct pin_longterm_test a = {
		.addr = (uintptr_t)base, .size = sz,
		.flags = wr ? USE_WRITE : 0,		/* USE_FAST unset => slow */
	};
	char *got = mmap(NULL, sz, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	__u64 ga = (uintptr_t)got;
	int rc = -1;

	if (got == MAP_FAILED)
		return -1;
	if (ioctl(fd, PIN_LONGTERM_TEST_START, &a)) {
		fprintf(stderr, "START(%s) failed: %s\n",
			wr ? "write" : "read", strerror(errno));
		goto out;
	}
	if (ioctl(fd, PIN_LONGTERM_TEST_READ, &ga)) {
		fprintf(stderr, "READ failed: %s\n", strerror(errno));
		ioctl(fd, PIN_LONGTERM_TEST_STOP);
		goto out;
	}
	ioctl(fd, PIN_LONGTERM_TEST_STOP);
	rc = memcmp(got, exp, sz) ? 1 : 0;
out:
	munmap(got, sz);
	return rc;
}

int main(void)
{
	PS = sysconf(_SC_PAGESIZE);
	setup_mthp();

	int fd = open(GUP_DEV, O_RDWR);

	if (fd < 0) {
		fprintf(stderr, "open %s: %s (CONFIG_GUP_TEST?)\n",
			GUP_DEV, strerror(errno));
		return 2;
	}

	char *r = mmap(NULL, REGION, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (r == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	fill(r, REGION, 0xA1);				/* pattern P */
	char *expP = malloc(REGION);

	memcpy(expP, r, REGION);

	int large = count_large_pages(r, REGION);

	printf("# %d/%zu pages in contiguous large-folio runs\n",
	       large, REGION / PS);
	if (large < (int)(REGION / PS) / 4)
		printf("# WARN: little mTHP backing; batch path lightly covered\n");

	/* A: read pin over writable mTHP -> batches -> content must equal P. */
	ok(pin_verify(fd, r, REGION, 0, expP) == 0,
	   "slow read-pin of mTHP returns correct contents");

	/* B: write pin -> FOLL_WRITE batch path -> content must equal P. */
	ok(pin_verify(fd, r, REGION, 1, expP) == 0,
	   "slow write-pin of mTHP returns correct contents");

	/* C: COW isolation. Child write-pins (unshares) then rewrites; parent P. */
	pid_t pid = fork();

	if (pid == 0) {
		int cfd = open(GUP_DEV, O_RDWR);
		int a = pin_verify(cfd, r, REGION, 1, expP);	/* COW copy == P */

		fill(r, REGION, 0xB2);			/* child writes Q */
		_exit(a == 0 ? 0 : 1);
	}
	int st = 0;

	waitpid(pid, &st, 0);
	ok(WIFEXITED(st) && WEXITSTATUS(st) == 0,
	   "child write-pin after COW returns correct (copied) contents");
	ok(memcmp(r, expP, REGION) == 0,
	   "parent contents intact after child COW writes");
	/* D: parent read-pin again after the COW split still correct. */
	ok(pin_verify(fd, r, REGION, 0, expP) == 0,
	   "parent slow read-pin after COW still correct");

	printf("# totals: pass:%d fail:%d\n", tap - fails, fails);
	return fails ? 1 : 0;
}
