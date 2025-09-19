// SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause
// SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION. All rights reserved.

#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/cdev.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/ioport.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include "mlxbf_pka_dev.h"
#include "mlxbf_pka_ring.h"

#define MLXBF_PKA_DRIVER_DESCRIPTION		"BlueField PKA driver"

#define MLXBF_PKA_DEVICE_ACPIHID_BF1		"MLNXBF10"
#define MLXBF_PKA_RING_DEVICE_ACPIHID_BF1	"MLNXBF11"

#define MLXBF_PKA_DEVICE_ACPIHID_BF2		"MLNXBF20"
#define MLXBF_PKA_RING_DEVICE_ACPIHID_BF2	"MLNXBF21"

#define MLXBF_PKA_DEVICE_ACPIHID_BF3		"MLNXBF51"
#define MLXBF_PKA_RING_DEVICE_ACPIHID_BF3	"MLNXBF52"

#define MLXBF_PKA_DEVICE_ACCESS_MODE	0666
#define MLXBF_PKA_DEVICE_RES_CNT	7
#define MLXBF_PKA_DEVICE_NAME_MAX	14

enum mlxbf_pka_mem_res_idx {
	MLXBF_PKA_ACPI_EIP154_IDX = 0,
	MLXBF_PKA_ACPI_WNDW_RAM_IDX,
	MLXBF_PKA_ACPI_ALT_WNDW_RAM_0_IDX,
	MLXBF_PKA_ACPI_ALT_WNDW_RAM_1_IDX,
	MLXBF_PKA_ACPI_ALT_WNDW_RAM_2_IDX,
	MLXBF_PKA_ACPI_ALT_WNDW_RAM_3_IDX,
	MLXBF_PKA_ACPI_CSR_IDX
};

enum mlxbf_pka_plat_type {
	/* Platform type Bluefield-1. */
	MLXBF_PKA_PLAT_TYPE_BF1 = 0,
	/* Platform type Bluefield-2. */
	MLXBF_PKA_PLAT_TYPE_BF2,
	/* Platform type Bluefield-3. */
	MLXBF_PKA_PLAT_TYPE_BF3
};

struct mlxbf_pka_drv_plat_info {
	enum mlxbf_pka_plat_type type;
	u64 wndw_ram_off_mask;
};

static const struct mlxbf_pka_drv_plat_info mlxbf_pka_bf1_info = {
	.type = MLXBF_PKA_PLAT_TYPE_BF1,
	.wndw_ram_off_mask = MLXBF_PKA_WINDOW_RAM_OFFSET_BF1_BF2_MASK,
};

static const struct mlxbf_pka_drv_plat_info mlxbf_pka_bf2_info = {
	.type = MLXBF_PKA_PLAT_TYPE_BF2,
	.wndw_ram_off_mask = MLXBF_PKA_WINDOW_RAM_OFFSET_BF1_BF2_MASK,
};

static const struct mlxbf_pka_drv_plat_info mlxbf_pka_bf3_info = {
	.type = MLXBF_PKA_PLAT_TYPE_BF3,
	.wndw_ram_off_mask = MLXBF_PKA_WINDOW_RAM_OFFSET_BF3_MASK,
};

static DEFINE_MUTEX(mlxbf_pka_drv_lock);

static u32 mlxbf_pka_device_cnt;
static u32 mlxbf_pka_ring_device_cnt;

static const char mlxbf_pka_acpihid_bf1[] = MLXBF_PKA_DEVICE_ACPIHID_BF1;
static const char mlxbf_pka_ring_acpihid_bf1[] = MLXBF_PKA_RING_DEVICE_ACPIHID_BF1;

static const char mlxbf_pka_acpihid_bf2[] = MLXBF_PKA_DEVICE_ACPIHID_BF2;
static const char mlxbf_pka_ring_acpihid_bf2[] = MLXBF_PKA_RING_DEVICE_ACPIHID_BF2;

static const char mlxbf_pka_acpihid_bf3[] = MLXBF_PKA_DEVICE_ACPIHID_BF3;
static const char mlxbf_pka_ring_acpihid_bf3[] = MLXBF_PKA_RING_DEVICE_ACPIHID_BF3;

static const struct acpi_device_id mlxbf_pka_drv_acpi_ids[] = {
	{ MLXBF_PKA_DEVICE_ACPIHID_BF1, (kernel_ulong_t)&mlxbf_pka_bf1_info, 0, 0 },
	{ MLXBF_PKA_RING_DEVICE_ACPIHID_BF1, 0, 0, 0 },
	{ MLXBF_PKA_DEVICE_ACPIHID_BF2, (kernel_ulong_t)&mlxbf_pka_bf2_info, 0, 0 },
	{ MLXBF_PKA_RING_DEVICE_ACPIHID_BF2, 0, 0, 0 },
	{ MLXBF_PKA_DEVICE_ACPIHID_BF3, (kernel_ulong_t)&mlxbf_pka_bf3_info, 0, 0 },
	{ MLXBF_PKA_RING_DEVICE_ACPIHID_BF3, 0, 0, 0 },
	{},
};

