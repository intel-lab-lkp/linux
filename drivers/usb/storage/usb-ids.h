/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _USB_STOR_IDS_H_
#define _USB_STOR_IDS_H_

#include <linux/types.h>
#include <linux/bug.h>

/* Conversion of 32-bit quirks flags for 32-bit platforms */
extern const unsigned long usb_stor_drv_info_u64_table_size;
extern const unsigned long usb_uas_drv_info_u64_table_size;
extern const u64 usb_stor_drv_info_u64_table[];
extern const u64 usb_uas_drv_info_u64_table[];

#if IS_ENABLED(CONFIG_64BIT)
/* 64-bit systems don't need to use the drv_info_64_table */
static u64 usb_stor_drv_info_to_flags(const u64 *drv_info_u64_table,
	unsigned long table_size, unsigned long idx)
{
	return idx;
}
#else
/* 32-bit systems need to look up flags if bits 31 or beyond are used */
static u64 usb_stor_drv_info_to_flags(const u64 *drv_info_u64_table,
	unsigned long table_size, unsigned long idx)
{
	u64 flags = 0;

	if (idx < (1UL << 31))
		return idx;

	idx -= (1UL << 31);

	if (idx < table_size)
		flags = drv_info_u64_table[idx];
	else
		pr_warn_once("usb_stor_drv_info_u64_table not updated");

	return flags;
}
#endif

#endif
