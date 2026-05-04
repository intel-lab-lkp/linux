// SPDX-License-Identifier: MIT
/*
 * MCTP-over-MAILBOX transport for Xe.
 *
 * Copyright 2026 Intel Corporation
 */

#include "xe_device_types.h"

#include <linux/netdevice.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>

#include <net/mctp.h>
#include <net/mctpdevice.h>
#include <net/pkt_sched.h>

#include <uapi/linux/if_arp.h>

#include "xe_mctp_mailbox.h"

#define XE_MCTP_MAILBOX_RX_POLL_MS 100

/** @mctp_mailbox: Struct for mctp over mailbox */
struct xe_mctp_mailbox {
	/** @mctp_mailbox.netdev: network device  */
	struct net_device *netdev;
	/** @running: true while netdev is opened */
	bool running;
	/** @work: worker to handle mctp requests from firmware */
	struct delayed_work work;
	/** @wq: workqueue to schecdule mctp rx worker */
	struct workqueue_struct *wq;
};

/*
 * mailbox protocol is interrupt free so for receive path i.e. endpoint to host
 * there is no irq available so rx handler need to be polled in worker periodically.
 */
static void mctp_mailbox_rx_handler(struct work_struct *work)
{
	struct xe_mctp_mailbox *mctp_mailbox =
		container_of(work, struct xe_mctp_mailbox, work.work);
	struct net_device *netdev = mctp_mailbox->netdev;

	if (!netdev)
		return;

	dev_hold(netdev);

	/*
	 * if (mctp_mailbox_rx_ready()) {
	 * Get data over MAILBOX
	 * Allocate skb and copy rx data to skb
	 * Queue skb to upper layer
	 * netif_rx(skb);
	}
	 */
	dev_put(netdev);

	if (mctp_mailbox->running)
		queue_delayed_work(mctp_mailbox->wq, &mctp_mailbox->work,
				   msecs_to_jiffies(XE_MCTP_MAILBOX_RX_POLL_MS));
}

static netdev_tx_t mctp_mailbox_start_xmit(struct sk_buff *skb,
					   struct net_device *dev)
{
	/* RFC stub: send skb over MAILBOX */
	dev_dstats_tx_dropped(dev);
	kfree_skb(skb);

	return NETDEV_TX_OK;
}

static int mctp_mailbox_open(struct net_device *dev)
{
	struct xe_mctp_mailbox *mctp_mailbox = netdev_priv(dev);

	mctp_mailbox->running = true;
	netif_start_queue(dev);

	queue_delayed_work(mctp_mailbox->wq, &mctp_mailbox->work, 0);

	return 0;
}

static int mctp_mailbox_stop(struct net_device *dev)
{
	struct xe_mctp_mailbox *mctp_mailbox = netdev_priv(dev);

	mctp_mailbox->running = false;
	netif_stop_queue(dev);

	cancel_delayed_work_sync(&mctp_mailbox->work);
	flush_workqueue(mctp_mailbox->wq);

	return 0;
}

static const struct net_device_ops mctp_mailbox_netdev_ops = {
	.ndo_start_xmit = mctp_mailbox_start_xmit,
	.ndo_open = mctp_mailbox_open,
	.ndo_stop = mctp_mailbox_stop,
};

static void mctp_mailbox_netdev_setup(struct net_device *dev)
{
	/* Populate netdev structure */
	dev->type = ARPHRD_MCTP;
	/*
	 *	dev->mtu = MCTP_MAILBOX_MTU_MIN;
	 *	dev->min_mtu = MCTP_MAILBOX_MTU_MIN;
	 *	dev->max_mtu = MCTP_MAILBOX_MTU_MAX;
	 *
	 *	dev->hard_header_len = sizeof(struct mctp_mailbox_hdr);
	 *	dev->tx_queue_len = DEFAULT_TX_QUEUE_LEN;
	 */
	dev->flags = IFF_NOARP;
	dev->netdev_ops = &mctp_mailbox_netdev_ops;
	dev->pcpu_stat_type = NETDEV_PCPU_STAT_DSTATS;
}

static void xe_mctp_mailbox_fini(void *arg)
{
	struct xe_device *xe = arg;
	struct xe_mctp_mailbox *mctp_mailbox = xe->mctp_mailbox;
	struct net_device *netdev;

	if (!mctp_mailbox)
		return;

	netdev = mctp_mailbox->netdev;
	if (!netdev) {
		xe->mctp_mailbox = NULL;
		return;
	}

	if (mctp_mailbox->wq) {
		mctp_mailbox->running = false;
		cancel_delayed_work_sync(&mctp_mailbox->work);
		destroy_workqueue(mctp_mailbox->wq);
		mctp_mailbox->wq = NULL;
	}

	xe->mctp_mailbox = NULL;
	mctp_unregister_netdev(netdev);
	free_netdev(netdev);
}

int xe_mctp_mailbox_init(struct xe_device *xe)
{
	struct xe_mctp_mailbox *mctp_mailbox;
	struct net_device *netdev;
	int ret, err;

	netdev = alloc_netdev(sizeof(*mctp_mailbox), "mctp_mailbox%d", NET_NAME_ENUM,
			      mctp_mailbox_netdev_setup);
	if (!netdev)
		return -ENOMEM;

	SET_NETDEV_DEV(netdev, xe->drm.dev);
	mctp_mailbox = netdev_priv(netdev);
	mctp_mailbox->netdev = netdev;

	ret = mctp_register_netdev(netdev, NULL, MCTP_PHYS_BINDING_VENDOR);
	if (ret) {
		free_netdev(netdev);
		return ret;
	}

	INIT_DELAYED_WORK(&mctp_mailbox->work, mctp_mailbox_rx_handler);
	mctp_mailbox->wq = alloc_ordered_workqueue("mctp-mailbox-ordered-wq", 0);
	if (!mctp_mailbox->wq) {
		mctp_unregister_netdev(netdev);
		free_netdev(netdev);
		return -ENOMEM;
	}

	xe->mctp_mailbox = mctp_mailbox;
	err = devm_add_action_or_reset(xe->drm.dev, xe_mctp_mailbox_fini, xe);
	if (err)
		return err;

	return 0;
}
