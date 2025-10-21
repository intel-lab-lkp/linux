// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright © 2025 Intel Corporation
 */

#include <linux/anon_inodes.h>
#include <linux/delay.h>
#include <linux/file.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/types.h>
#include <linux/vfio.h>
#include <linux/vfio_pci_core.h>

#include <drm/intel/xe_sriov_vfio.h>

/**
 * struct xe_vfio_pci_migration_file - file used for reading / writing migration data
 */
struct xe_vfio_pci_migration_file {
	/** @filp: pointer to underlying &struct file */
	struct file *filp;
	/** @lock: serializes accesses to migration data */
	struct mutex lock;
	/** @xe_vdev: backpointer to &struct xe_vfio_pci_core_device */
	struct xe_vfio_pci_core_device *xe_vdev;
};

/**
 * struct xe_vfio_pci_core_device - xe-specific vfio_pci_core_device
 *
 * Top level structure of xe_vfio_pci.
 */
struct xe_vfio_pci_core_device {
	/** @core_device: vendor-agnostic VFIO device */
	struct vfio_pci_core_device core_device;

	/** @mig_state: current device migration state */
	enum vfio_device_mig_state mig_state;

	/** @vfid: VF number used by PF, xe uses 1-based indexing for vfid */
	unsigned int vfid;

	/** @pf: pointer to driver_private of physical function */
	struct pci_dev *pf;

	/** @fd: &struct xe_vfio_pci_migration_file for userspace to read/write migration data */
	struct xe_vfio_pci_migration_file *fd;
};

#define xe_vdev_to_dev(xe_vdev) (&(xe_vdev)->core_device.pdev->dev)
#define xe_vdev_to_pdev(xe_vdev) ((xe_vdev)->core_device.pdev)

static void xe_vfio_pci_disable_file(struct xe_vfio_pci_migration_file *migf)
{
	struct xe_vfio_pci_core_device *xe_vdev = migf->xe_vdev;

	mutex_lock(&migf->lock);
	xe_vdev->fd = NULL;
	mutex_unlock(&migf->lock);
}

static void xe_vfio_pci_reset(struct xe_vfio_pci_core_device *xe_vdev)
{
	if (xe_vdev->fd)
		xe_vfio_pci_disable_file(xe_vdev->fd);

	xe_vdev->mig_state = VFIO_DEVICE_STATE_RUNNING;
}

static void xe_vfio_pci_reset_done(struct pci_dev *pdev)
{
	struct xe_vfio_pci_core_device *xe_vdev = pci_get_drvdata(pdev);
	int ret;

	ret = xe_sriov_vfio_wait_flr_done(xe_vdev->pf, xe_vdev->vfid);
	if (ret)
		dev_err(&pdev->dev, "Failed to wait for FLR: %d\n", ret);

	xe_vfio_pci_reset(xe_vdev);
}

static const struct pci_error_handlers xe_vfio_pci_err_handlers = {
	.reset_done = xe_vfio_pci_reset_done,
};

static int xe_vfio_pci_open_device(struct vfio_device *core_vdev)
{
	struct xe_vfio_pci_core_device *xe_vdev =
		container_of(core_vdev, struct xe_vfio_pci_core_device, core_device.vdev);
	struct vfio_pci_core_device *vdev = &xe_vdev->core_device;
	int ret;

	ret = vfio_pci_core_enable(vdev);
	if (ret)
		return ret;

	vfio_pci_core_finish_enable(vdev);

	return 0;
}

static int xe_vfio_pci_release_file(struct inode *inode, struct file *filp)
{
	struct xe_vfio_pci_migration_file *migf = filp->private_data;

	xe_vfio_pci_disable_file(migf);
	mutex_destroy(&migf->lock);
	kfree(migf);

	return 0;
}

static ssize_t xe_vfio_pci_save_read(struct file *filp, char __user *buf, size_t len, loff_t *pos)
{
	struct xe_vfio_pci_migration_file *migf = filp->private_data;
	ssize_t ret;

	if (pos)
		return -ESPIPE;

	mutex_lock(&migf->lock);
	ret = xe_sriov_vfio_data_read(migf->xe_vdev->pf, migf->xe_vdev->vfid, buf, len);
	mutex_unlock(&migf->lock);

	return ret;
}

