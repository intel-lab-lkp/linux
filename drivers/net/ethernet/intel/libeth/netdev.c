// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2024 Intel Corporation */

#include <linux/etherdevice.h>
#include <linux/ethtool.h>

#include <net/libeth/netdev.h>
#include <net/libeth/types.h>

#include "priv.h"

/**
 * __libeth_netdev_alloc - allocate a &net_device with libeth generic stats
 * @priv: sizeof() of the private structure with embedded &libeth_netdev_priv
 * @rqs: maximum number of Rx queues to be used
 * @sqs: maximum number of Tx queues to be used
 * @xdpsqs: maximum number of XDP Tx queues to be used
 *
 * Allocates a new &net_device and initializes the embedded &libeth_netdev_priv
 * and the libeth generic stats for it.
 * Use the non-underscored wrapper in drivers instead.
 *
 * Return: new &net_device on success, %NULL on error.
 */
struct net_device *__libeth_netdev_alloc(u32 priv, u32 rqs, u32 sqs,
					 u32 xdpsqs)
{
	struct net_device *dev;

	dev = alloc_etherdev_mqs(priv, sqs, rqs);
	if (!dev)
		return NULL;

	if (!libeth_stats_init_priv(dev, rqs, sqs, xdpsqs))
		goto err_netdev;

	return dev;

err_netdev:
	free_netdev(dev);

	return NULL;
}
EXPORT_SYMBOL_NS_GPL(__libeth_netdev_alloc, LIBETH);

/**
 * libeth_netdev_free - free a &net_device with libeth generic stats
 * @dev: &net_device to free
 *
 * Deinitializes and frees the embedded &libeth_netdev_priv and the netdev
 * itself, to be used if @dev was allocated using libeth_netdev_alloc().
 */
void libeth_netdev_free(struct net_device *dev)
{
	libeth_stats_free_priv(dev);
	free_netdev(dev);
}
EXPORT_SYMBOL_NS_GPL(libeth_netdev_free, LIBETH);

/**
 * __libeth_set_real_num_queues - set the actual number of queues in use
 * @dev: &net_device to configure
 * @rqs: actual number of Rx queues
 * @sqs: actual number of Tx queues
 * @xdpsqs: actual number of XDP Tx queues
 *
 * Sets the actual number of queues in use, to be called on ifup for netdevs
 * allocated via libeth_netdev_alloc().
 * Use the non-underscored wrapper in drivers instead.
 *
 * Return: %0 on success, -errno on error.
 */
int __libeth_set_real_num_queues(struct net_device *dev, u32 rqs, u32 sqs,
				 u32 xdpsqs)
{
	struct libeth_netdev_priv *priv = netdev_priv(dev);
	int ret;

	ret = netif_set_real_num_rx_queues(dev, rqs);
	if (ret)
		return ret;

	ret = netif_set_real_num_tx_queues(dev, sqs);
	if (ret)
		return ret;

	priv->curr_xdpsqs = xdpsqs;

	return 0;
}
EXPORT_SYMBOL_NS_GPL(__libeth_set_real_num_queues, LIBETH);

/* Ethtool */

/**
 * libeth_ethtool_get_sset_count - get the number of libeth generic stats
 * @dev: libeth-driven &net_device
 * @sset: ``ETH_SS_STATS`` only, for compatibility with Ethtool callbacks
 *
 * Can be used directly in &ethtool_ops if the driver doesn't have HW-specific
 * stats or called from the corresponding driver callback.
 *
 * Return: the number of stats/stringsets.
 */
int libeth_ethtool_get_sset_count(struct net_device *dev, int sset)
{
	if (sset != ETH_SS_STATS)
		return 0;

	return libeth_stats_get_sset_count(dev);
}
EXPORT_SYMBOL_NS_GPL(libeth_ethtool_get_sset_count, LIBETH);

/**
 * libeth_ethtool_get_strings - get libeth generic stats strings/names
 * @dev: libeth-driven &net_device
 * @sset: ``ETH_SS_STATS`` only, for compatibility with Ethtool callbacks
 * @data: container to fill with the stats names
 *
 * Can be used directly in &ethtool_ops if the driver doesn't have HW-specific
 * stats or called from the corresponding driver callback.
 * Note that the function doesn't advance the @data pointer, so it's better to
 * call it at the end to avoid code complication.
 */
void libeth_ethtool_get_strings(struct net_device *dev, u32 sset, u8 *data)
{
	if (sset != ETH_SS_STATS)
		return;

	libeth_stats_get_strings(dev, data);
}
EXPORT_SYMBOL_NS_GPL(libeth_ethtool_get_strings, LIBETH);

/**
 * libeth_ethtool_get_stats - get libeth generic stats counters
 * @dev: libeth-driven &net_device
 * @stats: unused, for compatibility with Ethtool callbacks
 * @data: container to fill with the stats counters
 *
 * Can be used directly in &ethtool_ops if the driver doesn't have HW-specific
 * stats or called from the corresponding driver callback.
 * Note that the function doesn't advance the @data pointer, so it's better to
 * call it at the end to avoid code complication. Anyhow, the order must be the
 * same as in the ::get_strings() implementation.
 */
void libeth_ethtool_get_stats(struct net_device *dev,
			      struct ethtool_stats *stats,
			      u64 *data)
{
	libeth_stats_get_data(dev, data);
}
EXPORT_SYMBOL_NS_GPL(libeth_ethtool_get_stats, LIBETH);

/* Module */

MODULE_DESCRIPTION("Common Ethernet library");
MODULE_LICENSE("GPL");
