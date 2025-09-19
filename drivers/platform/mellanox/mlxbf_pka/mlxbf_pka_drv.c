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

#define MLXBF_PKA_DRIVER_DESCRIPTION		"BlueField PKA driver"

#define MLXBF_PKA_DEVICE_ACPIHID_BF1		"MLNXBF10"

#define MLXBF_PKA_DEVICE_ACPIHID_BF2		"MLNXBF20"

#define MLXBF_PKA_DEVICE_ACPIHID_BF3		"MLNXBF51"

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

static const char mlxbf_pka_acpihid_bf1[] = MLXBF_PKA_DEVICE_ACPIHID_BF1;

static const char mlxbf_pka_acpihid_bf2[] = MLXBF_PKA_DEVICE_ACPIHID_BF2;

static const char mlxbf_pka_acpihid_bf3[] = MLXBF_PKA_DEVICE_ACPIHID_BF3;

static const struct acpi_device_id mlxbf_pka_drv_acpi_ids[] = {
	{ MLXBF_PKA_DEVICE_ACPIHID_BF1, (kernel_ulong_t)&mlxbf_pka_bf1_info, 0, 0 },
	{ MLXBF_PKA_DEVICE_ACPIHID_BF2, (kernel_ulong_t)&mlxbf_pka_bf2_info, 0, 0 },
	{ MLXBF_PKA_DEVICE_ACPIHID_BF3, (kernel_ulong_t)&mlxbf_pka_bf3_info, 0, 0 },
	{},
};

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
#define MLXBF_PKA_DRIVER_FLAG_DEVICE 2

struct mlxbf_pka_platdata {
	struct platform_device *pdev;
	struct mlxbf_pka_info *info;
	/* Generic spinlock. */
	spinlock_t lock;
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
