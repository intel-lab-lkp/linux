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
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <linux/iommufd.h>
#include <linux/pci_regs.h>
#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/vfio.h>

#include "kselftest_harness.h"
#include "kvm_util.h"
#include "processor.h"

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

#define VFIO_PCI_OFFSET_SHIFT 40

FIXTURE(guest_memfd_vfio) {
	char *cdev_path;
	struct kvm_vm *vm;
	int gmem_fd;
	int iommufd;
	int vfio_fd;
	int bar_fd;
};

FIXTURE_VARIANT(guest_memfd_vfio) {
	bool use_gmem;
};

FIXTURE_VARIANT_ADD(guest_memfd_vfio, direct) {
	.use_gmem = false,
};

FIXTURE_VARIANT_ADD(guest_memfd_vfio, guest_memfd) {
	.use_gmem = true,
};

FIXTURE_SETUP(guest_memfd_vfio)
{
	struct vfio_device_bind_iommufd bind_args = {};
	int ret;

	self->vm = NULL;
	self->gmem_fd = -1;
	self->bar_fd = -1;

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
	if (variant->use_gmem)
		bind_args.flags = VFIO_DEVICE_BIND_FLAG_CTRL_ONLY;

	ret = ioctl(self->vfio_fd, VFIO_DEVICE_BIND_IOMMUFD, &bind_args);
	ASSERT_EQ(0, ret);

	/*
	 * Create a SW_PROTECTED_VM with 1 runnable vCPU allocated for memslot0
	 * and stack/page table allocation, so guest_mapping can execute vCPUs
	 * and FIXTURE_TEARDOWN uniformly cleans up self->vm across both variants.
	 */
	self->vm = __vm_create((struct vm_shape){ .mode = VM_MODE_DEFAULT,
						  .type = KVM_X86_SW_PROTECTED_VM },
			       1, 0);

	if (variant->use_gmem) {
		uint64_t gmem_flags = GUEST_MEMFD_FLAG_USE_RESOURCE |
				      GUEST_MEMFD_FLAG_PFN_MODE |
				      GUEST_MEMFD_FLAG_MMAP |
				      GUEST_MEMFD_FLAG_INIT_SHARED;
		uint64_t gmem_size = (uint64_t)VFIO_PCI_NUM_REGIONS << VFIO_PCI_OFFSET_SHIFT;

		/*
		 * guest_memfd providers (including VFIO) is only intended to be
		 * used with gmem_in_place_conversion.
		 */
		TEST_REQUIRE(kvm_has_gmem_attributes);

		self->gmem_fd = vm_create_guest_memfd_resource(self->vm, gmem_size,
							       gmem_flags,
							       self->vfio_fd);
		ASSERT_GE(self->gmem_fd, 0);
		self->bar_fd = self->gmem_fd;
	} else {
		self->bar_fd = self->vfio_fd;
	}
}

FIXTURE_TEARDOWN(guest_memfd_vfio)
{
	if (self->gmem_fd >= 0)
		close(self->gmem_fd);
	if (self->vm)
		kvm_vm_free(self->vm);
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

TEST_F(guest_memfd_vfio, get_irq_info)
{
	struct vfio_irq_info irq_info = {};
	int ret;

	/*
	 * Test MSI-X interrupts (VFIO_PCI_MSIX_IRQ_INDEX):
	 * Intel 82576 supports up to 10 MSI-X vectors.
	 */
	irq_info.argsz = sizeof(irq_info);
	irq_info.index = VFIO_PCI_MSIX_IRQ_INDEX;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_IRQ_INFO, &irq_info);
	ASSERT_EQ(0, ret);
	ASSERT_GT(irq_info.count, 0);
	ASSERT_TRUE(irq_info.flags & VFIO_IRQ_INFO_EVENTFD);

	/*
	 * Test MSI interrupts (VFIO_PCI_MSI_IRQ_INDEX):
	 * Should report eventfd capability.
	 */
	memset(&irq_info, 0, sizeof(irq_info));
	irq_info.argsz = sizeof(irq_info);
	irq_info.index = VFIO_PCI_MSI_IRQ_INDEX;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_IRQ_INFO, &irq_info);
	ASSERT_EQ(0, ret);
	ASSERT_GT(irq_info.count, 0);
	ASSERT_TRUE(irq_info.flags & VFIO_IRQ_INFO_EVENTFD);

	/*
	 * Test legacy INTx interrupts (VFIO_PCI_INTX_IRQ_INDEX):
	 * Must support eventfd and masking when present.
	 */
	memset(&irq_info, 0, sizeof(irq_info));
	irq_info.argsz = sizeof(irq_info);
	irq_info.index = VFIO_PCI_INTX_IRQ_INDEX;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_IRQ_INFO, &irq_info);
	ASSERT_EQ(0, ret);
	ASSERT_LE(irq_info.count, 1);
	if (irq_info.count == 1) {
		ASSERT_EQ(VFIO_IRQ_INFO_EVENTFD |
			  VFIO_IRQ_INFO_MASKABLE |
			  VFIO_IRQ_INFO_AUTOMASKED,
			  irq_info.flags & (VFIO_IRQ_INFO_EVENTFD |
					    VFIO_IRQ_INFO_MASKABLE |
					    VFIO_IRQ_INFO_AUTOMASKED));
	}
}

