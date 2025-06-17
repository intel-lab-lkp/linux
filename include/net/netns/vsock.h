/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __NET_NET_NAMESPACE_VSOCK_H
#define __NET_NET_NAMESPACE_VSOCK_H

#include <linux/types.h>

// TODO: rename to VSOCK_NET_* ?
#define VSOCK_NS_MODE_GLOBAL	1
#define VSOCK_NS_MODE_LOCAL	(1 << 1)
#define VSOCK_NS_MODE_INVALID	(~0)
/* VSOCK_NS_MODE_WRITTEN_ONCE indicates "write-once" write has occurred */
#define VSOCK_NS_MODE_WRITTEN_ONCE	(1 << 7)

struct netns_vsock {
	struct ctl_table_header *vsock_hdr;
	spinlock_t lock;
	u8 ns_mode;
};
#endif /* __NET_NET_NAMESPACE_VSOCK_H */
