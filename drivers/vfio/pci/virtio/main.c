// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/vfio.h>
#include <linux/vfio_pci_core.h>
#include <linux/virtio_pci.h>
#include <linux/virtio_net.h>
#include <linux/virtio_pci_admin.h>

struct virtiovf_pci_core_device {
	struct vfio_pci_core_device core_device;
	u8 bar0_virtual_buf_size;
	u8 *bar0_virtual_buf;
	/* synchronize access to the virtual buf */
	struct mutex bar_mutex;
	void __iomem *notify_addr;
	u32 notify_offset;
	u8 notify_bar;
	u16 pci_cmd;
	u16 msix_ctrl;
};

static int
virtiovf_issue_legacy_rw_cmd(struct virtiovf_pci_core_device *virtvdev,
			     loff_t pos, char __user *buf,
			     size_t count, bool read)
{
	bool msix_enabled = virtvdev->msix_ctrl & PCI_MSIX_FLAGS_ENABLE;
	struct pci_dev *pdev = virtvdev->core_device.pdev;
	u8 *bar0_buf = virtvdev->bar0_virtual_buf;
	u16 opcode;
	int ret;

	mutex_lock(&virtvdev->bar_mutex);
	if (read) {
		opcode = (pos < VIRTIO_PCI_CONFIG_OFF(msix_enabled)) ?
			VIRTIO_ADMIN_CMD_LEGACY_COMMON_CFG_READ :
			VIRTIO_ADMIN_CMD_LEGACY_DEV_CFG_READ;
		ret = virtio_pci_admin_legacy_io_read(pdev, opcode, pos, count,
						      bar0_buf + pos);
		if (ret)
			goto out;
		if (copy_to_user(buf, bar0_buf + pos, count))
			ret = -EFAULT;
		goto out;
	}

	if (copy_from_user(bar0_buf + pos, buf, count)) {
		ret = -EFAULT;
		goto out;
	}

	opcode = (pos < VIRTIO_PCI_CONFIG_OFF(msix_enabled)) ?
			VIRTIO_ADMIN_CMD_LEGACY_COMMON_CFG_WRITE :
			VIRTIO_ADMIN_CMD_LEGACY_DEV_CFG_WRITE;
	ret = virtio_pci_admin_legacy_io_write(pdev, opcode, pos, count,
					       bar0_buf + pos);
out:
	mutex_unlock(&virtvdev->bar_mutex);
	return ret;
}

static int
translate_io_bar_to_mem_bar(struct virtiovf_pci_core_device *virtvdev,
			    loff_t pos, char __user *buf,
			    size_t count, bool read)
{
	struct vfio_pci_core_device *core_device = &virtvdev->core_device;
	u16 queue_notify;
	int ret;

	if (pos + count > virtvdev->bar0_virtual_buf_size)
		return -EINVAL;

	switch (pos) {
	case VIRTIO_PCI_QUEUE_NOTIFY:
		if (count != sizeof(queue_notify))
			return -EINVAL;
		if (read) {
			ret = vfio_pci_ioread16(core_device, true, &queue_notify,
						virtvdev->notify_addr);
			if (ret)
				return ret;
			if (copy_to_user(buf, &queue_notify,
					 sizeof(queue_notify)))
				return -EFAULT;
			break;
		}

		if (copy_from_user(&queue_notify, buf, count))
			return -EFAULT;

		ret = vfio_pci_iowrite16(core_device, true, queue_notify,
					 virtvdev->notify_addr);
		break;
	default:
		ret = virtiovf_issue_legacy_rw_cmd(virtvdev, pos, buf, count,
						   read);
	}

	return ret ? ret : count;
}

static bool range_intersect_range(loff_t range1_start, size_t count1,
				  loff_t range2_start, size_t count2,
				  loff_t *start_offset,
				  size_t *intersect_count,
				  size_t *register_offset)
{
	if (range1_start <= range2_start &&
	    range1_start + count1 > range2_start) {
		*start_offset = range2_start - range1_start;
		*intersect_count = min_t(size_t, count2,
					 range1_start + count1 - range2_start);
		if (register_offset)
			*register_offset = 0;
		return true;
	}

	if (range1_start > range2_start &&
	    range1_start < range2_start + count2) {
		*start_offset = range1_start;
		*intersect_count = min_t(size_t, count1,
					 range2_start + count2 - range1_start);
		if (register_offset)
			*register_offset = range1_start - range2_start;
		return true;
	}

	return false;
}