/*
 * PCI Command register configuration belongs to the PCI configuration space
 * region (VFIO_PCI_CONFIG_REGION_INDEX), which is accessed via the underlying
 * VFIO device descriptor (vfio_fd). In guest_memfd mode, bar_fd refers to the
 * guest_memfd descriptor representing memory BARs and cannot be used to read or
 * write PCI configuration space.
 */
static void vfio_pci_enable_memory_and_bus_master(struct __test_metadata *_metadata,
						  int vfio_fd)
{
	struct vfio_region_info cfg_info = {};
	ssize_t ret_bytes;
	uint16_t cmd;
	int ret;

	cfg_info.argsz = sizeof(cfg_info);
	cfg_info.index = VFIO_PCI_CONFIG_REGION_INDEX;
	ret = ioctl(vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &cfg_info);
	ASSERT_EQ(0, ret);

	/* Enable Memory Space and Bus Master in PCI Command register */
	ret_bytes = pread(vfio_fd, &cmd, sizeof(cmd), cfg_info.offset + PCI_COMMAND);
	ASSERT_EQ(sizeof(cmd), ret_bytes);
	cmd |= (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
	ret_bytes = pwrite(vfio_fd, &cmd, sizeof(cmd), cfg_info.offset + PCI_COMMAND);
	ASSERT_EQ(sizeof(cmd), ret_bytes);
}

TEST_F(guest_memfd_vfio, bar_rw)
{
	struct vfio_region_info bar0_info = {};
	ssize_t ret_bytes;
	uint32_t val;
	int ret;

	vfio_pci_enable_memory_and_bus_master(_metadata, self->vfio_fd);

	bar0_info.argsz = sizeof(bar0_info);
	bar0_info.index = VFIO_PCI_BAR0_REGION_INDEX;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0_info);
	ASSERT_EQ(0, ret);

	/*
	 * Read Device Status (E1000_STATUS) via pread() at BAR0 offset.
	 * Confirm device is readable and does not return 0xFFFFFFFF.
	 */
	val = 0;
	ret_bytes = pread(self->bar_fd, &val, sizeof(val), bar0_info.offset + E1000_STATUS);
	ASSERT_EQ(sizeof(val), ret_bytes);
	ASSERT_NE(0xFFFFFFFF, val);

	/*
	 * Write a distinct scratch test pattern to RAL0 via pwrite() and read
	 * it back via pread().
	 */
	val = 0xdeadbeef;
	ret_bytes = pwrite(self->bar_fd, &val, sizeof(val), bar0_info.offset + E1000_RAL0);
	ASSERT_EQ(sizeof(val), ret_bytes);

	val = 0;
	ret_bytes = pread(self->bar_fd, &val, sizeof(val), bar0_info.offset + E1000_RAL0);
	ASSERT_EQ(sizeof(val), ret_bytes);
	ASSERT_EQ(0xdeadbeef, val);
}

