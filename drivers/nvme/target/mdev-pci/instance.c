// SPDX-License-Identifier: GPL-2.0+
/*
 * Mediated NVMe instance VFIO code
 * Copyright (c) 2019 - Maxim Levitsky
 * Copyright (C) 2025 Oracle Corporation
 */

#include <linux/init.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/vfio.h>
#include <linux/sysfs.h>
#include <linux/mdev.h>
#include "../../../vfio/vfio.h"
#include "../nvmet.h"
#include "priv.h"

#define OFFSET_TO_REGION(offset) ((offset) >> 20)
#define REGION_TO_OFFSET(nr) (((u64)nr) << 20)

#define NVMET_MDEV_NAME "nvmet_mdev_pci"

static struct device *nvmet_mdev_root_dev;

struct mdev_nvme_vfio_region_info {
	struct vfio_region_info base;
	struct vfio_region_info_cap_sparse_mmap mmap_cap;
};

/* User memory removed */
static void nvmet_mdev_dma_unmap(struct vfio_device *vfio_dev, u64 iova,
				 u64 length)
{
	struct nvmet_mdev_vctrl *vctrl = vfio_dev_to_nvmet_mdev_vctrl(vfio_dev);

	mutex_lock(&vctrl->lock);
	/*
	 * TODO: This cannot be run while the device is in use.
	 */
	nvmet_mdev_vctrl_viommu_unmap(vctrl, iova, length);
	mutex_unlock(&vctrl->lock);
}

/* Helper function for bar/pci config read/write access */
static ssize_t nvmet_mdev_access(struct nvmet_mdev_vctrl *vctrl,
				 char *buf, size_t count,
				 loff_t pos, bool is_write)
{
	int index = OFFSET_TO_REGION(pos);
	int ret = -EINVAL;
	unsigned int offset;

	if (index >= VFIO_PCI_NUM_REGIONS || !vctrl->regions[index].rw)
		goto out;

	offset = pos - REGION_TO_OFFSET(index);
	if (offset + count > vctrl->regions[index].size)
		goto out;

	ret = vctrl->regions[index].rw(vctrl, offset, buf, count, is_write);
out:
	return ret;
}

/* Called when read() is done on the device */
static ssize_t nvmet_mdev_read(struct vfio_device *vfio_dev, char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct nvmet_mdev_vctrl *vctrl = vfio_dev_to_nvmet_mdev_vctrl(vfio_dev);
	unsigned int done = 0;
	int ret;

	while (count) {
		size_t filled;

		if (count >= 4 && !(*ppos % 4)) {
			u32 val;

			ret = nvmet_mdev_access(vctrl, (char *)&val,
						sizeof(val), *ppos, false);
			if (ret <= 0)
				goto read_err;

			if (copy_to_user(buf, &val, sizeof(val)))
				goto read_err;
			filled = sizeof(val);
		} else if (count >= 2 && !(*ppos % 2)) {
			u16 val;

			ret = nvmet_mdev_access(vctrl, (char *)&val,
						sizeof(val), *ppos, false);
			if (ret <= 0)
				goto read_err;
			if (copy_to_user(buf, &val, sizeof(val)))
				goto read_err;
			filled = sizeof(val);
		} else {
			u8 val;

			ret = nvmet_mdev_access(vctrl, (char *)&val,
						sizeof(val), *ppos, false);
			if (ret <= 0)
				goto read_err;
			if (copy_to_user(buf, &val, sizeof(val)))
				goto read_err;
			filled = sizeof(val);
		}

		count -= filled;
		done += filled;
		*ppos += filled;
		buf += filled;
	}
	return done;
read_err:
	return -EFAULT;
}