static ssize_t virtiovf_pci_read_config(struct vfio_device *core_vdev,
					char __user *buf, size_t count,
					loff_t *ppos)
{
	struct virtiovf_pci_core_device *virtvdev = container_of(
		core_vdev, struct virtiovf_pci_core_device, core_device.vdev);
	loff_t pos = *ppos & VFIO_PCI_OFFSET_MASK;
	size_t register_offset;
	loff_t copy_offset;
	size_t copy_count;
	__le32 val32;
	__le16 val16;
	u8 val8;
	int ret;

	ret = vfio_pci_core_read(core_vdev, buf, count, ppos);
	if (ret < 0)
		return ret;

	if (range_intersect_range(pos, count, PCI_DEVICE_ID, sizeof(val16),
				  &copy_offset, &copy_count, NULL)) {
		val16 = cpu_to_le16(0x1000);
		if (copy_to_user(buf + copy_offset, &val16, copy_count))
			return -EFAULT;
	}

	if ((virtvdev->pci_cmd & PCI_COMMAND_IO) &&
	    range_intersect_range(pos, count, PCI_COMMAND, sizeof(val16),
				  &copy_offset, &copy_count, &register_offset)) {
		if (copy_from_user((void *)&val16 + register_offset, buf + copy_offset,
				   copy_count))
			return -EFAULT;
		val16 |= cpu_to_le16(PCI_COMMAND_IO);
		if (copy_to_user(buf + copy_offset, (void *)&val16 + register_offset,
				 copy_count))
			return -EFAULT;
	}

	if (range_intersect_range(pos, count, PCI_REVISION_ID, sizeof(val8),
				  &copy_offset, &copy_count, NULL)) {
		/* Transional needs to have revision 0 */
		val8 = 0;
		if (copy_to_user(buf + copy_offset, &val8, copy_count))
			return -EFAULT;
	}

	if (range_intersect_range(pos, count, PCI_BASE_ADDRESS_0, sizeof(val32),
				  &copy_offset, &copy_count, NULL)) {
		val32 = cpu_to_le32(PCI_BASE_ADDRESS_SPACE_IO);
		if (copy_to_user(buf + copy_offset, &val32, copy_count))
			return -EFAULT;
	}

	if (range_intersect_range(pos, count, PCI_SUBSYSTEM_ID, sizeof(val16),
				  &copy_offset, &copy_count, NULL)) {
		/*
		 * Transitional devices use the PCI subsystem device id as
		 * virtio device id, same as legacy driver always did.
		 */
		val16 = cpu_to_le16(VIRTIO_ID_NET);
		if (copy_to_user(buf + copy_offset, &val16, copy_count))
			return -EFAULT;
	}

	return count;
}

static ssize_t
virtiovf_pci_core_read(struct vfio_device *core_vdev, char __user *buf,
		       size_t count, loff_t *ppos)
{
	struct virtiovf_pci_core_device *virtvdev = container_of(
		core_vdev, struct virtiovf_pci_core_device, core_device.vdev);
	struct pci_dev *pdev = virtvdev->core_device.pdev;
	unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	loff_t pos = *ppos & VFIO_PCI_OFFSET_MASK;
	int ret;

	if (!count)
		return 0;

	if (index == VFIO_PCI_CONFIG_REGION_INDEX)
		return virtiovf_pci_read_config(core_vdev, buf, count, ppos);

	if (index != VFIO_PCI_BAR0_REGION_INDEX)
		return vfio_pci_core_read(core_vdev, buf, count, ppos);

	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret) {
		pci_info_ratelimited(pdev, "runtime resume failed %d\n",
				     ret);
		return -EIO;
	}

	ret = translate_io_bar_to_mem_bar(virtvdev, pos, buf, count, true);
	pm_runtime_put(&pdev->dev);
	return ret;
}

