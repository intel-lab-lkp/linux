// SPDX-License-Identifier: GPL-2.0
/*
 * vfio-ISM driver for s390
 *
 * Copyright IBM Corp.
 */

#include "../vfio_pci_priv.h"

#define ISM_VFIO_PCI_OFFSET_SHIFT   48
#define ISM_VFIO_PCI_OFFSET_TO_INDEX(off) (off >> ISM_VFIO_PCI_OFFSET_SHIFT)
#define ISM_VFIO_PCI_INDEX_TO_OFFSET(index) ((u64)(index) << ISM_VFIO_PCI_OFFSET_SHIFT)
#define ISM_VFIO_PCI_OFFSET_MASK (((u64)(1) << ISM_VFIO_PCI_OFFSET_SHIFT) - 1)

struct ism_vfio_pci_core_device {
	struct vfio_pci_core_device core_device;
};

static int ism_pci_open_device(struct vfio_device *core_vdev)
{
	struct ism_vfio_pci_core_device *ivdev;
	struct vfio_pci_core_device *vdev;
	int ret;

	ivdev = container_of(core_vdev, struct ism_vfio_pci_core_device,
			     core_device.vdev);
	vdev = &ivdev->core_device;

	ret = vfio_pci_core_enable(vdev);
	if (ret)
		return ret;

	vfio_pci_core_finish_enable(vdev);
	return 0;
}

static ssize_t ism_vfio_pci_do_io_r(struct vfio_pci_core_device *vdev,
				    char __user *buf, loff_t off, size_t count,
				    int bar)
{
	struct zpci_dev *zdev = to_zpci(vdev->pdev);
	ssize_t ret, done = 0;
	u64 req, length, tmp;

	while (count) {
		if (count >= 8 && IS_ALIGNED(off, 8))
			length = 8;
		else if (count >= 4 && IS_ALIGNED(off, 4))
			length = 4;
		else if (count >= 2 && IS_ALIGNED(off, 2))
			length = 2;
		else
			length = 1;
		req = ZPCI_CREATE_REQ(READ_ONCE(zdev->fh), bar, length);
		/* use pcilg to prevent using MIO instructions */
		ret = __zpci_load(&tmp, req, off);
		if (ret)
			return ret;
		if (copy_to_user(buf, &tmp, length))
			return -EFAULT;
		count -= length;
		done += length;
		off += length;
		buf += length;
	}
	return done;
}

static ssize_t ism_vfio_pci_do_io_w(struct vfio_pci_core_device *vdev,
				    char __user *buf, loff_t off, size_t count,
				    int bar)
{
	struct zpci_dev *zdev = to_zpci(vdev->pdev);
	void *data __free(kfree) = NULL;
	ssize_t ret;
	u64 req;

	if (count > zdev->maxstbl)
		return -EINVAL;
	data = kzalloc(count, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	if (copy_from_user(data, buf, count))
		return -EFAULT;
	req = ZPCI_CREATE_REQ(READ_ONCE(zdev->fh), bar, count);
	ret = __zpci_store_block(data, req, off);
	if (ret)
		return ret;
	return count;
}

static ssize_t ism_vfio_pci_bar_rw(struct vfio_pci_core_device *vdev,
				   char __user *buf, size_t count, loff_t *ppos,
				   bool iswrite)
{
	int bar = ISM_VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	loff_t pos = *ppos & ISM_VFIO_PCI_OFFSET_MASK;
	resource_size_t end;
	ssize_t done = 0;

	if (pci_resource_start(vdev->pdev, bar))
		end = pci_resource_len(vdev->pdev, bar);
	else
		return -EINVAL;

	if (pos >= end)
		return -EINVAL;

	count = min(count, (size_t)(end - pos));

	if (iswrite)
		done = ism_vfio_pci_do_io_w(vdev, buf, pos, count, bar);
	else
		done = ism_vfio_pci_do_io_r(vdev, buf, pos, count, bar);

	if (done >= 0)
		*ppos += done;

	return done;
}

static ssize_t ism_vfio_pci_config_rw(struct vfio_pci_core_device *vdev,
				      char __user *buf, size_t count,
				      loff_t *ppos, bool iswrite)
{
	loff_t pos = *ppos;
	size_t done = 0;
	int ret = 0;

	pos &= ISM_VFIO_PCI_OFFSET_MASK;

	while (count) {
		/*
		 * zPCI must not use MIO instructions for config space access,
		 * so we can use common code path here.
		 */
		ret = vfio_pci_config_rw_single(vdev, buf, count, &pos, iswrite);
		if (ret < 0)
			return ret;

		count -= ret;
		done += ret;
		buf += ret;
		pos += ret;
	}

	*ppos += done;

	return done;
}

static ssize_t ism_vfio_pci_rw(struct vfio_device *core_vdev, char __user *buf,
			       size_t count, loff_t *ppos, bool iswrite)
{
	unsigned int index = ISM_VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	struct vfio_pci_core_device *vdev;
	int ret;

	vdev = container_of(core_vdev, struct vfio_pci_core_device, vdev);

	if (!count)
		return 0;

	switch (index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:
		ret = ism_vfio_pci_config_rw(vdev, buf, count, ppos, iswrite);
		break;

	case VFIO_PCI_BAR0_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
		ret = ism_vfio_pci_bar_rw(vdev, buf, count, ppos, iswrite);
		break;

	default:
		return -EINVAL;
	}

	return ret;
}

static ssize_t ism_vfio_pci_read(struct vfio_device *core_vdev,
				 char __user *buf, size_t count, loff_t *ppos)
{
	return ism_vfio_pci_rw(core_vdev, buf, count, ppos, false);
}

static ssize_t ism_vfio_pci_write(struct vfio_device *core_vdev,
				  const char __user *buf, size_t count,
				  loff_t *ppos)
{
	return ism_vfio_pci_rw(core_vdev, (char __user *)buf, count, ppos,
			       true);
}

static int ism_vfio_pci_ioctl_get_region_info(struct vfio_device *core_vdev,
					      struct vfio_region_info *info,
					      struct vfio_info_cap *caps)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	struct pci_dev *pdev = vdev->pdev;

	switch (info->index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:
		info->offset = ISM_VFIO_PCI_INDEX_TO_OFFSET(info->index);
		info->size = pdev->cfg_size;
		info->flags = VFIO_REGION_INFO_FLAG_READ |
			      VFIO_REGION_INFO_FLAG_WRITE;
		break;
	case VFIO_PCI_BAR0_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
		info->offset = ISM_VFIO_PCI_INDEX_TO_OFFSET(info->index);
		info->size = pci_resource_len(pdev, info->index);
		if (!info->size) {
			info->flags = 0;
			break;
		}
		info->flags = VFIO_REGION_INFO_FLAG_READ |
			      VFIO_REGION_INFO_FLAG_WRITE;
		break;
	default:
		info->offset = 0;
		info->size = 0;
		info->flags = 0;
	}
	return 0;
}

