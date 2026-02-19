// SPDX-License-Identifier: GPL-2.0-only
/*
 * init/noinitramfs.c
 *
 * Copyright (C) 2006, NXP Semiconductors, All Rights Reserved
 * Author: Jean-Paul Saman <jean-paul.saman@nxp.com>
 */
#include <linux/init.h>
#include <linux/umh.h>

#include "do_mounts.h"

/*
 * Create a simple rootfs
 */
static int __init default_rootfs(void)
{
	usermodehelper_enable();
	create_basic_rootfs();
	return 0;
}
rootfs_initcall(default_rootfs);
