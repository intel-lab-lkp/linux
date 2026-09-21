// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 Synopsys, Inc. and/or its affiliates.
 * Copyright (c) 2026 Google LLC
 *
 * Author: Vitor Soares <soares@synopsys.com>
 * Author: Sam Agazaryan <samagazaryan@google.com>
 */

#include <linux/cdev.h>
#include <linux/cleanup.h>
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <linux/i3c/i3cdev.h>

#include "internals.h"

struct i3cdev_data {
	struct list_head list;
	struct i3c_device *i3c;
	struct device dev;
	struct mutex xfer_lock; /* prevent detach while transferring */
	struct cdev cdev;
	int id;
};

static DEFINE_IDA(i3cdev_ida);
static LIST_HEAD(i3cdev_list);
static DEFINE_MUTEX(i3cdev_attach_lock);
static dev_t i3cdev_number;
#define I3C_MINORS (MINORMASK + 1)

static void i3cdev_dev_release(struct device *dev)
{
	struct i3cdev_data *i3cdev = container_of(dev, struct i3cdev_data, dev);

	ida_free(&i3cdev_ida, i3cdev->id);
	kfree(i3cdev);
}

static struct i3cdev_data *i3cdev_get_by_i3c(struct i3c_device *i3c)
{
	struct i3cdev_data *i3cdev;

	list_for_each_entry(i3cdev, &i3cdev_list, list) {
		if (i3cdev->i3c == i3c)
			return i3cdev;
	}

	return NULL;
}

static struct i3cdev_data *get_free_i3cdev(struct i3c_device *i3c)
{
	struct i3cdev_data *i3cdev;
	int id;

	id = ida_alloc_max(&i3cdev_ida, MINORMASK, GFP_KERNEL);
	if (id < 0) {
		pr_err("i3cdev: no minor number available!\n");
		return ERR_PTR(id);
	}

	i3cdev = kzalloc_obj(*i3cdev, GFP_KERNEL);
	if (!i3cdev) {
		ida_free(&i3cdev_ida, id);
		return ERR_PTR(-ENOMEM);
	}

	i3cdev->i3c = i3c;
	i3cdev->id = id;
	list_add_tail(&i3cdev->list, &i3cdev_list);

	return i3cdev;
}

