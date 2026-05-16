/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_SYNCOBJ_H_
#define _UAPI_LINUX_SYNCOBJ_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define SYNCOBJ_CREATE_SIGNALED			(1 << 0)

#define SYNCOBJ_WAIT_FLAGS_WAIT_ALL		(1 << 0)
#define SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT	(1 << 1)
#define SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE	(1 << 2)
#define SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE	(1 << 3)

#define SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED	(1 << 0)

struct syncobj_create_args {
	__s32 fd;
	__u32 flags;
};

struct syncobj_wait_args {
	__u64 fds;
	__u64 points;
	__s64 timeout_nsec;
	__u32 count;
	__u32 flags;
	__u32 first_signaled;
	__u32 pad;
	__u64 deadline_nsec;
};

struct syncobj_array_args {
	__u64 fds;
	__u64 points;
	__u32 count;
	__u32 flags;
};

struct syncobj_transfer_args {
	__s32 src_fd;
	__s32 dst_fd;
	__u64 src_point;
	__u64 dst_point;
	__u32 flags;
	__u32 pad;
};

struct syncobj_eventfd_args {
	__s32 syncobj_fd;
	__s32 eventfd;
	__u64 point;
	__u32 flags;
	__u32 pad;
};

struct syncobj_sync_file_args {
	__s32 syncobj_fd;
	__s32 sync_file_fd;
	__u64 point;
};

#define SYNCOBJ_IOC_BASE		0xCD

#define SYNCOBJ_IOC_CREATE		_IOWR(SYNCOBJ_IOC_BASE, 0, struct syncobj_create_args)
#define SYNCOBJ_IOC_WAIT		_IOWR(SYNCOBJ_IOC_BASE, 1, struct syncobj_wait_args)
#define SYNCOBJ_IOC_RESET		_IOW(SYNCOBJ_IOC_BASE,  2, struct syncobj_array_args)
#define SYNCOBJ_IOC_SIGNAL		_IOW(SYNCOBJ_IOC_BASE,  3, struct syncobj_array_args)
#define SYNCOBJ_IOC_QUERY		_IOW(SYNCOBJ_IOC_BASE,  4, struct syncobj_array_args)
#define SYNCOBJ_IOC_TRANSFER		_IOW(SYNCOBJ_IOC_BASE,  5, struct syncobj_transfer_args)
#define SYNCOBJ_IOC_EVENTFD		_IOW(SYNCOBJ_IOC_BASE,  6, struct syncobj_eventfd_args)
#define SYNCOBJ_IOC_EXPORT_SYNC_FILE	_IOWR(SYNCOBJ_IOC_BASE, 7, struct syncobj_sync_file_args)
#define SYNCOBJ_IOC_IMPORT_SYNC_FILE	_IOW(SYNCOBJ_IOC_BASE,  8, struct syncobj_sync_file_args)

#endif /* _UAPI_LINUX_SYNCOBJ_H_ */