static ssize_t
virtiovf_pci_core_write(struct vfio_device *core_vdev, const char __user *buf,
			size_t count, loff_t *ppos)
{
	struct virtiovf_pci_core_device *virtvdev = container_of(
		core_vdev, struct virtiovf_pci_core_device, core_device.vdev);
	struct pci_dev *pdev = virtvdev->core_device.pdev;
	unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	loff_t pos = *ppos & VFIO_PCI_OFFSET_MASK;
	int ret;

	if (!count)
		return 0;

	if (index == VFIO_PCI_CONFIG_REGION_INDEX) {
		size_t register_offset;
		loff_t copy_offset;
		size_t copy_count;

		if (range_intersect_range(pos, count, PCI_COMMAND, sizeof(virtvdev->pci_cmd),
					  &copy_offset, &copy_count,
					  &register_offset)) {
			if (copy_from_user((void *)&virtvdev->pci_cmd + register_offset,
					   buf + copy_offset,
					   copy_count))
				return -EFAULT;
		}

		if (range_intersect_range(pos, count, pdev->msix_cap + PCI_MSIX_FLAGS,
					  sizeof(virtvdev->msix_ctrl),
					  &copy_offset, &copy_count,
					  &register_offset)) {
			if (copy_from_user((void *)&virtvdev->msix_ctrl + register_offset,
					   buf + copy_offset,
					   copy_count))
				return -EFAULT;
		}
	}

	if (index != VFIO_PCI_BAR0_REGION_INDEX)
		return vfio_pci_core_write(core_vdev, buf, count, ppos);

	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret) {
		pci_info_ratelimited(pdev, "runtime resume failed %d\n", ret);
		return -EIO;
	}

	ret = translate_io_bar_to_mem_bar(virtvdev, pos, (char __user *)buf, count, false);
	pm_runtime_put(&pdev->dev);
	return ret;
}

static int
virtiovf_pci_ioctl_get_region_info(struct vfio_device *core_vdev,
				   unsigned int cmd, unsigned long arg)
{
	struct virtiovf_pci_core_device *virtvdev = container_of(
		core_vdev, struct virtiovf_pci_core_device, core_device.vdev);
	unsigned long minsz = offsetofend(struct vfio_region_info, offset);
	void __user *uarg = (void __user *)arg;
	struct vfio_region_info info = {};

	if (copy_from_user(&info, uarg, minsz))
		return -EFAULT;

	if (info.argsz < minsz)
		return -EINVAL;

	switch (info.index) {
	case VFIO_PCI_BAR0_REGION_INDEX:
		info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
		info.size = virtvdev->bar0_virtual_buf_size;
		info.flags = VFIO_REGION_INFO_FLAG_READ |
			     VFIO_REGION_INFO_FLAG_WRITE;
		return copy_to_user(uarg, &info, minsz) ? -EFAULT : 0;
	default:
		return vfio_pci_core_ioctl(core_vdev, cmd, arg);
	}
}

static long
virtiovf_vfio_pci_core_ioctl(struct vfio_device *core_vdev, unsigned int cmd,
			     unsigned long arg)
{
	switch (cmd) {
	case VFIO_DEVICE_GET_REGION_INFO:
		return virtiovf_pci_ioctl_get_region_info(core_vdev, cmd, arg);
	default:
		return vfio_pci_core_ioctl(core_vdev, cmd, arg);
	}
}

static int
virtiovf_set_notify_addr(struct virtiovf_pci_core_device *virtvdev)
{
	struct vfio_pci_core_device *core_device = &virtvdev->core_device;
	int ret;

	/*
	 * Setup the BAR where the 'notify' exists to be used by vfio as well
	 * This will let us mmap it only once and use it when needed.
	 */
	ret = vfio_pci_core_setup_barmap(core_device,
					 virtvdev->notify_bar);
	if (ret)
		return ret;

	virtvdev->notify_addr = core_device->barmap[virtvdev->notify_bar] +
			virtvdev->notify_offset;
	return 0;
}

static int virtiovf_pci_open_device(struct vfio_device *core_vdev)
{
	struct virtiovf_pci_core_device *virtvdev = container_of(
		core_vdev, struct virtiovf_pci_core_device, core_device.vdev);
	struct vfio_pci_core_device *vdev = &virtvdev->core_device;
	int ret;

	ret = vfio_pci_core_enable(vdev);
	if (ret)
		return ret;

	if (virtvdev->bar0_virtual_buf) {
		/*
		 * Upon close_device() the vfio_pci_core_disable() is called
		 * and will close all the previous mmaps, so it seems that the
		 * valid life cycle for the 'notify' addr is per open/close.
		 */
		ret = virtiovf_set_notify_addr(virtvdev);
		if (ret) {
			vfio_pci_core_disable(vdev);
			return ret;
		}
	}

	vfio_pci_core_finish_enable(vdev);
	return 0;
}