static const struct file_operations xe_vfio_pci_save_fops = {
	.owner = THIS_MODULE,
	.read = xe_vfio_pci_save_read,
	.release = xe_vfio_pci_release_file,
	.llseek = noop_llseek,
};

static ssize_t xe_vfio_pci_resume_write(struct file *filp, const char __user *buf,
					size_t len, loff_t *pos)
{
	struct xe_vfio_pci_migration_file *migf = filp->private_data;
	ssize_t ret;

	if (pos)
		return -ESPIPE;

	mutex_lock(&migf->lock);
	ret = xe_sriov_vfio_data_write(migf->xe_vdev->pf, migf->xe_vdev->vfid, buf, len);
	mutex_unlock(&migf->lock);

	return ret;
}

static const struct file_operations xe_vfio_pci_resume_fops = {
	.owner = THIS_MODULE,
	.write = xe_vfio_pci_resume_write,
	.release = xe_vfio_pci_release_file,
	.llseek = noop_llseek,
};

static const char *vfio_dev_state_str(u32 state)
{
	switch (state) {
	case VFIO_DEVICE_STATE_RUNNING: return "running";
	case VFIO_DEVICE_STATE_RUNNING_P2P: return "running_p2p";
	case VFIO_DEVICE_STATE_STOP_COPY: return "stopcopy";
	case VFIO_DEVICE_STATE_STOP: return "stop";
	case VFIO_DEVICE_STATE_RESUMING: return "resuming";
	case VFIO_DEVICE_STATE_ERROR: return "error";
	default: return "";
	}
}

enum xe_vfio_pci_file_type {
	XE_VFIO_FILE_SAVE = 0,
	XE_VFIO_FILE_RESUME,
};

static struct xe_vfio_pci_migration_file *
xe_vfio_pci_alloc_file(struct xe_vfio_pci_core_device *xe_vdev,
		       enum xe_vfio_pci_file_type type)
{
	struct xe_vfio_pci_migration_file *migf;
	const struct file_operations *fops;
	int flags;

	migf = kzalloc(sizeof(*migf), GFP_KERNEL);
	if (!migf)
		return ERR_PTR(-ENOMEM);

	fops = type == XE_VFIO_FILE_SAVE ? &xe_vfio_pci_save_fops : &xe_vfio_pci_resume_fops;
	flags = type == XE_VFIO_FILE_SAVE ? O_RDONLY : O_WRONLY;
	migf->filp = anon_inode_getfile("xe_vfio_mig", fops, migf, flags);
	if (IS_ERR(migf->filp)) {
		kfree(migf);
		return ERR_CAST(migf->filp);
	}

	mutex_init(&migf->lock);
	migf->xe_vdev = xe_vdev;
	xe_vdev->fd = migf;

	stream_open(migf->filp->f_inode, migf->filp);

	return migf;
}

