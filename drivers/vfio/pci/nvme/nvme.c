// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022, INTEL CORPORATION. All rights reserved
 * Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved
 */

#include <linux/device.h>
#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/types.h>
#include <linux/vfio.h>
#include <linux/anon_inodes.h>
#include <linux/kernel.h>
#include <linux/vfio_pci_core.h>

#include "nvme.h"

static void nvmevf_disable_fd(struct nvmevf_migration_file *migf)
{
	mutex_lock(&migf->lock);

	/* release the device states buffer */
	kvfree(migf->vf_data);
	migf->vf_data = NULL;
	migf->disabled = true;
	migf->total_length = 0;
	migf->filp->f_pos = 0;
	mutex_unlock(&migf->lock);
}

static void nvmevf_disable_fds(struct nvmevf_pci_core_device *nvmevf_dev)
{
	if (nvmevf_dev->resuming_migf) {
		nvmevf_disable_fd(nvmevf_dev->resuming_migf);
		fput(nvmevf_dev->resuming_migf->filp);
		nvmevf_dev->resuming_migf = NULL;
	}

	if (nvmevf_dev->saving_migf) {
		nvmevf_disable_fd(nvmevf_dev->saving_migf);
		fput(nvmevf_dev->saving_migf->filp);
		nvmevf_dev->saving_migf = NULL;
	}
}

static void nvmevf_state_mutex_unlock(struct nvmevf_pci_core_device *nvmevf_dev)
{
	lockdep_assert_held(&nvmevf_dev->state_mutex);
again:
	spin_lock(&nvmevf_dev->reset_lock);
	if (nvmevf_dev->deferred_reset) {
		nvmevf_dev->deferred_reset = false;
		spin_unlock(&nvmevf_dev->reset_lock);
		nvmevf_dev->mig_state = VFIO_DEVICE_STATE_RUNNING;
		nvmevf_disable_fds(nvmevf_dev);
		goto again;
	}
	mutex_unlock(&nvmevf_dev->state_mutex);
	spin_unlock(&nvmevf_dev->reset_lock);
}

static struct nvmevf_pci_core_device *nvmevf_drvdata(struct pci_dev *pdev)
{
	struct vfio_pci_core_device *core_device = dev_get_drvdata(&pdev->dev);

	return container_of(core_device, struct nvmevf_pci_core_device,
			    core_device);
}

static int nvmevf_pci_open_device(struct vfio_device *core_vdev)
{
	struct nvmevf_pci_core_device *nvmevf_dev;
	struct vfio_pci_core_device *vdev;
	int ret;

	nvmevf_dev = container_of(core_vdev, struct nvmevf_pci_core_device,
			core_device.vdev);
	vdev = &nvmevf_dev->core_device;

	ret = vfio_pci_core_enable(vdev);
	if (ret)
		return ret;

	if (nvmevf_dev->migrate_cap)
		nvmevf_dev->mig_state = VFIO_DEVICE_STATE_RUNNING;
	vfio_pci_core_finish_enable(vdev);
	return 0;
}

static void nvmevf_pci_close_device(struct vfio_device *core_vdev)
{
	struct nvmevf_pci_core_device *nvmevf_dev;

	nvmevf_dev = container_of(core_vdev, struct nvmevf_pci_core_device,
			core_device.vdev);

	if (nvmevf_dev->migrate_cap) {
		mutex_lock(&nvmevf_dev->state_mutex);
		nvmevf_disable_fds(nvmevf_dev);
		nvmevf_state_mutex_unlock(nvmevf_dev);
	}

	vfio_pci_core_close_device(core_vdev);
}

static const struct vfio_device_ops nvmevf_pci_ops = {
	.name = "nvme-vfio-pci",
	.release = vfio_pci_core_release_dev,
	.open_device = nvmevf_pci_open_device,
	.close_device = nvmevf_pci_close_device,
	.ioctl = vfio_pci_core_ioctl,
	.device_feature = vfio_pci_core_ioctl_feature,
	.read = vfio_pci_core_read,
	.write = vfio_pci_core_write,
	.mmap = vfio_pci_core_mmap,
	.request = vfio_pci_core_request,
	.match = vfio_pci_core_match,
};

static int nvmevf_pci_probe(struct pci_dev *pdev,
			    const struct pci_device_id *id)
{
	struct nvmevf_pci_core_device *nvmevf_dev;
	int ret;

	nvmevf_dev = vfio_alloc_device(nvmevf_pci_core_device, core_device.vdev,
				       &pdev->dev, &nvmevf_pci_ops);
	if (IS_ERR(nvmevf_dev))
		return PTR_ERR(nvmevf_dev);

	dev_set_drvdata(&pdev->dev, &nvmevf_dev->core_device);
	ret = vfio_pci_core_register_device(&nvmevf_dev->core_device);
	if (ret)
		goto out_put_dev;

	return 0;

out_put_dev:
	vfio_put_device(&nvmevf_dev->core_device.vdev);
	return ret;
}

static void nvmevf_pci_remove(struct pci_dev *pdev)
{
	struct nvmevf_pci_core_device *nvmevf_dev = nvmevf_drvdata(pdev);

	vfio_pci_core_unregister_device(&nvmevf_dev->core_device);
	vfio_put_device(&nvmevf_dev->core_device.vdev);
}

static void nvmevf_pci_aer_reset_done(struct pci_dev *pdev)
{
	struct nvmevf_pci_core_device *nvmevf_dev = nvmevf_drvdata(pdev);

	if (!nvmevf_dev->migrate_cap)
		return;

	/*
	 * As the higher VFIO layers are holding locks across reset and using
	 * those same locks with the mm_lock we need to prevent ABBA deadlock
	 * with the state_mutex and mm_lock.
	 * In case the state_mutex was taken already we defer the cleanup work
	 * to the unlock flow of the other running context.
	 */
	spin_lock(&nvmevf_dev->reset_lock);
	nvmevf_dev->deferred_reset = true;
	if (!mutex_trylock(&nvmevf_dev->state_mutex)) {
		spin_unlock(&nvmevf_dev->reset_lock);
		return;
	}
	spin_unlock(&nvmevf_dev->reset_lock);
	nvmevf_state_mutex_unlock(nvmevf_dev);
}

static const struct pci_error_handlers nvmevf_err_handlers = {
	.reset_done = nvmevf_pci_aer_reset_done,
	.error_detected = vfio_pci_core_aer_err_detected,
};

static struct pci_driver nvmevf_pci_driver = {
	.name = KBUILD_MODNAME,
	.probe = nvmevf_pci_probe,
	.remove = nvmevf_pci_remove,
	.err_handler = &nvmevf_err_handlers,
	.driver_managed_dma = true,
};

module_pci_driver(nvmevf_pci_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chaitanya Kulkarni <kch@nvidia.com>");
MODULE_DESCRIPTION("NVMe VFIO PCI - VFIO PCI driver with live migration support for NVMe");