TEST_F(guest_memfd_vfio, bar_mmap)
{
	struct vfio_region_info bar0_info = {};
	uint32_t val;
	void *bar0;
	int ret;

	vfio_pci_enable_memory_and_bus_master(_metadata, self->vfio_fd);

	bar0_info.argsz = sizeof(bar0_info);
	bar0_info.index = VFIO_PCI_BAR0_REGION_INDEX;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0_info);
	ASSERT_EQ(0, ret);
	ASSERT_TRUE(bar0_info.flags & VFIO_REGION_INFO_FLAG_MMAP);

	bar0 = mmap(NULL, bar0_info.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		    self->bar_fd, bar0_info.offset);
	ASSERT_NE(MAP_FAILED, bar0);

	/*
	 * Read Device Status (E1000_STATUS) via mapped pointer. Confirm device
	 * is readable and does not return 0xFFFFFFFF.
	 */
	val = READ_ONCE(*(uint32_t *)((char *)bar0 + E1000_STATUS));
	ASSERT_NE(0xFFFFFFFF, val);

	/*
	 * Write a distinct scratch test pattern to RAL0 via mapped pointer and
	 * read it back.
	 */
	WRITE_ONCE(*(uint32_t *)((char *)bar0 + E1000_RAL0), 0xdeadbeef);
	val = READ_ONCE(*(uint32_t *)((char *)bar0 + E1000_RAL0));
	ASSERT_EQ(0xdeadbeef, val);

	ASSERT_EQ(0, munmap(bar0, bar0_info.size));
}

TEST_F(guest_memfd_vfio, device_reset)
{
	struct vfio_region_info bar0_info = {};
	uint32_t val;
	void *bar0;
	int retries = 20;
	int ret;

	vfio_pci_enable_memory_and_bus_master(_metadata, self->vfio_fd);

	bar0_info.argsz = sizeof(bar0_info);
	bar0_info.index = VFIO_PCI_BAR0_REGION_INDEX;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0_info);
	ASSERT_EQ(0, ret);

	bar0 = mmap(NULL, bar0_info.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		    self->bar_fd, bar0_info.offset);
	ASSERT_NE(MAP_FAILED, bar0);

	/* Write scratch test pattern to RAL0 */
	WRITE_ONCE(*(uint32_t *)((char *)bar0 + E1000_RAL0), 0xdeadbeef);
	val = READ_ONCE(*(uint32_t *)((char *)bar0 + E1000_RAL0));
	ASSERT_EQ(0xdeadbeef, val);

	/* Trigger VFIO_DEVICE_RESET, handling possible lock contention (-EAGAIN) */
	do {
		ret = ioctl(self->vfio_fd, VFIO_DEVICE_RESET, NULL);
		if (ret != 0 && errno == EAGAIN)
			usleep(10000);
	} while (ret != 0 && errno == EAGAIN && retries-- > 0);
	ASSERT_EQ(0, ret);

	/*
	 * After device reset, PCI Command register memory decode is cleared by
	 * hardware. Re-enable it to access BAR0 registers again.
	 */
	vfio_pci_enable_memory_and_bus_master(_metadata, self->vfio_fd);

	/* Verify RAL0 was reset and reverted from scratch pattern */
	val = READ_ONCE(*(uint32_t *)((char *)bar0 + E1000_RAL0));
	ASSERT_NE(0xdeadbeef, val);

	ASSERT_EQ(0, munmap(bar0, bar0_info.size));
}

