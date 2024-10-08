// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PPS generators core file
 *
 * Copyright (C) 2024   Rodolfo Giometti <giometti@enneenne.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/timex.h>
#include <linux/uaccess.h>
#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/pps_gen_kernel.h>
#include <linux/slab.h>

/*
 * Local variables
 */

static int pps_gen_major;
static struct class *pps_gen_class;

static DEFINE_MUTEX(pps_gen_idr_lock);
static DEFINE_IDR(pps_gen_idr);

/*
 * Char device methods
 */

static long pps_gen_cdev_ioctl(struct file *file,
		unsigned int cmd, unsigned long arg)
{
	struct pps_gen_device *pps_gen = file->private_data;
	unsigned int __user *uiuarg = (unsigned int __user *) arg;
	unsigned int status;
	int ret;

	switch (cmd) {
	case PPS_GEN_SETENABLE:
		dev_dbg(pps_gen->dev, "PPS_GEN_SETENABLE\n");

		ret = get_user(status, uiuarg);
		if (ret)
			return -EFAULT;

		ret = pps_gen->info.enable(pps_gen, status);
		if (ret)
			return ret;
		pps_gen->enabled = status;

		break;

	default:
		return -ENOTTY;
	}

	return 0;
}

#ifdef CONFIG_COMPAT
static long pps_gen_cdev_compat_ioctl(struct file *file,
		unsigned int cmd, unsigned long arg)
{
	cmd = _IOC(_IOC_DIR(cmd), _IOC_TYPE(cmd), _IOC_NR(cmd), sizeof(void *));
	return pps_gen_cdev_ioctl(file, cmd, arg);
}
#else
#define pps_gen_cdev_compat_ioctl	NULL
#endif

static struct pps_gen_device *pps_gen_idr_get(unsigned long id)
{
	struct pps_gen_device *pps_gen;

	mutex_lock(&pps_gen_idr_lock);
	pps_gen = idr_find(&pps_gen_idr, id);
	if (pps_gen)
		kobject_get(&pps_gen->dev->kobj);

	mutex_unlock(&pps_gen_idr_lock);
	return pps_gen;
}

static int pps_gen_cdev_open(struct inode *inode, struct file *file)
{
	struct pps_gen_device *pps_gen = pps_gen_idr_get(iminor(inode));

	if (!pps_gen)
		return -ENODEV;

	file->private_data = pps_gen;
	return 0;
}

static int pps_gen_cdev_release(struct inode *inode, struct file *file)
{
	struct pps_gen_device *pps_gen = file->private_data;

	WARN_ON(pps_gen->id != iminor(inode));
	kobject_put(&pps_gen->dev->kobj);
	return 0;
}

/*
 * Char device stuff
 */

static const struct file_operations pps_gen_cdev_fops = {
	.owner		= THIS_MODULE,
	.compat_ioctl	= pps_gen_cdev_compat_ioctl,
	.unlocked_ioctl	= pps_gen_cdev_ioctl,
	.open		= pps_gen_cdev_open,
	.release	= pps_gen_cdev_release,
};

static void pps_gen_device_destruct(struct device *dev)
{
	struct pps_gen_device *pps_gen = dev_get_drvdata(dev);

	pr_debug("deallocating pps-gen%d\n", pps_gen->id);
	kfree(dev);
	kfree(pps_gen);
}