static struct pka {
	struct idr ring_idr;
	/* PKA ring device IDR lock mutex. */
	struct mutex idr_lock;
} pka;

struct mlxbf_pka_info {
	/* The device this info struct belongs to. */
	struct device *dev;
	/* Device name. */
	const char *name;
	/* Device ACPI HID. */
	const char *acpihid;
	/* Device flags. */
	u8 flag;
	struct module *module;
	/* Optional private data. */
	void *priv;
};

/* Defines for mlxbf_pka_info->flags. */
#define MLXBF_PKA_DRIVER_FLAG_RING_DEVICE 1
#define MLXBF_PKA_DRIVER_FLAG_DEVICE 2

struct mlxbf_pka_platdata {
	struct platform_device *pdev;
	struct mlxbf_pka_info *info;
	/* Generic spinlock. */
	spinlock_t lock;
};

struct mlxbf_pka_ring_region {
	u64 off;
	u64 addr;
	resource_size_t size;
	u32 flags;
	u32 type;
};

/* Defines for mlxbf_pka_ring_region->flags. */
/* Region supports read. */
#define MLXBF_PKA_RING_REGION_FLAG_READ		BIT(0)
/* Region supports write. */
#define MLXBF_PKA_RING_REGION_FLAG_WRITE	BIT(1)
/* Region supports mmap. */
#define MLXBF_PKA_RING_REGION_FLAG_MMAP		BIT(2)

/* Defines for mlxbf_pka_ring_region->type. */
/* Info control/status words. */
#define MLXBF_PKA_RING_RES_TYPE_WORDS  1
/* Count registers. */
#define MLXBF_PKA_RING_RES_TYPE_CNTRS  2
/* Window RAM region. */
#define MLXBF_PKA_RING_RES_TYPE_MEM    4

#define MLXBF_PKA_DRIVER_RING_DEV_MAX  MLXBF_PKA_MAX_NUM_RINGS

/* Defines for region index. */
#define MLXBF_PKA_RING_REGION_WORDS_IDX		0
#define MLXBF_PKA_RING_REGION_CNTRS_IDX		1
#define MLXBF_PKA_RING_REGION_MEM_IDX	  2
#define MLXBF_PKA_RING_REGION_OFFSET_SHIFT     40
#define MLXBF_PKA_RING_REGION_INDEX_TO_OFFSET(index) \
	((u64)(index) << MLXBF_PKA_RING_REGION_OFFSET_SHIFT)

struct mlxbf_pka_ring_device {
	struct mlxbf_pka_info *info;
	struct device *device;
	u32 device_id;
	u32 parent_device_id;
	/* PKA ring device mutex. */
	struct mutex mutex;
	struct mlxbf_pka_dev_ring_t *ring;
	u32 num_regions;
	struct mlxbf_pka_ring_region *regions;
	struct miscdevice misc;
};

#define MLXBF_PKA_DRIVER_DEV_MAX MLXBF_PKA_MAX_NUM_IO_BLOCKS

struct mlxbf_pka_device {
	struct mlxbf_pka_info *info;
	struct device *device;
	u32 device_id;
	struct resource *resource[MLXBF_PKA_DEVICE_RES_CNT];
	struct mlxbf_pka_dev_shim_s *shim;
};

static int mlxbf_pka_drv_verify_bootup_status(struct device *dev)
{
	const char *bootup_status;
	int ret;

	ret = device_property_read_string(dev, "bootup_done", &bootup_status);
	if (ret < 0) {
		dev_err(dev, "failed to read bootup_done property\n");
		return ret;
	}

	if (strcmp(bootup_status, "true")) {
		dev_err(dev, "device bootup required\n");
		return -ENODEV;
	}

	return 0;
}