TEST_F(guest_memfd_vfio, low_power_bar_revocation)
{
	struct vfio_device_feature feat = {};
	struct vfio_region_info bar0_info = {};
	ssize_t ret_bytes;
	uint32_t val;
	void *bar0;
	int ret;

	vfio_pci_enable_memory_and_bus_master(_metadata, self->vfio_fd);

	bar0_info.argsz = sizeof(bar0_info);
	bar0_info.index = VFIO_PCI_BAR0_REGION_INDEX;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0_info);
	ASSERT_EQ(0, ret);
	ASSERT_TRUE(bar0_info.flags & VFIO_REGION_INFO_FLAG_MMAP);

	bar0 = mmap(NULL, bar0_info.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		    self->bar_fd, bar0_info.offset);
	ASSERT_NE(MAP_FAILED, bar0);

	/* Probe support for VFIO_DEVICE_FEATURE_LOW_POWER_ENTRY */
	feat.argsz = sizeof(feat);
	feat.flags = VFIO_DEVICE_FEATURE_PROBE |
		     VFIO_DEVICE_FEATURE_SET |
		     VFIO_DEVICE_FEATURE_LOW_POWER_ENTRY;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_FEATURE, &feat);
	if (ret != 0) {
		ASSERT_EQ(0, munmap(bar0, bar0_info.size));
		SKIP(return, "LOW_POWER_ENTRY feature not supported on device");
	}

	/* Populate mapping and write test pattern to scratch register */
	WRITE_ONCE(*(uint32_t *)((char *)bar0 + E1000_RAL0), 0xdeadbeef);
	val = READ_ONCE(*(uint32_t *)((char *)bar0 + E1000_RAL0));
	ASSERT_EQ(0xdeadbeef, val);

	/* Enter low-power D3hot state */
	feat.argsz = sizeof(feat);
	feat.flags = VFIO_DEVICE_FEATURE_SET |
		     VFIO_DEVICE_FEATURE_LOW_POWER_ENTRY;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_FEATURE, &feat);
	ASSERT_EQ(0, ret);

	/*
	 * In low-power state, BARs are marked unavailable and mappings are
	 * revoked.
	 *
	 * - Host mmap accesses trigger SIGBUS because page tables were zapped
	 *   and get_pfn fails with -EFAULT while in low power state.
	 * - Host pread() and pwrite() on the BAR descriptor fail with -EIO.
	 */
	TEST_EXPECT_SIGBUS((void)READ_ONCE(*(uint32_t *)((char *)bar0 + E1000_RAL0)));
	TEST_EXPECT_SIGBUS(WRITE_ONCE(*(uint32_t *)((char *)bar0 + E1000_RAL0), 0x12345678));

	val = 0;
	ret_bytes = pread(self->bar_fd, &val, sizeof(val), bar0_info.offset + E1000_STATUS);
	ASSERT_EQ(-1, ret_bytes);
	ASSERT_EQ(EIO, errno);

	val = 0x12345678;
	ret_bytes = pwrite(self->bar_fd, &val, sizeof(val), bar0_info.offset + E1000_RAL0);
	ASSERT_EQ(-1, ret_bytes);
	ASSERT_EQ(EIO, errno);

	/* Exit low-power state back to D0 */
	feat.argsz = sizeof(feat);
	feat.flags = VFIO_DEVICE_FEATURE_SET |
		     VFIO_DEVICE_FEATURE_LOW_POWER_EXIT;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_FEATURE, &feat);
	ASSERT_EQ(0, ret);

	/*
	 * Access must succeed again after returning to active D0.
	 */
	val = READ_ONCE(*(uint32_t *)((char *)bar0 + E1000_RAL0));
	ASSERT_EQ(0xdeadbeef, val);

	val = 0;
	ret_bytes = pread(self->bar_fd, &val, sizeof(val), bar0_info.offset + E1000_STATUS);
	ASSERT_EQ(sizeof(val), ret_bytes);
	ASSERT_NE(0xFFFFFFFF, val);

	ASSERT_EQ(0, munmap(bar0, bar0_info.size));
}


static void verify_smaps_vm_flags(struct __test_metadata *_metadata,
				  void *addr, const char *flag_substr)
{
	unsigned long vma_start, vma_end;
	bool found_flag = false;
	bool in_vma = false;
	char *line = NULL;
	size_t size = 0;
	FILE *file;

	file = fopen("/proc/self/smaps", "r");
	ASSERT_NE(NULL, file);

	while (getline(&line, &size, file) > 0) {
		if (sscanf(line, "%lx-%lx", &vma_start, &vma_end) == 2) {
			if ((unsigned long)addr >= vma_start &&
			    (unsigned long)addr < vma_end)
				in_vma = true;
			else
				in_vma = false;
			continue;
		}

		if (in_vma && !strncmp(line, "VmFlags:", 8)) {
			if (strstr(line, flag_substr))
				found_flag = true;
			break;
		}
	}

	free(line);
	fclose(file);

	ASSERT_TRUE(found_flag);
}

