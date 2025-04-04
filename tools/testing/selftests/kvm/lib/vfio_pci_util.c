// SPDX-License-Identifier: GPL-2.0-only
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>
#include <endian.h>
#include <sys/ioctl.h>
#include <linux/mman.h>
#include <asm/barrier.h>
#include <sys/eventfd.h>
#include <linux/limits.h>

#include <linux/vfio.h>
#include <linux/pci_regs.h>

#include "test_util.h"
#include "kvm_util.h"
#include "vfio_pci_util.h"

#define VFIO_DEV_PATH	"/dev/vfio/vfio"
#define PCI_SYSFS_PATH	"/sys/bus/pci/devices/"

void *vfio_pci_map_bar(struct vfio_pci_dev *dev, unsigned int bar_idx,
		       uint64_t *size)
{
	struct vfio_region_info info = {
		.argsz = sizeof(struct vfio_region_info),
		.index = bar_idx,
	};
	int fd = dev->fd;
	void *bar;
	int prot;

	TEST_ASSERT(bar_idx <= VFIO_PCI_BAR5_REGION_INDEX,
		    "Invalid BAR index: %d", bar_idx);

	/* Currently only support the cases where the BAR can be mmap-ed */
	vfio_ioctl(fd, VFIO_DEVICE_GET_REGION_INFO, &info);
	TEST_ASSERT(info.flags & VFIO_REGION_INFO_FLAG_MMAP,
		    "BAR%d doesn't support mmap", bar_idx);

	TEST_ASSERT(info.flags & VFIO_REGION_INFO_FLAG_READ,
		    "BAR%d doesn't support read?", bar_idx);

	prot = PROT_READ;
	if (info.flags & VFIO_REGION_INFO_FLAG_WRITE)
		prot |= PROT_WRITE;

	bar = mmap(NULL, info.size, prot, MAP_FILE | MAP_SHARED, fd, info.offset);
	TEST_ASSERT(bar != MAP_FAILED, "mmap(BAR%d) failed", bar_idx);

	*size = info.size;
	return bar;
}

/*
 * Read the PCI config space data
 *
 * Input Args:
 *   vfio_pci: Pointer to struct vfio_pci_dev
 *   config: The config space field's offset to read from (eg: PCI_VENDOR_ID)
 *   size: The size to read from the config region (could be one or more fields).
 *   data: Pointer to the region where the read data is to be copied into
 *
 *  The data returned is in little-endian format, which is the standard for PCI config space.
 */
void vfio_pci_read_config_data(struct vfio_pci_dev *dev, size_t offset,
			       size_t size, void *data)
{
	struct vfio_region_info info = {
		.argsz = sizeof(struct vfio_region_info),
		.index = VFIO_PCI_CONFIG_REGION_INDEX,
	};
	int ret;

	vfio_ioctl(dev->fd, VFIO_DEVICE_GET_REGION_INFO, &info);

	TEST_ASSERT(offset + size <= PCI_CFG_SPACE_EXP_SIZE,
		    "Requested config (%lu) and size (%lu) is out of bounds (%u)",
		    offset, size, PCI_CFG_SPACE_EXP_SIZE);

	ret = pread(dev->fd, data, size, info.offset + offset);
	TEST_ASSERT(ret == size, "Failed to read the PCI config: 0x%lx\n", offset);
}

static unsigned int vfio_pci_get_group_from_dev(const char *bdf)
{
	char dev_iommu_group_path[PATH_MAX] = {0};
	unsigned int pci_dev_sysfs_path_len;
	char *pci_dev_sysfs_path;
	unsigned int group;
	int ret;

	pci_dev_sysfs_path_len = strlen(PCI_SYSFS_PATH) + strlen("DDDD:BB:DD.F/iommu_group") + 1;

	pci_dev_sysfs_path = calloc(1, pci_dev_sysfs_path_len);
	TEST_ASSERT(pci_dev_sysfs_path, "Insufficient memory for pci dev sysfs path");

	snprintf(pci_dev_sysfs_path, pci_dev_sysfs_path_len,
		 "%s%s/iommu_group", PCI_SYSFS_PATH, bdf);

	ret = readlink(pci_dev_sysfs_path, dev_iommu_group_path,
		       sizeof(dev_iommu_group_path));
	TEST_ASSERT(ret != -1, "Failed to get IOMMU group for device: %s", bdf);

	ret = sscanf(basename(dev_iommu_group_path), "%u", &group);
	TEST_ASSERT(ret == 1, "Failed to get IOMMU group for device: %s", bdf);

	free(pci_dev_sysfs_path);
	return group;
}

