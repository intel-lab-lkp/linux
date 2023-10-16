// SPDX-License-Identifier: GPL-2.0-only
#include "cxlpci.h"
#include "switch.h"
#include "core.h"

static inline struct cxl_swdev *to_cxl_swdev(struct device *dev)
{
	return container_of(dev, struct cxl_swdev, dev);
}

static char *cxl_swdev_devnode(const struct device *dev, umode_t *mode, kuid_t *uid,
			kgid_t *gid)
{
	return kasprintf(GFP_KERNEL, "cxl/%s", dev_name(dev));
}

static long __cxl_swdev_ioctl(struct cxl_swdev *cxlswd, unsigned int cmd,
			       unsigned long arg)
{
	switch (cmd) {
	case CXL_MEM_SEND_COMMAND:
		return cxl_send_cmd(&cxlswd->mbox, (void __user *)arg);
	default:
		return -ENOTTY;
	}
}

static long cxl_swdev_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct cxl_swdev *cxlswd = file->private_data;
	int rc = -ENXIO;

	down_read(&cxl_memdev_rwsem);
	if (!cxlswd->dying)
		rc = __cxl_swdev_ioctl(cxlswd, cmd, arg);
	up_read(&cxl_memdev_rwsem);

	return rc;
}

static int cxl_swdev_open(struct inode *inode, struct file *file)
{
	struct cxl_swdev *cxlswd =
		container_of(inode->i_cdev, typeof(*cxlswd), cdev);

	get_device(&cxlswd->dev);
	file->private_data = cxlswd;

	return 0;
}

static int cxl_swdev_release_file(struct inode *inode, struct file *file)
{
	struct cxl_swdev *cxlswd =
		container_of(inode->i_cdev, typeof(*cxlswd), cdev);

	put_device(&cxlswd->dev);

	return 0;
}

static const struct file_operations cxl_swdev_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = cxl_swdev_ioctl,
	.open = cxl_swdev_open,
	.release = cxl_swdev_release_file,
	.compat_ioctl = compat_ptr_ioctl,
	.llseek = noop_llseek,
};

void cxl_swdev_shutdown(struct cxl_swdev *cxlswd)
{
	down_write(&cxl_memdev_rwsem);
	cxlswd->dying = true;
	up_write(&cxl_memdev_rwsem);
}
EXPORT_SYMBOL_NS_GPL(cxl_swdev_shutdown, CXL);

static void cxl_swdev_release(struct device *dev)
{
	struct cxl_swdev *cxlswd = to_cxl_swdev(dev);

	ida_free(&cxl_memdev_ida, cxlswd->id);
	kfree(cxlswd);
}

static const struct device_type cxl_swdev_type = {
	.name = "cxl_swdev",
	.release = cxl_swdev_release,
	.devnode = cxl_swdev_devnode,
};

struct cxl_swdev *cxl_swdev_alloc(struct device *parent)
{
	struct cxl_swdev *cxlswd;
	struct device *dev;
	struct cdev *cdev;
	int rc;

	cxlswd = kzalloc(sizeof(*cxlswd), GFP_KERNEL);
	if (!cxlswd)
		return ERR_PTR(-ENOMEM);

	rc = ida_alloc_max(&cxl_memdev_ida, CXL_MEM_MAX_DEVS - 1, GFP_KERNEL);
	if (rc < 0) {
		kfree(cxlswd);
		return ERR_PTR(rc);
	}

	cxlswd->id = rc;
	dev = &cxlswd->dev;
	device_initialize(dev);
	dev->bus = &cxl_bus_type;
	dev->parent = parent;
	dev->devt = MKDEV(cxl_mem_major, cxlswd->id);
	dev->type = &cxl_swdev_type;
	device_set_pm_not_required(dev);
	cdev = &cxlswd->cdev;
	cdev_init(cdev, &cxl_swdev_fops);
	rc = dev_set_name(dev, "switch%d", cxlswd->id);
	if (rc) {
		put_device(dev);
		return ERR_PTR(rc);
	}

	return cxlswd;
}
EXPORT_SYMBOL_NS_GPL(cxl_swdev_alloc, CXL);
