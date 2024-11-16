// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>
#include <alloca.h>
#include "../kselftest.h"

#define OPT_STR "hpvaAsnHtbBS"
#define HELP_STR							\
"Usage: %s [-h] [-p] [-v] [-a] [-A] [-s] [-n] [-H] [-t] [-b] [-S]\n"	\
"-h\tshow this help\n"							\
"Interfaces:\n"								\
"-p\tphysical address page detective interface\n"			\
"-v\tvirtual address page detective  interface\n"			\
"Tests:\n"								\
"-a\ttest anonymous page\n"						\
"-A\ttest anonymous huge page\n"					\
"-s\ttest anonymous shared page\n"					\
"-n\ttest named shared page\n"						\
"-H\ttest HugeTLB shared page\n"					\
"-t\ttest tmpfs page\n"							\
"-b\ttest bad/fail input cases\n"					\
"-S\ttest stack page\n"							\
"If no arguments specified all tests are performed\n"			\

#define FIRST_LINE_PREFIX	"Page Detective: Investigating "
#define LAST_LINE_PREFIX	"Page Detective: Finished investigation of "

#define TMP_FILE		"/tmp/page_detective_test.out"

#define NR_HUGEPAGES		"/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"

#define NANO			1000000000ul
#define MICRO			1000000ul
#define MILLI			1000ul
#define BIT(nr)			(UL(1) << (nr))

#define ARG_INTERFACE_PHYS	BIT(0)
#define ARG_INTERFACE_VIRT	BIT(1)

#define ARG_TEST_ANON		BIT(2)
#define ARG_TEST_ANON_HUGE	BIT(3)
#define ARG_TEST_ANON_SHARED	BIT(4)
#define ARG_TEST_NAMED_SHARED	BIT(5)
#define ARG_TEST_HUGETLB_SHARED	BIT(6)
#define ARG_TEST_TMPFS		BIT(7)
#define ARG_TEST_FAIL_CASES	BIT(8)
#define ARG_TEST_STACK		BIT(9)

#define ARG_DEFAULT		(~0)		/* Run verything by default */

#define ARG_INTERFACE_MASK	(ARG_INTERFACE_PHYS | ARG_INTERFACE_VIRT)
#define ARG_TEST_MASK		(~ARG_INTERFACE_MASK)

#define INTERFACE_NAME(in) (((in) == ARG_INTERFACE_PHYS) ?		\
	"/sys/kernel/debug/page_detective/phys" :			\
	"/sys/kernel/debug/page_detective/virt")

#define PAGE_SIZE		((unsigned long)sysconf(_SC_PAGESIZE))
#define HUGE_PAGE_SIZE		(PAGE_SIZE * PAGE_SIZE / sizeof(uint64_t))

#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB		(21 << MAP_HUGE_SHIFT)
#endif

#ifndef MFD_HUGEPAGE
#define MFD_HUGEPAGE (MFD_GOOGLE_SPECIFIC_BASE << 0)
#endif

#ifndef MFD_GOOGLE_SPECIFIC_BASE
#define MFD_GOOGLE_SPECIFIC_BASE 0x0200U
#endif

static int old_dmesg;

static uint64_t virt_to_phys(uint64_t virt, uint64_t *physp)
{
	uint64_t tbloff, offset, tblen, pfn;
	int fd, nr;

	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0) {
		ksft_print_msg("%s open(/proc/self/pagemap): %s\n", __func__,
			       strerror(errno));
		return 1;
	}

	/* see: Documentation/admin-guide/mm/pagemap.rst */
	tbloff = virt / PAGE_SIZE * sizeof(uint64_t);
	offset = lseek(fd, tbloff, SEEK_SET);
	if (offset == (off_t)-1) {
		ksft_print_msg("%s lseek: %s\n", __func__, strerror(errno));
		return 1;
	}

	if (offset != tbloff) {
		ksft_print_msg("%s: cannot find virtual address\n", __func__);
		return 1;
	}

	nr = read(fd, &tblen, sizeof(uint64_t));
	if (nr != sizeof(uint64_t)) {
		ksft_print_msg("%s: read\n", __func__);
		return 1;
	}
	close(fd);

	if (!(tblen & (1ul << 63))) {
		ksft_print_msg("%s: present bit is not set\n", __func__);
		return 1;
	}

	/* Bits 0-54  page frame number (PFN) if present */
	pfn = tblen & 0x7fffffffffffffULL;

	*physp =  PAGE_SIZE * pfn | (virt & (PAGE_SIZE - 1));

	return 0;
}

