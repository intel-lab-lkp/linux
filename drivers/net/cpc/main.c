// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/module.h>

static int __init cpc_init(void)
{
	return 0;
}
module_init(cpc_init);

static void __exit cpc_exit(void)
{
}
module_exit(cpc_exit);

MODULE_DESCRIPTION("Silicon Labs CPC Protocol");
MODULE_AUTHOR("Damien Riégel <damien.riegel@silabs.com>");
MODULE_LICENSE("GPL");
