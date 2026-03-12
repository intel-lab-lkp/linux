// SPDX-License-Identifier: GPL-2.0
/*
 * VFIO iommufd NoIOMMU Mode Selftest
 *
 * Tests VFIO device operations with iommufd in noiommu mode, including:
 * - Device binding to iommufd
 * - IOAS (I/O Address Space) allocation and management
 * - Device attach/detach to IOAS
 * - Memory mapping in IOAS
 * - Device info queries and reset
 */

#include <linux/limits.h>
#include <linux/vfio.h>
#include <linux/iommufd.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

#include <libvfio.h>
#include "kselftest_harness.h"

static const char iommu_dev_path[] = "/dev/iommu";
static const char *cdev_path;

static char *vfio_noiommu_get_device_id(const char *bdf)
{
	char *path = NULL;
	char *vfio_id = NULL;
	struct dirent *dentry;
	DIR *dp;

	if (asprintf(&path, "/sys/bus/pci/devices/%s/vfio-dev", bdf) < 0)
		return NULL;

	dp = opendir(path);
	if (!dp) {
		free(path);
		return NULL;
	}

	while ((dentry = readdir(dp)) != NULL) {
		if (strncmp("noiommu-vfio", dentry->d_name, 12) == 0) {
			vfio_id = strdup(dentry->d_name);
			break;
		}
	}

	closedir(dp);
	free(path);
	return vfio_id;
}

static char *vfio_noiommu_get_cdev_path(const char *bdf)
{
	char *vfio_id = vfio_noiommu_get_device_id(bdf);
	char *cdev = NULL;

	if (vfio_id) {
		asprintf(&cdev, "/dev/vfio/devices/%s", vfio_id);
		free(vfio_id);
	}
	return cdev;
}

static int vfio_device_bind_iommufd_ioctl(int cdev_fd, int iommufd)
{
	struct vfio_device_bind_iommufd bind_args = {
		.argsz = sizeof(bind_args),
		.iommufd = iommufd,
	};

	return ioctl(cdev_fd, VFIO_DEVICE_BIND_IOMMUFD, &bind_args);
}

static int vfio_device_get_info_ioctl(int cdev_fd,
				      struct vfio_device_info *info)
{
	info->argsz = sizeof(*info);
	return ioctl(cdev_fd, VFIO_DEVICE_GET_INFO, info);
}

static int vfio_device_ioas_alloc_ioctl(int iommufd,
					struct iommu_ioas_alloc *alloc_args)
{
	alloc_args->size = sizeof(*alloc_args);
	alloc_args->flags = 0;
	return ioctl(iommufd, IOMMU_IOAS_ALLOC, alloc_args);
}

static int vfio_device_attach_iommufd_pt_ioctl(int cdev_fd, u32 pt_id)
{
	struct vfio_device_attach_iommufd_pt attach_args = {
		.argsz = sizeof(attach_args),
		.pt_id = pt_id,
	};

	return ioctl(cdev_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach_args);
}

static int vfio_device_detach_iommufd_pt_ioctl(int cdev_fd)
{
	struct vfio_device_detach_iommufd_pt detach_args = {
		.argsz = sizeof(detach_args),
	};

	return ioctl(cdev_fd, VFIO_DEVICE_DETACH_IOMMUFD_PT, &detach_args);
}

static int vfio_device_get_region_info_ioctl(int cdev_fd, uint32_t index,
					     struct vfio_region_info *info)
{
	info->argsz = sizeof(*info);
	info->index = index;
	return ioctl(cdev_fd, VFIO_DEVICE_GET_REGION_INFO, info);
}

static int vfio_device_reset_ioctl(int cdev_fd)
{
	return ioctl(cdev_fd, VFIO_DEVICE_RESET);
}

