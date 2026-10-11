// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: Copyright Red Hat

#include <linux/device.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/vfio.h>
#include <linux/vfio_pci_core.h>
#include <uapi/linux/vfio.h>

#include "qcom_vfio_pci.h"

static struct qcom_vfio_pci_device *to_qcom_vdev(struct vfio_pci_core_device *vdev)
{
	return container_of(vdev, struct qcom_vfio_pci_device, vdev);
}

/*
 * Intercept config space read operations to return cached host MSI values.
 * Call core VFIO if not intercepted.
 */
static ssize_t qcom_vfio_pci_read(struct vfio_device *core_vdev, char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	struct qcom_vfio_pci_device *qdev = to_qcom_vdev(vdev);
	unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	loff_t offset = *ppos & VFIO_PCI_OFFSET_MASK;
	ssize_t ret;

	if (index == VFIO_PCI_CONFIG_REGION_INDEX) {
		ret = qcom_vfio_pci_config_rw(qdev, buf, count, offset, false);
		if (ret != -ENOTTY) {
			if (ret > 0)
				*ppos += ret;
			return ret;
		}
	}

	return vfio_pci_core_read(core_vdev, buf, count, ppos);
}

static int qcom_vfio_pci_init_dev(struct vfio_device *core_vdev)
{
	struct qcom_vfio_pci_device *qdev =
		container_of(core_vdev, struct qcom_vfio_pci_device, vdev.vdev);
	struct pci_dev *pdev = to_pci_dev(core_vdev->dev);
	int ret;

	ret = vfio_pci_core_init_dev(core_vdev);
	if (ret)
		return ret;

	qdev->msi_offset = qcom_vfio_find_msi_offset(pdev);
	if (!qdev->msi_offset) {
		pci_err(pdev, "No free space in extended config for MSI quirk\n");
		pci_err(pdev, "qcom-vfio-pci cannot function without MSI passthrough, failing probe\n");
		vfio_pci_core_release_dev(core_vdev);
		return -ENODEV;
	}
	pci_info(pdev, "MSI passthrough quirk using offset 0x%x\n", qdev->msi_offset);

	qdev->msi_cached = false;
	qdev->msi_addr_lo = 0;
	qdev->msi_addr_hi = 0;
	qdev->msi_data = 0;

	return 0;
}

static const struct vfio_pci_device_ops qcom_vfio_pci_device_ops = {
	/* No device-specific operations needed */
};

static int qcom_vfio_pci_open_device(struct vfio_device *core_vdev)
{
	struct qcom_vfio_pci_device *qdev =
		container_of(core_vdev, struct qcom_vfio_pci_device, vdev.vdev);
	struct vfio_pci_core_device *vdev = &qdev->vdev;
	int ret;

	ret = vfio_pci_core_enable(vdev);
	if (ret)
		return ret;

	vfio_pci_core_finish_enable(vdev);
	return 0;
}

static void qcom_vfio_pci_close_device(struct vfio_device *core_vdev)
{
	struct qcom_vfio_pci_device *qdev =
		container_of(core_vdev, struct qcom_vfio_pci_device, vdev.vdev);

	qdev->msi_cached = false;
	qdev->msi_addr_lo = 0;
	qdev->msi_addr_hi = 0;
	qdev->msi_data = 0;

	vfio_pci_core_close_device(core_vdev);
}

/*
 * Wrap ioctl to intercept MSI allocation and cache host MSI values.
 * This is called after QEMU allocates MSI via VFIO_DEVICE_SET_IRQS.
 */
static long qcom_vfio_pci_ioctl(struct vfio_device *core_vdev,
				unsigned int cmd, unsigned long arg)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	struct qcom_vfio_pci_device *qdev = to_qcom_vdev(vdev);
	bool is_msi_trigger = false;
	struct vfio_irq_set hdr;
	long ret;

	/*
	 * Pre-fetch SET_IRQS header to avoid TOCTOU from
	 * re-reading userspace after the core ioctl completes.
	 */
	if (cmd == VFIO_DEVICE_SET_IRQS) {
		unsigned long minsz = offsetofend(struct vfio_irq_set, count);

		if (!copy_from_user(&hdr, (void __user *)arg, minsz)) {
			is_msi_trigger = (hdr.index == VFIO_PCI_MSI_IRQ_INDEX &&
					  (hdr.flags & VFIO_IRQ_SET_ACTION_TRIGGER));
			if (is_msi_trigger)
				pci_dbg(qdev->vdev.pdev,
					"SET_IRQS: idx=%u flags=0x%x start=%u count=%u\n",
					hdr.index, hdr.flags, hdr.start, hdr.count);
		}
	}

	ret = vfio_pci_core_ioctl(core_vdev, cmd, arg);

	if (ret == 0 && is_msi_trigger && !qdev->msi_cached)
		qcom_cache_and_write_msi(qdev);

	return ret;
}