/* Called when write() is done on the device */
static ssize_t nvmet_mdev_write(struct vfio_device *vfio_dev,
				const char __user *buf, size_t count,
				loff_t *ppos)
{
	struct nvmet_mdev_vctrl *vctrl = vfio_dev_to_nvmet_mdev_vctrl(vfio_dev);
	unsigned int done = 0;
	int ret;

	while (count) {
		size_t filled;

		if (count >= 4 && !(*ppos % 4)) {
			u32 val;

			if (copy_from_user(&val, buf, sizeof(val)))
				goto write_err;
			ret = nvmet_mdev_access(vctrl, (char *)&val,
						sizeof(val), *ppos, true);
			if (ret <= 0)
				goto write_err;
			filled = sizeof(val);
		} else if (count >= 2 && !(*ppos % 2)) {
			u16 val;

			if (copy_from_user(&val, buf, sizeof(val)))
				goto write_err;

			ret = nvmet_mdev_access(vctrl, (char *)&val,
						sizeof(val), *ppos, true);
			if (ret <= 0)
				goto write_err;
			filled = sizeof(val);
		} else {
			u8 val;

			if (copy_from_user(&val, buf, sizeof(val)))
				goto write_err;
			ret = nvmet_mdev_access(vctrl, (char *)&val,
						sizeof(val), *ppos, true);
			if (ret <= 0)
				goto write_err;
			filled = sizeof(val);
		}
		count -= filled;
		done += filled;
		*ppos += filled;
		buf += filled;
	}
	return done;
write_err:
	return -EFAULT;
}

/* Helper for IRQ number VFIO query */
static int nvmet_mdev_irq_counts(struct nvmet_mdev_vctrl *vctrl,
				 unsigned int irq_type)
{
	switch (irq_type) {
	case VFIO_PCI_INTX_IRQ_INDEX:
		return 1;
	case VFIO_PCI_MSIX_IRQ_INDEX:
		return MAX_VIRTUAL_IRQS;
	case VFIO_PCI_REQ_IRQ_INDEX:
		return 1;
	default:
		return 0;
	}
}

/* VFIO VFIO_IRQ_SET_ACTION_TRIGGER implementation */
static int nvmet_mdev_ioctl_set_irqs_trigger(struct nvmet_mdev_vctrl *vctrl,
					     u32 flags,
					     unsigned int irq_type,
					     unsigned int start,
					     unsigned int count,
					     void *data)
{
	u32 data_type = flags & VFIO_IRQ_SET_DATA_TYPE_MASK;
	u8 *bools = NULL;
	unsigned int i;
	int ret = -EINVAL;

	/* Asked to disable the current interrupt mode */
	if (data_type == VFIO_IRQ_SET_DATA_NONE && count == 0) {
		switch (irq_type) {
		case VFIO_PCI_REQ_IRQ_INDEX:
			nvmet_mdev_irqs_set_unplug_trigger(vctrl, -1);
			return 0;
		case VFIO_PCI_INTX_IRQ_INDEX:
			nvmet_mdev_irqs_disable(vctrl, NVME_MDEV_IMODE_INTX);
			return 0;
		case VFIO_PCI_MSIX_IRQ_INDEX:
			nvmet_mdev_irqs_disable(vctrl, NVME_MDEV_IMODE_MSIX);
			return 0;
		default:
			return -EINVAL;
		}
	}

	if (start + count > nvmet_mdev_irq_counts(vctrl, irq_type))
		return -EINVAL;

	switch (data_type) {
	case VFIO_IRQ_SET_DATA_BOOL:
		bools = (u8 *)data;
		fallthrough;
	case VFIO_IRQ_SET_DATA_NONE:
		if (irq_type == VFIO_PCI_REQ_IRQ_INDEX)
			return -EINVAL;

		for (i = 0 ; i < count ; i++) {
			int index = start + i;

			if (!bools || bools[i])
				nvmet_mdev_irq_trigger(vctrl, index);
		}
		return 0;

	case VFIO_IRQ_SET_DATA_EVENTFD:
		switch (irq_type) {
		case VFIO_PCI_REQ_IRQ_INDEX:
			return nvmet_mdev_irqs_set_unplug_trigger(vctrl,
							*(int32_t *)data);
		case VFIO_PCI_INTX_IRQ_INDEX:
			ret = nvmet_mdev_irqs_enable(vctrl,
						     NVME_MDEV_IMODE_INTX);
			break;
		case VFIO_PCI_MSIX_IRQ_INDEX:
			ret = nvmet_mdev_irqs_enable(vctrl,
						     NVME_MDEV_IMODE_MSIX);
			break;
		default:
			return -EINVAL;
		}
		if (ret)
			return ret;

		return nvmet_mdev_irqs_set_triggers(vctrl, start, count,
						    (int32_t *)data);
	default:
		return -EINVAL;
	}
}

