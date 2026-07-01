/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_UDMABUF_H
#define _UAPI_LINUX_UDMABUF_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define UDMABUF_FLAGS_CLOEXEC	0x01

/**
 * struct udmabuf_create - parameters for %UDMABUF_CREATE
 * @memfd: memfd containing the memory to export
 * @flags: %UDMABUF_FLAGS_CLOEXEC only; unknown bits are rejected
 * @offset: byte offset into @memfd (page aligned)
 * @size: number of bytes to export (page aligned, non-zero)
 */
struct udmabuf_create {
	__u32 memfd;
	__u32 flags;
	__u64 offset;
	__u64 size;
};

struct udmabuf_create_item {
	__u32 memfd;
	__u32 __pad;
	__u64 offset;
	__u64 size;
};

/**
 * struct udmabuf_create_list - parameters for %UDMABUF_CREATE_LIST
 * @flags: %UDMABUF_FLAGS_CLOEXEC only; unknown bits are rejected
 * @count: number of entries in @list (non-zero, capped by driver)
 * @list: flexible array of memfd regions to combine
 */
struct udmabuf_create_list {
	__u32 flags;
	__u32 count;
	struct udmabuf_create_item list[];
};

#define UDMABUF_CREATE       _IOW('u', 0x42, struct udmabuf_create)
#define UDMABUF_CREATE_LIST  _IOW('u', 0x43, struct udmabuf_create_list)

#endif /* _UAPI_LINUX_UDMABUF_H */
