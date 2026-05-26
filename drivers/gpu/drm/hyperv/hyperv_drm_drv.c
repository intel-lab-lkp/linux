// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2021 Microsoft
 */

#include <linux/aperture.h>
#include <linux/efi.h>
#include <linux/hyperv.h>
#include <linux/module.h>
#include <linux/pci.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_simple_kms_helper.h>

#include "hyperv_drm.h"

#define DRIVER_NAME "hyperv_drm"
#define DRIVER_DESC "DRM driver for Hyper-V synthetic video device"
#define DRIVER_MAJOR 1
#define DRIVER_MINOR 0

DEFINE_DRM_GEM_FOPS(hv_fops);

static struct drm_driver hvdrm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,

	.name		 = DRIVER_NAME,
	.desc		 = DRIVER_DESC,
	.major		 = DRIVER_MAJOR,
	.minor		 = DRIVER_MINOR,

	.fops		 = &hv_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
};

static int hvdrm_pci_probe(struct pci_dev *pdev,
			    const struct pci_device_id *ent)
{
	return 0;
}

static void hvdrm_pci_remove(struct pci_dev *pdev)
{
}

static const struct pci_device_id hvdrm_pci_tbl[] = {
	{
		.vendor = PCI_VENDOR_ID_MICROSOFT,
		.device = PCI_DEVICE_ID_HYPERV_VIDEO,
	},
	{ /* end of list */ }
};

/*
 * PCI stub to support gen1 VM.
 */
static struct pci_driver hvdrm_pci_driver = {
	.name =		KBUILD_MODNAME,
	.id_table =	hvdrm_pci_tbl,
	.probe =	hvdrm_pci_probe,
	.remove =	hvdrm_pci_remove,
};

static int hvdrm_setup_vram(struct hvdrm_drm_device *hv,
			     struct hv_device *hdev)
{
	struct drm_device *dev = &hv->dev;
	int ret;

	hv->fb_size = (unsigned long)hv->mmio_megabytes * 1024 * 1024;

	ret = vmbus_allocate_mmio(&hv->mem, hdev, 0, -1, hv->fb_size, 0x100000,
				  true);
	if (ret) {
		drm_err(dev, "Failed to allocate mmio\n");
		return -ENOMEM;
	}

	/*
	 * Map the VRAM cacheable for performance. This is also required for VM
	 * connect to display properly for ARM64 Linux VM, as the host also maps
	 * the VRAM cacheable.
	 */
	hv->vram = ioremap_cache(hv->mem->start, hv->fb_size);
	if (!hv->vram) {
		drm_err(dev, "Failed to map vram\n");
		ret = -ENOMEM;
		goto error;
	}

	hv->fb_base = hv->mem->start;
	return 0;

error:
	vmbus_free_mmio(hv->mem->start, hv->fb_size);
	return ret;
}

static int hvdrm_vmbus_probe(struct hv_device *hdev,
			      const struct hv_vmbus_device_id *dev_id)
{
	struct hvdrm_drm_device *hv;
	struct drm_device *dev;
	int ret;

	hv = devm_drm_dev_alloc(&hdev->device, &hvdrm_driver,
				struct hvdrm_drm_device, dev);
	if (IS_ERR(hv))
		return PTR_ERR(hv);

	dev = &hv->dev;
	init_completion(&hv->wait);
	hv_set_drvdata(hdev, hv);
	hv->hdev = hdev;

	ret = hvdrm_connect_vsp(hdev);
	if (ret) {
		drm_err(dev, "Failed to connect to vmbus.\n");
		goto err_hv_set_drv_data;
	}

	aperture_remove_all_conflicting_devices(hvdrm_driver.name);

	ret = hvdrm_setup_vram(hv, hdev);
	if (ret)
		goto err_vmbus_close;

	/*
	 * Should be done only once during init and resume. Failing to update
	 * vram location is not fatal. Device will update dirty area till
	 * preferred resolution only.
	 */
	ret = hvdrm_update_vram_location(hdev, hv->fb_base);
	if (ret)
		drm_warn(dev, "Failed to update vram location.\n");

	ret = hvdrm_mode_config_init(hv);
	if (ret)
		goto err_free_mmio;

	ret = drm_dev_register(dev, 0);
	if (ret) {
		drm_err(dev, "Failed to register drm driver.\n");
		goto err_free_mmio;
	}

	/* If DRM panic path is stubbed out VMBus code must do the unload */
	if (IS_ENABLED(CONFIG_DRM_PANIC))
		vmbus_set_skip_unload(true);

	drm_client_setup(dev, NULL);

	return 0;

err_free_mmio:
	iounmap(hv->vram);
	vmbus_free_mmio(hv->mem->start, hv->fb_size);
err_vmbus_close:
	vmbus_close(hdev->channel);
err_hv_set_drv_data:
	hv_set_drvdata(hdev, NULL);
	return ret;
}