static int __phys_test(unsigned long long phys)
{
	char phys_str[128];
	int fd, nr;

	fd = open("/sys/kernel/debug/page_detective/phys", O_WRONLY);
	if (fd < 0) {
		ksft_print_msg("%s open: %s\n", __func__, strerror(errno));
		return 4;
	}

	sprintf(phys_str, "%llu", phys);

	nr = write(fd, phys_str, strlen(phys_str));
	if (nr != strlen(phys_str)) {
		ksft_print_msg("%s write failed\n", __func__);
		return 1;
	}
	close(fd);

	return 0;
}

static int phys_test(char *mem)
{
	uint64_t phys;

	if (virt_to_phys((uint64_t)mem, &phys))
		return 1;

	return __phys_test(phys);
}

static int __virt_test(int pid, unsigned long virt)
{
	char virt_str[128];
	int fd, nr;

	fd = open("/sys/kernel/debug/page_detective/virt", O_WRONLY);
	if (fd < 0) {
		ksft_print_msg("%s open: %s\n", __func__, strerror(errno));
		return 4;
	}

	sprintf(virt_str, "%d %#lx", pid, virt);
	nr = write(fd, virt_str, strlen(virt_str));
	if (nr != strlen(virt_str)) {
		ksft_print_msg("%s: write(%s): %s\n", __func__, virt_str,
			       strerror(errno));
		close(fd);

		return 1;
	}
	close(fd);

	return 0;
}

static int virt_test(char *mem)
{
	return __virt_test(getpid(), (unsigned long)mem);
}

static int test_anon(int in)
{
	char *mem;
	int rv;

	mem = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
		   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED) {
		ksft_print_msg("%s mmap: %s\n", __func__, strerror(errno));
		return 1;
	}

	if (madvise(mem, PAGE_SIZE, MADV_NOHUGEPAGE)) {
		ksft_print_msg("%s madvise: %s\n", __func__, strerror(errno));
		return 1;
	}

	mem[0] = 0;

	if (in == ARG_INTERFACE_PHYS)
		rv = phys_test(mem);
	else
		rv = virt_test(mem);

	munmap(mem, PAGE_SIZE);

	return rv;
}

static int test_anon_huge(int in)
{
	uint64_t i;
	char *mem;
	int rv;

	mem = mmap(NULL, HUGE_PAGE_SIZE * 8, PROT_READ | PROT_WRITE,
		   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED) {
		ksft_print_msg("%s mmap: %s\n", __func__, strerror(errno));
		return 1;
	}

	if (madvise(mem, HUGE_PAGE_SIZE * 8, MADV_HUGEPAGE)) {
		ksft_print_msg("%s madvise: %s\n", __func__, strerror(errno));
		return 1;
	}

	/* Fault huge pages */
	for (i = 0; i < HUGE_PAGE_SIZE * 8; i += HUGE_PAGE_SIZE)
		mem[i] = 0;

	/* In case huge pages were not used for some reason */
	mem[HUGE_PAGE_SIZE * 7 + 101 * PAGE_SIZE] = 0;

	if (in == ARG_INTERFACE_PHYS)
		rv = phys_test(mem + HUGE_PAGE_SIZE * 7 + 101 * PAGE_SIZE);
	else
		rv = virt_test(mem + HUGE_PAGE_SIZE * 7 + 101 * PAGE_SIZE);

	munmap(mem, HUGE_PAGE_SIZE * 8);

	return rv;
}

static int test_anon_shared(int in)
{
	char *mem;
	int rv;

	mem = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
		   MAP_ANONYMOUS | MAP_SHARED, -1, 0);
	if (mem == MAP_FAILED) {
		ksft_print_msg("%s mmap: %s\n", __func__, strerror(errno));
		return 1;
	}

	if (madvise(mem, PAGE_SIZE, MADV_NOHUGEPAGE)) {
		ksft_print_msg("%s madvise: %s\n", __func__, strerror(errno));
		return 1;
	}

	mem[0] = 0;

	if (in == ARG_INTERFACE_PHYS)
		rv = phys_test(mem);
	else
		rv = virt_test(mem);

	munmap(mem, PAGE_SIZE);

	return rv;
}