TEST_F(guest_memfd_vfio, host_vma_flags)
{
	struct vfio_region_info bar0_info = {};
	void *bar0_mmap;
	int ret;

	bar0_info.argsz = sizeof(bar0_info);
	bar0_info.index = VFIO_PCI_BAR0_REGION_INDEX;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0_info);
	ASSERT_EQ(0, ret);
	ASSERT_TRUE(bar0_info.flags & VFIO_REGION_INFO_FLAG_MMAP);

	/* mmap the VFIO BAR directly into host userspace */
	bar0_mmap = mmap(NULL, bar0_info.size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, self->bar_fd, bar0_info.offset);
	ASSERT_NE(MAP_FAILED, bar0_mmap);

	/*
	 * Verify host VMA configuration in /proc/self/smaps:
	 * VFIO MMIO mappings must have io, pf, and de flags set.
	 */
	verify_smaps_vm_flags(_metadata, bar0_mmap, "io");
	verify_smaps_vm_flags(_metadata, bar0_mmap, "pf");
	verify_smaps_vm_flags(_metadata, bar0_mmap, "de");

	ASSERT_EQ(0, munmap(bar0_mmap, bar0_info.size));
}

static void guest_memslot_cacheability_code(uint64_t bar_gpa)
{
	uint32_t *reg = (uint32_t *)(bar_gpa + E1000_STATUS);
	uint32_t val;

	val = READ_ONCE(*reg);
	GUEST_ASSERT(val != 0xFFFFFFFF);

	GUEST_DONE();
}

TEST_F(guest_memfd_vfio, guest_mapping)
{
	struct vfio_region_info bar0_info = {};
	uint64_t bar_gpa = 0xc0000000;
	struct kvm_vcpu *vcpu;
	void *bar0_mmap;
	uint64_t *pte;
	int ret;

	vfio_pci_enable_memory_and_bus_master(_metadata, self->vfio_fd);

	bar0_info.argsz = sizeof(bar0_info);
	bar0_info.index = VFIO_PCI_BAR0_REGION_INDEX;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0_info);
	ASSERT_EQ(0, ret);
	ASSERT_TRUE(bar0_info.flags & VFIO_REGION_INFO_FLAG_MMAP);

	/* mmap the VFIO BAR directly into host userspace for memslot config */
	bar0_mmap = mmap(NULL, bar0_info.size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, self->bar_fd, bar0_info.offset);
	ASSERT_NE(MAP_FAILED, bar0_mmap);

	vcpu = vm_vcpu_add(self->vm, 0, guest_memslot_cacheability_code);
	if (variant->use_gmem)
		vm_set_user_memory_region2(self->vm, 1, KVM_MEM_GUEST_MEMFD,
					   bar_gpa, bar0_info.size, bar0_mmap,
					   self->gmem_fd, bar0_info.offset);
	else
		vm_set_user_memory_region(self->vm, 1, 0, bar_gpa,
					  bar0_info.size, bar0_mmap);

	virt_map(self->vm, bar_gpa, bar_gpa, bar0_info.size >> self->vm->page_shift);

	/* Run guest to fault in the page via stage 2, then verify S2 PTE */
	vcpu_args_set(vcpu, 1, bar_gpa);
	vcpu_run(vcpu);
	ASSERT_EQ(UCALL_DONE, get_ucall(vcpu, NULL));

	pte = vm_get_pte(self->vm, bar_gpa);
	ASSERT_NE(NULL, pte);
	ASSERT_TRUE(is_present_pte(&self->vm->mmu, pte));

	ASSERT_EQ(0, munmap(bar0_mmap, bar0_info.size));
}