static void vfio_pci_setup_group(struct vfio_pci_dev *dev, const char *bdf)
{
	char group_path[32];
	struct vfio_group_status group_status = {
	    .argsz = sizeof(group_status),
	};
	int group;

	group = vfio_pci_get_group_from_dev(bdf);
	snprintf(group_path, sizeof(group_path), "/dev/vfio/%d", group);

	dev->group_fd = open(group_path, O_RDWR);
	TEST_ASSERT(dev->group_fd >= 0,
		    "Failed to open the VFIO group %d for device: %s\n", group, bdf);

	__vfio_ioctl(dev->group_fd, VFIO_GROUP_GET_STATUS, &group_status);
	TEST_ASSERT(group_status.flags & VFIO_GROUP_FLAGS_VIABLE,
		    "Group %d for device %s not viable.  Ensure all devices are bound to vfio-pci",
		    group, bdf);

	vfio_ioctl(dev->group_fd, VFIO_GROUP_SET_CONTAINER, &dev->container_fd);
}

static void vfio_pci_set_iommu(struct vfio_pci_dev *dev, unsigned long iommu_type)
{
	TEST_ASSERT_EQ(__vfio_ioctl(dev->container_fd, VFIO_CHECK_EXTENSION, (void *)iommu_type), 1);
	vfio_ioctl(dev->container_fd, VFIO_SET_IOMMU, (void *)iommu_type);
}

static void vfio_pci_open_device(struct vfio_pci_dev *dev, const char *bdf)
{
	struct vfio_device_info dev_info = {
		.argsz = sizeof(dev_info),
	};

	dev->fd = __vfio_ioctl(dev->group_fd, VFIO_GROUP_GET_DEVICE_FD, bdf);
	TEST_ASSERT(dev->fd >= 0, "Failed to get the device fd\n");

	vfio_ioctl(dev->fd, VFIO_DEVICE_GET_INFO, &dev_info);

	TEST_ASSERT(!(dev_info.flags & VFIO_DEVICE_FLAGS_RESET),
		    "If VFIO tries to reset the VF, it will fail.");

	/* Require at least all BAR regions and the config space. */
	TEST_ASSERT(dev_info.num_regions >= VFIO_PCI_CONFIG_REGION_INDEX,
		    "Required number regions not supported (%d) for device: %s",
		    dev_info.num_regions, bdf);

	/* Check for at least VFIO_PCI_MSIX_IRQ_INDEX irqs */
	TEST_ASSERT(dev_info.num_irqs >= VFIO_PCI_MSIX_IRQ_INDEX,
		    "MSI-X IRQs (%d) not supported for device: %s",
		    dev_info.num_irqs, bdf);
}

/* bdf: PCI device's Domain:Bus:Device:Function in "DDDD:BB:DD.F" format */
struct vfio_pci_dev *__vfio_pci_init(const char *bdf, unsigned long iommu_type)
{
	struct vfio_pci_dev *dev;
	int vfio_version;

	TEST_ASSERT(bdf, "PCI BDF not supplied\n");

	dev = calloc(1, sizeof(*dev));
	TEST_ASSERT(dev, "Insufficient memory for vfio_pci_dev");

	dev->container_fd = open_path_or_exit(VFIO_DEV_PATH, O_RDWR);

	vfio_version = __vfio_ioctl(dev->container_fd, VFIO_GET_API_VERSION, NULL);
	TEST_REQUIRE(vfio_version == VFIO_API_VERSION);


	vfio_pci_setup_group(dev, bdf);
	vfio_pci_set_iommu(dev, iommu_type);
	vfio_pci_open_device(dev, bdf);

	return dev;
}

void vfio_pci_free(struct vfio_pci_dev *dev)
{
	close(dev->fd);
	vfio_ioctl(dev->group_fd, VFIO_GROUP_UNSET_CONTAINER, NULL);

	close(dev->group_fd);
	close(dev->container_fd);

	free(dev);
}
