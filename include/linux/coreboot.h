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

#include <linux/bits.h>
#include <linux/compiler_attributes.h>
#include <linux/device.h>
#include <linux/stddef.h>
#include <linux/types.h>

struct module;

typedef __aligned(4) u64 cb_u64;

/* List of coreboot entry structures that is used */

#define CB_TAG_FRAMEBUFFER 0x12
#define LB_TAG_CBMEM_ENTRY 0x31
#define LB_TAG_CFR_ROOT 0x47

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

#define LB_FRAMEBUFFER_ORIENTATION_NORMAL	0
#define LB_FRAMEBUFFER_ORIENTATION_BOTTOM_UP	1
#define LB_FRAMEBUFFER_ORIENTATION_LEFT_UP	2
#define LB_FRAMEBUFFER_ORIENTATION_RIGHT_UP	3

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
	u8  orientation;
};

/*
 * True if the coreboot-provided data is large enough to hold information
 * on the linear framebuffer. False otherwise.
 */
#define LB_FRAMEBUFFER_HAS_LFB(__fb) \
	((__fb)->size >= offsetofend(struct lb_framebuffer, reserved_mask_size))

/*
 * True if the coreboot-provided data is large enough to hold information
 * on the display orientation. False otherwise.
 */
#define LB_FRAMEBUFFER_HAS_ORIENTATION(__fb) \
	((__fb)->size >= offsetofend(struct lb_framebuffer, orientation))

#define CFR_VERSION 0

enum cfr_tags {
	CFR_TAG_OPTION_FORM		= 1,
	CFR_TAG_ENUM_VALUE		= 2,
	CFR_TAG_OPTION_ENUM		= 3,
	CFR_TAG_OPTION_NUMBER		= 4,
	CFR_TAG_OPTION_BOOL		= 5,
	CFR_TAG_OPTION_VARCHAR		= 6,
	CFR_TAG_VARCHAR_OPT_NAME	= 7,
	CFR_TAG_VARCHAR_UI_NAME		= 8,
	CFR_TAG_VARCHAR_UI_HELPTEXT	= 9,
	CFR_TAG_VARCHAR_DEF_VALUE	= 10,
	CFR_TAG_OPTION_COMMENT		= 11,
	CFR_TAG_DEP_VALUES		= 12,
	CFR_TAG_RUNTIME_APPLY		= 13,
};

enum cfr_option_flags {
	CFR_OPTFLAG_READONLY	= BIT(0),
	CFR_OPTFLAG_INACTIVE	= BIT(1),
	CFR_OPTFLAG_SUPPRESS	= BIT(2),
	CFR_OPTFLAG_VOLATILE	= BIT(3),
	CFR_OPTFLAG_RUNTIME	= BIT(4),
};

struct lb_cfr_varbinary {
	u32 tag;
	u32 size;
	u32 data_length;
} __packed;

struct lb_cfr_enum_value {
	u32 tag;
	u32 size;
	u32 value;
} __packed;

enum cfr_runtime_apply_method {
	CFR_RUNTIME_APPLY_NONE		= 0,
	CFR_RUNTIME_APPLY_APM_CNT	= 1,
};

struct lb_cfr_runtime_apply {
	u32 tag;
	u32 size;
	u32 method;
	u32 id;
} __packed;

struct lb_cfr_numeric_option {
	u32 tag;
	u32 size;
	cb_u64 object_id;
	cb_u64 dependency_id;
	u32 flags;
	u32 default_value;
	u32 min;
	u32 max;
	u32 step;
	u32 display_flags;
} __packed;

struct lb_cfr_varchar_option {
	u32 tag;
	u32 size;
	cb_u64 object_id;
	cb_u64 dependency_id;
	u32 flags;
} __packed;

struct lb_cfr_option_comment {
	u32 tag;
	u32 size;
	cb_u64 object_id;
	cb_u64 dependency_id;
	u32 flags;
} __packed;

struct lb_cfr_option_form {
	u32 tag;
	u32 size;
	cb_u64 object_id;
	cb_u64 dependency_id;
	u32 flags;
} __packed;

struct lb_cfr {
	u32 tag;
	u32 size;
	u32 version;
	u32 checksum;
} __packed;

struct coreboot_device_id;
struct coreboot_device;

/* A device, additionally with information from coreboot. */
struct coreboot_device {
	struct device dev;
	union {
		struct coreboot_table_entry entry;
		struct lb_cbmem_ref cbmem_ref;
		struct lb_cbmem_entry cbmem_entry;
		struct lb_framebuffer framebuffer;
		DECLARE_FLEX_ARRAY(u8, raw);
	};
};

static inline struct coreboot_device *dev_to_coreboot_device(struct device *dev)
{
	return container_of(dev, struct coreboot_device, dev);
}

/* A driver for handling devices described in coreboot tables. */
struct coreboot_driver {
	int (*probe)(struct coreboot_device *dev);
	void (*remove)(struct coreboot_device *dev);
	struct device_driver drv;
	const struct coreboot_device_id *id_table;
};

/* use a macro to avoid include chaining to get THIS_MODULE */
#define coreboot_driver_register(driver) \
	__coreboot_driver_register(driver, THIS_MODULE)
/* Register a driver that uses the data from a coreboot table. */
int __coreboot_driver_register(struct coreboot_driver *driver,
			       struct module *owner);

/* Unregister a driver that uses the data from a coreboot table. */
void coreboot_driver_unregister(struct coreboot_driver *driver);

/* module_coreboot_driver() - Helper macro for drivers that don't do
 * anything special in module init/exit.  This eliminates a lot of
 * boilerplate.  Each module may only use this macro once, and
 * calling it replaces module_init() and module_exit()
 */
#define module_coreboot_driver(__coreboot_driver) \
	module_driver(__coreboot_driver, coreboot_driver_register, \
			coreboot_driver_unregister)

#endif /* _LINUX_COREBOOT_H */
