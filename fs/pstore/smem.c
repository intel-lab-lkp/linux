// SPDX-License-Identifier: GPL-2.0
/*
 * Implements pstore backend driver for shared memory devices,
 * using the pstore/zone API.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/string.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pstore_smem.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/init_syscalls.h>
#include <linux/mount.h>

/*
 * All globals must only be accessed under the pstore_smem_lock
 * during the register/unregister functions.
 */
static DEFINE_MUTEX(pstore_smem_lock);
static struct pstore_device_info *pstore_device_info;

static int __register_pstore_device(struct pstore_device_info *dev)
{
	int ret;

	lockdep_assert_held(&pstore_smem_lock);

	if (!dev) {
		pr_err("NULL device info\n");
		return -EINVAL;
	}
	if (!dev->zone.total_size && !dev->zone.dmapped_cnt) {
		pr_err("zero sized device\n");
		return -EINVAL;
	}
	if (!dev->zone.read && !dev->zone.dmapped_cnt) {
		pr_err("no read handler for device\n");
		return -EINVAL;
	}
	if (!dev->zone.write && !dev->zone.dmapped_cnt) {
		pr_err("no write handler for device\n");
		return -EINVAL;
	}

	/* someone already registered before */
	if (pstore_device_info)
		return -EBUSY;

	/* zero means not limit on which backends to attempt to store. */
	if (!dev->flags)
		dev->flags = UINT_MAX;

	/* Initialize required zone ownership details. */
	dev->zone.name = KBUILD_MODNAME;
	dev->zone.owner = THIS_MODULE;

	ret = register_pstore_zone(&dev->zone);
	if (ret == 0)
		pstore_device_info = dev;

	return ret;
}
/**
 * register_pstore_smem_device() - register smem device to pstore
 *
 * @dev: smem device information
 *
 * Return:
 * * 0		- OK
 * * Others	- some error.
 */
int register_pstore_smem_device(struct pstore_device_info *dev)
{
	int ret;

	mutex_lock(&pstore_smem_lock);
	ret = __register_pstore_device(dev);
	mutex_unlock(&pstore_smem_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(register_pstore_smem_device);

static void __unregister_pstore_device(struct pstore_device_info *dev)
{
	lockdep_assert_held(&pstore_smem_lock);
	if (pstore_device_info && pstore_device_info == dev) {
		unregister_pstore_zone(&dev->zone);
		pstore_device_info = NULL;
	}
}

/**
 * unregister_pstore_smem_device() - unregister smem device from pstore
 *
 * @dev: smem device information
 */
void unregister_pstore_smem_device(struct pstore_device_info *dev)
{
	mutex_lock(&pstore_smem_lock);
	__unregister_pstore_device(dev);
	mutex_unlock(&pstore_smem_lock);
}
EXPORT_SYMBOL_GPL(unregister_pstore_smem_device);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Eugen Hristev <eugen.hristev@linaro.org>");
MODULE_DESCRIPTION("pstore backend for smem devices");