static int test_named_shared(int in)
{
	char *mem;
	int rv;

	mem = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
		   MAP_ANONYMOUS | MAP_SHARED, -1, 0);
	if (mem == MAP_FAILED) {
		ksft_print_msg("%s mmap: %s\n", __func__, strerror(errno));
		return 1;
	}

	if (madvise(mem, PAGE_SIZE, MADV_NOHUGEPAGE)) {
		ksft_print_msg("%s madvise: %s\n", __func__, strerror(errno));
		return 1;
	}

	mem[0] = 0;

	if (in == ARG_INTERFACE_PHYS)
		rv = phys_test(mem);
	else
		rv = virt_test(mem);

	munmap(mem, PAGE_SIZE);

	return rv;
}

static int test_hugetlb_shared(int in)
{
	char hugepg_add_cmd[256], hugepg_rm_cmd[256];
	unsigned long nr_hugepages;
	char *mem;
	FILE *f;
	int rv;

	f = fopen(NR_HUGEPAGES, "r");
	if (!f) {
		ksft_print_msg("%s fopen: %s\n", __func__, strerror(errno));
		return 4;
	}

	fscanf(f, "%lu", &nr_hugepages);
	fclose(f);
	sprintf(hugepg_add_cmd, "echo %lu > " NR_HUGEPAGES, nr_hugepages + 1);
	sprintf(hugepg_rm_cmd, "echo %lu > " NR_HUGEPAGES, nr_hugepages);

	if (system(hugepg_add_cmd)) {
		ksft_print_msg("%s system(hugepg_add_cmd): %s\n", __func__,
			       strerror(errno));
		return 4;
	}

	mem = mmap(NULL, HUGE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		   MAP_HUGETLB | MAP_HUGE_2MB | MAP_ANONYMOUS | MAP_SHARED,
		   -1, 0);
	if (mem == MAP_FAILED) {
		ksft_print_msg("%s mmap: %s\n", __func__, strerror(errno));
		return 1;
	}

	mem[0] = 0;

	if (in == ARG_INTERFACE_PHYS)
		rv = phys_test(mem);
	else
		rv = virt_test(mem);

	munmap(mem, HUGE_PAGE_SIZE);

	if (system(hugepg_rm_cmd)) {
		ksft_print_msg("%s system(hugepg_rm_cmd): %s\n", __func__,
			       strerror(errno));
		return 1;
	}

	return rv;
}

static int test_tmpfs(int in)
{
	char *mem;
	int fd;
	int rv;

	fd = memfd_create("tmpfs_page",
			  MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0) {
		ksft_print_msg("%s memfd_create: %s\n", __func__,
			       strerror(errno));
		return 1;
	}

	if (ftruncate(fd, PAGE_SIZE) == -1) {
		ksft_print_msg("%s ftruncate: %s\n", __func__, strerror(errno));
		return 1;
	}

	mem = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
		   MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED) {
		ksft_print_msg("%s mmap: %s\n", __func__, strerror(errno));
		return 1;
	}

	mem[0] = 0;

	if (in == ARG_INTERFACE_PHYS)
		rv = phys_test(mem);
	else
		rv = virt_test(mem);

	munmap(mem, PAGE_SIZE);
	close(fd);

	return rv;
}

static int test_stack(int in)
{
	char *mem = alloca(PAGE_SIZE);
	int rv;

	mem[0] = 0;
	if (in == ARG_INTERFACE_PHYS)
		rv = phys_test(mem);
	else
		rv = virt_test(mem);

	return rv;
}

static int test_bad_phys(void)
{
	int rv;

	rv = __phys_test(0);
	if (rv)
		return rv;

	rv = __phys_test(1);
	if (rv)
		return rv;

	rv = __phys_test(~0);

	return rv;
}