static int virtiovf_get_device_config_size(unsigned short device)
{
	/* Network card */
	return offsetofend(struct virtio_net_config, status);
}

static int virtiovf_read_notify_info(struct virtiovf_pci_core_device *virtvdev)
{
	u64 offset;
	int ret;
	u8 bar;

	ret = virtio_pci_admin_legacy_io_notify_info(virtvdev->core_device.pdev,
				VIRTIO_ADMIN_CMD_NOTIFY_INFO_FLAGS_OWNER_MEM,
				&bar, &offset);
	if (ret)
		return ret;

	virtvdev->notify_bar = bar;
	virtvdev->notify_offset = offset;
	return 0;
}

static int virtiovf_pci_init_device(struct vfio_device *core_vdev)
{
	struct virtiovf_pci_core_device *virtvdev = container_of(
		core_vdev, struct virtiovf_pci_core_device, core_device.vdev);
	struct pci_dev *pdev;
	int ret;

	ret = vfio_pci_core_init_dev(core_vdev);
	if (ret)
		return ret;

	pdev = virtvdev->core_device.pdev;
	ret = virtiovf_read_notify_info(virtvdev);
	if (ret)
		return ret;

	/* Being ready with a buffer that supports MSIX */
	virtvdev->bar0_virtual_buf_size = VIRTIO_PCI_CONFIG_OFF(true) +
				virtiovf_get_device_config_size(pdev->device);
	virtvdev->bar0_virtual_buf = kzalloc(virtvdev->bar0_virtual_buf_size,
					     GFP_KERNEL);
	if (!virtvdev->bar0_virtual_buf)
		return -ENOMEM;
	mutex_init(&virtvdev->bar_mutex);
	return 0;
}

static void virtiovf_pci_core_release_dev(struct vfio_device *core_vdev)
{
	struct virtiovf_pci_core_device *virtvdev = container_of(
		core_vdev, struct virtiovf_pci_core_device, core_device.vdev);

	kfree(virtvdev->bar0_virtual_buf);
	vfio_pci_core_release_dev(core_vdev);
}

static const struct vfio_device_ops virtiovf_acc_vfio_pci_tran_ops = {
	.name = "virtio-transitional-vfio-pci",
	.init = virtiovf_pci_init_device,
	.release = virtiovf_pci_core_release_dev,
	.open_device = virtiovf_pci_open_device,
	.close_device = vfio_pci_core_close_device,
	.ioctl = virtiovf_vfio_pci_core_ioctl,
	.read = virtiovf_pci_core_read,
	.write = virtiovf_pci_core_write,
	.mmap = vfio_pci_core_mmap,
	.request = vfio_pci_core_request,
	.match = vfio_pci_core_match,
	.bind_iommufd = vfio_iommufd_physical_bind,
	.unbind_iommufd = vfio_iommufd_physical_unbind,
	.attach_ioas = vfio_iommufd_physical_attach_ioas,
};

static const struct vfio_device_ops virtiovf_acc_vfio_pci_ops = {
	.name = "virtio-acc-vfio-pci",
	.init = vfio_pci_core_init_dev,
	.release = vfio_pci_core_release_dev,
	.open_device = virtiovf_pci_open_device,
	.close_device = vfio_pci_core_close_device,
	.ioctl = vfio_pci_core_ioctl,
	.device_feature = vfio_pci_core_ioctl_feature,
	.read = vfio_pci_core_read,
	.write = vfio_pci_core_write,
	.mmap = vfio_pci_core_mmap,
	.request = vfio_pci_core_request,
	.match = vfio_pci_core_match,
	.bind_iommufd = vfio_iommufd_physical_bind,
	.unbind_iommufd = vfio_iommufd_physical_unbind,
	.attach_ioas = vfio_iommufd_physical_attach_ioas,
};

static bool virtiovf_bar0_exists(struct pci_dev *pdev)
{
	struct resource *res = pdev->resource;

	return res->flags ? true : false;
}