static int mlxbf_pka_drv_ring_regions_init(struct mlxbf_pka_ring_device *ring_dev)
{
	struct mlxbf_pka_ring_region *region;
	struct mlxbf_pka_dev_ring_t *ring;
	struct mlxbf_pka_dev_res_t *res;
	u32 num_regions;

	ring = ring_dev->ring;
	if (!ring || !ring->shim)
		return -ENXIO;

	num_regions = ring->resources_num;
	ring_dev->num_regions = num_regions;
	ring_dev->regions = devm_kcalloc(ring_dev->device,
					 num_regions,
					 sizeof(struct mlxbf_pka_ring_region),
					 GFP_KERNEL);
	if (!ring_dev->regions)
		return -ENOMEM;

	/* Information words region. */
	res = &ring->resources.info_words;
	region = &ring_dev->regions[MLXBF_PKA_RING_REGION_WORDS_IDX];
	/* Map offset to the physical address. */
	region->off = MLXBF_PKA_RING_REGION_INDEX_TO_OFFSET(MLXBF_PKA_RING_REGION_WORDS_IDX);
	region->addr = res->base;
	region->size = res->size;
	region->type = MLXBF_PKA_RING_RES_TYPE_WORDS;
	region->flags = MLXBF_PKA_RING_REGION_FLAG_MMAP |
			MLXBF_PKA_RING_REGION_FLAG_READ |
			MLXBF_PKA_RING_REGION_FLAG_WRITE;

	/* Counters registers region. */
	res = &ring->resources.counters;
	region = &ring_dev->regions[MLXBF_PKA_RING_REGION_CNTRS_IDX];
	/* Map offset to the physical address. */
	region->off = MLXBF_PKA_RING_REGION_INDEX_TO_OFFSET(MLXBF_PKA_RING_REGION_CNTRS_IDX);
	region->addr = res->base;
	region->size = res->size;
	region->type = MLXBF_PKA_RING_RES_TYPE_CNTRS;
	region->flags = MLXBF_PKA_RING_REGION_FLAG_MMAP |
			MLXBF_PKA_RING_REGION_FLAG_READ |
			MLXBF_PKA_RING_REGION_FLAG_WRITE;

	/* Window RAM region. */
	res = &ring->resources.window_ram;
	region = &ring_dev->regions[MLXBF_PKA_RING_REGION_MEM_IDX];
	/* Map offset to the physical address. */
	region->off = MLXBF_PKA_RING_REGION_INDEX_TO_OFFSET(MLXBF_PKA_RING_REGION_MEM_IDX);
	region->addr = res->base;
	region->size = res->size;
	region->type = MLXBF_PKA_RING_RES_TYPE_MEM;
	region->flags = MLXBF_PKA_RING_REGION_FLAG_MMAP |
			MLXBF_PKA_RING_REGION_FLAG_READ |
			MLXBF_PKA_RING_REGION_FLAG_WRITE;

	return 0;
}

static void mlxbf_pka_drv_ring_regions_cleanup(struct mlxbf_pka_ring_device *ring_dev)
{
	/* Clear PKA ring device regions. */
	ring_dev->num_regions = 0;
}

static int mlxbf_pka_drv_ring_open(void *device_data)
{
	struct mlxbf_pka_ring_device *ring_dev = device_data;
	struct mlxbf_pka_info *info = ring_dev->info;
	struct mlxbf_pka_ring_info_t ring_info;
	int ret;

	dev_dbg(ring_dev->device, "open ring device (device_data:%p)\n", ring_dev);

	if (!try_module_get(info->module))
		return -ENODEV;

	ring_info.ring_id = ring_dev->device_id;
	ret = mlxbf_pka_dev_open_ring(ring_dev->device, &ring_info);
	if (ret) {
		dev_dbg(ring_dev->device, "failed to open ring\n");
		goto exit_open_ring;
	}

	/* Initialize regions. */
	ret = mlxbf_pka_drv_ring_regions_init(ring_dev);
	if (ret)
		goto exit_ring_regions_init;

	return 0;

exit_ring_regions_init:
	mlxbf_pka_dev_close_ring(&ring_info);

exit_open_ring:
	module_put(info->module);

	return ret;
}

static void mlxbf_pka_drv_ring_release(void *device_data)
{
	struct mlxbf_pka_ring_device *ring_dev = device_data;
	struct mlxbf_pka_info *info = ring_dev->info;
	struct mlxbf_pka_ring_info_t ring_info;
	int ret;

	dev_dbg(ring_dev->device, "release ring device (device_data:%p)\n", ring_dev);

	mlxbf_pka_drv_ring_regions_cleanup(ring_dev);

	ring_info.ring_id = ring_dev->device_id;
	ret = mlxbf_pka_dev_close_ring(&ring_info);
	if (ret)
		dev_dbg(ring_dev->device, "failed to close ring\n");

	module_put(info->module);
}

static int mlxbf_pka_drv_ring_mmap_region(struct mlxbf_pka_ring_region region,
					  struct vm_area_struct *vma)
{
	u64 req_len, pgoff, req_start;