static int test_bad_virt(void)
{
	int rv;

	rv = __virt_test(0, 0);
	if (rv == 4)
		return rv;

	if (!rv) {
		ksft_print_msg("%s: write(0, 0) did not fail\n", __func__);
		return 1;
	}

	if (!__virt_test(0, -1)) {
		ksft_print_msg("%s: write(0, -1) did not fail\n", __func__);
		return 1;
	}

	if (!__virt_test(0, -1)) {
		ksft_print_msg("%s: write(0, -1) did not fail\n", __func__);
		return 1;
	}

	if (!__virt_test(-1, 0)) {
		ksft_print_msg("%s: write(-1, 0) did not fail\n", __func__);
		return 1;
	}

	return 0;
}

static int test_fail_cases(int in)
{
	if (in == ARG_INTERFACE_VIRT)
		return test_bad_virt();
	else
		return test_bad_phys();
}

/* Return time in nanosecond since epoch */
static uint64_t gethrtime(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);

	return (ts.tv_sec * NANO) + ts.tv_nsec;
}

static int sanity_check_fail_cases(int in, int test_ttpe, FILE *f)
{
	char *line;
	size_t len;
	ssize_t nr;

	line = NULL;
	len = 0;
	while ((nr = getline(&line, &len, f)) != -1) {
		char *l = strchr(line, ']') + 2;	/* skip time stamp */

		if (l == (char *)2)
			continue;

		ksft_print_msg("%s", l);
	}

	free(line);

	return 0;
}

static int sanity_check_test_result(int in, int test_type, uint64_t start)
{
	uint64_t end = gethrtime() + NANO;
	char dmesg[256], *line;
	int first_line, last_line;
	size_t len;
	ssize_t nr;
	FILE *f;

	sprintf(dmesg, "dmesg --since=@%ld.%06ld --until=@%ld.%06ld > "
		TMP_FILE, start / NANO, (start / MILLI) % MICRO, end / NANO,
		(end / MILLI) % MICRO);

	if (old_dmesg)
		system("dmesg > " TMP_FILE);
	else
		system(dmesg);
	f = fopen(TMP_FILE, "r");
	if (!f) {
		ksft_print_msg("%s: %s", __func__, strerror(errno));
		return 1;
	}

	if (test_type == ARG_TEST_FAIL_CASES) {
		sanity_check_fail_cases(in, test_type, f);
		fclose(f);
		unlink(TMP_FILE);
		return 0;
	}

	line = NULL;
	len = 0;
	first_line = 0;
	last_line = 0;
	while ((nr = getline(&line, &len, f)) != -1) {
		char *l = strchr(line, ']') + 2;	/* skip time stamp */

		if (l == (char *)2)
			continue;

		if (!first_line) {
			first_line = !strncmp(l, FIRST_LINE_PREFIX,
					     strlen(FIRST_LINE_PREFIX));
		} else if (!last_line) {
			last_line = !strncmp(l, LAST_LINE_PREFIX,
					     strlen(LAST_LINE_PREFIX));
			if (last_line)
				ksft_print_msg("%s", l);
		}

		/*
		 * Output everything between the first and last line of page
		 * detective output
		 */
		if (first_line && !last_line)
			ksft_print_msg("%s", l);
	}
	ksft_print_msg("\n");

	free(line);
	fclose(f);
	unlink(TMP_FILE);

	if (!first_line) {
		ksft_print_msg("Test failed, bad first line\n");
		return 1;
	}
	if (!last_line) {
		ksft_print_msg("Test failed, bad last line\n");
		return 1;
	}

	return 0;
}

/*
 * interface	ARG_INTERFACE_VIRT or ARG_INTERFACE_PHYS
 * test_arg	one of the ARG_TEST_*
 * test_f	function for this test
 * arg		the provided user arguments.
 *
 * Run the test using the provided interface if it is requested interface arg.
 */
