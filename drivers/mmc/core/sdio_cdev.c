// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDIO userspace passthrough character device.
 *
 * Copyright (C) 2026 Shawn Lin <shawn.lin@linux.dev>
 *
 * Every SDIO function gets a /dev/<function name> node (e.g.
 * /dev/mmc1:0001:1); ioctls on the node issue CMD52 (single register
 * access, function 0 included for the CCCR/FBR) and CMD53 (extended
 * multi-byte access) to the card.
 *
 * Raw access can disturb the state a bound function driver maintains,
 * so the ioctls require CAP_SYS_ADMIN and take the host claim for the
 * duration of each command.
 */
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <uapi/linux/mmc/sdio_ioctl.h>

#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio_func.h>

#include "sdio_cdev.h"
#include "sdio_ops.h"

static int sdio_cdev_rw(struct sdio_func *func,
			struct sdio_ioc_rw __user *uarg)
{
	struct sdio_ioc_rw rw;
	u8 data = 0;
	int err, ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (copy_from_user(&rw, uarg, sizeof(rw)))
		return -EFAULT;

	if (rw.fn > 7)
		return -EINVAL;

	pm_runtime_get_sync(&func->dev);
	sdio_claim_host(func);
	if (rw.write && !rw.raw) {
		/*
		 * Plain write: pass a NULL out so the kernel does not set
		 * the CMD52 RAW bit - out doubles as the RAW flag there.
		 */
		err = mmc_io_rw_direct(func->card, 1, rw.fn, rw.addr,
				       rw.data, NULL);
	} else {
		err = mmc_io_rw_direct(func->card, rw.write, rw.fn, rw.addr,
				       rw.write ? rw.data : 0, &data);
	}
	sdio_release_host(func);
	pm_runtime_put_autosuspend(&func->dev);

	if (err) {
		dev_err(&func->dev, "CMD52 fn %u addr 0x%05x failed: %d\n",
			rw.fn, rw.addr, err);
		return err;
	}

	if (!rw.write || rw.raw)
		rw.data = data;
	ret = copy_to_user(uarg, &rw, sizeof(rw));

	return ret ? -EFAULT : 0;
}

static int sdio_cdev_rw_ext(struct sdio_func *func,
			    struct sdio_ioc_rw_ext __user *uarg)
{
	struct sdio_ioc_rw_ext rw;
	u8 *buf;
	int err;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (copy_from_user(&rw, uarg, sizeof(rw)))
		return -EFAULT;

	if (rw.fn == 0 || rw.fn > 7)
		return -EINVAL;
	if (!rw.count || rw.count > 512)
		return -EINVAL;

	pm_runtime_get_sync(&func->dev);

	/*
	 * Stage through a kmalloc'd buffer: rw.data lives on the ioctl
	 * handler's stack, which is vmalloc'd with CONFIG_VMAP_STACK -
	 * virt_to_page() on a vmalloc address is invalid, and the PIO
	 * and DMA paths of the host drivers both derive struct pages
	 * from the data buffer.
	 */
	buf = kmalloc(rw.count, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (rw.write)
		memcpy(buf, rw.data, rw.count);

	sdio_claim_host(func);
	err = mmc_io_rw_extended(func->card, rw.write, rw.fn, rw.addr,
				 rw.incr_addr, buf, 0, rw.count);
	sdio_release_host(func);
	pm_runtime_put_autosuspend(&func->dev);

	if (!err && !rw.write)
		memcpy(rw.data, buf, rw.count);

	kfree(buf);

	if (err) {
		dev_err(&func->dev, "CMD53 fn %u addr 0x%05x failed: %d\n",
			rw.fn, rw.addr, err);
		return err;
	}

	return 0;
}

static long sdio_cdev_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct miscdevice *misc = file->private_data;
	struct sdio_func *func = container_of(misc->parent,
					      struct sdio_func, dev);

	switch (cmd) {
	case SDIO_IOC_RW:
		return sdio_cdev_rw(func, (struct sdio_ioc_rw __user *)arg);
	case SDIO_IOC_RW_EXT:
		return sdio_cdev_rw_ext(func,
					(struct sdio_ioc_rw_ext __user *)arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations sdio_cdev_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= sdio_cdev_ioctl,
};

int sdio_cdev_create(struct sdio_func *func)
{
	struct miscdevice *misc;
	int ret;

	misc = kzalloc_obj(*misc);
	if (!misc)
		return -ENOMEM;

	misc->minor = MISC_DYNAMIC_MINOR;
	misc->name = dev_name(&func->dev);
	misc->fops = &sdio_cdev_fops;
	misc->parent = &func->dev;
	func->miscdev = misc;

	ret = misc_register(func->miscdev);
	if (ret) {
		dev_err(&func->dev, "failed to register /dev/%s: %d\n",
			dev_name(&func->dev), ret);
		kfree(misc);
		func->miscdev = NULL;
	}

	return ret;
}

void sdio_cdev_destroy(struct sdio_func *func)
{
	if (!func->miscdev)
		return;

	misc_deregister(func->miscdev);
	kfree(func->miscdev);
	func->miscdev = NULL;
}
