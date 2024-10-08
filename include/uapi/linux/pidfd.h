/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */

#ifndef _UAPI_LINUX_PIDFD_H
#define _UAPI_LINUX_PIDFD_H

#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/ioctl.h>

/* Flags for pidfd_open().  */
#define PIDFD_NONBLOCK	O_NONBLOCK
#define PIDFD_THREAD	O_EXCL

/* Flags for pidfd_send_signal(). */
#define PIDFD_SIGNAL_THREAD		(1UL << 0)
#define PIDFD_SIGNAL_THREAD_GROUP	(1UL << 1)
#define PIDFD_SIGNAL_PROCESS_GROUP	(1UL << 2)

/* Flags for pidfd_info. */
#define PIDFD_INFO_CGROUPID		(1UL << 0)

struct pidfd_info {
	/* Let userspace request expensive stuff explictly, and let the kernel
	 * indicate whether it knows about it. */
	__u64 request_mask;
	/*
	 * The information contained in the following fields might be stale at the
	 * time it is received, as the target process might have exited as soon as
	 * the IOCTL was processed, and there is no way to avoid that. However, it
	 * is guaranteed that if the call was successful, then the information was
	 * correct and referred to the intended process at the time the work was
	 * performed. */
	__u64 cgroupid;
	__u32 pid;
	__u32 tgid;
	__u32 ppid;
	__u32 ruid;
	__u32 rgid;
	__u32 euid;
	__u32 egid;
	__u32 suid;
	__u32 sgid;
	__u32 fsuid;
	__u32 fsgid;
	__u32 spare0[1];
};

#define PIDFS_IOCTL_MAGIC 0xFF

#define PIDFD_GET_CGROUP_NAMESPACE            _IO(PIDFS_IOCTL_MAGIC, 1)
#define PIDFD_GET_IPC_NAMESPACE               _IO(PIDFS_IOCTL_MAGIC, 2)
#define PIDFD_GET_MNT_NAMESPACE               _IO(PIDFS_IOCTL_MAGIC, 3)
#define PIDFD_GET_NET_NAMESPACE               _IO(PIDFS_IOCTL_MAGIC, 4)
#define PIDFD_GET_PID_NAMESPACE               _IO(PIDFS_IOCTL_MAGIC, 5)
#define PIDFD_GET_PID_FOR_CHILDREN_NAMESPACE  _IO(PIDFS_IOCTL_MAGIC, 6)
#define PIDFD_GET_TIME_NAMESPACE              _IO(PIDFS_IOCTL_MAGIC, 7)
#define PIDFD_GET_TIME_FOR_CHILDREN_NAMESPACE _IO(PIDFS_IOCTL_MAGIC, 8)
#define PIDFD_GET_USER_NAMESPACE              _IO(PIDFS_IOCTL_MAGIC, 9)
#define PIDFD_GET_UTS_NAMESPACE               _IO(PIDFS_IOCTL_MAGIC, 10)
#define PIDFD_GET_INFO                        _IOWR(PIDFS_IOCTL_MAGIC, 11, struct pidfd_info)

#endif /* _UAPI_LINUX_PIDFD_H */
