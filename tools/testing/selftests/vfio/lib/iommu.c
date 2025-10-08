// SPDX-License-Identifier: GPL-2.0-only
#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <uapi/linux/types.h>
#include <linux/limits.h>
#include <linux/mman.h>
#include <linux/types.h>
#include <linux/vfio.h>
#include <linux/iommufd.h>

#include "../../../kselftest.h"
#include <libvfio.h>

const char *default_iommu_mode = "iommufd";

/* Reminder: Keep in sync with FIXTURE_VARIANT_ADD_ALL_IOMMU_MODES(). */
static const struct iommu_mode iommu_modes[] = {
	{
		.name = "vfio_type1_iommu",
		.container_path = "/dev/vfio/vfio",
		.iommu_type = VFIO_TYPE1_IOMMU,
	},
	{
		.name = "vfio_type1v2_iommu",
		.container_path = "/dev/vfio/vfio",
		.iommu_type = VFIO_TYPE1v2_IOMMU,
	},
	{
		.name = "iommufd_compat_type1",
		.container_path = "/dev/iommu",
		.iommu_type = VFIO_TYPE1_IOMMU,
	},
	{
		.name = "iommufd_compat_type1v2",
		.container_path = "/dev/iommu",
		.iommu_type = VFIO_TYPE1v2_IOMMU,
	},
	{
		.name = "iommufd",
	},
};

iova_t __iommu_hva2iova(struct iommu *iommu, void *vaddr)
{
	struct dma_region *region;

	list_for_each_entry(region, &iommu->dma_regions, link) {
		if (vaddr < region->vaddr)
			continue;

		if (vaddr >= region->vaddr + region->size)
			continue;

		return region->iova + (vaddr - region->vaddr);
	}

	return INVALID_IOVA;
}

iova_t iommu_hva2iova(struct iommu *iommu, void *vaddr)
{
	iova_t iova = __iommu_hva2iova(iommu, vaddr);

	VFIO_ASSERT_NE(iova, INVALID_IOVA, "VA %p is not mapped\n", vaddr);
	return iova;
}

static void vfio_iommu_dma_map(struct iommu *iommu, struct dma_region *region)
{
	struct vfio_iommu_type1_dma_map args = {
		.argsz = sizeof(args),
		.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
		.vaddr = (u64)region->vaddr,
		.iova = region->iova,
		.size = region->size,
	};

	ioctl_assert(iommu->container_fd, VFIO_IOMMU_MAP_DMA, &args);
}

static void iommufd_dma_map(struct iommu *iommu, struct dma_region *region)
{
	struct iommu_ioas_map args = {
		.size = sizeof(args),
		.flags = IOMMU_IOAS_MAP_READABLE |
			 IOMMU_IOAS_MAP_WRITEABLE |
			 IOMMU_IOAS_MAP_FIXED_IOVA,
		.user_va = (u64)region->vaddr,
		.iova = region->iova,
		.length = region->size,
		.ioas_id = iommu->ioas_id,
	};

	ioctl_assert(iommu->iommufd, IOMMU_IOAS_MAP, &args);
}

void iommu_map(struct iommu *iommu, struct dma_region *region)
{
	if (iommu->iommufd)
		iommufd_dma_map(iommu, region);
	else
		vfio_iommu_dma_map(iommu, region);

	list_add(&region->link, &iommu->dma_regions);
}

static void vfio_iommu_dma_unmap(struct iommu *iommu, struct dma_region *region)
{
	struct vfio_iommu_type1_dma_unmap args = {
		.argsz = sizeof(args),
		.iova = region->iova,
		.size = region->size,
	};

	ioctl_assert(iommu->container_fd, VFIO_IOMMU_UNMAP_DMA, &args);
}

static void iommufd_dma_unmap(struct iommu *iommu, struct dma_region *region)
{
	struct iommu_ioas_unmap args = {
		.size = sizeof(args),
		.iova = region->iova,
		.length = region->size,
		.ioas_id = iommu->ioas_id,
	};

	ioctl_assert(iommu->iommufd, IOMMU_IOAS_UNMAP, &args);
}

void iommu_unmap(struct iommu *iommu, struct dma_region *region)
{
	if (iommu->iommufd)
		iommufd_dma_unmap(iommu, region);
	else
		vfio_iommu_dma_unmap(iommu, region);

	list_del(&region->link);
}

static const struct iommu_mode *lookup_iommu_mode(const char *iommu_mode)
{
	int i;

	if (!iommu_mode)
		iommu_mode = default_iommu_mode;

	for (i = 0; i < ARRAY_SIZE(iommu_modes); i++) {
		if (strcmp(iommu_mode, iommu_modes[i].name))
			continue;

		return &iommu_modes[i];
	}

	VFIO_FAIL("Unrecognized IOMMU mode: %s\n", iommu_mode);
}

static u32 iommufd_ioas_alloc(int iommufd)
{
	struct iommu_ioas_alloc args = {
		.size = sizeof(args),
	};

	ioctl_assert(iommufd, IOMMU_IOAS_ALLOC, &args);
	return args.out_ioas_id;
}

struct iommu *iommu_init(const char *iommu_mode)
{
	const char *container_path;
	struct iommu *iommu;
	int version;

	iommu = calloc(1, sizeof(*iommu));
	VFIO_ASSERT_NOT_NULL(iommu);

	INIT_LIST_HEAD(&iommu->dma_regions);

	iommu->mode = lookup_iommu_mode(iommu_mode);

	container_path = iommu->mode->container_path;
	if (container_path) {
		iommu->container_fd = open(container_path, O_RDWR);
		VFIO_ASSERT_GE(iommu->container_fd, 0, "open(%s) failed\n", container_path);

		version = ioctl(iommu->container_fd, VFIO_GET_API_VERSION);
		VFIO_ASSERT_EQ(version, VFIO_API_VERSION, "Unsupported version: %d\n", version);
	} else {
		/*
		 * Require device->iommufd to be >0 so that a simple non-0 check can be
		 * used to check if iommufd is enabled. In practice open() will never
		 * return 0 unless stdin is closed.
		 */
		iommu->iommufd = open("/dev/iommu", O_RDWR);
		VFIO_ASSERT_GT(iommu->iommufd, 0);

		iommu->ioas_id = iommufd_ioas_alloc(iommu->iommufd);
	}

	return iommu;
}

void iommu_cleanup(struct iommu *iommu)
{
	if (iommu->iommufd)
		VFIO_ASSERT_EQ(close(iommu->iommufd), 0);
	else
		VFIO_ASSERT_EQ(close(iommu->container_fd), 0);

	free(iommu);
}
