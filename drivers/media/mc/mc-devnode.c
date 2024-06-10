// SPDX-License-Identifier: GPL-2.0-only
/*
 * Media device node
 *
 * Copyright (C) 2010 Nokia Corporation
 *
 * Based on drivers/media/video/v4l2_dev.c code authored by
 *	Mauro Carvalho Chehab <mchehab@kernel.org> (version 2)
 *	Alan Cox, <alan@lxorguk.ukuu.org.uk> (version 1)
 *
 * Contacts: Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 *	     Sakari Ailus <sakari.ailus@iki.fi>
 *
 * --
 *
 * Generic media device node infrastructure to register and unregister
 * character devices using a dynamic major number and proper reference
 * counting.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include <media/media-devnode.h>

#define MEDIA_NUM_DEVICES	256
#define MEDIA_NAME		"media"

static dev_t media_dev_t;

/*
 *	Active devices
 */
static DEFINE_MUTEX(media_devnode_lock);
static DECLARE_BITMAP(media_devnode_nums, MEDIA_NUM_DEVICES);

/* Called when the last user of the media device exits. */
static void media_devnode_release(struct device *cd)
{
	struct media_devnode *devnode = to_media_devnode(cd);

	/* If the devnode has a ref, it is simply released by the user. */
	if (devnode->ref)
		return;

	/* Release media_devnode and perform other cleanups as needed. */
	if (devnode->release)
		devnode->release(devnode);
}

static void media_devnode_ref_release(struct device *cd)
{
	struct media_devnode_compat_ref *ref =
		container_of_const(cd, struct media_devnode_compat_ref, dev);

	kfree(ref);
}

struct media_devnode *to_media_devnode(struct device *dev)
{
	if (dev->release == media_devnode_release)
		return container_of(dev, struct media_devnode, dev);

	return container_of(dev, struct media_devnode_compat_ref, dev)->devnode;
}

static const struct bus_type media_bus_type = {
	.name = MEDIA_NAME,
};

static bool media_devnode_is_registered_compat(struct media_devnode_fh *fh)
{
	if (fh->ref)
		return atomic_read(&fh->ref->registered);

	return media_devnode_is_registered(fh->devnode);
}

static ssize_t media_read(struct file *filp, char __user *buf,
		size_t sz, loff_t *off)
{
	struct media_devnode *devnode = media_devnode_data(filp);

	if (!media_devnode_is_registered_compat(filp->private_data))
		return -EIO;
	if (!devnode->fops->read)
		return -EINVAL;
	return devnode->fops->read(filp, buf, sz, off);
}

static ssize_t media_write(struct file *filp, const char __user *buf,
		size_t sz, loff_t *off)
{
	struct media_devnode *devnode = media_devnode_data(filp);

	if (!media_devnode_is_registered_compat(filp->private_data))
		return -EIO;
	if (!devnode->fops->write)
		return -EINVAL;
	return devnode->fops->write(filp, buf, sz, off);
}

static __poll_t media_poll(struct file *filp,
			       struct poll_table_struct *poll)
{
	struct media_devnode *devnode = media_devnode_data(filp);

	if (!media_devnode_is_registered_compat(filp->private_data))
		return EPOLLERR | EPOLLHUP;
	if (!devnode->fops->poll)
		return DEFAULT_POLLMASK;
	return devnode->fops->poll(filp, poll);
}

static long
__media_ioctl(struct file *filp, unsigned int cmd, unsigned long arg,
	      long (*ioctl_func)(struct file *filp, unsigned int cmd,
				 unsigned long arg))
{
	if (!ioctl_func)
		return -ENOTTY;

	return ioctl_func(filp, cmd, arg);
}

static long media_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct media_devnode *devnode = media_devnode_data(filp);

	if (!media_devnode_is_registered_compat(filp->private_data))
		return -EIO;

	return __media_ioctl(filp, cmd, arg, devnode->fops->ioctl);
}

#ifdef CONFIG_COMPAT

