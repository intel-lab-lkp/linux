// SPDX-License-Identifier: GPL-2.0-only

#include <linux/netdevice.h>
#include <net/netdev_queues.h>

#include "dev.h"

int netdev_alloc_config(struct net_device *dev)
{
	struct netdev_config *cfg;

	cfg = kzalloc(sizeof(*dev->cfg), GFP_KERNEL_ACCOUNT);
	if (!cfg)
		return -ENOMEM;

	dev->cfg = cfg;
	dev->cfg_pending = cfg;
	return 0;
}

void __netdev_free_config(struct netdev_config *cfg)
{
	kfree(cfg);
}

void netdev_free_config(struct net_device *dev)
{
	WARN_ON(dev->cfg != dev->cfg_pending);
	__netdev_free_config(dev->cfg);
}

int netdev_reconfig_start(struct net_device *dev)
{
	struct netdev_config *cfg;

	WARN_ON(dev->cfg != dev->cfg_pending);
	cfg = kmemdup(dev->cfg, sizeof(*dev->cfg), GFP_KERNEL_ACCOUNT);
	if (!cfg)
		return -ENOMEM;

	dev->cfg_pending = cfg;
	return 0;
}
