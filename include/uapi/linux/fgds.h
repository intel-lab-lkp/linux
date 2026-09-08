/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Fast GPU Direct Storage user-kernel ABI definitions.
 *
 * Copyright (C) 2026 KylinSoft. Co., Ltd. All rights reserved.
 */
#ifndef _FGDS_H
#define _FGDS_H

#include <linux/types.h>
#include <linux/ioctl.h>

/*
 * Register a GPU dma-buf range.
 *
 * @dmabuf_fd: File descriptor exported by the GPU runtime.
 * @flags: Reserved for future extentions, must be 0.
 * @dmabuf_offset: Byte offset inside dma-buf (PAGE_SIZE aligned).
 * @size: Range size in bytes (PAGE_SIZE aligned, non-zero).
 * @idx: Output token to pass as mmap(2) offset.
 *
 * Return mmap token by @idx on success. Caller may close @dmabuf_fd after
 * registration. Higher platform alignment (e.g., 64KB) must be handled
 * in user space.
 */
struct fgds_ioctl_reg_buffer {
	/* Input */
	__s32 dmabuf_fd;
	__u32 flags;		/* Reserved, must be 0 */
	__u64 dmabuf_offset;
	__u64 size;

	/* Output */
	__u64 idx;
};

/*
 * Unregister a GPU dma-buf range
 *
 * @idx: Token returned by %FGDS_IOCTL_REG_BUFFER.
 *
 */
struct fgds_ioctl_unreg_buffer {
	__u64 idx;
};

#define FGDS_IOCTL_MAGIC	0x88

#define FGDS_IOCTL_REG_BUFFER	_IOWR(FGDS_IOCTL_MAGIC, 0x01, \
				      struct fgds_ioctl_reg_buffer)
#define FGDS_IOCTL_UNREG_BUFFER	_IOW(FGDS_IOCTL_MAGIC, 0x02, \
				     struct fgds_ioctl_unreg_buffer)

#endif /* _FGDS_H */