static long media_compat_ioctl(struct file *filp, unsigned int cmd,
			       unsigned long arg)
{
	struct media_devnode *devnode = media_devnode_data(filp);

	if (!media_devnode_is_registered_compat(filp->private_data))
		return -EIO;

	return __media_ioctl(filp, cmd, arg, devnode->fops->compat_ioctl);
}

#endif /* CONFIG_COMPAT */

/* Override for the open function */
static int media_open(struct inode *inode, struct file *filp)
{
	struct media_devnode_cdev *mcdev;
	struct media_devnode *devnode;
	struct media_devnode_fh *fh;
	int ret;

	/* Check if the media device is available. This needs to be done with
	 * the media_devnode_lock held to prevent an open/unregister race:
	 * without the lock, the device could be unregistered and freed between
	 * the media_devnode_is_registered() and get_device() calls, leading to
	 * a crash.
	 */
	mutex_lock(&media_devnode_lock);
	mcdev = container_of(inode->i_cdev, struct media_devnode_cdev, cdev);
	if (mcdev->is_compat_ref)
		devnode = container_of(mcdev, struct media_devnode_compat_ref,
				       mcdev)->devnode;
	else
		devnode = container_of(mcdev, struct media_devnode, mcdev);
	/* return ENXIO if the media device has been removed
	   already or if it is not registered anymore. */
	if (!media_devnode_is_registered(devnode)) {
		mutex_unlock(&media_devnode_lock);
		return -ENXIO;
	}
	/* and increase the device refcount */
	get_device(media_devnode_dev(devnode));
	mutex_unlock(&media_devnode_lock);

	ret = devnode->fops->open(devnode, filp);
	if (ret) {
		put_device(media_devnode_dev(devnode));
		return ret;
	}

	fh = filp->private_data;
	fh->devnode = devnode;

	return 0;
}

/* Override for the release function */
static int media_release(struct inode *inode, struct file *filp)
{
	struct media_devnode_fh *fh = filp->private_data;
	struct device *dev;

	if (!fh->ref) {
		dev = &fh->devnode->dev;
		fh->devnode->fops->release(filp);
	} else {
		dev = &fh->ref->dev;
		fh->ref->release(filp);
	}

	filp->private_data = NULL;

	put_device(dev);

	return 0;
}

static const struct file_operations media_devnode_fops = {
	.owner = THIS_MODULE,
	.read = media_read,
	.write = media_write,
	.open = media_open,
	.unlocked_ioctl = media_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = media_compat_ioctl,
#endif /* CONFIG_COMPAT */
	.release = media_release,
	.poll = media_poll,
	.llseek = no_llseek,
};

void media_devnode_init(struct media_devnode *devnode)
{
	device_initialize(&devnode->dev);
	devnode->dev.release = media_devnode_release;
	devnode->minor = -1;
}

/*
 * Best effort media device lifetime management for old drivers
 *
 * Drivers that do not manage the lifetime of the media device are provided with
 * a best effort lifetime management support. This means that as the driver does
 * not release the media device once all users are gone but when the device is
 * unbound, there are bound to be (brief) moments when released memory may get
 * accessed. All drivers should be converted to release their memory at a safe
 * time, i.e. provide a release callback in struct media_file_operations to do
 * so. This is especially important for drivers for devices that are
 * unpluggable, e.g. USB devices.
 *
 * A second struct device is used to manage the lifetime of a helper object,
 * struct media_devnode_compat_ref. For a media device, one is initialised in
 * media_devnode_register and put in media_devnode_unregister. This object is
 * also used as the device of the media character device so file handles to the
 * media device have a reference to this object. When the media device is
 * released, any file handle retains a reference to this helper that also
 * contains the media device's registration status. If a media device is
 * released and a user space process attempts to access the file handle, an
 * error is returned.
 *
 * The struct device in struct media_devnode is put at media_device_cleanup and
 * uses an empty release callback, reflecting the expectation the driver will
 * release the memory of the media device at unbind time.
 */
int __must_check media_devnode_register(struct media_devnode *devnode,
					struct module *owner)
{
	struct media_devnode_compat_ref *ref = devnode->ref;
	struct cdev *cdev;
	struct device *dev;
	int minor;
	int ret;