/* VFIO_DEVICE_GET_INFO ioctl implementation */
static int nvmet_mdev_ioctl_get_info(struct nvmet_mdev_vctrl *vctrl,
				     void __user *arg)
{
	struct vfio_device_info info;
	unsigned int minsz = offsetofend(struct vfio_device_info, num_irqs);

	if (copy_from_user(&info, (void __user *)arg, minsz))
		return -EFAULT;
	if (info.argsz < minsz)
		return -EINVAL;

	info.flags = VFIO_DEVICE_FLAGS_PCI | VFIO_DEVICE_FLAGS_RESET;
	info.num_regions = VFIO_PCI_NUM_REGIONS;
	info.num_irqs = VFIO_PCI_NUM_IRQS;

	if (copy_to_user(arg, &info, minsz))
		return -EFAULT;
	return 0;
}

/* VFIO_DEVICE_GET_REGION_INFO ioctl implementation */
static int nvmet_mdev_ioctl_get_reg_info(struct nvmet_mdev_vctrl *vctrl,
					 void __user *arg)
{
	struct nvmet_mdev_io_region *region;
	struct mdev_nvme_vfio_region_info *info;
	unsigned long minsz, outsz, maxsz;
	int ret = 0;

	minsz = offsetofend(struct vfio_region_info, offset);
	maxsz = sizeof(struct mdev_nvme_vfio_region_info) +
				sizeof(struct vfio_region_sparse_mmap_area);

	info = kzalloc(maxsz, GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	if (copy_from_user(info, arg, minsz)) {
		ret = -EFAULT;
		goto out;
	}

	outsz = info->base.argsz;
	if (outsz < minsz || outsz > maxsz) {
		ret = -EINVAL;
		goto out;
	}

	if (info->base.index >= VFIO_PCI_NUM_REGIONS) {
		ret = -EINVAL;
		goto out;
	}

	region = &vctrl->regions[info->base.index];
	info->base.offset = REGION_TO_OFFSET(info->base.index);
	info->base.argsz = maxsz;
	info->base.size = region->size;

	info->base.flags = VFIO_REGION_INFO_FLAG_READ |
				VFIO_REGION_INFO_FLAG_WRITE;

	if (region->mmap_ops) {
		info->base.flags |= (VFIO_REGION_INFO_FLAG_MMAP |
						VFIO_REGION_INFO_FLAG_CAPS);

		info->base.cap_offset =
			offsetof(struct mdev_nvme_vfio_region_info, mmap_cap);

		info->mmap_cap.header.id = VFIO_REGION_INFO_CAP_SPARSE_MMAP;
		info->mmap_cap.header.version = 1;
		info->mmap_cap.header.next = 0;
		info->mmap_cap.nr_areas = 1;
		info->mmap_cap.areas[0].offset = region->mmap_area_start;
		info->mmap_cap.areas[0].size = region->mmap_area_size;
	}

	if (copy_to_user(arg, info, outsz))
		ret = -EFAULT;
out:
	kfree(info);
	return ret;
}

/* VFIO_DEVICE_GET_IRQ_INFO ioctl implementation */
static int nvmet_mdev_ioctl_get_irq_info(struct nvmet_mdev_vctrl *vctrl,
					 void __user *arg)
{
	struct vfio_irq_info info;
	unsigned int minsz = offsetofend(struct vfio_irq_info, count);

	if (copy_from_user(&info, arg, minsz))
		return -EFAULT;
	if (info.argsz < minsz)
		return -EINVAL;

	info.count = nvmet_mdev_irq_counts(vctrl, info.index);
	info.flags = VFIO_IRQ_INFO_EVENTFD;

	if (info.index == VFIO_PCI_INTX_IRQ_INDEX)
		info.flags |= VFIO_IRQ_INFO_MASKABLE | VFIO_IRQ_INFO_AUTOMASKED;

	if (copy_to_user(arg, &info, minsz))
		return -EFAULT;
	return 0;
}

/* VFIO VFIO_DEVICE_SET_IRQS ioctl implementation */
static int nvmet_mdev_ioctl_set_irqs(struct nvmet_mdev_vctrl *vctrl,
				     void __user *arg)
{
	int ret, irqcount;
	struct vfio_irq_set hdr;
	u8 *data = NULL;
	size_t data_size = 0;
	unsigned long minsz = offsetofend(struct vfio_irq_set, count);

	if (copy_from_user(&hdr, arg, minsz))
		return -EFAULT;

	irqcount = nvmet_mdev_irq_counts(vctrl, hdr.index);
	ret = vfio_set_irqs_validate_and_prepare(&hdr,
						 irqcount,
						 VFIO_PCI_NUM_IRQS,
						 &data_size);
	if (ret)
		return ret;

	if (data_size) {
		data = memdup_user((arg + minsz), data_size);
		if (IS_ERR(data))
			return PTR_ERR(data);
	}

	ret = -ENOTTY;
	switch (hdr.index) {
	case VFIO_PCI_INTX_IRQ_INDEX:
	case VFIO_PCI_MSIX_IRQ_INDEX:
	case VFIO_PCI_REQ_IRQ_INDEX:
		switch (hdr.flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) {
		case VFIO_IRQ_SET_ACTION_MASK:
		case VFIO_IRQ_SET_ACTION_UNMASK:
			/* pretend to support this (even with eventfd) */
			ret = hdr.index == VFIO_PCI_INTX_IRQ_INDEX ?
					0 : -EINVAL;
			break;
		case VFIO_IRQ_SET_ACTION_TRIGGER:
			ret = nvmet_mdev_ioctl_set_irqs_trigger(vctrl,
								hdr.flags,
								hdr.index,
								hdr.start,
								hdr.count,
								data);
			break;
		}
		break;
	}

	kfree(data);
	return ret;
}

static long nvmet_mdev_ioctl(struct vfio_device *vfio_dev, unsigned int cmd,
			     unsigned long arg)
{
	struct nvmet_mdev_vctrl *vctrl = vfio_dev_to_nvmet_mdev_vctrl(vfio_dev);

	switch (cmd) {
	case VFIO_DEVICE_GET_INFO:
		return nvmet_mdev_ioctl_get_info(vctrl, (void __user *)arg);
	case VFIO_DEVICE_GET_REGION_INFO:
		return nvmet_mdev_ioctl_get_reg_info(vctrl, (void __user *)arg);
	case VFIO_DEVICE_GET_IRQ_INFO:
		return nvmet_mdev_ioctl_get_irq_info(vctrl, (void __user *)arg);
	case VFIO_DEVICE_SET_IRQS:
		return nvmet_mdev_ioctl_set_irqs(vctrl, (void __user *)arg);
	case VFIO_DEVICE_RESET:
		nvmet_mdev_vctrl_reset(vctrl);
		return 0;
	default:
		return -ENOTTY;
	}
}

/* mmap() implementation (doorbell area) */
static int nvmet_mdev_mmap(struct vfio_device *vfio_dev,
			   struct vm_area_struct *vma)
{
	struct nvmet_mdev_vctrl *vctrl = vfio_dev_to_nvmet_mdev_vctrl(vfio_dev);
	int index = OFFSET_TO_REGION((u64)vma->vm_pgoff << PAGE_SHIFT);
	unsigned long size, start;

	if (index >= VFIO_PCI_NUM_REGIONS || !vctrl->regions[index].mmap_ops)
		return -EINVAL;

	if (vma->vm_end < vma->vm_start)
		return -EINVAL;

	size = vma->vm_end - vma->vm_start;
	start = vma->vm_pgoff << PAGE_SHIFT;

	if (start < vctrl->regions[index].mmap_area_start)
		return -EINVAL;
	if (size > vctrl->regions[index].mmap_area_size)
		return -EINVAL;

	if ((vma->vm_flags & VM_SHARED) == 0)
		return -EINVAL;

	vma->vm_ops = vctrl->regions[index].mmap_ops;
	vma->vm_private_data = vctrl;
	return 0;
}

static const struct vfio_device_ops nvme_vfio_dev_ops = {
	.open_device		= nvmet_mdev_vctrl_open,
	.close_device		= nvmet_mdev_vctrl_release,
	.read			= nvmet_mdev_read,
	.write			= nvmet_mdev_write,
	.mmap			= nvmet_mdev_mmap,
	.ioctl			= nvmet_mdev_ioctl,
	.dma_unmap		= nvmet_mdev_dma_unmap,
	.bind_iommufd		= vfio_iommufd_emulated_bind,
	.unbind_iommufd		= vfio_iommufd_emulated_unbind,
	.attach_ioas		= vfio_iommufd_emulated_attach_ioas,
	.detach_ioas		= vfio_iommufd_emulated_detach_ioas,
};

/* Called when new mediated device is created */
static int nvmet_mdev_probe(struct mdev_device *mdev)
{
	struct nvmet_mdev_port *mport = container_of(mdev->type->parent,
						     struct nvmet_mdev_port,
						     parent);
	struct nvmet_mdev_vctrl *vctrl;
	struct nvmet_ctrl *ctrl;
	u16 cntlid;
	int ret;

	ret = kstrtou16(mdev->type->sysfs_name, 10, &cntlid);
	if (ret)
		return -EINVAL;

	mutex_lock(&mport->mutex);
	/* Release the refcount taken on the ctrl at remove. */
	ctrl = nvmet_find_get_static_ctrl(mport->nvmet_port,
					  nvmet_mdev_ops.type, cntlid);
	if (!ctrl) {
		ret = -ENODEV;
		goto unlock;
	}

	vctrl = vfio_alloc_device(nvmet_mdev_vctrl, vfio_dev, &mdev->dev,
				  &nvme_vfio_dev_ops);
	if (IS_ERR(vctrl)) {
		ret = PTR_ERR(vctrl);
		goto put_ctrl;
	}

	vctrl->nvmet_ctrl = ctrl;
	ctrl->drvdata = vctrl;
	dev_set_drvdata(&mdev->dev, vctrl);

	ret = vfio_register_emulated_iommu_dev(&vctrl->vfio_dev);
	if (ret)
		goto put_vfio_dev;

	ret = nvmet_mdev_vctrl_create(vctrl, mdev);
	if (ret)
		goto unreg_vfio_dev;

	mutex_unlock(&mport->mutex);
	return 0;

unreg_vfio_dev:
	vfio_unregister_group_dev(&vctrl->vfio_dev);
put_vfio_dev:
	vfio_put_device(&vctrl->vfio_dev);
put_ctrl:
	nvmet_ctrl_put(ctrl);
unlock:
	mutex_unlock(&mport->mutex);
	return ret;
}

/*
 * TODO handle if this is called while opened.
 *
 * Depending on if we go with configfs based setup we can get a refcount to
 * the conifgfs objects to prevent them from being removed while in use.
 * However, the mdev bus hotplug removal cannot be stopped right now
 * but we may want to modify it to just not allow it.
 */
void nvmet_mdev_remove_ctrl(struct nvmet_mdev_vctrl *vctrl)
{
	struct nvmet_ctrl *ctrl;

	if (!vctrl)
		return;

	ctrl = vctrl->nvmet_ctrl;
	if (!ctrl)
		return;

	nvmet_mdev_irq_raise_unplug_event(vctrl);
	nvmet_mdev_vctrl_destroy(vctrl);

	dev_set_drvdata(vctrl->vfio_dev.dev, vctrl);
	vctrl->nvmet_ctrl = NULL;
	ctrl->drvdata = NULL;

	vfio_unregister_group_dev(&vctrl->vfio_dev);
	vfio_put_device(&vctrl->vfio_dev);

	if (ctrl)
		nvmet_ctrl_put(ctrl);
}

/* Called when a mediated device is removed via sysfs or parent removal */
static void nvmet_mdev_remove(struct mdev_device *mdev)
{
	struct nvmet_mdev_port *mport = container_of(mdev->type->parent,
						     struct nvmet_mdev_port,
						     parent);

	mutex_lock(&mport->mutex);
	nvmet_mdev_remove_ctrl(dev_get_drvdata(&mdev->dev));
	mutex_unlock(&mport->mutex);
}

static unsigned int nvmet_mdev_get_available(struct mdev_type *mtype)
{
	/* No limit since nvmet does not restrict it either */
	return UINT_MAX;
}

static ssize_t poll_timeout_ms_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct nvmet_mdev_vctrl *vctrl = dev_get_drvdata(dev);
	int ret;

	if (!vctrl)
		return -ENODEV;

	ret = kstrtoint(buf, 10, &vctrl->poll_timeout_ms);
	if (ret)
		return ret;

	return count;
}

