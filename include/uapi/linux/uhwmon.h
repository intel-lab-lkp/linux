/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
 */

#ifndef _UAPI_LINUX_UHWMON_H
#define _UAPI_LINUX_UHWMON_H

#include <linux/hwmon.h>
#include <linux/ioctl.h>
#include <linux/types.h>

#define UHWMON_ATTR_CONSTANT (1U << 0)

/*
 * type and attr are hwmon enum identifiers, not HWMON_* bitmasks. channel is
 * zero-based. Numeric attributes use size = 0; labels use a NUL-inclusive size
 * in 1..PAGE_SIZE and omit the sysfs newline. Only constants set value or text.
 */
struct uhwmon_attribute {
	__u32 type;
	__u32 attr;
	__u32 channel;
	__u32 mode;
	__u32 flags;
	__u32 size;
	__aligned_s64 value;
	__aligned_u64 text;
};

/* name points to a NUL-terminated chip name; reserved must be zero. */
struct uhwmon_create {
	__aligned_u64 name;
	__aligned_u64 attrs;
	__u32 num_attrs;
	__u32 reserved;
};

#define UHWMON_READ 1
#define UHWMON_WRITE 2

/* attr indexes the registration array; value carries a numeric write. */
struct uhwmon_request {
	__aligned_u64 id;
	__u32 attr;
	__u32 op;
	__aligned_s64 value;
};

/* Only successful label reads append size bytes of NUL-terminated text. */
struct uhwmon_reply {
	__aligned_u64 id;
	__s32 status;
	__u32 reserved;
	__aligned_s64 value;
	char text[];
};

/* CREATE also reconnects an identical, currently unowned device. */
#define UHWMON_CREATE _IOW(0xb9, 0, struct uhwmon_create)
#define UHWMON_NOTIFY _IOW(0xb9, 1, __u32)
#define UHWMON_DESTROY _IO(0xb9, 2)

#endif /* _UAPI_LINUX_UHWMON_H */
