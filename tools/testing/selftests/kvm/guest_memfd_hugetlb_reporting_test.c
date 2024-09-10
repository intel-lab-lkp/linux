#include <fcntl.h>
#include <linux/falloc.h>
#include <linux/kvm.h>
#include <linux/limits.h>
#include <linux/memfd.h>
#include <string.h>
#include <sys/mman.h>

#include "kvm_util.h"
#include "test_util.h"
#include "processor.h"

static int read_int(const char *file_name)
{
	FILE *fp;
	int num;

	fp = fopen(file_name, "r");
	TEST_ASSERT(fp != NULL, "Error opening file %s!\n", file_name);

	TEST_ASSERT_EQ(fscanf(fp, "%d", &num), 1);

	fclose(fp);

	return num;
}

enum hugetlb_statistic {
	FREE_HUGEPAGES,
	NR_HUGEPAGES,
	NR_OVERCOMMIT_HUGEPAGES,
	RESV_HUGEPAGES,
	SURPLUS_HUGEPAGES,
	NR_TESTED_HUGETLB_STATISTICS,
};

static const char *hugetlb_statistics[NR_TESTED_HUGETLB_STATISTICS] = {
	[FREE_HUGEPAGES] = "free_hugepages",
	[NR_HUGEPAGES] = "nr_hugepages",
	[NR_OVERCOMMIT_HUGEPAGES] = "nr_overcommit_hugepages",
	[RESV_HUGEPAGES] = "resv_hugepages",
	[SURPLUS_HUGEPAGES] = "surplus_hugepages",
};

enum test_page_size {
	TEST_SZ_2M,
	TEST_SZ_1G,
	NR_TEST_SIZES,
};

struct test_param {
	size_t page_size;
	int memfd_create_flags;
	int guest_memfd_flags;
	char *path_suffix;
};

const struct test_param *test_params(enum test_page_size size)
{
	static const struct test_param params[] = {
		[TEST_SZ_2M] = {
			.page_size = PG_SIZE_2M,
			.memfd_create_flags = MFD_HUGETLB | MFD_HUGE_2MB,
			.guest_memfd_flags = KVM_GUEST_MEMFD_HUGETLB | KVM_GUEST_MEMFD_HUGE_2MB,
			.path_suffix = "2048kB",
		},
		[TEST_SZ_1G] = {
			.page_size = PG_SIZE_1G,
			.memfd_create_flags = MFD_HUGETLB | MFD_HUGE_1GB,
			.guest_memfd_flags = KVM_GUEST_MEMFD_HUGETLB | KVM_GUEST_MEMFD_HUGE_1GB,
			.path_suffix = "1048576kB",
		},
	};

	return &params[size];
}

static int read_statistic(enum test_page_size size, enum hugetlb_statistic statistic)
{
	char path[PATH_MAX] = "/sys/kernel/mm/hugepages/hugepages-";

	strcat(path, test_params(size)->path_suffix);
	strcat(path, "/");
	strcat(path, hugetlb_statistics[statistic]);

	return read_int(path);
}

static int baseline[NR_TEST_SIZES][NR_TESTED_HUGETLB_STATISTICS];

static void establish_baseline(void)
{
	int i, j;

	for (i = 0; i < NR_TEST_SIZES; ++i)
		for (j = 0; j < NR_TESTED_HUGETLB_STATISTICS; ++j)
			baseline[i][j] = read_statistic(i, j);
}