static const struct vfio_device_ops ism_pci_ops = {
	.name = "ism-vfio-pci",
	.init = vfio_pci_core_init_dev,
	.release = vfio_pci_core_release_dev,
	.open_device = ism_pci_open_device,
	.close_device = vfio_pci_core_close_device,
	.ioctl = vfio_pci_core_ioctl,
	.get_region_info_caps = ism_vfio_pci_ioctl_get_region_info,
	.device_feature = vfio_pci_core_ioctl_feature,
	.read = ism_vfio_pci_read,
	.write = ism_vfio_pci_write,
	.request = vfio_pci_core_request,
	.match = vfio_pci_core_match,
	.match_token_uuid = vfio_pci_core_match_token_uuid,
	.bind_iommufd = vfio_iommufd_physical_bind,
	.unbind_iommufd = vfio_iommufd_physical_unbind,
	.attach_ioas = vfio_iommufd_physical_attach_ioas,
	.detach_ioas = vfio_iommufd_physical_detach_ioas,
};

static int ism_vfio_pci_probe(struct pci_dev *pdev,
			      const struct pci_device_id *id)
{
	struct ism_vfio_pci_core_device *ivpcd;
	int ret;

	ivpcd = vfio_alloc_device(ism_vfio_pci_core_device, core_device.vdev,
				  &pdev->dev, &ism_pci_ops);
	if (IS_ERR(ivpcd))
		return PTR_ERR(ivpcd);

	dev_set_drvdata(&pdev->dev, &ivpcd->core_device);
	ret = vfio_pci_core_register_device(&ivpcd->core_device);
	if (ret)
		vfio_put_device(&ivpcd->core_device.vdev);
	return ret;
}

static void ism_vfio_pci_remove(struct pci_dev *pdev)
{
	struct vfio_pci_core_device *core_device;
	struct ism_vfio_pci_core_device *ivpcd;

	core_device = dev_get_drvdata(&pdev->dev);
	ivpcd = container_of(core_device, struct ism_vfio_pci_core_device,
			     core_device);

	vfio_pci_core_unregister_device(&ivpcd->core_device);
	vfio_put_device(&ivpcd->core_device.vdev);
}

static const struct pci_device_id ism_device_table[] = {
	{ PCI_DRIVER_OVERRIDE_DEVICE_VFIO(PCI_VENDOR_ID_IBM,
					  PCI_DEVICE_ID_IBM_ISM) },
	{}
};
MODULE_DEVICE_TABLE(pci, ism_device_table);

static struct pci_driver ism_vfio_pci_driver = {
	.name = KBUILD_MODNAME,
	.id_table = ism_device_table,
	.probe = ism_vfio_pci_probe,
	.remove = ism_vfio_pci_remove,
	.driver_managed_dma = true,
};

module_pci_driver(ism_vfio_pci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("vfio-pci variant driver for the IBM Internal Shared Memory (ISM) device");
MODULE_AUTHOR("IBM Corporation");
