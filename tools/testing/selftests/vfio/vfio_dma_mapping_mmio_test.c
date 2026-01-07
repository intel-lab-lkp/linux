// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include <uapi/linux/types.h>
#include <linux/pci_regs.h>
#include <linux/sizes.h>
#include <linux/vfio.h>

#include <libvfio.h>

#include "../kselftest_harness.h"

static const char *device_bdf;

static int largest_mapped_bar(struct vfio_pci_device *device)
{
	int bar_idx = -1;
	u64 bar_size = 0;

	for (int i = 0; i < PCI_STD_NUM_BARS; i++) {
		struct vfio_pci_bar *bar = &device->bars[i];

		if (!bar->vaddr)
			continue;

		if (!(bar->info.flags & VFIO_REGION_INFO_FLAG_WRITE))
			continue;

		if (bar->info.size > bar_size) {
			bar_size = bar->info.size;
			bar_idx = i;
		}
	}

	return bar_idx;
}

FIXTURE(vfio_dma_mapping_mmio_test) {
	struct iommu *iommu;
	struct vfio_pci_device *device;
	struct iova_allocator *iova_allocator;
	int bar_idx;
};

FIXTURE_VARIANT(vfio_dma_mapping_mmio_test) {
	const char *iommu_mode;
};

#define FIXTURE_VARIANT_ADD_IOMMU_MODE(_iommu_mode)			       \
FIXTURE_VARIANT_ADD(vfio_dma_mapping_mmio_test, _iommu_mode) {	       \
	.iommu_mode = #_iommu_mode,					       \
}

FIXTURE_VARIANT_ADD_IOMMU_MODE(vfio_type1_iommu);
FIXTURE_VARIANT_ADD_IOMMU_MODE(vfio_type1v2_iommu);

#undef FIXTURE_VARIANT_ADD_IOMMU_MODE

FIXTURE_SETUP(vfio_dma_mapping_mmio_test)
{
	self->iommu = iommu_init(variant->iommu_mode);
	self->device = vfio_pci_device_init(device_bdf, self->iommu);
	self->iova_allocator = iova_allocator_init(self->iommu);
	self->bar_idx = largest_mapped_bar(self->device);
}

FIXTURE_TEARDOWN(vfio_dma_mapping_mmio_test)
{
	iova_allocator_cleanup(self->iova_allocator);
	vfio_pci_device_cleanup(self->device);
	iommu_cleanup(self->iommu);
}

TEST_F(vfio_dma_mapping_mmio_test, map_full_bar)
{
	struct vfio_pci_bar *bar;
	struct dma_region region;

	if (self->bar_idx < 0)
		SKIP(return, "No mappable BAR found on device %s", device_bdf);

	bar = &self->device->bars[self->bar_idx];

	region = (struct dma_region) {
		.vaddr = bar->vaddr,
		.size = bar->info.size,
		.iova = iova_allocator_alloc(self->iova_allocator, bar->info.size),
	};

	printf("Mapping BAR%d: vaddr=%p size=0x%lx iova=0x%lx\n",
	       self->bar_idx, region.vaddr, region.size, region.iova);

	iommu_map(self->iommu, &region);
	iommu_unmap(self->iommu, &region);
}

TEST_F(vfio_dma_mapping_mmio_test, map_partial_bar)
{
	struct vfio_pci_bar *bar;
	struct dma_region region;
	size_t page_size;

	if (self->bar_idx < 0)
		SKIP(return, "No mappable BAR found on device %s", device_bdf);

	bar = &self->device->bars[self->bar_idx];
	page_size = getpagesize();

	if (bar->info.size < 2 * page_size)
		SKIP(return, "BAR%d too small for partial mapping test (size=0x%llx)",
		     self->bar_idx, bar->info.size);

	region = (struct dma_region) {
		.vaddr = bar->vaddr,
		.size = page_size,
		.iova = iova_allocator_alloc(self->iova_allocator, page_size),
	};

	printf("Mapping BAR%d (partial): vaddr=%p size=0x%lx iova=0x%lx\n",
	       self->bar_idx, region.vaddr, region.size, region.iova);

	iommu_map(self->iommu, &region);
	iommu_unmap(self->iommu, &region);
}

int main(int argc, char *argv[])
{
	device_bdf = vfio_selftests_get_bdf(&argc, argv);
	return test_harness_run(argc, argv);
}
