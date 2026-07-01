// SPDX-License-Identifier: GPL-2.0-only
#include <limits.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wordexp.h>

#include <linux/iommufd.h>
#include <linux/limits.h>
#include <linux/memfd.h>
#include <linux/mman.h>
#include <linux/sizes.h>
#include <linux/time64.h>
#include <linux/vfio.h>

#include <libvfio.h>

#include "kselftest_harness.h"

static const char *device_bdf;

struct test_params {
	u64 size;
	int mmap_flags;
};

struct test_params test_params;

struct iommu_mapping {
	u64 pgd;
	u64 p4d;
	u64 pud;
	u64 pmd;
	u64 pte;
};

static void timer_start(struct timespec *start) {
	clock_gettime(CLOCK_MONOTONIC, start);
}

static double timer_elapsed_ms(struct timespec start)
{
	struct timespec end;

	clock_gettime(CLOCK_MONOTONIC, &end);

	return (double)(end.tv_sec - start.tv_sec) * MSEC_PER_SEC +
	       (double)(end.tv_nsec - start.tv_nsec) / NSEC_PER_MSEC;
}

FIXTURE(vfio_dma_mapping_perf_test) {
	struct iommu *iommu;
	struct vfio_pci_device *device;
	struct iova_allocator *iova_allocator;
};

FIXTURE_VARIANT(vfio_dma_mapping_perf_test) {
	const char *iommu_mode;
	int mmap_flags;
};

#define FIXTURE_VARIANT_ADD_IOMMU_MODE(_iommu_mode, _name, _mmap_flags)	       \
FIXTURE_VARIANT_ADD(vfio_dma_mapping_perf_test, _iommu_mode ## _ ## _name) {   \
	.iommu_mode = #_iommu_mode,					       \
	.mmap_flags = MAP_ANONYMOUS | MAP_PRIVATE | (_mmap_flags),	       \
}

FIXTURE_VARIANT_ADD_ALL_IOMMU_MODES(anonymous, 0);
FIXTURE_VARIANT_ADD_ALL_IOMMU_MODES(anonymous_hugetlb_2mb, MAP_HUGETLB | MAP_HUGE_2MB);
FIXTURE_VARIANT_ADD_ALL_IOMMU_MODES(anonymous_hugetlb_1gb, MAP_HUGETLB | MAP_HUGE_1GB);

#undef FIXTURE_VARIANT_ADD_IOMMU_MODE

FIXTURE_SETUP(vfio_dma_mapping_perf_test)
{
	self->iommu = iommu_init(variant->iommu_mode);
	self->device = vfio_pci_device_init(device_bdf, self->iommu);
	self->iova_allocator = iova_allocator_init(self->iommu);
}

FIXTURE_TEARDOWN(vfio_dma_mapping_perf_test)
{
	iova_allocator_cleanup(self->iova_allocator);
	vfio_pci_device_cleanup(self->device);
	iommu_cleanup(self->iommu);
}

TEST_F(vfio_dma_mapping_perf_test, dma_map_unmap)
{
	const u64 size = test_params.size;
	const int flags = variant->mmap_flags | test_params.mmap_flags;
	struct dma_region region;
	struct timespec start;
	u64 unmapped;
	int rc;

	timer_start(&start);
	region.vaddr = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0);
	printf("Completed mmap() in %.2lfms\n", timer_elapsed_ms(start));

	/* Skip the test if there aren't enough HugeTLB pages available. */
	if (flags & MAP_HUGETLB && region.vaddr == MAP_FAILED)
		SKIP(return, "mmap() failed: %s (%d)\n", strerror(errno), errno);
	else
		ASSERT_NE(region.vaddr, MAP_FAILED);

	region.iova = iova_allocator_alloc(self->iova_allocator, size);
	region.size = size;

	timer_start(&start);
	iommu_map(self->iommu, &region);
	printf("Mapped HVA %p (size %luG) at IOVA 0x%lx in %.2lfms\n",
	       region.vaddr, size / SZ_1G, region.iova, timer_elapsed_ms(start));
	ASSERT_EQ(region.iova, to_iova(self->device, region.vaddr));

	timer_start(&start);
	rc = __iommu_unmap(self->iommu, &region, &unmapped);
	printf("Unmapped IOVA 0x%lx in %.2lfms\n", region.iova, timer_elapsed_ms(start));
	ASSERT_EQ(rc, 0);
	ASSERT_EQ(unmapped, region.size);

	timer_start(&start);
	ASSERT_TRUE(!munmap(region.vaddr, size));
	printf("Completed munmap() in %.2lfms\n", timer_elapsed_ms(start));
}

