/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_MEMFD_TRIPWIRE_H
#define _UAPI_LINUX_MEMFD_TRIPWIRE_H

#include <linux/ioctl.h>

#define MEMFD_TRIPWIRE_IOC		0xDA

/*
 * MEMFD_TRIPWIRE_ACK serves two purpose: First, it re-arms the mechanism to
 * make sure future write activity triggers a POLLIN notification. Second, it
 * makes sure that all writes up to the ACK are visible to the calling process.
 * It is also guaranteed that no writes can sneak through unnoticed, i.e. after
 * ACK concurrent writes have either taken place and are visible to the
 * consumer, or will generate subsequent POLLIN events.
 */
#define MEMFD_TRIPWIRE_ACK		_IO(MEMFD_TRIPWIRE_IOC, 0x00)

#endif /* _UAPI_LINUX_MEMFD_TRIPWIRE_H */
