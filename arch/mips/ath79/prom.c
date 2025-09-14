// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Atheros AR71XX/AR724X/AR913X specific prom routines
 *
 *  Copyright (C) 2015 Laurent Fasnacht <l@libres.ch>
 *  Copyright (C) 2008-2010 Gabor Juhos <juhosg@openwrt.org>
 *  Copyright (C) 2008 Imre Kaloz <kaloz@openwrt.org>
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/string.h>
#include <linux/initramfs.h>

#include <asm/bootinfo.h>
#include <asm/addrspace.h>
#include <asm/fw/fw.h>

#include "common.h"

void __init prom_init(void)
{
	fw_init_cmdline();

#ifdef CONFIG_BLK_DEV_INITRD
	/* Read the initrd address from the firmware environment */
	virt_external_initramfs_start = fw_getenvl("initrd_start");
	if (virt_external_initramfs_start) {
		virt_external_initramfs_start = KSEG0ADDR(virt_external_initramfs_start);
		virt_external_initramfs_end = virt_external_initramfs_start + fw_getenvl("initrd_size");
	}
#endif
}