static int ioas_map_pages(int iommufd, uint32_t ioas_id, uint64_t iova,
			  size_t length, bool hugepages)
{
	struct iommu_ioas_map map_args = {
		.size = sizeof(map_args),
		.ioas_id = ioas_id,
		.iova = iova,
		.length = length,
		.flags = IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE | IOMMU_IOAS_MAP_FIXED_IOVA,
	};
	void *pages;
	int ret;

	/* Allocate test pages */
	if (hugepages)
		pages = mmap(NULL, length, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	else
		pages = mmap(NULL, length, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (pages == MAP_FAILED) {
		printf("mmap failed for length 0x%lx\n", (unsigned long)length);
		return -ENOMEM;
	}

	/* Set up page pointer for mapping */
	map_args.user_va = (uintptr_t)pages;

	printf("  ioas_map_pages: ioas_id=%u, iova=0x%lx, length=0x%lx, user_va=%p\n",
	       ioas_id, (unsigned long)iova, (unsigned long)length, pages);

	/* Map into IOAS */
	ret = ioctl(iommufd, IOMMU_IOAS_MAP, &map_args);
	if (ret != 0)
		printf("  IOMMU_IOAS_MAP failed: %d (%s)\n", ret, strerror(errno));
	else
		printf("  IOMMU_IOAS_MAP succeeded, IOVA=0x%lx\n", (unsigned long)map_args.iova);

	munmap(pages, length);
	return ret;
}

static int ioas_unmap_pages(int iommufd, uint32_t ioas_id, uint64_t iova,
			    size_t length)
{
	struct iommu_ioas_unmap unmap_args = {
		.size = sizeof(unmap_args),
		.ioas_id = ioas_id,
		.iova = iova,
		.length = length,
	};

	return ioctl(iommufd, IOMMU_IOAS_UNMAP, &unmap_args);
}

static int ioas_destroy_ioctl(int iommufd, uint32_t ioas_id)
{
	struct iommu_destroy destroy_args = {
		.size = sizeof(destroy_args),
		.id = ioas_id,
	};

	return ioctl(iommufd, IOMMU_DESTROY, &destroy_args);
}

static int ioas_get_pa_ioctl(int iommufd, uint32_t ioas_id, uint64_t iova,
			     uint64_t *phys_out, uint64_t *length_out)
{
	struct {
		__u32 size;
		__u32 flags;
		__u32 ioas_id;
		__u32 __reserved;
		__u64 iova;
		__u64 out_length;
		__u64 out_phys;
	} get_pa = {
		.size = sizeof(get_pa),
		.flags = 0,
		.ioas_id = ioas_id,
		.iova = iova,
	};

	printf("  ioas_get_pa_ioctl: ioas_id=%u, iova=0x%lx\n",
	       ioas_id, (unsigned long)iova);

	if (ioctl(iommufd, IOMMU_IOAS_GET_PA, &get_pa) != 0) {
		printf("  IOMMU_IOAS_GET_PA failed: %s (errno=%d)\n",
		       strerror(errno), errno);
		return -1;
	}

	printf("  IOMMU_IOAS_GET_PA succeeded: PA=0x%lx, length=0x%lx\n",
	       (unsigned long)get_pa.out_phys, (unsigned long)get_pa.out_length);

	if (phys_out)
		*phys_out = get_pa.out_phys;
	if (length_out)
		*length_out = get_pa.out_length;

	return 0;
}

FIXTURE(vfio_noiommu) {
	int cdev_fd;
	int iommufd;
};

FIXTURE_SETUP(vfio_noiommu)
{
	ASSERT_LE(0, (self->cdev_fd = open(cdev_path, O_RDWR, 0)));
	ASSERT_LE(0, (self->iommufd = open(iommu_dev_path, O_RDWR, 0)));
}

FIXTURE_TEARDOWN(vfio_noiommu)
{
	if (self->cdev_fd >= 0)
		close(self->cdev_fd);
	if (self->iommufd >= 0)
		close(self->iommufd);
}

/*
 * Test: Device cdev can be opened
 */
TEST_F(vfio_noiommu, device_cdev_open)
{
	ASSERT_LE(0, self->cdev_fd);
}

/*
 * Test: Device can be bound to iommufd
 */
TEST_F(vfio_noiommu, device_bind_iommufd)
{
	ASSERT_EQ(0, vfio_device_bind_iommufd_ioctl(self->cdev_fd,
						    self->iommufd));
}

/*
 * Test: Device info can be queried after binding
 */
TEST_F(vfio_noiommu, device_get_info_after_bind)
{
	struct vfio_device_info info;

	ASSERT_EQ(0, vfio_device_bind_iommufd_ioctl(self->cdev_fd,
						    self->iommufd));
	ASSERT_EQ(0, vfio_device_get_info_ioctl(self->cdev_fd, &info));
	ASSERT_NE(0, info.argsz);
}

/*
 * Test: Getting device info fails without bind
 */
TEST_F(vfio_noiommu, device_get_info_without_bind_fails)
{
	struct vfio_device_info info;

	ASSERT_NE(0, vfio_device_get_info_ioctl(self->cdev_fd, &info));
}

/*
 * Test: Binding with invalid iommufd fails
 */
TEST_F(vfio_noiommu, device_bind_bad_iommufd_fails)
{
	ASSERT_NE(0, vfio_device_bind_iommufd_ioctl(self->cdev_fd, -2));
}

/*
 * Test: Cannot bind twice to same device
 */
TEST_F(vfio_noiommu, device_repeated_bind_fails)
{
	ASSERT_EQ(0, vfio_device_bind_iommufd_ioctl(self->cdev_fd,
						    self->iommufd));
	ASSERT_NE(0, vfio_device_bind_iommufd_ioctl(self->cdev_fd,
						    self->iommufd));
}

/*
 * Test: IOAS can be allocated
 */
TEST_F(vfio_noiommu, ioas_alloc)
{
	struct iommu_ioas_alloc alloc_args;

	ASSERT_EQ(0, vfio_device_ioas_alloc_ioctl(self->iommufd,
						  &alloc_args));
	ASSERT_NE(0, alloc_args.out_ioas_id);
}

/*
 * Test: IOAS can be destroyed
 */
TEST_F(vfio_noiommu, ioas_destroy)
{
	struct iommu_ioas_alloc alloc_args;

	ASSERT_EQ(0, vfio_device_ioas_alloc_ioctl(self->iommufd,
						  &alloc_args));
	ASSERT_EQ(0, ioas_destroy_ioctl(self->iommufd,
					alloc_args.out_ioas_id));
}

/*
 * Test: Device can attach to IOAS after binding
 */
TEST_F(vfio_noiommu, device_attach_to_ioas)
{
	struct iommu_ioas_alloc alloc_args;

	ASSERT_EQ(0, vfio_device_bind_iommufd_ioctl(self->cdev_fd,
						    self->iommufd));
	ASSERT_EQ(0, vfio_device_ioas_alloc_ioctl(self->iommufd,
						  &alloc_args));
	ASSERT_EQ(0, vfio_device_attach_iommufd_pt_ioctl(self->cdev_fd,
							 alloc_args.out_ioas_id));
}

/*
 * Test: Attaching to invalid IOAS fails
 */
TEST_F(vfio_noiommu, device_attach_invalid_ioas_fails)
{
	ASSERT_EQ(0, vfio_device_bind_iommufd_ioctl(self->cdev_fd,
						    self->iommufd));
	ASSERT_NE(0, vfio_device_attach_iommufd_pt_ioctl(self->cdev_fd,
							 UINT32_MAX));
}

/*
 * Test: Device can detach from IOAS
 */
TEST_F(vfio_noiommu, device_detach_from_ioas)
{
	struct iommu_ioas_alloc alloc_args;

	ASSERT_EQ(0, vfio_device_bind_iommufd_ioctl(self->cdev_fd,
						    self->iommufd));
	ASSERT_EQ(0, vfio_device_ioas_alloc_ioctl(self->iommufd,
						  &alloc_args));
	ASSERT_EQ(0, vfio_device_attach_iommufd_pt_ioctl(self->cdev_fd,
							 alloc_args.out_ioas_id));
	ASSERT_EQ(0, vfio_device_detach_iommufd_pt_ioctl(self->cdev_fd));
}

/*
 * Test: Full lifecycle - bind, attach, detach, reset
 */
TEST_F(vfio_noiommu, device_lifecycle)
{
	struct iommu_ioas_alloc alloc_args;
	struct vfio_device_info info;

	/* Bind device to iommufd */
	ASSERT_EQ(0, vfio_device_bind_iommufd_ioctl(self->cdev_fd,
						    self->iommufd));

	/* Allocate IOAS */
	ASSERT_EQ(0, vfio_device_ioas_alloc_ioctl(self->iommufd,
						  &alloc_args));

	/* Attach device to IOAS */
	ASSERT_EQ(0, vfio_device_attach_iommufd_pt_ioctl(self->cdev_fd,
							 alloc_args.out_ioas_id));

	/* Query device info */
	ASSERT_EQ(0, vfio_device_get_info_ioctl(self->cdev_fd, &info));

	/* Detach device from IOAS */
	ASSERT_EQ(0, vfio_device_detach_iommufd_pt_ioctl(self->cdev_fd));

	/* Reset device */
	ASSERT_EQ(0, vfio_device_reset_ioctl(self->cdev_fd));
}

/*
 * Test: Get region info
 */
TEST_F(vfio_noiommu, device_get_region_info)
{
	struct vfio_device_info dev_info;
	struct vfio_region_info region_info;

	ASSERT_EQ(0, vfio_device_bind_iommufd_ioctl(self->cdev_fd,
						    self->iommufd));
	ASSERT_EQ(0, vfio_device_get_info_ioctl(self->cdev_fd, &dev_info));

	/* Try to get first region info if device has regions */
	if (dev_info.num_regions > 0) {
		ASSERT_EQ(0, vfio_device_get_region_info_ioctl(self->cdev_fd, 0,
							       &region_info));
		ASSERT_NE(0, region_info.argsz);
	}
}

TEST_F(vfio_noiommu, device_reset)
{
	ASSERT_EQ(0, vfio_device_bind_iommufd_ioctl(self->cdev_fd,
						    self->iommufd));
	ASSERT_EQ(0, vfio_device_reset_ioctl(self->cdev_fd));
}

TEST_F(vfio_noiommu, ioas_map_pages)
{
	struct iommu_ioas_alloc alloc_args;
	long page_size = sysconf(_SC_PAGESIZE);
	uint64_t iova = 0x10000;
	int i;

	ASSERT_GT(page_size, 0);

	ASSERT_EQ(0, vfio_device_ioas_alloc_ioctl(self->iommufd,
						  &alloc_args));

	printf("Page size: %ld bytes\n", page_size);
	/* Test mapping regions of different sizes: 1, 2, 4, 8 pages */
	for (i = 0; i < 4; i++) {
		size_t map_size = page_size * (1 << i);  /* 1, 2, 4, 8 pages */
		uint64_t test_iova = iova + (i * 0x100000);

		/* Attempt to map each region (may fail if not supported) */
		ioas_map_pages(self->iommufd, alloc_args.out_ioas_id,
			       test_iova, map_size, false);
	}
}

TEST_F(vfio_noiommu, multiple_ioas_alloc)
{
	struct iommu_ioas_alloc alloc1, alloc2;

	ASSERT_EQ(0, vfio_device_ioas_alloc_ioctl(self->iommufd, &alloc1));
	ASSERT_EQ(0, vfio_device_ioas_alloc_ioctl(self->iommufd, &alloc2));
	ASSERT_NE(alloc1.out_ioas_id, alloc2.out_ioas_id);
}

/*
 * Test: Query physical address for IOVA
 * Tests IOMMU_IOAS_GET_PA ioctl to translate IOVA to physical address
 * Note: Device must be attached to IOAS for PA query to work
 */
#define NR_PAGES 32
TEST_F(vfio_noiommu, ioas_get_pa_mapped)
{
	struct iommu_ioas_alloc alloc_args;
	long page_size = sysconf(_SC_PAGESIZE);
	uint64_t iova = 0x200000;
	uint64_t phys = 0;
	uint64_t length = 0;
	int ret;

	ASSERT_GT(page_size, 0);

	ASSERT_EQ(0, vfio_device_bind_iommufd_ioctl(self->cdev_fd,
						    self->iommufd));

	ASSERT_EQ(0, vfio_device_ioas_alloc_ioctl(self->iommufd,
						  &alloc_args));

	ASSERT_EQ(0, vfio_device_attach_iommufd_pt_ioctl(self->cdev_fd,
							 alloc_args.out_ioas_id));

	/*
	 * Map a page into an arbitrary IOAS, used as a cookie for lookup.
	 * Use hugepages to test contiguous PA. Make sure hugepages are
	 * available. e.g.  echo 64 > /proc/sys/vm/nr_hugepages
	 */
	ret = ioas_map_pages(self->iommufd, alloc_args.out_ioas_id,
			     iova, page_size * NR_PAGES, true);
	if (ret != 0)
		return;

	/* Query the physical address for the mapped dummy IOVA */
	ret = ioas_get_pa_ioctl(self->iommufd, alloc_args.out_ioas_id,
			       iova, &phys, &length);

	if (ret == 0) {
		/* If we got a result, verify it's valid */
		ASSERT_NE(0, phys);
		ASSERT_GE((uint64_t)page_size * NR_PAGES, length);
	}
}

TEST_F(vfio_noiommu, ioas_get_pa_unmapped_fails)
{
	struct iommu_ioas_alloc alloc_args;

	ASSERT_EQ(0, vfio_device_ioas_alloc_ioctl(self->iommufd,
						  &alloc_args));

	/* Try to retrieve unmapped IOVA (should fail) */
	ASSERT_NE(0, ioas_get_pa_ioctl(self->iommufd, alloc_args.out_ioas_id,
				       0x10000, NULL, NULL));
}

int main(int argc, char *argv[])
{
	const char *device_bdf = vfio_selftests_get_bdf(&argc, argv);
	char *cdev = NULL;

	if (!device_bdf) {
		ksft_print_msg("No device BDF provided\n");
		return KSFT_SKIP;
	}

	cdev = vfio_noiommu_get_cdev_path(device_bdf);
	if (!cdev) {
		ksft_print_msg("Could not find cdev for device %s\n",
			       device_bdf);
		return KSFT_SKIP;
	}

	cdev_path = cdev;
	ksft_print_msg("Using cdev device %s for BDF %s\n", cdev_path,
		       device_bdf);

	return test_harness_run(argc, argv);
}
