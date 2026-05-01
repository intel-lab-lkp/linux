// SPDX-License-Identifier: GPL-2.0-only
/*
 * VFIO ZPCI devices support
 *
 * Copyright (C) IBM Corp. 2020.  All rights reserved.
 *	Author(s): Pierre Morel <pmorel@linux.ibm.com>
 *                 Matthew Rosato <mjrosato@linux.ibm.com>
 */
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/uaccess.h>
#include <linux/vfio.h>
#include <linux/vfio_zdev.h>
#include <linux/kvm_host.h>
#include <asm/pci_clp.h>
#include <asm/pci_io.h>

#include "vfio_pci_priv.h"

/*
 * Add the Base PCI Function information to the device info region.
 */
static int zpci_base_cap(struct zpci_dev *zdev, struct vfio_info_cap *caps)
{
	struct vfio_device_info_cap_zpci_base cap = {
		.header.id = VFIO_DEVICE_INFO_CAP_ZPCI_BASE,
		.header.version = 2,
		.start_dma = zdev->start_dma,
		.end_dma = zdev->end_dma,
		.pchid = zdev->pchid,
		.vfn = zdev->vfn,
		.fmb_length = zdev->fmb_length,
		.pft = zdev->pft,
		.gid = zdev->pfgid,
		.fh = zdev->fh
	};

	return vfio_info_add_capability(caps, &cap.header, sizeof(cap));
}

/*
 * Add the Base PCI Function Group information to the device info region.
 */
static int zpci_group_cap(struct zpci_dev *zdev, struct vfio_info_cap *caps)
{
	struct vfio_device_info_cap_zpci_group cap = {
		.header.id = VFIO_DEVICE_INFO_CAP_ZPCI_GROUP,
		.header.version = 2,
		.dasm = zdev->dma_mask,
		.msi_addr = zdev->msi_addr,
		.flags = VFIO_DEVICE_INFO_ZPCI_FLAG_REFRESH,
		.mui = zdev->fmb_update,
		.noi = zdev->max_msi,
		.maxstbl = ZPCI_MAX_WRITE_SIZE,
		.version = zdev->version,
		.reserved = 0,
		.imaxstbl = zdev->maxstbl
	};

	return vfio_info_add_capability(caps, &cap.header, sizeof(cap));
}

/*
 * Add the device utility string to the device info region.
 */
static int zpci_util_cap(struct zpci_dev *zdev, struct vfio_info_cap *caps)
{
	struct vfio_device_info_cap_zpci_util *cap;
	int cap_size = sizeof(*cap) + CLP_UTIL_STR_LEN;
	int ret;

	cap = kmalloc(cap_size, GFP_KERNEL);
	if (!cap)
		return -ENOMEM;

	cap->header.id = VFIO_DEVICE_INFO_CAP_ZPCI_UTIL;
	cap->header.version = 1;
	cap->size = CLP_UTIL_STR_LEN;
	memcpy(cap->util_str, zdev->util_str, cap->size);

	ret = vfio_info_add_capability(caps, &cap->header, cap_size);

	kfree(cap);

	return ret;
}

/*
 * Add the function path string to the device info region.
 */
static int zpci_pfip_cap(struct zpci_dev *zdev, struct vfio_info_cap *caps)
{
	struct vfio_device_info_cap_zpci_pfip *cap;
	int cap_size = sizeof(*cap) + CLP_PFIP_NR_SEGMENTS;
	int ret;

	cap = kmalloc(cap_size, GFP_KERNEL);
	if (!cap)
		return -ENOMEM;

	cap->header.id = VFIO_DEVICE_INFO_CAP_ZPCI_PFIP;
	cap->header.version = 1;
	cap->size = CLP_PFIP_NR_SEGMENTS;
	memcpy(cap->pfip, zdev->pfip, cap->size);

	ret = vfio_info_add_capability(caps, &cap->header, cap_size);

	kfree(cap);

	return ret;
}

/*
 * Add all supported capabilities to the VFIO_DEVICE_GET_INFO capability chain.
 */
int vfio_pci_info_zdev_add_caps(struct vfio_pci_core_device *vdev,
				struct vfio_info_cap *caps)
{
	struct zpci_dev *zdev = to_zpci(vdev->pdev);
	int ret;

	if (!zdev)
		return -ENODEV;

	ret = zpci_base_cap(zdev, caps);
	if (ret)
		return ret;

