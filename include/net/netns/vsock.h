/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __NET_NET_NAMESPACE_VSOCK_H
#define __NET_NET_NAMESPACE_VSOCK_H

#include <linux/types.h>

enum vsock_net_mode {
	VSOCK_NET_MODE_GLOBAL,
	VSOCK_NET_MODE_LOCAL,
	VSOCK_NET_MODE_LOCKED,
};

struct netns_vsock {
	struct ctl_table_header *sysctl_hdr;

	/* protected by the vsock_table_lock in af_vsock.c */
	u32 port;

	enum vsock_net_mode mode;

	/* 0 (GLOBAL)
	 * 1 (LOCAL)
	 * 2 (GLOBAL + LOCKED)
	 * 3 (LOCAL + LOCKED)
	 */
	int child_ns_mode;
};
#endif /* __NET_NET_NAMESPACE_VSOCK_H */
