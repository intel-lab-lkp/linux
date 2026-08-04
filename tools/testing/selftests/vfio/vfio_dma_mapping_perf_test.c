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

FIXTURE(vfio_dma_mapping_perf_test) {
	struct iommu *iommu;
	struct vfio_pci_device *device;
	struct iova_allocator *iova_allocator;
};

FIXTURE_VARIANT(vfio_dma_mapping_perf_test) {
	const char *iommu_mode;
	int mmap_flags;
};

#define FIXTURE_VARIANT_ADD_IOMMU_MODE(_iommu_mode, _name, _mmap_flags)		  \
FIXTURE_VARIANT_ADD(vfio_dma_mapping_perf_test, _iommu_mode ## _ ## _name) {	  \
	.iommu_mode = #_iommu_mode,						  \
	.mmap_flags = MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE | (_mmap_flags), \
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

	printf("mmap size = %lluG\n", (unsigned long long)(size / SZ_1G));

	TIME("mmap",
	     region.vaddr = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0));

	/* Skip the test if there aren't enough HugeTLB pages available. */
	if (flags & MAP_HUGETLB && region.vaddr == MAP_FAILED)
		SKIP(return, "mmap() failed: %s (%d)\n", strerror(errno), errno);
	else
		ASSERT_NE(region.vaddr, MAP_FAILED);

	region.iova = iova_allocator_alloc(self->iova_allocator, size);
	region.size = size;

	TIME("IOMMU map", iommu_map(self->iommu, &region));
	ASSERT_EQ(region.iova, to_iova(self->device, region.vaddr));

	TIME("IOMMU unmap", iommu_unmap(self->iommu, &region));
	TIME("munmap", ASSERT_EQ(0, munmap(region.vaddr, size)));
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

#define FIXTURE_VARIANT_ADD_MEMFD_MODE(_name, _mmap_flags, _memfd_flags)      \
FIXTURE_VARIANT_ADD(vfio_dma_mapping_perf_memfd_test, iommufd ## _ ## _name) {\
	.iommu_mode = MODE_IOMMUFD,					      \
	.mmap_flags = MAP_SHARED | MAP_POPULATE | (_mmap_flags),	      \
	.memfd_flags = (_memfd_flags),					      \
}

FIXTURE_VARIANT_ADD_MEMFD_MODE(memfd, 0, 0);
FIXTURE_VARIANT_ADD_MEMFD_MODE(memfd_hugetlb_2mb,
				    MAP_HUGETLB | MAP_HUGE_2MB,
				    MFD_HUGETLB | MFD_HUGE_2MB);
FIXTURE_VARIANT_ADD_MEMFD_MODE(memfd_hugetlb_1gb,
				    MAP_HUGETLB | MAP_HUGE_1GB,
				    MFD_HUGETLB | MFD_HUGE_1GB);

#undef FIXTURE_VARIANT_ADD_MEMFD_MODE

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

	TIME("memfd_create",
	     *fd = memfd_create("vfio_dma_mapping_perf_memfd_test", mfd_flags));
	if (*fd < 0)
		return MAP_FAILED;

	if (ftruncate(*fd, size))
		goto out;

	TIME("mmap",
	     buf = mmap(NULL, size, PROT_READ | PROT_WRITE, mmap_flags, *fd, 0));

out:
	if (buf == MAP_FAILED)
		close(*fd);

	return buf;
}

static void teardown_memfd(int fd, u64 size, void *vaddr)
{
	if (vaddr != MAP_FAILED)
		TIME("munmap", VFIO_ASSERT_EQ(0, munmap(vaddr, size)));

	if (fd != -1)
		TIME("close", VFIO_ASSERT_EQ(0, close(fd)));
}

TEST_F(vfio_dma_mapping_perf_memfd_test, dma_map_unmap_from_file)
{
	const u64 size = SZ_1G;
	const int flags = variant->mmap_flags;
	struct dma_region region;
	int fd;

	printf("mmap size = %lluG\n", (unsigned long long)(size / SZ_1G));

	region.vaddr = setup_memfd(&fd, size, variant->mmap_flags, variant->memfd_flags);

	/* Skip the test if there aren't enough HugeTLB pages available. */
	if (flags & MAP_HUGETLB && region.vaddr == MAP_FAILED)
		SKIP(return, "setup_memfd() failed: %s (%d)\n", strerror(errno), errno);
	else
		ASSERT_NE(region.vaddr, MAP_FAILED);

	region.iova = iova_allocator_alloc(self->iova_allocator, size);
	region.size = size;

	TIME("IOMMU map", iommufd_map_file(self->iommu, &region, fd));
	ASSERT_EQ(region.iova, to_iova(self->device, region.vaddr));

	TIME("IOMMU unmap", iommu_unmap(self->iommu, &region));

	teardown_memfd(fd, size, region.vaddr);
}

int main(int argc, char *argv[])
{
	device_bdf = vfio_selftests_get_bdf(&argc, argv);
	return test_harness_run(argc, argv);
}