static void assert_stats_at_baseline(void)
{
	TEST_ASSERT_EQ(read_statistic(TEST_SZ_2M, FREE_HUGEPAGES),
		       baseline[TEST_SZ_2M][FREE_HUGEPAGES]);
	TEST_ASSERT_EQ(read_statistic(TEST_SZ_2M, NR_HUGEPAGES),
		       baseline[TEST_SZ_2M][NR_HUGEPAGES]);
	TEST_ASSERT_EQ(read_statistic(TEST_SZ_2M, NR_OVERCOMMIT_HUGEPAGES),
		       baseline[TEST_SZ_2M][NR_OVERCOMMIT_HUGEPAGES]);
	TEST_ASSERT_EQ(read_statistic(TEST_SZ_2M, RESV_HUGEPAGES),
		       baseline[TEST_SZ_2M][RESV_HUGEPAGES]);
	TEST_ASSERT_EQ(read_statistic(TEST_SZ_2M, SURPLUS_HUGEPAGES),
		       baseline[TEST_SZ_2M][SURPLUS_HUGEPAGES]);

	TEST_ASSERT_EQ(read_statistic(TEST_SZ_1G, FREE_HUGEPAGES),
		       baseline[TEST_SZ_1G][FREE_HUGEPAGES]);
	TEST_ASSERT_EQ(read_statistic(TEST_SZ_1G, NR_HUGEPAGES),
		       baseline[TEST_SZ_1G][NR_HUGEPAGES]);
	TEST_ASSERT_EQ(read_statistic(TEST_SZ_1G, NR_OVERCOMMIT_HUGEPAGES),
		       baseline[TEST_SZ_1G][NR_OVERCOMMIT_HUGEPAGES]);
	TEST_ASSERT_EQ(read_statistic(TEST_SZ_1G, RESV_HUGEPAGES),
		       baseline[TEST_SZ_1G][RESV_HUGEPAGES]);
	TEST_ASSERT_EQ(read_statistic(TEST_SZ_1G, SURPLUS_HUGEPAGES),
		       baseline[TEST_SZ_1G][SURPLUS_HUGEPAGES]);
}

static void assert_stats(enum test_page_size size, int num_reserved, int num_faulted)
{
	TEST_ASSERT_EQ(read_statistic(size, FREE_HUGEPAGES),
		       baseline[size][FREE_HUGEPAGES] - num_faulted);
	TEST_ASSERT_EQ(read_statistic(size, NR_HUGEPAGES),
		       baseline[size][NR_HUGEPAGES]);
	TEST_ASSERT_EQ(read_statistic(size, NR_OVERCOMMIT_HUGEPAGES),
		       baseline[size][NR_OVERCOMMIT_HUGEPAGES]);
	TEST_ASSERT_EQ(read_statistic(size, RESV_HUGEPAGES),
		       baseline[size][RESV_HUGEPAGES] + num_reserved - num_faulted);
	TEST_ASSERT_EQ(read_statistic(size, SURPLUS_HUGEPAGES),
		       baseline[size][SURPLUS_HUGEPAGES]);
}

/* Use hugetlb behavior as a baseline. guest_memfd should have comparable behavior. */
static void test_hugetlb_behavior(enum test_page_size test_size)
{
	const struct test_param *param;
	char *mem;
	int memfd;

	param = test_params(test_size);

	assert_stats_at_baseline();

	memfd = memfd_create("guest_memfd_hugetlb_reporting_test",
			     param->memfd_create_flags);

	mem = mmap(NULL, param->page_size, PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_HUGETLB, memfd, 0);
	TEST_ASSERT(mem != MAP_FAILED, "Couldn't mmap()");

	assert_stats(test_size, 1, 0);

	*mem = 'A';

	assert_stats(test_size, 1, 1);

	munmap(mem, param->page_size);

	assert_stats(test_size, 1, 1);

	madvise(mem, param->page_size, MADV_DONTNEED);

	assert_stats(test_size, 1, 1);

	madvise(mem, param->page_size, MADV_REMOVE);

	assert_stats(test_size, 1, 1);

	close(memfd);

	assert_stats_at_baseline();
}

static void test_guest_memfd_behavior(enum test_page_size test_size)
{
	const struct test_param *param;
	struct kvm_vm *vm;
	int guest_memfd;

	param = test_params(test_size);

	assert_stats_at_baseline();

	vm = vm_create_barebones_type(KVM_X86_SW_PROTECTED_VM);

	guest_memfd = vm_create_guest_memfd(vm, param->page_size,
					    param->guest_memfd_flags);

	assert_stats(test_size, 1, 0);

	fallocate(guest_memfd, FALLOC_FL_KEEP_SIZE, 0, param->page_size);

	assert_stats(test_size, 1, 1);

	fallocate(guest_memfd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE, 0,
		  param->page_size);

	assert_stats(test_size, 1, 0);

	close(guest_memfd);

	assert_stats_at_baseline();

	kvm_vm_free(vm);
}

int main(int argc, char *argv[])
{
	establish_baseline();

	test_hugetlb_behavior(TEST_SZ_2M);
	test_hugetlb_behavior(TEST_SZ_1G);

	test_guest_memfd_behavior(TEST_SZ_2M);
	test_guest_memfd_behavior(TEST_SZ_1G);
}