static const struct vfio_device_ops qcom_vfio_pci_ops = {
	.name		= "qcom-vfio-pci",
	.init		= qcom_vfio_pci_init_dev,
	.release	= vfio_pci_core_release_dev,
	.open_device	= qcom_vfio_pci_open_device,
	.close_device	= qcom_vfio_pci_close_device,
	.ioctl		= qcom_vfio_pci_ioctl,
	.get_region_info_caps = vfio_pci_ioctl_get_region_info,
	.device_feature	= vfio_pci_core_ioctl_feature,
	.read		= qcom_vfio_pci_read,
	.write		= vfio_pci_core_write,
	.mmap		= vfio_pci_core_mmap,
	.request	= vfio_pci_core_request,
	.match		= vfio_pci_core_match,
	.match_token_uuid = vfio_pci_core_match_token_uuid,
	.bind_iommufd	= vfio_iommufd_physical_bind,
	.unbind_iommufd	= vfio_iommufd_physical_unbind,
	.attach_ioas	= vfio_iommufd_physical_attach_ioas,
	.detach_ioas	= vfio_iommufd_physical_detach_ioas,
};

static int qcom_vfio_pci_probe(struct pci_dev *pdev,
			       const struct pci_device_id *id)
{
	struct qcom_vfio_pci_device *qdev;
	int ret;

	qdev = vfio_alloc_device(qcom_vfio_pci_device, vdev.vdev,
				 &pdev->dev, &qcom_vfio_pci_ops);
	if (IS_ERR(qdev))
		return PTR_ERR(qdev);

	dev_set_drvdata(&pdev->dev, &qdev->vdev);
	qdev->vdev.pci_ops = &qcom_vfio_pci_device_ops;
	ret = vfio_pci_core_register_device(&qdev->vdev);
	if (ret)
		goto out_put_vdev;

	return 0;

out_put_vdev:
	vfio_put_device(&qdev->vdev.vdev);
	return ret;
}

static void qcom_vfio_pci_remove(struct pci_dev *pdev)
{
	struct vfio_pci_core_device *vdev = dev_get_drvdata(&pdev->dev);
	struct qcom_vfio_pci_device *qdev = to_qcom_vdev(vdev);

	vfio_pci_core_unregister_device(&qdev->vdev);
	vfio_put_device(&qdev->vdev.vdev);
}

static const struct pci_device_id qcom_vfio_pci_table[] = {
	/* Qualcomm ath11k WiFi */
	{ PCI_DRIVER_OVERRIDE_DEVICE_VFIO(PCI_VENDOR_ID_QCOM, 0x1103) },
	/* Qualcomm ath12k WiFi */
	{ PCI_DRIVER_OVERRIDE_DEVICE_VFIO(PCI_VENDOR_ID_QCOM, 0x1107) },
	{ }
};

MODULE_DEVICE_TABLE(pci, qcom_vfio_pci_table);

static struct pci_driver qcom_vfio_pci_driver = {
	.name			= "qcom-vfio-pci",
	.id_table		= qcom_vfio_pci_table,
	.probe			= qcom_vfio_pci_probe,
	.remove			= qcom_vfio_pci_remove,
	.driver_managed_dma	= true,
};

static char ids[1024] __initdata;
module_param_string(ids, ids, sizeof(ids), 0);
MODULE_PARM_DESC(ids, "Initial PCI IDs to add to the driver, format is \"vendor:device[:subvendor[:subdevice[:class[:class_mask]]]]\" and multiple comma separated entries can be specified");

static void __init qcom_vfio_pci_fill_ids(void)
{
	char *p, *id;
	int rc;

	if (ids[0] == '\0')
		return;

	p = ids;
	while ((id = strsep(&p, ","))) {
		unsigned int vendor, device, subvendor = PCI_ANY_ID,
			subdevice = PCI_ANY_ID, class = 0, class_mask = 0;
		int fields;

		if (!strlen(id))
			continue;

		fields = sscanf(id, "%x:%x:%x:%x:%x:%x",
				&vendor, &device, &subvendor, &subdevice,
				&class, &class_mask);

		if (fields < 2) {
			pr_warn("qcom-vfio-pci: invalid id string \"%s\"\n", id);
			continue;
		}

		rc = pci_add_dynid(&qcom_vfio_pci_driver, vendor, device,
				   subvendor, subdevice, class, class_mask, 0);
		if (rc)
			pr_warn("qcom-vfio-pci: failed to add dynamic id [%04x:%04x[%04x:%04x]] class %#08x/%08x (%d)\n",
				vendor, device, subvendor, subdevice,
				class, class_mask, rc);
		else
			pr_info("qcom-vfio-pci: add [%04x:%04x[%04x:%04x]] class %#08x/%08x\n",
				vendor, device, subvendor, subdevice,
				class, class_mask);
	}
}

static int __init qcom_vfio_pci_init(void)
{
	int ret;

	ret = pci_register_driver(&qcom_vfio_pci_driver);
	if (ret)
		return ret;

	qcom_vfio_pci_fill_ids();

	return 0;
}

static void __exit qcom_vfio_pci_exit(void)
{
	pci_unregister_driver(&qcom_vfio_pci_driver);
}

module_init(qcom_vfio_pci_init);
module_exit(qcom_vfio_pci_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jose Ignacio Tornos Martinez <jtornosm@redhat.com>");
MODULE_DESCRIPTION("Qualcomm VFIO PCI - Device-Specific VFIO PCI driver for Qualcomm devices");
