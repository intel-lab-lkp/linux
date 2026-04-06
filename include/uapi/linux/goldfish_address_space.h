/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_LINUX_GOLDFISH_ADDRESS_SPACE_H
#define _UAPI_LINUX_GOLDFISH_ADDRESS_SPACE_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define GOLDFISH_ADDRESS_SPACE_DEVICE_NAME "goldfish_address_space"

struct goldfish_address_space_allocate_block {
	__u64 size;
	__u64 offset;
	__u64 phys_addr;
};

struct goldfish_address_space_ping {
	__u64 offset;
	__u64 size;
	__u64 metadata;
	__u32 version;
	__u32 wait_fd;
	__u32 wait_flags;
	__u32 direction;
};

struct goldfish_address_space_claim_shared {
	__u64 offset;
	__u64 size;
};

#define GOLDFISH_ADDRESS_SPACE_IOCTL_MAGIC 'G'

#define GOLDFISH_ADDRESS_SPACE_IOCTL_OP(OP, T) \
	_IOWR(GOLDFISH_ADDRESS_SPACE_IOCTL_MAGIC, OP, T)

#define GOLDFISH_ADDRESS_SPACE_IOCTL_ALLOCATE_BLOCK \
	GOLDFISH_ADDRESS_SPACE_IOCTL_OP(10, \
		struct goldfish_address_space_allocate_block)

#define GOLDFISH_ADDRESS_SPACE_IOCTL_DEALLOCATE_BLOCK \
	GOLDFISH_ADDRESS_SPACE_IOCTL_OP(11, __u64)

#define GOLDFISH_ADDRESS_SPACE_IOCTL_PING \
	GOLDFISH_ADDRESS_SPACE_IOCTL_OP(12, \
		struct goldfish_address_space_ping)

#define GOLDFISH_ADDRESS_SPACE_IOCTL_CLAIM_SHARED \
	GOLDFISH_ADDRESS_SPACE_IOCTL_OP(13, \
		struct goldfish_address_space_claim_shared)

#define GOLDFISH_ADDRESS_SPACE_IOCTL_UNCLAIM_SHARED \
	GOLDFISH_ADDRESS_SPACE_IOCTL_OP(14, __u64)

#endif /* _UAPI_LINUX_GOLDFISH_ADDRESS_SPACE_H */
