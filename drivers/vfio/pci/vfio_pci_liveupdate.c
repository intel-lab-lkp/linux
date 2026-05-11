// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2026, Google LLC.
 * Vipin Sharma <vipinsh@google.com>
 * David Matlack <dmatlack@google.com>
 */

/**
 * DOC: VFIO PCI Preservation via LUO
 *
 * VFIO PCI devices can be preserved over a kexec using the Live Update
 * Orchestrator (LUO) file preservation. This allows userspace (such as a VMM)
 * to transfer an in-use device to the next kernel.
 *
 * .. note::
 *    The support for preserving VFIO PCI devices is currently *partial* and
 *    should be considered *experimental*. It should only be used by developers
 *    working on expanding the support for the time being.
 *
 *    To avoid accidental usage while the support is still experimental, this
 *    support is hidden behind a default-disable config option
 *    ``CONFIG_VFIO_PCI_LIVEUPDATE``. Once the kernel support has stabilized and
 *    become complete, this option will be enabled by default when
 *    ``CONFIG_VFIO_PCI`` and ``CONFIG_LIVEUPDATE`` are enabled.
 *
 * Usage Example
 * =============
 *
 * VFIO PCI devices can be preserved across a kexec by preserving the file
 * associated with the device in a LUO session::
 *
 *   device_fd = open("/dev/vfio/devices/vfioX");
 *   ...
 *   ioctl(session_fd, LIVEUPDATE_SESSION_PRESERVE_FD, { ..., device_fd, ...});
 *
 * .. note::
 *    LUO will hold an extra reference to the device file for as long as it is
 *    preserved, so there is no way for the file to be destroyed or the device
 *    to be unbound from the vfio-pci driver while it is preserved.
 *
 * After kexec, the preserved VFIO device file can be retrieved from the session
 * just like any other preserved file::
 *
 *   ioctl(session_fd, LIVEUPDATE_SESSION_RETRIEVE_FD, &arg);
 *   device_fd = arg.fd;
 *   ...
 *   ioctl(session_fd, LIVEUPDATE_SESSION_FINISH, ...);
 *
 * .. note::
 *    After kexec, if a device was preserved by the previous kernel, attempting
 *    to open a new file for the device via its character device
 *    (``/dev/vfio/devices/X``) or via ``VFIO_GROUP_GET_DEVICE_FD`` will fail
 *    with ``-EBUSY``.
 *
 * Restrictions
 * ============
 *
 * The kernel imposes the following restrictions when preserving VFIO devices:
 *
 *  * The device must be bound to the ``vfio-pci`` driver.
 *
 *  * ``CONFIG_VFIO_PCI_ZDEV_KVM`` must not be enabled. This may be relaxed in
 *    the future.
 *
 *  * The device must not be an Intel display device. This may be relaxed in
 *    the future.
 *
 *  * No support for VFIO PCI DMA buffer. This is to keep code simpler, it will
 *    be removed in future.
 *
 *  * The device file descriptor must be obtained by opening the VFIO character
 *    device (``/dev/vfio/devices/vfioX``) and not via
 *    ``VFIO_GROUP_GET_DEVICE_FD``.
 *
 *  * The device must have interrupt disable prior to kexec. Failure to disable
 *    interrupts on the device will cause the ``reboot(LINUX_REBOOT_CMD_KEXEC)``
 *    syscall (to initiate the kexec) to fail.
 *
 * In addition, the device must meet all of the restrictions imposed by the
 * core PCI layer documented at :doc:`/PCI/liveupdate`.
 *
 * Preservation Behavior
 * =====================
 *
 * The eventual goal of this support is to avoid disrupting the workload, state,
 * or configuration of each preserved device during a Live Update. This would
 * include allowing the device to perform DMA to preserved memory buffers and
 * perform P2P DMA to other preserved devices. However, there are many pieces
 * that still need to land in the kernel.
 *
 * For now, VFIO only preserves the following state for for devices:
 *
 *  * The PCI Segment, Bus, Device, and Function numbers of the device. The
 *    kernel guarantees the these will not change across a kexec when a device
 *    is preserved.
 *
 * Since the kernel is not yet prepared to preserve all parts of the device and
 * its dependencies (such as DMA mappings), VFIO currently resets and restores
 * preserved devices back into an idle state during kexec, before handing off
 * control to the next kernel. This will be relaxed in future versions of the
 * kernel once it is safe to allow the device to keep running across kexec.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/errno.h>
#include <linux/file.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/vfio_pci.h>
#include <linux/liveupdate.h>
#include <linux/module.h>
#include <linux/vfio.h>

#include "vfio_pci_priv.h"

static bool vfio_pci_liveupdate_can_preserve(struct liveupdate_file_handler *handler,
					     struct file *file)
{
	struct vfio_device *device = vfio_device_from_file(file);
	struct vfio_pci_core_device *vdev;
	struct pci_dev *pdev;

	if (!device)
		return false;

	/* Live Update support is limited to cdev files. */
	if (!vfio_device_cdev_opened(device))
		return false;

	if (device->ops != &vfio_pci_ops)
		return false;

	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	pdev = vdev->pdev;

	/*
	 * Don't support specialized vfio-pci devices for now since they haven't
	 * been tested.
	 */
	if (IS_ENABLED(CONFIG_VFIO_PCI_ZDEV_KVM) || vfio_pci_is_intel_display(pdev))
		return false;

	return true;
}

