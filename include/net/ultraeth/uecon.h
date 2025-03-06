/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */

#ifndef _UECON_H
#define _UECON_H
#include <net/ip_tunnels.h>

#define UECON_DEFAULT_PORT 5432

struct socket;
struct net_device;

struct uecon_ndev_priv {
	struct uet_context *context;
	struct socket __rcu *sock;
	struct net_device *dev;
	__be16 udp_port;
};

extern const struct net_device_ops uecon_netdev_ops;
int uecon_netdev_init(struct uet_context *ctx);
void uecon_netdev_uninit(struct uet_context *ctx);

int uecon_rtnl_link_register(void);
void uecon_rtnl_link_unregister(void);

int uecon_netdev_register(void);
void uecon_netdev_unregister(void);
#endif /* _UECON_H */