static ssize_t poll_timeout_ms_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct nvmet_mdev_vctrl *vctrl = dev_get_drvdata(dev);

	if (!vctrl)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n", vctrl->poll_timeout_ms);
}
static DEVICE_ATTR_RW(poll_timeout_ms);

static struct attribute *nvmet_mdev_dev_settings_atttributes[] = {
	&dev_attr_poll_timeout_ms.attr,
	NULL
};

static const struct attribute_group nvmet_mdev_setting_attr_group = {
	.name = "settings",
	.attrs = nvmet_mdev_dev_settings_atttributes,
};

static const struct attribute_group *nvmet_mdev_dev_attr_groups[] = {
	&nvmet_mdev_setting_attr_group,
	NULL,
};

static struct mdev_driver nvmet_mdev_driver = {
	.device_api		= VFIO_DEVICE_API_PCI_STRING,
	.driver = {
		.name		= NVMET_MDEV_NAME,
		.owner		= THIS_MODULE,
		.dev_groups	= nvmet_mdev_dev_attr_groups,
	},
	.probe			= nvmet_mdev_probe,
	.remove			= nvmet_mdev_remove,
	.get_available		= nvmet_mdev_get_available,
};

static const struct bus_type nvmet_mdev_bus = {
	.name		= NVMET_MDEV_NAME,
};

