// SPDX-License-Identifier: GPL-2.0

#include <linux/netdevice.h>
#include <linux/skbuff.h>

__rust_helper void rust_helper_dev_kfree_skb(struct sk_buff *skb)
{
	dev_kfree_skb(skb);
}

__rust_helper void rust_helper_dev_lstats_add(struct net_device *dev, unsigned int len)
{
	dev_lstats_add(dev, len);
}

__rust_helper void *rust_helper_netdev_priv(const struct net_device *dev)
{
	return netdev_priv(dev);
}