FIXTURE(vfio_dma_mapping_perf_memfd_test) {
	struct iommu *iommu;
	struct vfio_pci_device *device;
	struct iova_allocator *iova_allocator;
};

FIXTURE_VARIANT(vfio_dma_mapping_perf_memfd_test) {
	const char *iommu_mode;
	int mmap_flags;
	int memfd_flags;
};

#define FIXTURE_VARIANT_ADD_IOMMU_MODE(_iommu_mode, _name, _mmap_flags, _memfd_flags) \
FIXTURE_VARIANT_ADD(vfio_dma_mapping_perf_memfd_test, _iommu_mode ## _ ## _name) {    \
	.iommu_mode = #_iommu_mode,						      \
	.mmap_flags = MAP_SHARED | (_mmap_flags),				      \
	.memfd_flags = MAP_SHARED | (_memfd_flags),				      \
}

FIXTURE_VARIANT_ADD_ALL_IOMMU_MODES(memfd, 0, 0);
FIXTURE_VARIANT_ADD_ALL_IOMMU_MODES(memfd_hugetlb_2mb,
				    MAP_HUGETLB | MAP_HUGE_2MB,
				    MFD_HUGETLB | MFD_HUGE_2MB);
FIXTURE_VARIANT_ADD_ALL_IOMMU_MODES(memfd_hugetlb_1gb,
				    MAP_HUGETLB | MAP_HUGE_1GB,
				    MFD_HUGETLB | MFD_HUGE_1GB);

#undef FIXTURE_VARIANT_ADD_IOMMU_MODE

FIXTURE_SETUP(vfio_dma_mapping_perf_memfd_test)
{
	self->iommu = iommu_init(variant->iommu_mode);
	self->device = vfio_pci_device_init(device_bdf, self->iommu);
	self->iova_allocator = iova_allocator_init(self->iommu);
}

FIXTURE_TEARDOWN(vfio_dma_mapping_perf_memfd_test)
{
	iova_allocator_cleanup(self->iova_allocator);
	vfio_pci_device_cleanup(self->device);
	iommu_cleanup(self->iommu);
}

static void *setup_memfd(int *fd, u64 size, int mmap_flags, int mfd_flags)
{
	void *buf = MAP_FAILED;
	struct timespec start;

	timer_start(&start);
	*fd = memfd_create("vfio_dma_mapping_perf_memfd_test", mfd_flags);
	printf("Completed memfd_create() in %.2lfms\n", timer_elapsed_ms(start));
	if (*fd <= 0)
		return MAP_FAILED;

	if (ftruncate(*fd, size))
		goto out;

	timer_start(&start);
	buf = mmap(NULL, size, PROT_READ | PROT_WRITE, mmap_flags, *fd, 0);
	printf("Completed mmap() for memfd in %.2lfms\n", timer_elapsed_ms(start));

out:
	if (buf == MAP_FAILED)
		close(*fd);

	return buf;
}

static void teardown_memfd(int fd, u64 size, void *vaddr)
{
	struct timespec start;

	if (vaddr != MAP_FAILED) {
		timer_start(&start);
		munmap(vaddr, size);
		printf("Completed munmap() in %.2lfms\n", timer_elapsed_ms(start));
	}

	if (fd != -1) {
		timer_start(&start);
		close(fd);
		printf("Completed close() in %.2lfms\n", timer_elapsed_ms(start));
	}
}

