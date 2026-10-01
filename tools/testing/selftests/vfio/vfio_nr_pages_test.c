// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/sizes.h>

#include <libvfio.h>

#include "../kselftest_harness.h"

static const char *device_bdf;

static long read_nr_pages(struct iommu *iommu)
{
	char path[256];
	char line[128];
	FILE *f;
	long val = -1;
	int fd = iommu->container_fd != -1 ? iommu->container_fd : iommu->iommufd;

	snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
	f = fopen(path, "r");
	if (!f)
		return -1;

	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "iommu-nr-pages: %ld", &val) == 1)
			break;
	}

	fclose(f);
	return val;
}

FIXTURE(vfio_nr_pages_test) {
	struct iommu *iommu;
	struct vfio_pci_device *device;
};

FIXTURE_VARIANT(vfio_nr_pages_test) {
	const char *iommu_mode;
};

#define FIXTURE_VARIANT_ADD_IOMMU_MODE(_name) \
FIXTURE_VARIANT_ADD(vfio_nr_pages_test, _name) { \
	.iommu_mode = #_name, \
}

FIXTURE_VARIANT_ADD_ALL_IOMMU_MODES();

FIXTURE_SETUP(vfio_nr_pages_test)
{
	self->iommu = iommu_init(variant->iommu_mode);
	if (!self->iommu)
		SKIP(return, "IOMMU mode %s not supported", variant->iommu_mode);

	self->device = vfio_pci_device_init(device_bdf, self->iommu);
	if (!self->device) {
		iommu_cleanup(self->iommu);
		SKIP(return, "Failed to initialize VFIO device");
	}
}

FIXTURE_TEARDOWN(vfio_nr_pages_test)
{
	if (self->device)
		vfio_pci_device_cleanup(self->device);
	if (self->iommu)
		iommu_cleanup(self->iommu);
}

TEST_F(vfio_nr_pages_test, sparse_mapping_sticky_reclamation)
{
	long nr_pages_initial, nr_pages_mapped, nr_pages_final;
	struct dma_region region1 = {0};
	struct dma_region region2 = {0};

	if (!self->iommu || !self->device)
		SKIP(return, "Fixture setup failed");

	nr_pages_initial = read_nr_pages(self->iommu);
	if (nr_pages_initial < 0)
		SKIP(return, "iommu-nr-pages not reported by this IOMMU");

	/* Map a page at start */
	region1.size = SZ_4K;
	region1.vaddr = mmap_reserve(region1.size, region1.size, 0);
	ASSERT_NE(MAP_FAILED, region1.vaddr);
	region1.iova = 0x100000;
	iommu_map(self->iommu, &region1);

	/* Map another page far away (1GB stride) to force deep page tables */
	region2.size = SZ_4K;
	region2.vaddr = mmap_reserve(region2.size, region2.size, 0);
	ASSERT_NE(MAP_FAILED, region2.vaddr);
	region2.iova = 0x100000 + SZ_1G;
	iommu_map(self->iommu, &region2);

	nr_pages_mapped = read_nr_pages(self->iommu);
	ASSERT_GT(nr_pages_mapped, nr_pages_initial);

	/* Unmap both regions */
	iommu_unmap(self->iommu, &region1);
	iommu_unmap(self->iommu, &region2);

	nr_pages_final = read_nr_pages(self->iommu);

	/* Page tables should remain allocated */
	ASSERT_EQ(nr_pages_final, nr_pages_mapped);

	munmap(region1.vaddr, region1.size);
	munmap(region2.vaddr, region2.size);
}

int main(int argc, char *argv[])
{
	device_bdf = vfio_selftests_get_bdf(&argc, argv);
	return test_harness_run(argc, argv);
}
