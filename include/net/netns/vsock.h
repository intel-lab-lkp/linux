/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __NET_NET_NAMESPACE_VSOCK_H
#define __NET_NET_NAMESPACE_VSOCK_H

#include <linux/types.h>

#define VSOCK_NET_MODE_GLOBAL	1
#define VSOCK_NET_MODE_LOCAL	(1 << 1)

struct vsock_loopback;

struct netns_vsock {
	struct ctl_table_header *vsock_hdr;
	spinlock_t lock;

	/* protected by lock */
	u8 ns_mode;
	bool written;
	struct vsock_loopback *loopback;
};
#endif /* __NET_NET_NAMESPACE_VSOCK_H */