static struct file *
xe_vfio_set_state(struct xe_vfio_pci_core_device *xe_vdev, u32 new)
{
	u32 cur = xe_vdev->mig_state;
	int ret;

	dev_dbg(xe_vdev_to_dev(xe_vdev),
		"state: %s->%s\n", vfio_dev_state_str(cur), vfio_dev_state_str(new));

	/*
	 * "STOP" handling is reused for "RUNNING_P2P", as the device doesn't have the capability to
	 * selectively block p2p DMA transfers.
	 * The device is not processing new workload requests when the VF is stopped, and both
	 * memory and MMIO communication channels are transferred to destination (where processing
	 * will be resumed).
	 */
	if ((cur == VFIO_DEVICE_STATE_RUNNING && new == VFIO_DEVICE_STATE_STOP) ||
	    (cur == VFIO_DEVICE_STATE_RUNNING && new == VFIO_DEVICE_STATE_RUNNING_P2P)) {
		ret = xe_sriov_vfio_stop(xe_vdev->pf, xe_vdev->vfid);
		if (ret)
			goto err;

		return NULL;
	}

	if ((cur == VFIO_DEVICE_STATE_RUNNING_P2P && new == VFIO_DEVICE_STATE_STOP) ||
	    (cur == VFIO_DEVICE_STATE_STOP && new == VFIO_DEVICE_STATE_RUNNING_P2P))
		return NULL;

	if ((cur == VFIO_DEVICE_STATE_STOP && new == VFIO_DEVICE_STATE_RUNNING) ||
	    (cur == VFIO_DEVICE_STATE_RUNNING_P2P && new == VFIO_DEVICE_STATE_RUNNING)) {
		ret = xe_sriov_vfio_run(xe_vdev->pf, xe_vdev->vfid);
		if (ret)
			goto err;

		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_STOP && new == VFIO_DEVICE_STATE_STOP_COPY) {
		struct xe_vfio_pci_migration_file *migf;

		migf = xe_vfio_pci_alloc_file(xe_vdev, XE_VFIO_FILE_SAVE);
		if (IS_ERR(migf)) {
			ret = PTR_ERR(migf);
			goto err;
		}

		ret = xe_sriov_vfio_stop_copy_enter(xe_vdev->pf, xe_vdev->vfid);
		if (ret) {
			fput(migf->filp);
			goto err;
		}

		return migf->filp;
	}

	if ((cur == VFIO_DEVICE_STATE_STOP_COPY && new == VFIO_DEVICE_STATE_STOP)) {
		if (xe_vdev->fd)
			xe_vfio_pci_disable_file(xe_vdev->fd);

		xe_sriov_vfio_stop_copy_exit(xe_vdev->pf, xe_vdev->vfid);

		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_STOP && new == VFIO_DEVICE_STATE_RESUMING) {
		struct xe_vfio_pci_migration_file *migf;

		migf = xe_vfio_pci_alloc_file(xe_vdev, XE_VFIO_FILE_RESUME);
		if (IS_ERR(migf)) {
			ret = PTR_ERR(migf);
			goto err;
		}

		ret = xe_sriov_vfio_resume_enter(xe_vdev->pf, xe_vdev->vfid);
		if (ret) {
			fput(migf->filp);
			goto err;
		}

		return migf->filp;
	}

	if (cur == VFIO_DEVICE_STATE_RESUMING && new == VFIO_DEVICE_STATE_STOP) {
		if (xe_vdev->fd)
			xe_vfio_pci_disable_file(xe_vdev->fd);

		xe_sriov_vfio_resume_exit(xe_vdev->pf, xe_vdev->vfid);

		return NULL;
	}

	if (new == VFIO_DEVICE_STATE_ERROR)
		xe_sriov_vfio_error(xe_vdev->pf, xe_vdev->vfid);

	WARN(true, "Unknown state transition %d->%d", cur, new);
	return ERR_PTR(-EINVAL);

err:
	dev_dbg(xe_vdev_to_dev(xe_vdev),
		"Failed to transition state: %s->%s err=%d\n",
		vfio_dev_state_str(cur), vfio_dev_state_str(new), ret);
	return ERR_PTR(ret);
}

static struct file *
xe_vfio_pci_set_device_state(struct vfio_device *core_vdev,
			     enum vfio_device_mig_state new_state)
{
	struct xe_vfio_pci_core_device *xe_vdev =
		container_of(core_vdev, struct xe_vfio_pci_core_device, core_device.vdev);
	enum vfio_device_mig_state next_state;
	struct file *f = NULL;
	int ret;

	while (new_state != xe_vdev->mig_state) {
		ret = vfio_mig_get_next_state(core_vdev, xe_vdev->mig_state,
					      new_state, &next_state);
		if (ret) {
			f = ERR_PTR(ret);
			break;
		}
		f = xe_vfio_set_state(xe_vdev, next_state);
		if (IS_ERR(f))
			break;

		xe_vdev->mig_state = next_state;

		/* Multiple state transitions with non-NULL file in the middle */
		if (f && new_state != xe_vdev->mig_state) {
			fput(f);
			f = ERR_PTR(-EINVAL);
			break;
		}
	}

	return f;
}

static int xe_vfio_pci_get_device_state(struct vfio_device *core_vdev,
					enum vfio_device_mig_state *curr_state)
{
	struct xe_vfio_pci_core_device *xe_vdev =
		container_of(core_vdev, struct xe_vfio_pci_core_device, core_device.vdev);

	*curr_state = xe_vdev->mig_state;

	return 0;
}

static int xe_vfio_pci_get_data_size(struct vfio_device *vdev,
				     unsigned long *stop_copy_length)
{
	struct xe_vfio_pci_core_device *xe_vdev =
		container_of(vdev, struct xe_vfio_pci_core_device, core_device.vdev);

	*stop_copy_length = xe_sriov_vfio_stop_copy_size(xe_vdev->pf, xe_vdev->vfid);

	return 0;
}