static ssize_t
i3cdev_read(struct file *file, char __user *buf, size_t count, loff_t *f_pos)
{
	char *tmp __free(kfree) = NULL;
	struct i3cdev_data *i3cdev = file->private_data;
	struct i3c_xfer xfers = {
		.rnw = true,
	};
	struct i3c_device *i3c;
	int ret;

	count = min_t(size_t, count, type_max(xfers.len));
	xfers.len = count;

	tmp = kzalloc(count, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	guard(mutex)(&i3cdev->xfer_lock);
	i3c = i3cdev->i3c;
	if (!i3c || i3c->dev.driver)
		return -ENODEV;

	xfers.data.in = tmp;

	dev_dbg(&i3c->dev, "Reading %zu bytes.\n", count);

	ret = i3c_device_do_xfers(i3c, &xfers, 1, I3C_SDR);
	if (ret)
		return ret;

	if (copy_to_user(buf, tmp, xfers.actual_len))
		return -EFAULT;

	return xfers.actual_len;
}

static ssize_t
i3cdev_write(struct file *file, const char __user *buf, size_t count,
	     loff_t *f_pos)
{
	void *tmp __free(kfree) = NULL;
	struct i3cdev_data *i3cdev = file->private_data;
	struct i3c_xfer xfers = {
		.rnw = false,
	};
	struct i3c_device *i3c;
	int ret;

	count = min_t(size_t, count, type_max(xfers.len));
	xfers.len = count;

	tmp = memdup_user(buf, count);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	guard(mutex)(&i3cdev->xfer_lock);
	i3c = i3cdev->i3c;
	if (!i3c || i3c->dev.driver)
		return -ENODEV;

	xfers.data.out = tmp;

	dev_dbg(&i3c->dev, "Writing %zu bytes.\n", count);

	ret = i3c_device_do_xfers(i3c, &xfers, 1, I3C_SDR);
	if (ret)
		return ret;

	return count;
}

static int
i3cdev_do_xfer(struct i3c_device *dev, struct i3c_ioc_xfer *xfers,
	       struct i3c_ioc_xfer __user *u_xfers, unsigned int nxfers)
{
	struct i3c_xfer *k_xfers __free(kfree) = NULL;
	enum i3c_xfer_mode mode = xfers[0].mode;
	u8 **data_ptrs;
	int i, j, nalloc, ret = 0;

	/* Since we have nxfers we may allocate k_xfer + *data_ptrs together */
	k_xfers = kcalloc(nxfers, sizeof(*k_xfers) + sizeof(*data_ptrs),
			  GFP_KERNEL);
	if (!k_xfers)
		return -ENOMEM;

	/* set data_ptrs to be after nxfers * i3c_xfer */
	data_ptrs = (void *)k_xfers + (nxfers * sizeof(*k_xfers));

	for (i = 0; i < nxfers; i++) {
		bool is_read;

		if (xfers[i].mode != mode) {
			ret = -EINVAL;
			break;
		}

		if (memchr_inv(xfers[i].pad, 0, sizeof(xfers[i].pad))) {
			ret = -EINVAL;
			break;
		}

		if (mode == I3C_SDR) {
			if (xfers[i].rnw != I3C_DEV_DIR_WRITE &&
			    xfers[i].rnw != I3C_DEV_DIR_READ) {
				ret = -EINVAL;
				break;
			}
			is_read = xfers[i].rnw == I3C_DEV_DIR_READ;
		} else {
			is_read = xfers[i].cmd & 0x80;
		}

		if (is_read) {
			data_ptrs[i] = kzalloc(xfers[i].len, GFP_KERNEL);
			if (!data_ptrs[i]) {
				ret = -ENOMEM;
				break;
			}
			k_xfers[i].data.in = data_ptrs[i];
		} else {
			data_ptrs[i] = memdup_user(u64_to_user_ptr(xfers[i].data),
						   xfers[i].len);
			if (IS_ERR(data_ptrs[i])) {
				ret = PTR_ERR(data_ptrs[i]);
				break;
			}
			k_xfers[i].data.out = data_ptrs[i];
		}

		k_xfers[i].cmd = xfers[i].cmd;
		k_xfers[i].len = xfers[i].len;
	}
	nalloc = i;

	if (ret < 0)
		goto err_free_mem;

	ret = i3c_device_do_xfers(dev, k_xfers, nxfers, mode);
	if (ret)
		goto err_free_mem;

	for (i = 0; i < nxfers; i++) {
		bool is_read = (mode == I3C_SDR) ?
			       (xfers[i].rnw == I3C_DEV_DIR_READ) :
			       (xfers[i].cmd & 0x80);

		if (is_read) {
			if (copy_to_user(u64_to_user_ptr(xfers[i].data),
					 data_ptrs[i], k_xfers[i].actual_len) ||
			    put_user(k_xfers[i].actual_len,
				     &u_xfers[i].actual_len)) {
				ret = -EFAULT;
				break;
			}
		}
	}

err_free_mem:
	for (j = 0; j < nalloc; j++)
		kfree(data_ptrs[j]);
	return ret;
}

static struct i3c_ioc_xfer *
i3cdev_get_ioc_xfer(unsigned int cmd, struct i3c_ioc_xfer __user *u_xfers,
		    unsigned int *nxfers)
{
	u32 tmp = _IOC_SIZE(cmd);

	if ((tmp % sizeof(struct i3c_ioc_xfer)) != 0)
		return ERR_PTR(-EINVAL);

	*nxfers = tmp / sizeof(struct i3c_ioc_xfer);
	if (*nxfers == 0)
		return ERR_PTR(-EINVAL);

	return memdup_user(u_xfers, tmp);
}

static int
i3cdev_ioc_xfer(struct i3c_device *i3c, unsigned int cmd,
		struct i3c_ioc_xfer __user *u_xfers)
{
	struct i3c_ioc_xfer *k_xfers __free(kfree) = NULL;
	unsigned int nxfers;

	k_xfers = i3cdev_get_ioc_xfer(cmd, u_xfers, &nxfers);
	if (IS_ERR(k_xfers))
		return PTR_ERR(k_xfers);

	return i3cdev_do_xfer(i3c, k_xfers, u_xfers, nxfers);
}

static long
i3cdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct i3cdev_data *i3cdev = file->private_data;
	struct i3c_device *i3c;

	if (_IOC_TYPE(cmd) != I3C_DEV_IOC_MAGIC)
		return -ENOTTY;

	/* Use the xfer_lock to prevent device detach during ioctl call */
	guard(mutex)(&i3cdev->xfer_lock);
	i3c = i3cdev->i3c;
	if (!i3c || i3c->dev.driver)
		return -ENODEV;

	dev_dbg(&i3c->dev, "ioctl, cmd=0x%02x, arg=0x%02lx\n", cmd, arg);

	/* Check command number and direction */
	if (_IOC_NR(cmd) == _IOC_NR(I3C_IOC_XFER(0)) &&
	    _IOC_DIR(cmd) == (_IOC_READ | _IOC_WRITE))
		return i3cdev_ioc_xfer(i3c, cmd,
				       (struct i3c_ioc_xfer __user *)arg);

	return -ENOTTY;
}

static int i3cdev_open(struct inode *inode, struct file *file)
{
	struct i3cdev_data *i3cdev = container_of(inode->i_cdev,
						  struct i3cdev_data,
						  cdev);
	file->private_data = i3cdev;

	return 0;
}

static int i3cdev_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;

	return 0;
}

static const struct file_operations i3cdev_fops = {
	.owner		= THIS_MODULE,
	.read		= i3cdev_read,
	.write		= i3cdev_write,
	.unlocked_ioctl	= i3cdev_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.open		= i3cdev_open,
	.release	= i3cdev_release,
};