#define VIRTIOVF_USE_ADMIN_CMD_BITMAP \
	(BIT_ULL(VIRTIO_ADMIN_CMD_LIST_QUERY) | \
	 BIT_ULL(VIRTIO_ADMIN_CMD_LIST_USE) | \
	 BIT_ULL(VIRTIO_ADMIN_CMD_LEGACY_COMMON_CFG_WRITE) | \
	 BIT_ULL(VIRTIO_ADMIN_CMD_LEGACY_COMMON_CFG_READ) | \
	 BIT_ULL(VIRTIO_ADMIN_CMD_LEGACY_DEV_CFG_WRITE) | \
	 BIT_ULL(VIRTIO_ADMIN_CMD_LEGACY_DEV_CFG_READ) | \
	 BIT_ULL(VIRTIO_ADMIN_CMD_LEGACY_NOTIFY_INFO))

static bool virtiovf_support_legacy_access(struct pci_dev *pdev)
{
	int buf_size = DIV_ROUND_UP(VIRTIO_ADMIN_MAX_CMD_OPCODE, 64) * 8;
	u8 *buf;
	int ret;

	buf = kzalloc(buf_size, GFP_KERNEL);
	if (!buf)
		return false;

	ret = virtio_pci_admin_list_query(pdev, buf, buf_size);
	if (ret)
		goto end;

	if ((le64_to_cpup((__le64 *)buf) & VIRTIOVF_USE_ADMIN_CMD_BITMAP) !=
		VIRTIOVF_USE_ADMIN_CMD_BITMAP) {
		ret = -EOPNOTSUPP;
		goto end;
	}

	/* Confirm the used commands */
	memset(buf, 0, buf_size);
	*(__le64 *)buf = cpu_to_le64(VIRTIOVF_USE_ADMIN_CMD_BITMAP);
	ret = virtio_pci_admin_list_use(pdev, buf, buf_size);
end:
	kfree(buf);
	return ret ? false : true;
}

static int virtiovf_pci_probe(struct pci_dev *pdev,
			      const struct pci_device_id *id)
{
	const struct vfio_device_ops *ops = &virtiovf_acc_vfio_pci_ops;
	struct virtiovf_pci_core_device *virtvdev;
	int ret;

	if (pdev->is_virtfn && virtiovf_support_legacy_access(pdev) &&
	    !virtiovf_bar0_exists(pdev) && pdev->msix_cap)
		ops = &virtiovf_acc_vfio_pci_tran_ops;

	virtvdev = vfio_alloc_device(virtiovf_pci_core_device, core_device.vdev,
				     &pdev->dev, ops);
	if (IS_ERR(virtvdev))
		return PTR_ERR(virtvdev);

	dev_set_drvdata(&pdev->dev, &virtvdev->core_device);
	ret = vfio_pci_core_register_device(&virtvdev->core_device);
	if (ret)
		goto out;
	return 0;
out:
	vfio_put_device(&virtvdev->core_device.vdev);
	return ret;
}

static void virtiovf_pci_remove(struct pci_dev *pdev)
{
	struct virtiovf_pci_core_device *virtvdev = dev_get_drvdata(&pdev->dev);

	vfio_pci_core_unregister_device(&virtvdev->core_device);
	vfio_put_device(&virtvdev->core_device.vdev);
}

static const struct pci_device_id virtiovf_pci_table[] = {
	/* Only virtio-net is supported/tested so far */
	{ PCI_DRIVER_OVERRIDE_DEVICE_VFIO(PCI_VENDOR_ID_REDHAT_QUMRANET, 0x1041) },
	{}
};

MODULE_DEVICE_TABLE(pci, virtiovf_pci_table);

static struct pci_driver virtiovf_pci_driver = {
	.name = KBUILD_MODNAME,
	.id_table = virtiovf_pci_table,
	.probe = virtiovf_pci_probe,
	.remove = virtiovf_pci_remove,
	.err_handler = &vfio_pci_core_err_handlers,
	.driver_managed_dma = true,
};

module_pci_driver(virtiovf_pci_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yishai Hadas <yishaih@nvidia.com>");
MODULE_DESCRIPTION(
	"VIRTIO VFIO PCI - User Level meta-driver for VIRTIO device family");