static int vfio_pci_liveupdate_preserve(struct liveupdate_file_op_args *args)
{
	struct vfio_device *device = vfio_device_from_file(args->file);
	struct vfio_pci_core_device_ser *ser;
	struct vfio_pci_core_device *vdev;
	struct pci_dev *pdev;
	int ret;

	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	pdev = vdev->pdev;

	ret = pci_liveupdate_preserve(pdev);
	if (ret)
		return ret;

	ser = kho_alloc_preserve(sizeof(*ser));
	if (IS_ERR(ser)) {
		ret = PTR_ERR(ser);
		goto err_unpreserve;
	}

	ser->bdf = pci_dev_id(pdev);
	ser->domain = pci_domain_nr(pdev->bus);

	args->serialized_data = virt_to_phys(ser);
	return 0;

err_unpreserve:
	pci_liveupdate_unpreserve(pdev);
	return ret;
}

static void vfio_pci_liveupdate_unpreserve(struct liveupdate_file_op_args *args)
{
	struct vfio_device *device = vfio_device_from_file(args->file);

	pci_liveupdate_unpreserve(to_pci_dev(device->dev));
	kho_unpreserve_free(phys_to_virt(args->serialized_data));
}

static int vfio_pci_liveupdate_freeze(struct liveupdate_file_op_args *args)
{
	struct vfio_device *device = vfio_device_from_file(args->file);
	struct vfio_pci_core_device *vdev;
	struct pci_dev *pdev;
	int ret;

	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	pdev = vdev->pdev;

	guard(mutex)(&device->dev_set->lock);

	/*
	 * Userspace must disable interrupts on the device prior to freeze so
	 * that the device does not send any interrupts until new interrupt
	 * handlers have been established by the next kernel.
	 */
	if (vdev->irq_type != VFIO_PCI_NUM_IRQS) {
		pci_err(pdev, "Freeze failed! Interrupts are still enabled.\n");
		return -EINVAL;
	}

	guard(rwsem_write)(&vdev->memory_lock);

	/*
	 * Userspace must make sure device is not in the lower power state for
	 * live update. We may relax this in future.
	 */
	if (pdev->current_state != PCI_D0) {
		pci_err(pdev, "Freeze failed! Device not in D0 state.\n");
		return -EINVAL;
	}

	/*
	 * Reset is a temporary measure to provide kernel after kexec a clean
	 * device while VFIO live update work is under development and not
	 * fully supported.  It will go away once continuous DMA support is
	 * added to device preservation.
	 */
	vfio_pci_zap_bars(vdev);
	ret = pci_load_saved_state(pdev, vdev->pci_saved_state);
	if (ret)
		return ret;
	pci_clear_master(pdev);
	vfio_pci_core_try_reset(vdev);
	pci_restore_state(pdev);
	return 0;
}

