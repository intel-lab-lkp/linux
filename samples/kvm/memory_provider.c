// SPDX-License-Identifier: GPL-2.0
/*
 * memory_provider - a sample memory provider.
 *
 * Confidential VMs are not supported.
 */

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

static const struct file_operations mp_ctl_fops = {
	.owner		= THIS_MODULE,
};

static struct miscdevice mp_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "memory_provider",
	.fops	= &mp_ctl_fops,
};

static int __init mp_init(void)
{
	return misc_register(&mp_dev);
}
module_init(mp_init);

static void __exit mp_exit(void)
{
	misc_deregister(&mp_dev);
}
module_exit(mp_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Sample memory provider for guest_memfd and iommufd");
