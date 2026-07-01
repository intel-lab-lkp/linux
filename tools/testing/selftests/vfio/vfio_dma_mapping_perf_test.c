// SPDX-License-Identifier: GPL-2.0-only
#include <limits.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

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
	const u64 size = SZ_1G;
	const int flags = variant->mmap_flags;
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

int main(int argc, char *argv[])
{
	device_bdf = vfio_selftests_get_bdf(&argc, argv);
	return test_harness_run(argc, argv);
}