static int match_device(struct device *dev, const void *arg)
{
	struct vfio_device *device = container_of(dev, struct vfio_device, device);
	const struct vfio_pci_core_device_ser *ser = arg;
	struct pci_dev *pdev;

	pdev = dev_is_pci(device->dev) ? to_pci_dev(device->dev) : NULL;
	if (!pdev)
		return false;

	return ser->bdf == pci_dev_id(pdev) && ser->domain == pci_domain_nr(pdev->bus);
}

static int vfio_pci_liveupdate_retrieve(struct liveupdate_file_op_args *args)
{
	struct vfio_pci_core_device_ser *ser;
	struct vfio_pci_core_device *vdev;
	struct vfio_device *device;
	struct file *file;
	int ret = 0;

	ser = phys_to_virt(args->serialized_data);

	device = vfio_find_device(ser, match_device);
	if (!device)
		return -ENODEV;

	file = vfio_device_liveupdate_cdev_open(device);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto out;
	}

	vdev = container_of(device, struct vfio_pci_core_device, vdev);
	vdev->liveupdate_incoming_state = ser;

	args->file = file;
out:
	/* Drop the reference from vfio_find_device() */
	put_device(&device->device);
	return ret;
}

static bool vfio_pci_liveupdate_can_finish(struct liveupdate_file_op_args *args)
{
	struct vfio_pci_core_device *vdev;
	struct vfio_device *device;

	if (args->retrieve_status <= 0)
		return false;

	device = vfio_device_from_file(args->file);
	vdev = container_of(device, struct vfio_pci_core_device, vdev);

	/* Check that vdev->liveupdate_incoming_state is no longer in use. */
	guard(mutex)(&device->dev_set->lock);
	return !vdev->liveupdate_incoming_state;
}

static void vfio_pci_liveupdate_finish(struct liveupdate_file_op_args *args)
{
	struct vfio_device *device;

	if (!args->file) {
		pr_err("Finish called on an unretrieved file.\n");
		return;
	}

	device = vfio_device_from_file(args->file);
	pci_liveupdate_finish(to_pci_dev(device->dev));
	kho_restore_free(phys_to_virt(args->serialized_data));
}

static const struct liveupdate_file_ops vfio_pci_liveupdate_file_ops = {
	.can_preserve = vfio_pci_liveupdate_can_preserve,
	.preserve = vfio_pci_liveupdate_preserve,
	.unpreserve = vfio_pci_liveupdate_unpreserve,
	.freeze = vfio_pci_liveupdate_freeze,
	.retrieve = vfio_pci_liveupdate_retrieve,
	.can_finish = vfio_pci_liveupdate_can_finish,
	.finish = vfio_pci_liveupdate_finish,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler vfio_pci_liveupdate_fh = {
	.ops = &vfio_pci_liveupdate_file_ops,
	.compatible = VFIO_PCI_LUO_FH_COMPATIBLE,
};

int __init vfio_pci_liveupdate_init(void)
{
	int ret;

	ret = liveupdate_register_file_handler(&vfio_pci_liveupdate_fh);
	if (ret)
		goto err_return;

	ret = pci_liveupdate_register_flb(&vfio_pci_liveupdate_fh);
	if (ret)
		goto err_unregister;

	return 0;

err_unregister:
	liveupdate_unregister_file_handler(&vfio_pci_liveupdate_fh);
err_return:
	return (ret == -EOPNOTSUPP) ? 0 : ret;
}

void vfio_pci_liveupdate_cleanup(void)
{
	pci_liveupdate_unregister_flb(&vfio_pci_liveupdate_fh);
	liveupdate_unregister_file_handler(&vfio_pci_liveupdate_fh);
}