#define TEST(interface, test_type, test_f, arg)					\
	do {									\
		int __in = (interface);						\
		int __tt = (test_type);						\
		int __rv;							\
										\
		if ((arg) & __tt) {						\
			uint64_t start = gethrtime();				\
										\
			if (old_dmesg)						\
				system("dmesg -C");				\
			else							\
				usleep(100000);					\
										\
			__rv = test_f(__in);					\
			if (__rv == 4) {					\
				ksft_test_result_skip(#test_f " via [%s]\n",	\
						      INTERFACE_NAME(__in));	\
				break;						\
			}							\
										\
			if (__rv) {						\
				ksft_test_result_fail(#test_f " via [%s]\n",	\
						      INTERFACE_NAME(__in));	\
				break;						\
			}							\
										\
			if (sanity_check_test_result(__in, __tt,		\
						     start)) {			\
				ksft_test_result_fail(#test_f " via [%s]\n",	\
						      INTERFACE_NAME(__in));	\
				break;						\
			}							\
			ksft_test_result_pass(#test_f " via [%s]\n",		\
					      INTERFACE_NAME(__in));		\
		}								\
	} while (0)

static void run_tests(int in, int arg)
{
	if (in & arg) {
		TEST(in, ARG_TEST_ANON, test_anon, arg);
		TEST(in, ARG_TEST_ANON_HUGE, test_anon_huge, arg);
		TEST(in, ARG_TEST_ANON_SHARED, test_anon_shared, arg);
		TEST(in, ARG_TEST_NAMED_SHARED, test_named_shared, arg);
		TEST(in, ARG_TEST_HUGETLB_SHARED, test_hugetlb_shared, arg);
		TEST(in, ARG_TEST_TMPFS, test_tmpfs, arg);
		TEST(in, ARG_TEST_STACK, test_stack, arg);
		TEST(in, ARG_TEST_FAIL_CASES, test_fail_cases, arg);
	}
}

static int count_tests(int in, int arg)
{
	int tests = 0;

	if (in & arg) {
		tests += (arg & ARG_TEST_ANON) ? 1 : 0;
		tests += (arg & ARG_TEST_ANON_HUGE) ? 1 : 0;
		tests += (arg & ARG_TEST_ANON_SHARED) ? 1 : 0;
		tests += (arg & ARG_TEST_NAMED_SHARED) ? 1 : 0;
		tests += (arg & ARG_TEST_HUGETLB_SHARED) ? 1 : 0;
		tests += (arg & ARG_TEST_TMPFS) ? 1 : 0;
		tests += (arg & ARG_TEST_STACK) ? 1 : 0;
		tests += (arg & ARG_TEST_FAIL_CASES) ? 1 : 0;
	}

	return tests;
}

int main(int argc, char **argv)
{
	int arg = 0;
	int opt;

	while ((opt = getopt(argc, argv, OPT_STR)) != -1) {
		switch (opt) {
		case 'h':
			printf(HELP_STR, argv[0]);
			exit(EXIT_SUCCESS);
		case 'v':
			arg |= ARG_INTERFACE_VIRT;
			break;
		case 'p':
			arg |= ARG_INTERFACE_PHYS;
			break;
		case 'a':
			arg |= ARG_TEST_ANON;
			break;
		case 'A':
			arg |= ARG_TEST_ANON_HUGE;
			break;
		case 's':
			arg |= ARG_TEST_ANON_SHARED;
			break;
		case 'n':
			arg |= ARG_TEST_NAMED_SHARED;
			break;
		case 'H':
			arg |= ARG_TEST_HUGETLB_SHARED;
			break;
		case 't':
			arg |= ARG_TEST_TMPFS;
			break;
		case 'S':
			arg |= ARG_TEST_STACK;
			break;
		case 'b':
			arg |= ARG_TEST_FAIL_CASES;
			break;
		default:
			errx(EXIT_FAILURE, HELP_STR, argv[0]);
		}
	}

	if (arg == 0)
		arg = ARG_DEFAULT;
	if (!(arg & ARG_INTERFACE_MASK))
		errx(EXIT_FAILURE, "No page detective interface specified");

	if (!(arg & ARG_TEST_MASK))
		errx(EXIT_FAILURE, "No tests specified");

	/* Return 1 when dmesg does not have --since and --until arguments */
	old_dmesg = system("dmesg --since @0 > /dev/null 2>&1");

	ksft_print_header();
	ksft_set_plan(count_tests(ARG_INTERFACE_VIRT, arg)
		      + count_tests(ARG_INTERFACE_PHYS, arg));

	run_tests(ARG_INTERFACE_VIRT, arg);
	run_tests(ARG_INTERFACE_PHYS, arg);
	ksft_finished();
}
