/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/netdevice.h>
#include <net/netdev_queues.h>
#include <net/netdev_rx_queue.h>

int netdev_rx_queue_restart(struct net_device *dev, unsigned int rxq)
{
	void *new_mem;
	void *old_mem;
	int err;

	if (!dev->queue_mgmt_ops->ndo_queue_stop ||
	    !dev->queue_mgmt_ops->ndo_queue_mem_free ||
	    !dev->queue_mgmt_ops->ndo_queue_mem_alloc ||
	    !dev->queue_mgmt_ops->ndo_queue_start)
		return -EOPNOTSUPP;

	new_mem = dev->queue_mgmt_ops->ndo_queue_mem_alloc(dev, rxq);
	if (!new_mem)
		return -ENOMEM;

	rtnl_lock();
	err = dev->queue_mgmt_ops->ndo_queue_stop(dev, rxq, &old_mem);
	if (err)
		goto err_free_new_mem;

	err = dev->queue_mgmt_ops->ndo_queue_start(dev, rxq, new_mem);
	if (err)
		goto err_start_queue;
	rtnl_unlock();

	dev->queue_mgmt_ops->ndo_queue_mem_free(dev, old_mem);

	return 0;

err_start_queue:
	/* Restarting the queue with old_mem should be successful as we haven't
	 * changed any of the queue configuration, and there is not much we can
	 * do to recover from a failure here.
	 *
	 * WARN if the we fail to recover the old rx queue, and at least free
	 * old_mem so we don't also leak that.
	 */
	if (dev->queue_mgmt_ops->ndo_queue_start(dev, rxq, old_mem)) {
		WARN(1,
		     "Failed to restart old queue in error path. RX queue %d may be unhealthy.",
		     rxq);
		dev->queue_mgmt_ops->ndo_queue_mem_free(dev, &old_mem);
	}

err_free_new_mem:
	dev->queue_mgmt_ops->ndo_queue_mem_free(dev, new_mem);
	rtnl_unlock();

	return err;
}
EXPORT_SYMBOL_GPL(netdev_rx_queue_restart);
