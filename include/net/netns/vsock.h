/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __NET_NET_NAMESPACE_VSOCK_H
#define __NET_NET_NAMESPACE_VSOCK_H

#include <linux/types.h>

enum vsock_net_mode {
	VSOCK_NET_MODE_GLOBAL,
	VSOCK_NET_MODE_LOCAL,
};

struct vsock_loopback;

struct netns_vsock {
	struct ctl_table_header *vsock_hdr;
	spinlock_t lock;

	/* protected by lock */
	enum vsock_net_mode mode;
	bool written;
#if IS_ENABLED(CONFIG_VSOCKETS_LOOPBACK)
	struct vsock_loopback *loopback;
#endif
};
#endif /* __NET_NET_NAMESPACE_VSOCK_H */