static int pps_gen_register_cdev(struct pps_gen_device *pps_gen)
{
	int err;
	dev_t devt;

	mutex_lock(&pps_gen_idr_lock);

	err = idr_alloc(&pps_gen_idr, pps_gen, 0, PPS_GEN_MAX_SOURCES,
					GFP_KERNEL);
	if (err < 0) {
		if (err == -ENOSPC) {
			pr_err("%s: too many PPS sources in the system\n",
			       pps_gen->info.name);
			err = -EBUSY;
		}
		goto out_unlock;
	}
	pps_gen->id = err;

	devt = MKDEV(pps_gen_major, pps_gen->id);
	pps_gen->dev = device_create(pps_gen_class, pps_gen->info.parent, devt,
					pps_gen, "pps-gen%d", pps_gen->id);
	if (IS_ERR(pps_gen->dev)) {
		err = PTR_ERR(pps_gen->dev);
		goto free_idr;
	}

	/* Override the release function with our own */
	pps_gen->dev->release = pps_gen_device_destruct;

	pr_debug("generator %s got cdev (%d:%d)\n",
			pps_gen->info.name, pps_gen_major, pps_gen->id);

	kobject_get(&pps_gen->dev->kobj);
	mutex_unlock(&pps_gen_idr_lock);
	return 0;

free_idr:
	idr_remove(&pps_gen_idr, pps_gen->id);
out_unlock:
	mutex_unlock(&pps_gen_idr_lock);
	return err;
}

static void pps_gen_unregister_cdev(struct pps_gen_device *pps_gen)
{
	pr_debug("unregistering pps-gen%d\n", pps_gen->id);
	device_destroy(pps_gen_class, pps_gen->dev->devt);

	/* Now we can release the ID for re-use */
	mutex_lock(&pps_gen_idr_lock);
	idr_remove(&pps_gen_idr, pps_gen->id);
	kobject_put(&pps_gen->dev->kobj);
	mutex_unlock(&pps_gen_idr_lock);
}

/*
 * Exported functions
 */

/* pps_gen_register_source - add a PPS generator in the system
 * @info: the PPS generator info struct
 *
 * The function returns, in case of success, the PPS generaor device. Otherwise
 * ERR_PTR(errno).
 */

struct pps_gen_device *pps_gen_register_source(struct pps_gen_source_info *info)
{
        struct pps_gen_device *pps_gen;
        int err;

        pps_gen = kzalloc(sizeof(struct pps_gen_device), GFP_KERNEL);
        if (pps_gen == NULL) {
                err = -ENOMEM;
                goto pps_gen_register_source_exit;
        }
        pps_gen->info = *info;
	pps_gen->enabled = false;

        /* Create the char device */
        err = pps_gen_register_cdev(pps_gen);
        if (err < 0) {
                pr_err("%s: unable to create char device\n",
                                        info->name);
                goto kfree_pps_gen;
        }

        dev_info(pps_gen->dev, "new PPS generator %s\n", info->name);

        return pps_gen;

kfree_pps_gen:
        kfree(pps_gen);

pps_gen_register_source_exit:
        pr_err("%s: unable to register generaor\n", info->name);

        return ERR_PTR(err);
}
EXPORT_SYMBOL(pps_gen_register_source);

/* pps_gen_unregister_source - remove a PPS generator from the system
 * @pps_gen: the PPS generator
 */

void pps_gen_unregister_source(struct pps_gen_device *pps_gen)
{
        pps_gen_unregister_cdev(pps_gen);
}
EXPORT_SYMBOL(pps_gen_unregister_source);

/*
 * Module stuff
 */

static void __exit pps_gen_exit(void)
{
	class_destroy(pps_gen_class);
	__unregister_chrdev(pps_gen_major, 0, PPS_GEN_MAX_SOURCES, "pps-gen");
}

static int __init pps_gen_init(void)
{
	pps_gen_class = class_create("pps-gen");
	if (IS_ERR(pps_gen_class)) {
		pr_err("failed to allocate class\n");
		return PTR_ERR(pps_gen_class);
	}
	pps_gen_class->dev_groups = pps_gen_groups;

	pps_gen_major = __register_chrdev(0, 0, PPS_GEN_MAX_SOURCES, "pps-gen",
				      &pps_gen_cdev_fops);
	if (pps_gen_major < 0) {
		pr_err("failed to allocate char device region\n");
		goto remove_class;
	}

	return 0;

remove_class:
	class_destroy(pps_gen_class);
	return pps_gen_major;
}

subsys_initcall(pps_gen_init);
module_exit(pps_gen_exit);

MODULE_AUTHOR("Rodolfo Giometti <giometti@enneenne.com>");
MODULE_DESCRIPTION("LinuxPPS generators support");
MODULE_LICENSE("GPL");