	ret = zpci_group_cap(zdev, caps);
	if (ret)
		return ret;

	if (zdev->util_str_avail) {
		ret = zpci_util_cap(zdev, caps);
		if (ret)
			return ret;
	}

	ret = zpci_pfip_cap(zdev, caps);

	return ret;
}

int vfio_pci_zdev_open_device(struct vfio_pci_core_device *vdev)
{
	struct zpci_dev *zdev = to_zpci(vdev->pdev);

	if (!zdev)
		return -ENODEV;

	if (!vdev->vdev.kvm)
		return 0;

	if (zpci_kvm_hook.kvm_register)
		return zpci_kvm_hook.kvm_register(zdev, vdev->vdev.kvm);

	return -ENOENT;
}

void vfio_pci_zdev_close_device(struct vfio_pci_core_device *vdev)
{
	struct zpci_dev *zdev = to_zpci(vdev->pdev);

	if (!zdev || !vdev->vdev.kvm)
		return;

	if (zpci_kvm_hook.kvm_unregister)
		zpci_kvm_hook.kvm_unregister(zdev);
}

int vfio_pci_zdev_feature_fmb(struct vfio_pci_core_device *vdev, u32 flags,
			      void __user *arg, size_t argsz)
{
	struct zpci_dev *zdev;
	struct vfio_device_feature_zpci_fmb fmb = {0};
	u32 ops = VFIO_DEVICE_FEATURE_GET | VFIO_DEVICE_FEATURE_SET;
	int ret;

	ret = vfio_check_feature(flags, argsz, ops, sizeof(fmb));
	if (ret != 1)
		return ret;

	zdev = to_zpci(vdev->pdev);
	if (!zdev)
		return -ENODEV;

	mutex_lock(&zdev->fmb_lock);
	if (flags & VFIO_DEVICE_FEATURE_SET) {
		if (copy_from_user(&fmb, arg, sizeof(fmb))) {
			ret = -EFAULT;
			goto release_lock;
		}

		if (fmb.flags & VFIO_DEVICE_FEATURE_ZPCI_FMB_FLAGS_ENABLED)
			ret = zpci_fmb_reenable_device(zdev);
		else
			ret = zpci_fmb_disable_device(zdev);
		goto release_lock;
	}

	ret = 0;
	if (zdev->fmb) {
		fmb.flags |= VFIO_DEVICE_FEATURE_ZPCI_FMB_FLAGS_ENABLED;
	} else {
		fmb.flags &= ~VFIO_DEVICE_FEATURE_ZPCI_FMB_FLAGS_ENABLED;
		goto release_lock;
	}

	fmb.format = zdev->fmb->format;
	fmb.fmt_ind = zdev->fmb->fmt_ind;
	fmb.samples = zdev->fmb->samples;
	fmb.last_update = zdev->fmb->last_update;
	fmb.ld_ops = zdev->fmb->ld_ops;
	fmb.st_ops = zdev->fmb->st_ops;
	fmb.stb_ops = zdev->fmb->stb_ops;
	fmb.rpcit_ops = zdev->fmb->rpcit_ops;

	switch (zdev->fmb->format) {
	case 0:
		if (zdev->fmb->fmt_ind & ZPCI_FMB_DMA_COUNTER_VALID) {
			fmb.fmt0.dma_rbytes = zdev->fmb->fmt0.dma_rbytes;
			fmb.fmt0.dma_wbytes = zdev->fmb->fmt0.dma_wbytes;
		}
		break;
	case 1:
		fmb.fmt1.rx_bytes = zdev->fmb->fmt1.rx_bytes;
		fmb.fmt1.rx_packets = zdev->fmb->fmt1.rx_packets;
		fmb.fmt1.tx_bytes = zdev->fmb->fmt1.tx_bytes;
		fmb.fmt1.tx_packets = zdev->fmb->fmt1.tx_packets;
		break;
	case 2:
		fmb.fmt2.consumed_work_units = zdev->fmb->fmt2.consumed_work_units;
		fmb.fmt2.max_work_units = zdev->fmb->fmt2.max_work_units;
		break;
	case 3:
		fmb.fmt3.tx_bytes = zdev->fmb->fmt3.tx_bytes;
		break;
	}

	if (copy_to_user(arg, &fmb, sizeof(fmb)))
		ret = -EFAULT;

release_lock:
	mutex_unlock(&zdev->fmb_lock);
	return ret;
}