	req_len = vma->vm_end - vma->vm_start;
	pgoff = vma->vm_pgoff & ((1U << (MLXBF_PKA_RING_REGION_OFFSET_SHIFT - PAGE_SHIFT)) - 1);
	req_start = pgoff << PAGE_SHIFT;

	region.size = roundup(region.size, PAGE_SIZE);

	if (req_start + req_len > region.size)
		return -EINVAL;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vma->vm_pgoff = (region.addr >> PAGE_SHIFT) + pgoff;

	return remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff, req_len, vma->vm_page_prot);
}

static int mlxbf_pka_drv_ring_mmap(void *device_data, struct vm_area_struct *vma)
{
	struct mlxbf_pka_ring_device *ring_dev = device_data;
	struct mlxbf_pka_ring_region *region;
	unsigned int index;

	dev_dbg(ring_dev->device, "mmap device\n");

	index = vma->vm_pgoff >> (MLXBF_PKA_RING_REGION_OFFSET_SHIFT - PAGE_SHIFT);

	if (vma->vm_end < vma->vm_start)
		return -EINVAL;
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	if (index >= ring_dev->num_regions)
		return -EINVAL;
	if (vma->vm_start & ~PAGE_MASK)
		return -EINVAL;
	if (vma->vm_end & ~PAGE_MASK)
		return -EINVAL;

	region = &ring_dev->regions[index];

	if (!(region->flags & MLXBF_PKA_RING_REGION_FLAG_MMAP))
		return -EINVAL;

	if (!(region->flags & MLXBF_PKA_RING_REGION_FLAG_READ) && (vma->vm_flags & VM_READ))
		return -EINVAL;

	if (!(region->flags & MLXBF_PKA_RING_REGION_FLAG_WRITE) && (vma->vm_flags & VM_WRITE))
		return -EINVAL;

	vma->vm_private_data = ring_dev;

	if (region->type & MLXBF_PKA_RING_RES_TYPE_CNTRS ||
	    region->type & MLXBF_PKA_RING_RES_TYPE_MEM)
		return mlxbf_pka_drv_ring_mmap_region(ring_dev->regions[index], vma);

	if (region->type & MLXBF_PKA_RING_RES_TYPE_WORDS)
		/* Currently user space is not allowed to access this region. */
		return -EINVAL;

	return -EINVAL;
}

static long mlxbf_pka_drv_ring_ioctl(void *device_data, unsigned int cmd, unsigned long arg)
{
	struct mlxbf_pka_ring_device *ring_dev = device_data;

	if (cmd == MLXBF_PKA_RING_GET_REGION_INFO) {
		struct mlxbf_pka_dev_region_info info;

		info.mem_index = MLXBF_PKA_RING_REGION_MEM_IDX;
		info.mem_offset = ring_dev->regions[info.mem_index].off;
		info.mem_size = ring_dev->regions[info.mem_index].size;

		info.reg_index = MLXBF_PKA_RING_REGION_CNTRS_IDX;
		info.reg_offset = ring_dev->regions[info.reg_index].off;
		info.reg_size = ring_dev->regions[info.reg_index].size;

		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		else
			return 0;

	} else if (cmd == MLXBF_PKA_GET_RING_INFO) {
		struct mlxbf_pka_dev_hw_ring_info *this_ring_info;
		struct mlxbf_pka_dev_hw_ring_info  hw_ring_info;

		this_ring_info = ring_dev->ring->ring_info;

		hw_ring_info.cmd_base = this_ring_info->cmd_base;
		hw_ring_info.rslt_base = this_ring_info->rslt_base;
		hw_ring_info.size = this_ring_info->size;
		hw_ring_info.host_desc_size = this_ring_info->host_desc_size;
		hw_ring_info.in_order = this_ring_info->in_order;
		hw_ring_info.cmd_rd_ptr = this_ring_info->cmd_rd_ptr;
		hw_ring_info.rslt_wr_ptr = this_ring_info->rslt_wr_ptr;
		hw_ring_info.cmd_rd_stats = this_ring_info->cmd_rd_ptr;
		hw_ring_info.rslt_wr_stats = this_ring_info->rslt_wr_stats;

		if (copy_to_user((void __user *)arg, &hw_ring_info, sizeof(hw_ring_info)))
			return -EFAULT;
		else
			return 0;

	} else if (cmd == MLXBF_PKA_CLEAR_RING_COUNTERS) {
		return mlxbf_pka_dev_clear_ring_counters(ring_dev->ring);
	}

	return -ENOTTY;
}

