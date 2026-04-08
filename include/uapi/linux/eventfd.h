/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_EVENTFD_H
#define _UAPI_LINUX_EVENTFD_H

#include <linux/fcntl.h>
#include <linux/ioctl.h>
#include <linux/types.h>

#define EFD_SEMAPHORE (1 << 0)
#define EFD_CLOEXEC O_CLOEXEC
#define EFD_NONBLOCK O_NONBLOCK

/* Flow-control ioctls: configure the per-fd counter maximum. */
#define EFD_IOC_SET_MAXIMUM	_IOW('J', 0, __u64)
#define EFD_IOC_GET_MAXIMUM	_IOR('J', 1, __u64)

#endif /* _UAPI_LINUX_EVENTFD_H */