static void hvdrm_vmbus_remove(struct hv_device *hdev)
{
	struct drm_device *dev = hv_get_drvdata(hdev);
	struct hvdrm_drm_device *hv = to_hv(dev);

	vmbus_set_skip_unload(false);
	drm_dev_unplug(dev);
	drm_atomic_helper_shutdown(dev);
	vmbus_close(hdev->channel);
	hv_set_drvdata(hdev, NULL);

	iounmap(hv->vram);
	vmbus_free_mmio(hv->mem->start, hv->fb_size);
}

static void hvdrm_vmbus_shutdown(struct hv_device *hdev)
{
	drm_atomic_helper_shutdown(hv_get_drvdata(hdev));
}

static int hvdrm_vmbus_suspend(struct hv_device *hdev)
{
	struct drm_device *dev = hv_get_drvdata(hdev);
	int ret;

	ret = drm_mode_config_helper_suspend(dev);
	if (ret)
		return ret;

	vmbus_close(hdev->channel);

	return 0;
}

static int hvdrm_vmbus_resume(struct hv_device *hdev)
{
	struct drm_device *dev = hv_get_drvdata(hdev);
	struct hvdrm_drm_device *hv = to_hv(dev);
	int ret;

	ret = hvdrm_connect_vsp(hdev);
	if (ret)
		return ret;

	ret = hvdrm_update_vram_location(hdev, hv->fb_base);
	if (ret)
		return ret;

	return drm_mode_config_helper_resume(dev);
}

static const struct hv_vmbus_device_id hvdrm_vmbus_tbl[] = {
	/* Synthetic Video Device GUID */
	{HV_SYNTHVID_GUID},
	{}
};

static struct hv_driver hvdrm_hv_driver = {
	.name = KBUILD_MODNAME,
	.id_table = hvdrm_vmbus_tbl,
	.probe = hvdrm_vmbus_probe,
	.remove = hvdrm_vmbus_remove,
	.shutdown = hvdrm_vmbus_shutdown,
	.suspend = hvdrm_vmbus_suspend,
	.resume = hvdrm_vmbus_resume,
	.driver = {
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

static int __init hvdrm_init(void)
{
	int ret;

	if (drm_firmware_drivers_only())
		return -ENODEV;

	ret = pci_register_driver(&hvdrm_pci_driver);
	if (ret != 0)
		return ret;

	return vmbus_driver_register(&hvdrm_hv_driver);
}

static void __exit hvdrm_exit(void)
{
	vmbus_driver_unregister(&hvdrm_hv_driver);
	pci_unregister_driver(&hvdrm_pci_driver);
}

module_init(hvdrm_init);
module_exit(hvdrm_exit);

MODULE_DEVICE_TABLE(pci, hvdrm_pci_tbl);
MODULE_DEVICE_TABLE(vmbus, hvdrm_vmbus_tbl);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Deepak Rawat <drawat.floss@gmail.com>");
MODULE_DESCRIPTION("DRM driver for Hyper-V synthetic video device");
