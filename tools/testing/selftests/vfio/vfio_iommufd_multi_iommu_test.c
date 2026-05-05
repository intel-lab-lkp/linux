// SPDX-License-Identifier: GPL-2.0-only
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/sizes.h>
#include <linux/vfio.h>

#include <libvfio.h>

#include "kselftest_harness.h"

static const char *device_bdf;

#define IOMMU_DEFAULT 0
#define IOMMU_WITH_PT 1
#define IOMMU_WITHOUT_PT 2

static void region_setup(struct iommu *iommu,
			 struct iova_allocator *iova_allocator,
			 struct dma_region *region, u64 size)
{
	const int flags = MAP_SHARED | MAP_ANONYMOUS;
	const int prot = PROT_READ | PROT_WRITE;
	void *vaddr;

	vaddr = mmap(NULL, size, prot, flags, -1, 0);
	VFIO_ASSERT_NE(vaddr, MAP_FAILED);

	region->vaddr = vaddr;
	region->iova = iova_allocator_alloc(iova_allocator, size);
	region->size = size;

	iommu_map(iommu, region);
}

static void region_teardown(struct iommu *iommu, struct dma_region *region)
{
	iommu_unmap(iommu, region);
	VFIO_ASSERT_EQ(munmap(region->vaddr, region->size), 0);
}

FIXTURE(vfio_iommufd_multi_iommu_test) {
	struct iommu *iommu;
	struct vfio_pci_device *device;
	struct iova_allocator *iova_allocator;
	struct dma_region memcpy_region;
	void *vaddr;

	u64 size;
	void *src;
	void *dst;
	iova_t src_iova;
	iova_t dst_iova;
};

FIXTURE_SETUP(vfio_iommufd_multi_iommu_test)
{
	struct vfio_pci_driver *driver;

	self->iommu = iommu_init("iommufd");
	self->device = vfio_pci_device_init(device_bdf, self->iommu);
	self->iova_allocator = iova_allocator_init(self->iommu);

	driver = &self->device->driver;

	region_setup(self->iommu, self->iova_allocator, &self->memcpy_region, SZ_1G);
	region_setup(self->iommu, self->iova_allocator, &driver->region, SZ_2M);

	if (driver->ops)
		vfio_pci_driver_init(self->device);

	self->size = self->memcpy_region.size / 2;
	self->src = self->memcpy_region.vaddr;
	self->dst = self->src + self->size;

	self->src_iova = to_iova(self->device, self->src);
	self->dst_iova = to_iova(self->device, self->dst);
}

FIXTURE_TEARDOWN(vfio_iommufd_multi_iommu_test)
{
	struct vfio_pci_driver *driver = &self->device->driver;

	if (driver->ops)
		vfio_pci_driver_remove(self->device);

	region_teardown(self->iommu, &self->memcpy_region);
	region_teardown(self->iommu, &driver->region);

	iova_allocator_cleanup(self->iova_allocator);
	vfio_pci_device_cleanup(self->device);
	iommu_cleanup(self->iommu);
}

FIXTURE_VARIANT(vfio_iommufd_multi_iommu_test) {
	u32 start_iommu;
	u32 end_iommu;
};

#define ADD_IOMMU_VARIANT(S, E) \
	FIXTURE_VARIANT_ADD(vfio_iommufd_multi_iommu_test, S##_to_##E) { \
		.start_iommu = IOMMU_##S, \
		.end_iommu = IOMMU_##E \
	};

#define ADD_ALL_VARIANTS(S) \
	ADD_IOMMU_VARIANT(S, DEFAULT) \
	ADD_IOMMU_VARIANT(S, WITH_PT) \
	ADD_IOMMU_VARIANT(S, WITHOUT_PT)

ADD_ALL_VARIANTS(DEFAULT)
ADD_ALL_VARIANTS(WITH_PT)
ADD_ALL_VARIANTS(WITHOUT_PT)

static struct iommu *setup_variant_iommu(struct iommu *fixture_iommu,
					 u32 dev_id, u32 type)
{
	switch (type) {
	case IOMMU_WITH_PT:
		return iommufd_iommu_init(fixture_iommu->iommufd, dev_id,
					  IOMMUFD_IOMMU_INIT_CREATE_PT);
	case IOMMU_WITHOUT_PT:
		return iommufd_iommu_init(fixture_iommu->iommufd, dev_id, 0);
	default:
		return fixture_iommu;
	}
}

static void test_memcpy(struct vfio_pci_device *device, void *src, void *dst,
			iova_t src_iova, iova_t dst_iova, u64 size, char val)
{
	if (!device->driver.ops)
		return;

	memset(src, val, size);
	memset(dst, 0, size);

	vfio_pci_driver_memcpy_start(device, src_iova, dst_iova, size, 100);
	VFIO_ASSERT_EQ(vfio_pci_driver_memcpy_wait(device), 0);
	VFIO_ASSERT_EQ(memcmp(src, dst, size), 0);
}

TEST_F(vfio_iommufd_multi_iommu_test, memcpy)
{
	struct dma_region memcpy_region1, driver_region1;
	struct dma_region memcpy_region2, driver_region2;
	struct iommu *iommu1;
	struct iommu *iommu2;

	iommu1 = setup_variant_iommu(self->iommu, self->device->dev_id,
				     variant->start_iommu);
	if (iommu1 != self->iommu) {
		memcpy_region1 = self->memcpy_region;
		driver_region1 = self->device->driver.region;

		iommu_map(iommu1, &memcpy_region1);
		iommu_map(iommu1, &driver_region1);

		vfio_pci_device_attach_iommu(self->device, iommu1);
	}

	test_memcpy(self->device, self->src, self->dst, self->src_iova,
		    self->dst_iova, self->size, 'x');

	iommu2 = setup_variant_iommu(self->iommu, self->device->dev_id,
				     variant->end_iommu);
	if (iommu2 != self->iommu) {
		memcpy_region2 = self->memcpy_region;
		driver_region2 = self->device->driver.region;

		iommu_map(iommu2, &memcpy_region2);
		iommu_map(iommu2, &driver_region2);
	}

	vfio_pci_device_attach_iommu(self->device, iommu2);
	test_memcpy(self->device, self->src, self->dst, self->src_iova,
		    self->dst_iova, self->size, 'a');

	vfio_pci_device_attach_iommu(self->device, iommu1);
	test_memcpy(self->device, self->src, self->dst, self->src_iova,
		    self->dst_iova, self->size, '1');

	if (iommu1 != self->iommu) {
		/* attach back to default iommu for cleanup */
		vfio_pci_device_attach_iommu(self->device, self->iommu);
		iommu_unmap(iommu1, &memcpy_region1);
		iommu_unmap(iommu1, &driver_region1);
		iommu_cleanup(iommu1);
	}

	if (iommu2 != self->iommu) {
		iommu_unmap(iommu2, &memcpy_region2);
		iommu_unmap(iommu2, &driver_region2);
		iommu_cleanup(iommu2);
	}
}

int main(int argc, char *argv[])
{
	device_bdf = vfio_selftests_get_bdf(&argc, argv);

	return test_harness_run(argc, argv);
}