TEST_F(vfio_dma_mapping_perf_memfd_test, dma_map_unmap_from_file)
{
	const u64 size = test_params.size;
	struct dma_region region;
	struct timespec start;
	u64 unmapped;
	int rc, fd;

	region.vaddr = setup_memfd(&fd, size,
				   variant->mmap_flags | test_params.mmap_flags,
				   variant->memfd_flags);
	ASSERT_NE(region.vaddr, MAP_FAILED);

	region.iova = iova_allocator_alloc(self->iova_allocator, size);
	region.size = size;

	timer_start(&start);
	if (strcmp(variant->iommu_mode, MODE_IOMMUFD) == 0) {
		iommufd_map_file(self->iommu, &region, fd);
	} else {
		iommu_map(self->iommu, &region);
	}
	printf("Mapped HVA %p (size %luG) at IOVA 0x%lx in %.2lfms\n",
	       region.vaddr, size / SZ_1G, region.iova, timer_elapsed_ms(start));
	ASSERT_EQ(region.iova, to_iova(self->device, region.vaddr));

	timer_start(&start);
	rc = __iommu_unmap(self->iommu, &region, &unmapped);
	printf("Unmapped IOVA 0x%lx in %.2lfms\n", region.iova, timer_elapsed_ms(start));
	ASSERT_EQ(rc, 0);
	ASSERT_EQ(unmapped, region.size);

	teardown_memfd(fd, size, region.vaddr);
}

/*
 * Parses "[0-9]+[kmgt]?".
 */
size_t parse_size(const char *size)
{
	size_t base;
	char *scale;
	int shift = 0;

	VFIO_ASSERT_TRUE(size && isdigit(size[0]),
			 "Need at least one digit in '%s'.", size);

	base = strtoull(size, &scale, 0);

	VFIO_ASSERT_TRUE(base != ULLONG_MAX, "Overflow parsing size!");

	switch (tolower(*scale)) {
	case 't':
		shift = 40;
		break;
	case 'g':
		shift = 30;
		break;
	case 'm':
		shift = 20;
		break;
	case 'k':
		shift = 10;
		break;
	case 'b':
	case '\0':
		shift = 0;
		break;
	default:
		VFIO_FAIL("Unknown size letter '%c'.", *scale);
	}

	VFIO_ASSERT_TRUE((base << shift) >> shift == base,
			 "Overflow scaling size!");

	return base << shift;
}

static void help(char *name)
{
	puts("");
	printf("usage: %s [-h|-p] [-b bytes] [-a \"test harness args\"]\n", name);
	puts("");
	printf(" -h: Display this help message.\n"
	       " -b: Specify the size of the DMA region to be mapped\n"
	       "     and unmapped. e.g. 16M or 8G, (default: 1G)\n"
	       " -p: Append 'MAP_POPULATE' to the mmap() flags to avoid\n"
	       "     prefaulting while mapping DMA regions. Instead, any\n"
	       "     and all prefaulting needed will happen during the\n"
	       "     mmap() call. This will make mapping DMA regions\n"
	       "     more consistent.\n"
	       " -a: Args that are forwarded to the test harness,\n"
	       "     e.g. -a \"-t dma_map_unmap_from_file\"\n");
}

struct harness_args
{
	int argc;
	char **argv;
	wordexp_t exp;
};

static void populate_harness_args(struct harness_args *args, const char *argv_0,
				  const char *cmdlne)
{
	if (wordexp(argv_0, &args->exp, WRDE_NOCMD) == 0 &&
	    wordexp(cmdlne, &args->exp, WRDE_APPEND | WRDE_NOCMD) == 0) {
		args->argc = args->exp.we_wordc;
		args->argv = args->exp.we_wordv;
	}
}

static void setup_test(struct harness_args *args, int argc, char *argv[])
{
	int opt;

	test_params = (struct test_params) {
		.size = SZ_1G,
		.mmap_flags = 0,
	};

	while ((opt = getopt(argc, argv, "a:b:ph")) != -1) {
		switch (opt) {
		case 'a':
			populate_harness_args(args, argv[0], optarg);
			break;
		case 'b':
			test_params.size = parse_size(optarg);
			break;
		case 'p':
			test_params.mmap_flags = MAP_POPULATE;
			break;
		case 'h':
		default:
			help(argv[0]);
			goto out;
		}
	}

out:
	// Reset getopt() state to allow the test harness to use it.
	optind = 1; 
}

static void teardown_test(struct harness_args *args)
{
	if (args->argv) {
		args->argc = 0;
		args->argv = NULL;
		wordfree(&args->exp);
	}
}

int main(int argc, char *argv[])
{
	struct harness_args args = (struct harness_args) {
		.argc = 0,
		.argv = NULL,
	};
	int r;

	setup_test(&args, argc, argv);
	device_bdf = vfio_selftests_get_bdf(&argc, argv);
	r = test_harness_run(args.argc, args.argv);
	teardown_test(&args);

	return r;
}
