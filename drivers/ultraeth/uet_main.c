// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>

static int __init uet_init(void)
{
	return 0;
}

static void __exit uet_exit(void)
{
}

module_init(uet_init);
module_exit(uet_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ultra Ethernet core");