/* ------------------------------------------------------------------------- */

static const struct class i3cdev_class = {
	.name = "i3cdev",
};

static int i3cdev_attach(struct device *dev, void *dummy)
{
	struct i3cdev_data *i3cdev;
	struct i3c_device *i3c;
	int res;

	if (dev->type == &i3c_masterdev_type)
		return 0;

	i3c = dev_to_i3cdev(dev);

	guard(mutex)(&i3cdev_attach_lock);
	if (dev->driver || i3cdev_get_by_i3c(i3c))
		return 0;

	/* Get a device */
	i3cdev = get_free_i3cdev(i3c);
	if (IS_ERR(i3cdev))
		return PTR_ERR(i3cdev);

	mutex_init(&i3cdev->xfer_lock);
	cdev_init(&i3cdev->cdev, &i3cdev_fops);
	i3cdev->cdev.owner = THIS_MODULE;

	device_initialize(&i3cdev->dev);
	i3cdev->dev.devt = MKDEV(MAJOR(i3cdev_number), i3cdev->id);
	i3cdev->dev.class = &i3cdev_class;
	i3cdev->dev.parent = &i3c->dev;
	i3cdev->dev.release = i3cdev_dev_release;

	res = dev_set_name(&i3cdev->dev, "bus!i3c!%s", dev_name(&i3c->dev));
	if (res)
		goto error_put_dev;

	res = cdev_device_add(&i3cdev->cdev, &i3cdev->dev);
	if (res)
		goto error_put_dev;

	pr_debug("i3cdev: I3C device [%s] registered as minor %d\n",
		 dev_name(&i3c->dev), i3cdev->id);
	return 0;

error_put_dev:
	list_del(&i3cdev->list);
	put_device(&i3cdev->dev);
	return res;
}

static int i3cdev_detach(struct device *dev, void *dummy)
{
	struct i3cdev_data *i3cdev;
	struct i3c_device *i3c;

	if (dev->type == &i3c_masterdev_type)
		return 0;

	i3c = dev_to_i3cdev(dev);

	guard(mutex)(&i3cdev_attach_lock);
	i3cdev = i3cdev_get_by_i3c(i3c);
	if (!i3cdev)
		return 0;

	list_del(&i3cdev->list);

	/* Prevent transfers while cdev removal */
	scoped_guard(mutex, &i3cdev->xfer_lock)
		i3cdev->i3c = NULL;

	cdev_device_del(&i3cdev->cdev, &i3cdev->dev);
	put_device(&i3cdev->dev);

	pr_debug("i3cdev: device [%s] unregistered\n", dev_name(&i3c->dev));

	return 0;
}

static int i3cdev_notifier_call(struct notifier_block *nb,
				unsigned long action,
				void *data)
{
	struct device *dev = data;

	switch (action) {
	case BUS_NOTIFY_ADD_DEVICE:
	case BUS_NOTIFY_UNBOUND_DRIVER:
	case BUS_NOTIFY_DRIVER_NOT_BOUND:
		i3cdev_attach(dev, NULL);
		break;
	case BUS_NOTIFY_DEL_DEVICE:
	case BUS_NOTIFY_REMOVED_DEVICE:
	case BUS_NOTIFY_BIND_DRIVER:
		i3cdev_detach(dev, NULL);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block i3cdev_notifier = {
	.notifier_call = i3cdev_notifier_call,
};

static int __init i3cdev_init(void)
{
	int res;

	/* Dynamically request unused major number */
	res = alloc_chrdev_region(&i3cdev_number, 0, I3C_MINORS, "i3c");
	if (res)
		goto out;

	/* Register device class to populate sysfs entries */
	res = class_register(&i3cdev_class);
	if (res)
		goto out_unreg_chrdev;

	/* Keep track of busses which have devices to add or remove later */
	res = bus_register_notifier(&i3c_bus_type, &i3cdev_notifier);
	if (res)
		goto out_unreg_class;

	/* Bind to already existing device without driver right away */
	i3c_for_each_dev(NULL, i3cdev_attach);

	return 0;

out_unreg_class:
	class_unregister(&i3cdev_class);
out_unreg_chrdev:
	unregister_chrdev_region(i3cdev_number, I3C_MINORS);
out:
	pr_err("%s: Driver Initialisation failed\n", __FILE__);
	return res;
}

static void __exit i3cdev_exit(void)
{
	bus_unregister_notifier(&i3c_bus_type, &i3cdev_notifier);
	i3c_for_each_dev(NULL, i3cdev_detach);
	class_unregister(&i3cdev_class);
	unregister_chrdev_region(i3cdev_number, I3C_MINORS);
}

MODULE_AUTHOR("Vitor Soares <soares@synopsys.com>");
MODULE_DESCRIPTION("I3C /dev entries driver");
MODULE_LICENSE("GPL");

module_init(i3cdev_init);
module_exit(i3cdev_exit);
