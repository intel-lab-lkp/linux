// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <net/ultraeth/uet_context.h>
#include <net/ultraeth/uecon.h>

#include "uet_netlink.h"

static int __init uet_init(void)
{
	int err;

	err = genl_register_family(&ultraeth_nl_family);
	if (err)
		goto out_err;

	err = uecon_rtnl_link_register();
	if (err)
		goto rtnl_link_err;

	return 0;

rtnl_link_err:
	genl_unregister_family(&ultraeth_nl_family);
out_err:
	return err;
}

static void __exit uet_exit(void)
{
	genl_unregister_family(&ultraeth_nl_family);
	uet_context_destroy_all();
	uecon_rtnl_link_unregister();
}

module_init(uet_init);
module_exit(uet_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ultra Ethernet core");
