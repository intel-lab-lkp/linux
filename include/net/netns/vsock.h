/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __NET_NET_NAMESPACE_VSOCK_H
#define __NET_NET_NAMESPACE_VSOCK_H

#include <linux/types.h>

enum vsock_net_mode {
	VSOCK_NET_MODE_GLOBAL,
	VSOCK_NET_MODE_LOCAL,
};

struct netns_vsock {
	struct ctl_table_header *sysctl_hdr;
	enum vsock_net_mode mode;
	enum vsock_net_mode child_ns_mode;
};
#endif /* __NET_NET_NAMESPACE_VSOCK_H */