static const struct vfio_migration_ops xe_vfio_pci_migration_ops = {
	.migration_set_state = xe_vfio_pci_set_device_state,
	.migration_get_state = xe_vfio_pci_get_device_state,
	.migration_get_data_size = xe_vfio_pci_get_data_size,
};

static void xe_vfio_pci_migration_init(struct vfio_device *core_vdev)
{
	struct xe_vfio_pci_core_device *xe_vdev =
		container_of(core_vdev, struct xe_vfio_pci_core_device, core_device.vdev);
	struct pci_dev *pdev = to_pci_dev(core_vdev->dev);

	if (!xe_sriov_vfio_migration_supported(pdev->physfn))
		return;

	/* vfid starts from 1 for xe */
	xe_vdev->vfid = pci_iov_vf_id(pdev) + 1;
	xe_vdev->pf = pdev->physfn;

	core_vdev->migration_flags = VFIO_MIGRATION_STOP_COPY | VFIO_MIGRATION_P2P;
	core_vdev->mig_ops = &xe_vfio_pci_migration_ops;
}

static int xe_vfio_pci_init_dev(struct vfio_device *core_vdev)
{
	struct pci_dev *pdev = to_pci_dev(core_vdev->dev);

	if (pdev->is_virtfn && strcmp(pdev->physfn->dev.driver->name, "xe") == 0)
		xe_vfio_pci_migration_init(core_vdev);

	return vfio_pci_core_init_dev(core_vdev);
}

static const struct vfio_device_ops xe_vfio_pci_ops = {
	.name = "xe-vfio-pci",
	.init = xe_vfio_pci_init_dev,
	.release = vfio_pci_core_release_dev,
	.open_device = xe_vfio_pci_open_device,
	.close_device = vfio_pci_core_close_device,
	.ioctl = vfio_pci_core_ioctl,
	.device_feature = vfio_pci_core_ioctl_feature,
	.read = vfio_pci_core_read,
	.write = vfio_pci_core_write,
	.mmap = vfio_pci_core_mmap,
	.request = vfio_pci_core_request,
	.match = vfio_pci_core_match,
	.match_token_uuid = vfio_pci_core_match_token_uuid,
	.bind_iommufd = vfio_iommufd_physical_bind,
	.unbind_iommufd = vfio_iommufd_physical_unbind,
	.attach_ioas = vfio_iommufd_physical_attach_ioas,
	.detach_ioas = vfio_iommufd_physical_detach_ioas,
};

static int xe_vfio_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct xe_vfio_pci_core_device *xe_vdev;
	int ret;

	xe_vdev = vfio_alloc_device(xe_vfio_pci_core_device, core_device.vdev, &pdev->dev,
				    &xe_vfio_pci_ops);
	if (IS_ERR(xe_vdev))
		return PTR_ERR(xe_vdev);

	dev_set_drvdata(&pdev->dev, &xe_vdev->core_device);

	ret = vfio_pci_core_register_device(&xe_vdev->core_device);
	if (ret) {
		vfio_put_device(&xe_vdev->core_device.vdev);
		return ret;
	}

	return 0;
}

static void xe_vfio_pci_remove(struct pci_dev *pdev)
{
	struct xe_vfio_pci_core_device *xe_vdev = pci_get_drvdata(pdev);

	vfio_pci_core_unregister_device(&xe_vdev->core_device);
	vfio_put_device(&xe_vdev->core_device.vdev);
}

static const struct pci_device_id xe_vfio_pci_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_ANY_ID),
		.class = PCI_BASE_CLASS_DISPLAY << 16, .class_mask = 0xff << 16,
		.override_only = PCI_ID_F_VFIO_DRIVER_OVERRIDE },
	{}
};
MODULE_DEVICE_TABLE(pci, xe_vfio_pci_table);

static struct pci_driver xe_vfio_pci_driver = {
	.name = "xe-vfio-pci",
	.id_table = xe_vfio_pci_table,
	.probe = xe_vfio_pci_probe,
	.remove = xe_vfio_pci_remove,
	.err_handler = &xe_vfio_pci_err_handlers,
	.driver_managed_dma = true,
};
module_pci_driver(xe_vfio_pci_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("VFIO PCI driver with migration support for Intel Graphics");
