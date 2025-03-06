// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <net/ultraeth/uet_context.h>

#include "uet_netlink.h"

static int __init uet_init(void)
{
	return genl_register_family(&ultraeth_nl_family);
}

static void __exit uet_exit(void)
{
	genl_unregister_family(&ultraeth_nl_family);
	uet_context_destroy_all();
}

module_init(uet_init);
module_exit(uet_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ultra Ethernet core");
