// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal standalone test to verify an emulated or physical Intel 82576 (igb)
 * NIC bound to vfio-pci via IOMMUFD cdev, without creating a KVM guest.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <linux/iommufd.h>
#include <linux/pci_regs.h>
#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/vfio.h>

#include "kselftest_harness.h"

#define PCI_VENDOR_ID_INTEL 0x8086
#define PCI_DEVICE_ID_INTEL_82576 0x10C9

/* Intel 82576 BAR0 register offsets */
#define E1000_CTRL	0x00000 /* Device Control - RW */
#define E1000_STATUS	0x00008 /* Device Status - RO */
#define E1000_RAL0	0x05400 /* Receive Address Low (0) - RW */
#define E1000_RAH0	0x05404 /* Receive Address High (0) - RW */

static char *find_igb_cdev_path(void)
{
	const char *sys_pci = "/sys/bus/pci/devices";
	struct dirent *dev_entry;
	DIR *dir = opendir(sys_pci);
	char cdev_path[PATH_MAX];
	char path[PATH_MAX];

	if (!dir)
		return NULL;

	while ((dev_entry = readdir(dir)) != NULL) {
		FILE *f;
		uint32_t vendor = 0, device = 0;
		DIR *vfio_dir;
		struct dirent *vfio_entry;

		if (dev_entry->d_name[0] == '.')
			continue;

		snprintf(path, sizeof(path), "%s/%s/vendor", sys_pci, dev_entry->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fscanf(f, "0x%x", &vendor) != 1) {
			fclose(f);
			continue;
		}
		fclose(f);

		snprintf(path, sizeof(path), "%s/%s/device", sys_pci, dev_entry->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fscanf(f, "0x%x", &device) != 1) {
			fclose(f);
			continue;
		}
		fclose(f);

		if (vendor != PCI_VENDOR_ID_INTEL || device != PCI_DEVICE_ID_INTEL_82576)
			continue;

		/* Found an Intel 82576 device. Check if bound to vfio-pci cdev. */
		snprintf(path, sizeof(path), "%s/%s/vfio-dev", sys_pci, dev_entry->d_name);
		vfio_dir = opendir(path);
		if (!vfio_dir)
			continue;

		while ((vfio_entry = readdir(vfio_dir)) != NULL) {
			if (strncmp(vfio_entry->d_name, "vfio", 4) == 0) {
				snprintf(cdev_path, sizeof(cdev_path), "/dev/vfio/devices/%s",
					 vfio_entry->d_name);
				closedir(vfio_dir);
				closedir(dir);
				return strdup(cdev_path);
			}
		}
		closedir(vfio_dir);
	}

	closedir(dir);
	return NULL;
}

FIXTURE(guest_memfd_vfio) {
	char *cdev_path;
	int iommufd;
	int vfio_fd;
};

FIXTURE_SETUP(guest_memfd_vfio)
{
	struct vfio_device_bind_iommufd bind_args = {};
	int ret;

	self->cdev_path = find_igb_cdev_path();
	if (!self->cdev_path)
		SKIP(return, "Intel 82576 device bound to vfio not found\n");

	self->iommufd = open("/dev/iommu", O_RDWR);
	if (self->iommufd < 0) {
		free(self->cdev_path);
		self->cdev_path = NULL;
		SKIP(return, "Failed to open /dev/iommu\n");
	}

	self->vfio_fd = open(self->cdev_path, O_RDWR);
	ASSERT_GE(self->vfio_fd, 0);

	bind_args.argsz = sizeof(bind_args);
	bind_args.iommufd = self->iommufd;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_BIND_IOMMUFD, &bind_args);
	ASSERT_EQ(0, ret);
}

FIXTURE_TEARDOWN(guest_memfd_vfio)
{
	if (self->vfio_fd >= 0)
		close(self->vfio_fd);
	if (self->iommufd >= 0)
		close(self->iommufd);
	free(self->cdev_path);
}

TEST_F(guest_memfd_vfio, get_device_info)
{
	struct vfio_device_info dev_info = {};
	int ret;

	dev_info.argsz = sizeof(dev_info);
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_INFO, &dev_info);
	ASSERT_EQ(0, ret);
	ASSERT_TRUE(dev_info.flags & VFIO_DEVICE_FLAGS_PCI);
	ASSERT_TRUE(dev_info.flags & VFIO_DEVICE_FLAGS_RESET);
	ASSERT_GE(dev_info.num_regions, VFIO_PCI_NUM_REGIONS);
	ASSERT_GT(dev_info.num_irqs, 0);
}

TEST_F(guest_memfd_vfio, get_region_info)
{
	struct vfio_region_info reg_info = {};
	int ret;

	/* Test BAR0: 128 KiB MMIO register space */
	reg_info.argsz = sizeof(reg_info);
	reg_info.index = VFIO_PCI_BAR0_REGION_INDEX;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &reg_info);
	ASSERT_EQ(0, ret);
	ASSERT_EQ(0x20000, reg_info.size);
	ASSERT_EQ(VFIO_REGION_INFO_FLAG_READ |
		  VFIO_REGION_INFO_FLAG_WRITE |
		  VFIO_REGION_INFO_FLAG_MMAP,
		  reg_info.flags & (VFIO_REGION_INFO_FLAG_READ |
				    VFIO_REGION_INFO_FLAG_WRITE |
				    VFIO_REGION_INFO_FLAG_MMAP));

	/* Test BAR3: 16 KiB MSI-X table and PBA */
	memset(&reg_info, 0, sizeof(reg_info));
	reg_info.argsz = sizeof(reg_info);
	reg_info.index = VFIO_PCI_BAR3_REGION_INDEX;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &reg_info);
	ASSERT_EQ(0, ret);
	ASSERT_EQ(0x4000, reg_info.size);

	/* Test PCIe Config Space: 4096 bytes extended config */
	memset(&reg_info, 0, sizeof(reg_info));
	reg_info.argsz = sizeof(reg_info);
	reg_info.index = VFIO_PCI_CONFIG_REGION_INDEX;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &reg_info);
	ASSERT_EQ(0, ret);
	ASSERT_EQ(PCI_CFG_SPACE_EXP_SIZE, reg_info.size);
	ASSERT_EQ(VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE,
		  reg_info.flags & (VFIO_REGION_INFO_FLAG_READ |
				    VFIO_REGION_INFO_FLAG_WRITE));
}

TEST_HARNESS_MAIN