static int mlxbf_pka_drv_open(struct inode *inode, struct file *filep)
{
	struct mlxbf_pka_ring_device *ring_dev;
	int ret;

	scoped_guard(mutex, &pka.idr_lock) {
		ring_dev = idr_find(&pka.ring_idr, iminor(inode));
	}
	if (!ring_dev) {
		pr_err("mlxbf_pka error: failed to find idr for device\n");
		return -ENODEV;
	}

	ret = mlxbf_pka_drv_ring_open(ring_dev);
	if (ret)
		return ret;

	filep->private_data = ring_dev;
	return ret;
}

static int mlxbf_pka_drv_release(struct inode *inode, struct file *filep)
{
	struct mlxbf_pka_ring_device *ring_dev = filep->private_data;

	filep->private_data = NULL;
	mlxbf_pka_drv_ring_release(ring_dev);

	return 0;
}

static int mlxbf_pka_drv_mmap(struct file *filep, struct vm_area_struct *vma)
{
	return mlxbf_pka_drv_ring_mmap(filep->private_data, vma);
}

static long mlxbf_pka_drv_unlocked_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	return mlxbf_pka_drv_ring_ioctl(filep->private_data, cmd, arg);
}

static const struct file_operations mlxbf_pka_ring_fops = {
	.owner = THIS_MODULE,
	.open = mlxbf_pka_drv_open,
	.release = mlxbf_pka_drv_release,
	.unlocked_ioctl = mlxbf_pka_drv_unlocked_ioctl,
	.mmap = mlxbf_pka_drv_mmap,
};

static int mlxbf_pka_drv_add_ring_device(struct mlxbf_pka_ring_device *ring_dev)
{
	struct device *dev = ring_dev->device;
	char name[MLXBF_PKA_DEVICE_NAME_MAX];
	int minor_number;
	int ret;

	scnprintf(name, sizeof(name), MLXBF_PKA_DEVFS_RING_DEVICES, ring_dev->device_id);

	ring_dev->misc.minor = MISC_DYNAMIC_MINOR;
	ring_dev->misc.name = &name[0];
	ring_dev->misc.mode = MLXBF_PKA_DEVICE_ACCESS_MODE;
	ring_dev->misc.fops = &mlxbf_pka_ring_fops;

	ret = misc_register(&ring_dev->misc);
	if (ret) {
		dev_err(dev, "ring device registration failed: ret=%d\n", ret);
		return ret;
	}

	scoped_guard(mutex, &pka.idr_lock) {
		minor_number = idr_alloc(&pka.ring_idr, ring_dev, ring_dev->misc.minor,
					 MINORMASK + 1, GFP_KERNEL);
	}
	if (minor_number != ring_dev->misc.minor) {
		dev_err(dev, "failed to allocate minor number %d\n", ring_dev->misc.minor);
		return minor_number;
	}

	dev_dbg(dev, "ring device minor:%d\n", ring_dev->misc.minor);

	return ret;
}

static struct mlxbf_pka_ring_device *mlxbf_pka_drv_del_ring_device(struct device *dev)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct mlxbf_pka_platdata *priv = platform_get_drvdata(pdev);
	struct mlxbf_pka_info *info = priv->info;
	struct mlxbf_pka_ring_device *ring_dev = info->priv;

	if (ring_dev) {
		scoped_guard(mutex, &pka.idr_lock) {
			idr_remove(&pka.ring_idr, ring_dev->misc.minor);
		}
		misc_deregister(&ring_dev->misc);
	}

	return ring_dev;
}

static void mlxbf_pka_drv_get_mem_res(struct mlxbf_pka_device *mlxbf_pka_dev,
				      struct mlxbf_pka_dev_mem_res *mem_res,
				      u64 wndw_ram_off_mask)
{
	enum mlxbf_pka_mem_res_idx acpi_mem_idx;

	acpi_mem_idx = MLXBF_PKA_ACPI_EIP154_IDX;
	mem_res->wndw_ram_off_mask = wndw_ram_off_mask;

	/* PKA EIP154 MMIO base address. */
	mem_res->eip154_base = mlxbf_pka_dev->resource[acpi_mem_idx]->start;
	mem_res->eip154_size = resource_size(mlxbf_pka_dev->resource[acpi_mem_idx]);
	acpi_mem_idx++;

	/* PKA window RAM base address. */
	mem_res->wndw_ram_base = mlxbf_pka_dev->resource[acpi_mem_idx]->start;
	mem_res->wndw_ram_size = resource_size(mlxbf_pka_dev->resource[acpi_mem_idx]);
	acpi_mem_idx++;

	/*
	 * PKA alternate window RAM base address.
	 * Note: the size of all the alt window RAM is the same, depicted by
	 * 'alt_wndw_ram_size' variable. All alt window RAM resources are read
	 * here even though not all of them are used currently.
	 */
	mem_res->alt_wndw_ram_0_base = mlxbf_pka_dev->resource[acpi_mem_idx]->start;
	mem_res->alt_wndw_ram_size = resource_size(mlxbf_pka_dev->resource[acpi_mem_idx]);

	if (mem_res->alt_wndw_ram_size != MLXBF_PKA_WINDOW_RAM_REGION_SIZE)
		dev_warn(mlxbf_pka_dev->device, "alternate Window RAM size from ACPI is wrong.\n");

	acpi_mem_idx++;

	mem_res->alt_wndw_ram_1_base = mlxbf_pka_dev->resource[acpi_mem_idx]->start;
	acpi_mem_idx++;

	mem_res->alt_wndw_ram_2_base = mlxbf_pka_dev->resource[acpi_mem_idx]->start;
	acpi_mem_idx++;

	mem_res->alt_wndw_ram_3_base = mlxbf_pka_dev->resource[acpi_mem_idx]->start;
	acpi_mem_idx++;

	/* PKA CSR base address. */
	mem_res->csr_base = mlxbf_pka_dev->resource[acpi_mem_idx]->start;
	mem_res->csr_size = resource_size(mlxbf_pka_dev->resource[acpi_mem_idx]);
}

