// SPDX-License-Identifier: GPL-2.0-only

#include <linux/netdevice.h>
#include <net/netdev_queues.h>

#include "dev.h"

int netdev_alloc_config(struct net_device *dev)
{
	struct netdev_config *cfg;
	unsigned int maxqs;

	cfg = kzalloc(sizeof(*dev->cfg), GFP_KERNEL_ACCOUNT);
	if (!cfg)
		return -ENOMEM;

	maxqs = max(dev->num_rx_queues, dev->num_tx_queues);
	cfg->qcfg = kcalloc(maxqs, sizeof(*cfg->qcfg), GFP_KERNEL_ACCOUNT);
	if (!cfg->qcfg)
		goto err_free_cfg;

	dev->cfg = cfg;
	dev->cfg_pending = cfg;
	return 0;

err_free_cfg:
	kfree(cfg);
	return -ENOMEM;
}

void __netdev_free_config(struct netdev_config *cfg)
{
	kfree(cfg->qcfg);
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
	unsigned int maxqs;

	WARN_ON(dev->cfg != dev->cfg_pending);
	cfg = kmemdup(dev->cfg, sizeof(*dev->cfg), GFP_KERNEL_ACCOUNT);
	if (!cfg)
		return -ENOMEM;

	maxqs = max(dev->num_rx_queues, dev->num_tx_queues);
	cfg->qcfg = kmemdup_array(dev->cfg->qcfg, maxqs, sizeof(*cfg->qcfg),
				  GFP_KERNEL_ACCOUNT);
	if (!cfg->qcfg)
		goto err_free_cfg;

	dev->cfg_pending = cfg;
	return 0;

err_free_cfg:
	kfree(cfg);
	return -ENOMEM;
}

void netdev_queue_config_update_cnt(struct net_device *dev, unsigned int txq,
				    unsigned int rxq)
{
	size_t len;

	if (rxq < dev->real_num_rx_queues) {
		len = (dev->real_num_rx_queues - rxq) * sizeof(*dev->cfg->qcfg);

		memset(&dev->cfg->qcfg[rxq], 0, len);
		memset(&dev->cfg_pending->qcfg[rxq], 0, len);
	}
}

void __netdev_queue_config(struct net_device *dev, int rxq,
			   struct netdev_queue_config *qcfg, bool pending)
{
	const struct netdev_config *cfg;

	cfg = pending ? dev->cfg_pending : dev->cfg;

	memset(qcfg, 0, sizeof(*qcfg));

	/* Get defaults from the driver, in case user config not set */
	if (dev->queue_mgmt_ops->ndo_queue_cfg_defaults)
		dev->queue_mgmt_ops->ndo_queue_cfg_defaults(dev, rxq, qcfg);

	/* Set config based on device-level settings */
	if (cfg->rx_buf_len)
		qcfg->rx_buf_len = cfg->rx_buf_len;

	/* Set config dedicated to this queue */
	if (rxq >= 0) {
		const struct netdev_queue_config *user_cfg = &cfg->qcfg[rxq];

		if (user_cfg->rx_buf_len)
			qcfg->rx_buf_len = user_cfg->rx_buf_len;
	}
}

/**
 * netdev_queue_config() - get configuration for a given queue
 * @dev:  net_device instance
 * @rxq:  index of the queue of interest
 * @qcfg: queue configuration struct (output)
 *
 * Render the configuration for a given queue. This helper should be used
 * by drivers which support queue configuration to retrieve config for
 * a particular queue.
 *
 * @qcfg is an output parameter and is always fully initialized by this
 * function. Some values may not be set by the user, drivers may either
 * deal with the "unset" values in @qcfg, or provide the callback
 * to populate defaults in queue_management_ops.
 *
 * Note that this helper returns pending config, as it is expected that
 * "old" queues are retained until config is successful so they can
 * be restored directly without asking for the config.
 */
void netdev_queue_config(struct net_device *dev, int rxq,
			 struct netdev_queue_config *qcfg)
{
	__netdev_queue_config(dev, rxq, qcfg, true);
}
EXPORT_SYMBOL(netdev_queue_config);

int netdev_queue_config_revalidate(struct net_device *dev,
				   struct netlink_ext_ack *extack)
{
	const struct netdev_queue_mgmt_ops *qops = dev->queue_mgmt_ops;
	struct netdev_queue_config qcfg;
	int i, err;

	if (!qops || !qops->ndo_queue_cfg_validate)
		return 0;

	for (i = -1; i < (int)dev->real_num_rx_queues; i++) {
		netdev_queue_config(dev, i, &qcfg);
		err = qops->ndo_queue_cfg_validate(dev, i, &qcfg, extack);
		if (err)
			return err;
	}

	return 0;
}