TEST_F(guest_memfd_vfio, dmabuf_creation)
{
	struct vfio_device_feature_dma_buf *dmabuf_feature;
	struct vfio_device_feature probe_feat = {};
	struct vfio_region_info bar0_info = {};
	struct vfio_device_feature *feat;
	size_t feat_size;
	int dmabuf_fd;
	int ret;

	/*
	 * Probe support for VFIO_DEVICE_FEATURE_DMA_BUF. This requires kernel
	 * support for CONFIG_VFIO_PCI_DMABUF (CONFIG_PCI_P2PDMA). In guest_memfd
	 * mode, VFIO_DEVICE_BIND_FLAG_CTRL_ONLY is active, so the probe itself
	 * is rejected with -EOPNOTSUPP by the control-only allowlist.
	 */
	probe_feat.argsz = sizeof(probe_feat);
	probe_feat.flags = VFIO_DEVICE_FEATURE_PROBE |
			   VFIO_DEVICE_FEATURE_GET |
			   VFIO_DEVICE_FEATURE_DMA_BUF;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_FEATURE, &probe_feat);
	if (variant->use_gmem) {
		ASSERT_LT(ret, 0);
		ASSERT_EQ(EOPNOTSUPP, errno);
	} else if (ret != 0) {
		SKIP(return, "Kernel lacks CONFIG_VFIO_PCI_DMABUF support\n");
	}

	bar0_info.argsz = sizeof(bar0_info);
	bar0_info.index = VFIO_PCI_BAR0_REGION_INDEX;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0_info);
	ASSERT_EQ(0, ret);

	/*
	 * Allocate vfio_device_feature with trailing vfio_device_feature_dma_buf
	 * containing one dma_range covering BAR0.
	 */
	feat_size = sizeof(*feat) + sizeof(*dmabuf_feature) +
		    sizeof(struct vfio_region_dma_range);
	feat = calloc(1, feat_size);
	ASSERT_NE(NULL, feat);

	feat->argsz = feat_size;
	feat->flags = VFIO_DEVICE_FEATURE_GET | VFIO_DEVICE_FEATURE_DMA_BUF;

	dmabuf_feature = (struct vfio_device_feature_dma_buf *)feat->data;
	dmabuf_feature->region_index = VFIO_PCI_BAR0_REGION_INDEX;
	dmabuf_feature->open_flags = O_RDWR | O_CLOEXEC;
	dmabuf_feature->nr_ranges = 1;
	dmabuf_feature->dma_ranges[0].offset = 0;
	dmabuf_feature->dma_ranges[0].length = bar0_info.size;

	dmabuf_fd = ioctl(self->vfio_fd, VFIO_DEVICE_FEATURE, feat);
	if (variant->use_gmem) {
		ASSERT_LT(dmabuf_fd, 0);
		ASSERT_EQ(EOPNOTSUPP, errno);
	} else {
		ASSERT_GE(dmabuf_fd, 0);

		/*
		 * VFIO PCI dmabufs are intended for peer-to-peer kernel DMA and
		 * do not implement mmap or read operations. Verify mmap returns
		 * -EINVAL.
		 */
		ASSERT_EQ(MAP_FAILED, mmap(NULL, bar0_info.size,
					   PROT_READ | PROT_WRITE,
					   MAP_SHARED, dmabuf_fd, 0));
		ASSERT_EQ(EINVAL, errno);
		close(dmabuf_fd);
	}

	free(feat);
}

TEST_F(guest_memfd_vfio, ioeventfd_creation)
{
	struct vfio_device_ioeventfd ioeventfd = {};
	struct vfio_region_info bar0_info = {};
	int ret;
	int efd;

	bar0_info.argsz = sizeof(bar0_info);
	bar0_info.index = VFIO_PCI_BAR0_REGION_INDEX;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0_info);
	ASSERT_EQ(0, ret);

	efd = eventfd(0, EFD_CLOEXEC);
	ASSERT_GE(efd, 0);

	/* Register ioeventfd on BAR0 targeting RAL0 with 4-byte width */
	ioeventfd.argsz = sizeof(ioeventfd);
	ioeventfd.flags = VFIO_DEVICE_IOEVENTFD_32;
	ioeventfd.offset = bar0_info.offset + E1000_RAL0;
	ioeventfd.data = 0xdeadbeef;
	ioeventfd.fd = efd;

	ret = ioctl(self->vfio_fd, VFIO_DEVICE_IOEVENTFD, &ioeventfd);
	if (variant->use_gmem) {
		ASSERT_LT(ret, 0);
		ASSERT_EQ(EOPNOTSUPP, errno);
	} else {
		ASSERT_EQ(0, ret);

		/* Unregister ioeventfd by passing fd = -1 with identical match criteria */
		ioeventfd.fd = -1;
		ret = ioctl(self->vfio_fd, VFIO_DEVICE_IOEVENTFD, &ioeventfd);
		ASSERT_EQ(0, ret);
	}

	close(efd);
}