static void nvmet_mdev_destroy_types(struct nvmet_mdev_port *mport)
{
	int i;

	for (i = 0; i < mport->type_count; i++)
		kfree(mport->types[i].name);
}

static int nvmet_mdev_setup_types(void *priv, struct nvmet_port *port,
				  struct nvmet_ctrl *ctrl)
{
	struct nvmet_mdev_port *mport = priv;
	struct nvmet_mdev_type *type;

	if (mport->type_count == mport->ctrl_count) {
		pr_err("Invalid number of controllers found %d\n",
		       mport->ctrl_count);
		return -EINVAL;
	}

	type = &mport->types[mport->type_count];
	type->name = kasprintf(GFP_KERNEL, "%hu", ctrl->cntlid);
	if (!type->name)
		return -ENOMEM;

	mport->mdev_types[mport->type_count] = &type->type;
	mport->mdev_types[mport->type_count]->sysfs_name = type->name;

	mport->type_count++;
	return 0;
}

static void nvmet_mdev_dev_release(struct device *dev)
{
	struct nvmet_mdev_port *mport = container_of(dev,
						     struct nvmet_mdev_port,
						     device);
	nvmet_mdev_destroy_types(mport);
	kfree(mport->types);
	kfree(mport->mdev_types);
	kfree(mport);
}