/*
 * Note: this function must be serialized because it calls
 * 'mlxbf_pka_dev_register_shim' which manipulates common counters for the
 * PKA devices.
 */
static int mlxbf_pka_drv_register_device(struct mlxbf_pka_device *mlxbf_pka_dev,
					 u64 wndw_ram_off_mask)
{
	struct mlxbf_pka_dev_mem_res mem_res;
	u32 mlxbf_pka_shim_id;
	int ret;

	/* Assert that the driver lock is held for serialization */
	lockdep_assert_held(&mlxbf_pka_drv_lock);

	mlxbf_pka_shim_id = mlxbf_pka_dev->device_id;

	mlxbf_pka_drv_get_mem_res(mlxbf_pka_dev, &mem_res, wndw_ram_off_mask);

	ret = mlxbf_pka_dev_register_shim(mlxbf_pka_dev->device,
					  mlxbf_pka_shim_id,
					  &mem_res,
					  &mlxbf_pka_dev->shim);
	if (ret) {
		dev_dbg(mlxbf_pka_dev->device, "failed to register shim\n");
		return ret;
	}

	return 0;
}

static int mlxbf_pka_drv_unregister_device(struct mlxbf_pka_device *mlxbf_pka_dev)
{
	if (!mlxbf_pka_dev || !mlxbf_pka_dev->shim)
		return -EINVAL;

	dev_dbg(mlxbf_pka_dev->device, "unregister device shim\n");
	return mlxbf_pka_dev_unregister_shim(mlxbf_pka_dev->device, mlxbf_pka_dev->shim);
}

/*
 * Note: this function must be serialized because it calls
 * 'mlxbf_pka_dev_register_ring' which manipulates common counters for the PKA
 * ring devices.
 */
static int mlxbf_pka_drv_register_ring_device(struct mlxbf_pka_ring_device *ring_dev)
{
	u32 shim_id = ring_dev->parent_device_id;
	u32 ring_id = ring_dev->device_id;
	int ret;

	ret = mlxbf_pka_dev_register_ring(ring_dev->device, ring_id, shim_id, &ring_dev->ring);
	if (ret) {
		dev_dbg(ring_dev->device, "failed to register ring device\n");
		return ret;
	}

	return 0;
}

static void mlxbf_pka_drv_unregister_ring_device(struct mlxbf_pka_ring_device *ring_dev)
{
	if (!ring_dev)
		return;

	if (!ring_dev->ring)
		return;

	dev_dbg(ring_dev->device, "unregister ring device\n");
	mlxbf_pka_dev_unregister_ring(ring_dev->device, ring_dev->ring);
}