TEST_F(guest_memfd_vfio, conversions)
{
	struct vfio_region_info bar0_info = {};
	struct vfio_region_info bar3_info = {};
	uint64_t err_offset = 0;
	int ret;

	if (!variant->use_gmem)
		SKIP(return, "Attribute conversions only apply to guest_memfd");

	bar0_info.argsz = sizeof(bar0_info);
	bar0_info.index = VFIO_PCI_BAR0_REGION_INDEX;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0_info);
	ASSERT_EQ(0, ret);

	bar3_info.argsz = sizeof(bar3_info);
	bar3_info.index = VFIO_PCI_BAR3_REGION_INDEX;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &bar3_info);
	ASSERT_EQ(0, ret);

	/*
	 * BAR0 is an operational MMIO register space with no MSI-X table.
	 * Converting its pages to private should succeed.
	 */
	ret = __gmem_set_private(self->gmem_fd, bar0_info.offset,
				 bar0_info.size, &err_offset);
	ASSERT_EQ(0, ret);

	/* Converting back to shared should succeed. */
	ret = __gmem_set_shared(self->gmem_fd, bar0_info.offset,
				bar0_info.size, &err_offset);
	ASSERT_EQ(0, ret);

	/*
	 * BAR3 contains the MSI-X vector table and PBA. Disallowing private
	 * mappings of the MSI-X table is enforced lazily via NO_PRIVATE PFN
	 * attributes rather than eagerly rejecting software attribute conversion.
	 * Converting BAR3 attributes to private in guest_memfd succeeds, but
	 * attempting to fault the MSI-X page into the guest as private is
	 * rejected with -EFAULT in kvm_gmem_get_pfn(), causing KVM to exit with
	 * KVM_EXIT_MEMORY_FAULT.
	 */
	ret = __gmem_set_private(self->gmem_fd, bar3_info.offset,
				 bar3_info.size, &err_offset);
	ASSERT_EQ(0, ret);

	/* Shared conversion on BAR3 should also succeed. */
	ret = __gmem_set_shared(self->gmem_fd, bar3_info.offset,
				bar3_info.size, &err_offset);
	ASSERT_EQ(0, ret);
}

static uint32_t guest_write_val;

static void guest_rw_test_code(uint64_t bar_gpa)
{
	uint32_t *ral0 = (uint32_t *)(bar_gpa + E1000_RAL0);
	uint32_t *status = (uint32_t *)(bar_gpa + E1000_STATUS);
	uint32_t val;

	for (;;) {
		uint32_t write_val = READ_ONCE(guest_write_val);

		/* Read read-only device STATUS register */
		val = READ_ONCE(*status);
		GUEST_ASSERT(val != 0xFFFFFFFF);

		/* Write and read-back read-write RAL0 register */
		WRITE_ONCE(*ral0, write_val);
		val = READ_ONCE(*ral0);
		GUEST_ASSERT_EQ(val, write_val);

		GUEST_SYNC(0);
	}
}

