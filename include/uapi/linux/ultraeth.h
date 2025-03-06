/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */

#ifndef _UAPI_LINUX_ULTRAETH_H
#define _UAPI_LINUX_ULTRAETH_H

#include <asm/byteorder.h>
#include <linux/types.h>

#define UET_DEFAULT_PORT 5432
#define UET_SVC_MAX_LEN 64

enum {
	UET_ADDR_F_VALID_FEP_CAP	= (1 << 0),
	UET_ADDR_F_VALID_ADDR		= (1 << 1),
	UET_ADDR_F_VALID_PID_ON_FEP	= (1 << 2),
	UET_ADDR_F_VALID_RI		= (1 << 3),
	UET_ADDR_F_VALID_INIT_ID	= (1 << 4),
	UET_ADDR_F_ADDRESS_MODE		= (1 << 5),
	UET_ADDR_F_ADDRESS_TYPE		= (1 << 6),
	UET_ADDR_F_MTU_LIMITED		= (1 << 7),
};

#define UET_ADDR_FLAG_IP_VER (1 << 6)

struct fep_in_address {
	union {
		__be32 ip;
		__u8 ip6[16];
	};
	__u16 family;
};

struct fep_address {
	struct fep_in_address in_address;

	__u16 flags;
	__u16 fep_caps;
	__u16 start_resource_index;
	__u16 num_resource_indices;
	__u32 initiator_id;
	__u16 pid_on_fep;
	__u16 padding;
	__u8 version;
};
#endif /* _UAPI_LINUX_ULTRAETH_H */