static int mlxbf_pka_drv_probe_device(struct mlxbf_pka_info *info)
{
	struct mlxbf_pka_drv_plat_info *plat_info;
	enum mlxbf_pka_mem_res_idx acpi_mem_idx;
	struct mlxbf_pka_device *mlxbf_pka_dev;
	const struct acpi_device_id *aid;
	struct platform_device *pdev;
	u64 wndw_ram_off_mask;
	struct device *dev;
	int ret;

	if (!info)
		return -EINVAL;

	dev = info->dev;
	pdev = to_platform_device(dev);

	mlxbf_pka_dev = devm_kzalloc(dev, sizeof(*mlxbf_pka_dev), GFP_KERNEL);
	if (!mlxbf_pka_dev)
		return -ENOMEM;

	scoped_guard(mutex, &mlxbf_pka_drv_lock) {
		mlxbf_pka_device_cnt += 1;
		if (mlxbf_pka_device_cnt > MLXBF_PKA_DRIVER_DEV_MAX) {
			dev_dbg(dev, "cannot support %u devices\n", mlxbf_pka_device_cnt);
			return -ENOSPC;
		}
		mlxbf_pka_dev->device_id = mlxbf_pka_device_cnt - 1;
	}

	mlxbf_pka_dev->info = info;
	mlxbf_pka_dev->device = dev;
	info->flag = MLXBF_PKA_DRIVER_FLAG_DEVICE;

	for (acpi_mem_idx = MLXBF_PKA_ACPI_EIP154_IDX;
	     acpi_mem_idx < MLXBF_PKA_DEVICE_RES_CNT;
	     acpi_mem_idx++) {
		mlxbf_pka_dev->resource[acpi_mem_idx] = platform_get_resource(pdev,
									      IORESOURCE_MEM,
									      acpi_mem_idx);
	}

	/* Verify PKA bootup status. */
	ret = mlxbf_pka_drv_verify_bootup_status(dev);
	if (ret)
		return ret;

	/* Window RAM offset mask is platform dependent. */
	aid = acpi_match_device(mlxbf_pka_drv_acpi_ids, dev);
	if (!aid)
		return -ENODEV;

	plat_info = (struct mlxbf_pka_drv_plat_info *)aid->driver_data;
	if (!plat_info) {
		dev_err(dev, "missing platform data\n");
		return -EINVAL;
	}

	wndw_ram_off_mask = plat_info->wndw_ram_off_mask;

	scoped_guard(mutex, &mlxbf_pka_drv_lock) {
		ret = mlxbf_pka_drv_register_device(mlxbf_pka_dev, wndw_ram_off_mask);
		if (ret) {
			dev_dbg(dev, "failed to register shim\n");
			return ret;
		}
	}

	info->priv = mlxbf_pka_dev;

	return 0;
}

static void mlxbf_pka_drv_remove_device(struct platform_device *pdev)
{
	struct mlxbf_pka_platdata *priv = platform_get_drvdata(pdev);
	struct mlxbf_pka_info *info = priv->info;
	struct mlxbf_pka_device *mlxbf_pka_dev = (struct mlxbf_pka_device *)info->priv;

	if (!mlxbf_pka_dev)
		return;

	mlxbf_pka_drv_unregister_device(mlxbf_pka_dev);
}

static int mlxbf_pka_drv_probe_ring_device(struct mlxbf_pka_info *info)
{
	struct mlxbf_pka_ring_device *ring_dev;
	struct device *dev = info->dev;
	int ret;

	if (!info)
		return -EINVAL;

	ring_dev = devm_kzalloc(dev, sizeof(*ring_dev), GFP_KERNEL);
	if (!ring_dev)
		return -ENOMEM;

	if (!mlxbf_pka_ring_device_cnt) {
		mutex_init(&pka.idr_lock);
		scoped_guard(mutex, &pka.idr_lock) {
			/* Only initialize IDR if there is no ring device registered. */
			idr_init(&pka.ring_idr);
		}
	}

	scoped_guard(mutex, &mlxbf_pka_drv_lock) {
		mlxbf_pka_ring_device_cnt += 1;
		if (mlxbf_pka_ring_device_cnt > MLXBF_PKA_DRIVER_RING_DEV_MAX) {
			dev_dbg(dev, "cannot support %u ring devices\n", mlxbf_pka_ring_device_cnt);
			return -ENOSPC;
		}
		ring_dev->device_id = mlxbf_pka_ring_device_cnt - 1;
		ring_dev->parent_device_id = mlxbf_pka_device_cnt - 1;
	}

	ring_dev->info = info;
	ring_dev->device = dev;
	info->flag = MLXBF_PKA_DRIVER_FLAG_RING_DEVICE;
	mutex_init(&ring_dev->mutex);

	/* Verify PKA bootup status. */
	ret = mlxbf_pka_drv_verify_bootup_status(dev);
	if (ret)
		return ret;

	scoped_guard(mutex, &mlxbf_pka_drv_lock) {
		/* Add PKA ring device. */
		ret = mlxbf_pka_drv_add_ring_device(ring_dev);
		if (ret) {
			dev_dbg(dev, "failed to add ring device %u\n", ring_dev->device_id);
			return ret;
		}

		/* Register PKA ring device. */
		ret = mlxbf_pka_drv_register_ring_device(ring_dev);
		if (ret) {
			dev_dbg(dev, "failed to register ring device\n");
			goto err_register_ring;
		}
	}

	info->priv = ring_dev;

	return 0;

 err_register_ring:
	mlxbf_pka_drv_del_ring_device(dev);
	return ret;
}

