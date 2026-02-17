/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * coreboot.h
 *
 * Coreboot device and driver interfaces.
 *
 * Copyright 2014 Gerd Hoffmann <kraxel@redhat.com>
 * Copyright 2017 Google Inc.
 * Copyright 2017 Samuel Holland <samuel@sholland.org>
 */

#ifndef _LINUX_COREBOOT_H
#define _LINUX_COREBOOT_H

#include <linux/compiler_attributes.h>
#include <linux/types.h>

typedef __aligned(4) u64 cb_u64;

/* List of coreboot entry structures that is used */

#define CB_TAG_FRAMEBUFFER 0x12
#define LB_TAG_CBMEM_ENTRY 0x31

/* Generic */
struct coreboot_table_entry {
	u32 tag;
	u32 size;
};

/* Points to a CBMEM entry */
struct lb_cbmem_ref {
	u32 tag;
	u32 size;

	cb_u64 cbmem_addr;
};

/* Corresponds to LB_TAG_CBMEM_ENTRY */
struct lb_cbmem_entry {
	u32 tag;
	u32 size;

	cb_u64 address;
	u32 entry_size;
	u32 id;
};

/* Describes framebuffer setup by coreboot */
struct lb_framebuffer {
	u32 tag;
	u32 size;

	cb_u64 physical_address;
	u32 x_resolution;
	u32 y_resolution;
	u32 bytes_per_line;
	u8  bits_per_pixel;
	u8  red_mask_pos;
	u8  red_mask_size;
	u8  green_mask_pos;
	u8  green_mask_size;
	u8  blue_mask_pos;
	u8  blue_mask_size;
	u8  reserved_mask_pos;
	u8  reserved_mask_size;
};

#endif /* _LINUX_COREBOOT_H */