	if (devnode->minor != -1)
		return -EINVAL;

	/* Part 1: Find a free minor number */
	mutex_lock(&media_devnode_lock);
	minor = find_first_zero_bit(media_devnode_nums, MEDIA_NUM_DEVICES);
	if (minor == MEDIA_NUM_DEVICES) {
		mutex_unlock(&media_devnode_lock);
		pr_err("could not get a free minor\n");
		return -ENFILE;
	}

	set_bit(minor, media_devnode_nums);
	mutex_unlock(&media_devnode_lock);

	devnode->minor = minor;

	/* Part 2: Initialize the media and character devices */
	cdev = ref ? &ref->mcdev.cdev : &devnode->mcdev.cdev;
	cdev_init(cdev, &media_devnode_fops);
	cdev->owner = owner;
	kobject_set_name(&cdev->kobj, "media%d", devnode->minor);

	if (!ref) {
		dev = &devnode->dev;
	} else {
		ref->mcdev.is_compat_ref = true;
		device_initialize(&ref->dev);
		atomic_set(&ref->registered, 1);
		ref->devnode = devnode;
		ref->release = devnode->fops->release;
		dev = &ref->dev;
		dev->release = media_devnode_ref_release;
	}
	dev->bus = &media_bus_type;
	dev->devt = MKDEV(MAJOR(media_dev_t), devnode->minor);
	if (devnode->parent)
		dev->parent = devnode->parent;
	dev_set_name(dev, "media%d", devnode->minor);

	/* Part 3: Add the media and character devices */
	set_bit(MEDIA_FLAG_REGISTERED, &devnode->flags);
	ret = cdev_device_add(cdev, dev);
	if (ret < 0) {
		pr_err("%s: cdev_device_add failed\n", __func__);
		goto cdev_add_error;
	}

	return 0;

cdev_add_error:
	mutex_lock(&media_devnode_lock);
	clear_bit(devnode->minor, media_devnode_nums);
	mutex_unlock(&media_devnode_lock);

	return ret;
}

void media_devnode_unregister(struct media_devnode *devnode)
{
	/* Check if devnode was ever registered at all */
	if (!media_devnode_is_registered(devnode))
		return;

	if (devnode->ref)
		atomic_set(&devnode->ref->registered, 0);

	mutex_lock(&media_devnode_lock);
	clear_bit(MEDIA_FLAG_REGISTERED, &devnode->flags);
	mutex_unlock(&media_devnode_lock);

	cdev_device_del(devnode->ref ? &devnode->ref->mcdev.cdev :
			&devnode->mcdev.cdev, media_devnode_dev(devnode));

	mutex_lock(&media_devnode_lock);
	clear_bit(devnode->minor, media_devnode_nums);
	mutex_unlock(&media_devnode_lock);
}

/*
 *	Initialise media for linux
 */
static int __init media_devnode_module_init(void)
{
	int ret;

	pr_info("Linux media interface: v0.10\n");
	ret = alloc_chrdev_region(&media_dev_t, 0, MEDIA_NUM_DEVICES,
				  MEDIA_NAME);
	if (ret < 0) {
		pr_warn("unable to allocate major\n");
		return ret;
	}

	ret = bus_register(&media_bus_type);
	if (ret < 0) {
		unregister_chrdev_region(media_dev_t, MEDIA_NUM_DEVICES);
		pr_warn("bus_register failed\n");
		return -EIO;
	}

	return 0;
}

static void __exit media_devnode_module_exit(void)
{
	bus_unregister(&media_bus_type);
	unregister_chrdev_region(media_dev_t, MEDIA_NUM_DEVICES);
}

subsys_initcall(media_devnode_module_init);
module_exit(media_devnode_module_exit)

MODULE_AUTHOR("Laurent Pinchart <laurent.pinchart@ideasonboard.com>");
MODULE_DESCRIPTION("Device node registration for media drivers");
MODULE_LICENSE("GPL");