static void mlxbf_pka_drv_remove_ring_device(struct platform_device *pdev)
{
	struct mlxbf_pka_ring_device *ring_dev;
	struct device *dev = &pdev->dev;

	ring_dev = mlxbf_pka_drv_del_ring_device(dev);
	if (ring_dev) {
		mlxbf_pka_drv_unregister_ring_device(ring_dev);
		mlxbf_pka_ring_device_cnt--;
	}

	if (!mlxbf_pka_ring_device_cnt) {
		scoped_guard(mutex, &pka.idr_lock) {
			/* Only destroy IDR if there is no ring device registered. */
			idr_destroy(&pka.ring_idr);
		}
	}
}

static int mlxbf_pka_drv_acpi_probe(struct platform_device *pdev, struct mlxbf_pka_info *info)
{
	struct device *dev = &pdev->dev;
	struct acpi_device *adev;
	int ret;

	if (acpi_disabled)
		return -ENOENT;

	adev = ACPI_COMPANION(dev);
	if (!adev) {
		dev_dbg(dev, "ACPI companion device not found for %s\n", pdev->name);
		return -ENODEV;
	}

	info->acpihid = acpi_device_hid(adev);
	if (WARN_ON(!info->acpihid))
		return -EINVAL;

	if (!strcmp(info->acpihid, mlxbf_pka_acpihid_bf1) ||
	    !strcmp(info->acpihid, mlxbf_pka_acpihid_bf2) ||
	    !strcmp(info->acpihid, mlxbf_pka_acpihid_bf3)) {
		ret = mlxbf_pka_drv_probe_device(info);
		if (ret) {
			dev_dbg(dev, "failed to register device\n");
			return ret;
		}
		dev_info(dev, "device probed\n");
	} else if (!strcmp(info->acpihid, mlxbf_pka_ring_acpihid_bf1) ||
		   !strcmp(info->acpihid, mlxbf_pka_ring_acpihid_bf2) ||
		   !strcmp(info->acpihid, mlxbf_pka_ring_acpihid_bf3)) {
		ret = mlxbf_pka_drv_probe_ring_device(info);
		if (ret) {
			dev_dbg(dev, "failed to register ring device\n");
			return ret;
		}
		dev_dbg(dev, "ring device probed\n");
	}

	return 0;
}

static int mlxbf_pka_drv_probe(struct platform_device *pdev)
{
	struct mlxbf_pka_platdata *priv;
	struct device *dev = &pdev->dev;
	struct mlxbf_pka_info *info;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	spin_lock_init(&priv->lock);
	priv->pdev = pdev;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->name = pdev->name;
	info->module = THIS_MODULE;
	info->dev = dev;
	priv->info = info;

	platform_set_drvdata(pdev, priv);

	ret = mlxbf_pka_drv_acpi_probe(pdev, info);
	if (ret) {
		dev_dbg(dev, "unknown device\n");
		return ret;
	}

	return ret;
}

static void mlxbf_pka_drv_remove(struct platform_device *pdev)
{
	struct mlxbf_pka_platdata *priv = platform_get_drvdata(pdev);
	struct mlxbf_pka_info *info = priv->info;

	if (info->flag == MLXBF_PKA_DRIVER_FLAG_RING_DEVICE) {
		dev_info(&pdev->dev, "remove ring device\n");
		mlxbf_pka_drv_remove_ring_device(pdev);
	}

	if (info->flag == MLXBF_PKA_DRIVER_FLAG_DEVICE) {
		dev_info(&pdev->dev, "remove PKA device\n");
		mlxbf_pka_drv_remove_device(pdev);
	}
}

MODULE_DEVICE_TABLE(acpi, mlxbf_pka_drv_acpi_ids);

static struct platform_driver mlxbf_pka_drv = {
	.driver = {
		   .name = KBUILD_MODNAME,
		   .acpi_match_table = ACPI_PTR(mlxbf_pka_drv_acpi_ids),
		  },
	.probe = mlxbf_pka_drv_probe,
	.remove = mlxbf_pka_drv_remove,
};

module_platform_driver(mlxbf_pka_drv);
MODULE_DESCRIPTION(MLXBF_PKA_DRIVER_DESCRIPTION);
MODULE_AUTHOR("Ron Li <xiangrongl@nvidia.com>");
MODULE_AUTHOR("Khalil Blaiech <kblaiech@nvidia.com>");
MODULE_AUTHOR("Mahantesh Salimath <mahantesh@nvidia.com>");
MODULE_AUTHOR("Shih-Yi Chen <shihyic@nvidia.com>");
MODULE_LICENSE("Dual BSD/GPL");
