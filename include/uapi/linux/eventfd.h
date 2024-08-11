/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_EVENTFD_H
#define _UAPI_LINUX_EVENTFD_H

#include <linux/fcntl.h>

#define EFD_SEMAPHORE (1 << 0)
#define EFD_CLOEXEC O_CLOEXEC
#define EFD_NONBLOCK O_NONBLOCK

struct eventfd_qos {
	__u32 token_capacity;
	__u32 token_rate;
};

#define EFD_IOC_SET_QOS	_IOW('E', 0, struct eventfd_qos)
#define EFD_IOC_GET_QOS	_IOR('E', 0, struct eventfd_qos)

#endif /* _UAPI_LINUX_EVENTFD_H */
