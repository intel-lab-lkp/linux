/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * netdev_tx_clk.h - allow net_device TX clock control
 * Author: Arkadiusz Kubalewski <arkadiusz.kubalewski@intel.com>
 */

#ifndef __NETDEV_TX_CLK_H
#define __NETDEV_TX_CLK_H

#include <linux/netdevice.h>

/**
 * struct netdev_tx_clk_ops - TX clock operations
 * @enable: switch to this clock (called when user writes "1" to sysfs)
 * @is_enabled: check if this clock is currently active
 *
 * Note: one clock must always be active, writing "0" to disable is not
 * supported.
 */
struct netdev_tx_clk_ops {
	int (*enable)(void *priv_data);
	int (*is_enabled)(void *priv_data);
};
#if IS_ENABLED(CONFIG_NET_TX_CLK)

int netdev_tx_clk_register(struct net_device *ndev, const char *clk_name,
			   const struct netdev_tx_clk_ops *ops,
			   void *priv_data);

void netdev_tx_clk_cleanup(struct net_device *ndev);
#else

static inline int netdev_tx_clk_register(struct net_device *ndev, const char *clk_name,
					 const struct netdev_tx_clk_ops *ops,
					 void *priv_data)
{
	return 0;
}

static inline void netdev_tx_clk_cleanup(struct net_device *ndev) { }
#endif

#endif /* __NETDEV_TX_CLK_H */