int nvmet_mdev_register_port(struct nvmet_mdev_port *mport)
{
	int ret;

	mport->types = kcalloc(mport->ctrl_count, sizeof(*mport->types),
			       GFP_KERNEL);
	if (!mport->types)
		return -ENOMEM;

	mport->mdev_types = kcalloc(mport->ctrl_count,
				    sizeof(*mport->mdev_types), GFP_KERNEL);
	if (!mport->mdev_types) {
		ret = -ENOMEM;
		goto free_types;
	}

	ret = nvmet_for_each_static_ctrl(mport->nvmet_port, nvmet_mdev_ops.type,
					 nvmet_mdev_setup_types, mport);
	if (ret)
		/* we might have partially setup the types arrays */
		goto destroy_types;

	mport->device.parent = nvmet_mdev_root_dev;
	mport->device.bus = &nvmet_mdev_bus;
	mport->device.release = nvmet_mdev_dev_release;
	dev_set_name(&mport->device, "port-%s",
		     config_item_name(&mport->nvmet_port->group.cg_item));

	ret = device_register(&mport->device);
	if (ret)
		goto free_mdev_types;

	ret = mdev_register_parent(&mport->parent, &mport->device,
				   &nvmet_mdev_driver, mport->mdev_types,
				   mport->type_count);
	if (ret)
		goto unreg_dev;

	return 0;

unreg_dev:
	device_unregister(&mport->device);
destroy_types:
	nvmet_mdev_destroy_types(mport);
free_mdev_types:
	kfree(mport->mdev_types);
free_types:
	kfree(mport->types);
	return ret;
}