TEST_F(guest_memfd_vfio, mmap_shared_private_rw)
{
	struct vfio_region_info bar0_info = {};
	uint64_t bar_gpa = 0xc0000000;
	struct kvm_vcpu *vcpu;
	void *bar0_mmap;
	uint32_t *h_ral0;
	uint32_t *h_status;
	uint32_t val;
	int ret;

	if (!variant->use_gmem)
		SKIP(return, "Shared and private attribute rw test only applies to guest_memfd");

	vfio_pci_enable_memory_and_bus_master(_metadata, self->vfio_fd);

	bar0_info.argsz = sizeof(bar0_info);
	bar0_info.index = VFIO_PCI_BAR0_REGION_INDEX;
	ret = ioctl(self->vfio_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0_info);
	ASSERT_EQ(0, ret);
	ASSERT_TRUE(bar0_info.flags & VFIO_REGION_INFO_FLAG_MMAP);

	/* mmap the VFIO BAR through guest_memfd descriptor */
	bar0_mmap = mmap(NULL, bar0_info.size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, self->bar_fd, bar0_info.offset);
	ASSERT_NE(MAP_FAILED, bar0_mmap);

	h_status = (uint32_t *)((uintptr_t)bar0_mmap + E1000_STATUS);
	h_ral0 = (uint32_t *)((uintptr_t)bar0_mmap + E1000_RAL0);

	/* Configure KVM guest memslot backed by guest_memfd */
	vcpu = vm_vcpu_add(self->vm, 0, guest_rw_test_code);
	vm_set_user_memory_region2(self->vm, 1, KVM_MEM_GUEST_MEMFD,
				   bar_gpa, bar0_info.size, bar0_mmap,
				   self->gmem_fd, bar0_info.offset);
	virt_map(self->vm, bar_gpa, bar_gpa, bar0_info.size >> self->vm->page_shift);

	/*
	 * Phase 1: Shared memory access from Host and Guest.
	 * Initially, the entire guest_memfd resource is shared.
	 */
	/* Host read from STATUS and write/read-back to RAL0 */
	val = READ_ONCE(*h_status);
	ASSERT_NE(0xFFFFFFFF, val);
	WRITE_ONCE(*h_ral0, 0x11223344);
	val = READ_ONCE(*h_ral0);
	ASSERT_EQ(0x11223344, val);

	/* Guest read/write to shared BAR memory */
	vcpu_args_set(vcpu, 1, bar_gpa);
	guest_write_val = 0xaabbccdd;
	sync_global_to_guest(self->vm, guest_write_val);
	vcpu_run(vcpu);
	ASSERT_EQ(UCALL_SYNC, get_ucall(vcpu, NULL));

	/* Verify host sees the value written by guest in shared mode */
	val = READ_ONCE(*h_ral0);
	ASSERT_EQ(0xaabbccdd, val);

	/*
	 * Phase 2: Convert BAR0 memory to Private.
	 */
	gmem_set_private(self->gmem_fd, bar0_info.offset, bar0_info.size);

	/*
	 * Host accesses to private guest_memfd memory must fail with SIGBUS.
	 */
	TEST_EXPECT_SIGBUS((void)READ_ONCE(*h_status));
	TEST_EXPECT_SIGBUS(WRITE_ONCE(*h_ral0, 0x12345678));

	/*
	 * Guest read and write access to private BAR memory must succeed.
	 */
	guest_write_val = 0x55667788;
	sync_global_to_guest(self->vm, guest_write_val);
	vcpu_run(vcpu);
	ASSERT_EQ(UCALL_SYNC, get_ucall(vcpu, NULL));

	/*
	 * Phase 3: Convert BAR0 memory back to Shared.
	 */
	gmem_set_shared(self->gmem_fd, bar0_info.offset, bar0_info.size);

	/* Host read/write access must now succeed again */
	val = READ_ONCE(*h_status);
	ASSERT_NE(0xFFFFFFFF, val);
	/* Host sees the value written by the guest while private */
	val = READ_ONCE(*h_ral0);
	ASSERT_EQ(0x55667788, val);

	WRITE_ONCE(*h_ral0, 0x99aabbcc);
	val = READ_ONCE(*h_ral0);
	ASSERT_EQ(0x99aabbcc, val);

	/* Guest read/write access continues to succeed in shared mode */
	guest_write_val = 0xdeedbeef;
	sync_global_to_guest(self->vm, guest_write_val);
	vcpu_run(vcpu);
	ASSERT_EQ(UCALL_SYNC, get_ucall(vcpu, NULL));

	val = READ_ONCE(*h_ral0);
	ASSERT_EQ(0xdeedbeef, val);

	ASSERT_EQ(0, munmap(bar0_mmap, bar0_info.size));
}

TEST_HARNESS_MAIN