void nvmet_mdev_unregister_port(struct nvmet_mdev_port *mport)
{
	mdev_unregister_parent(&mport->parent);
	device_unregister(&mport->device);
}

static int nvmet_mdev_register_root_device(void)
{
	int ret;

	nvmet_mdev_root_dev = root_device_register(NVMET_MDEV_NAME);
	if (IS_ERR(nvmet_mdev_root_dev))
		return PTR_ERR(nvmet_mdev_root_dev);

	ret = bus_register(&nvmet_mdev_bus);
	if (ret)
		goto unreg_root;

	nvmet_unregister_transport(&nvmet_mdev_ops);
	return 0;

unreg_root:
	root_device_unregister(nvmet_mdev_root_dev);
	return ret;
}

static void nvmet_mdev_unregister_root_device(void)
{
	root_device_unregister(nvmet_mdev_root_dev);
	bus_unregister(&nvmet_mdev_bus);
}

static int __init nvmet_mdev_init(void)
{
	int ret;

	ret = nvmet_mdev_register_root_device();
	if (ret)
		return ret;

	ret = mdev_register_driver(&nvmet_mdev_driver);
	if (ret)
		goto unreg_root;

	ret = nvmet_register_transport(&nvmet_mdev_ops);
	if (ret)
		goto unreg_driver;

	return 0;

unreg_driver:
	mdev_unregister_driver(&nvmet_mdev_driver);
unreg_root:
	nvmet_mdev_unregister_root_device();
	return ret;
}

static void __exit nvmet_mdev_exit(void)
{
	nvmet_unregister_transport(&nvmet_mdev_ops);
	nvmet_mdev_unregister_root_device();
	mdev_unregister_driver(&nvmet_mdev_driver);
}

module_init(nvmet_mdev_init);
module_exit(nvmet_mdev_exit);
