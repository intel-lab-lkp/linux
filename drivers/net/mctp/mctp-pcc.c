// SPDX-License-Identifier: GPL-2.0
/*
 * mctp-pcc.c - Driver for MCTP over PCC.
 * Copyright (c) 2024-2025, Ampere Computing LLC
 *
 */

/* Implementation of MCTP over PCC DMTF Specification DSP0256
 * https://www.dmtf.org/sites/default/files/standards/documents/DSP0292_1.0.0WIP50.pdf
 */

#include <linux/acpi.h>
#include <linux/if_arp.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/skbuff.h>
#include <linux/hrtimer.h>

#include <acpi/acpi_bus.h>
#include <acpi/acpi_drivers.h>
#include <acpi/acrestyp.h>
#include <acpi/actbl.h>
#include <net/mctp.h>
#include <net/mctpdevice.h>
#include <acpi/pcc.h>

#include "../../mailbox/mailbox.h"

#define MCTP_SIGNATURE          "MCTP"
#define MCTP_SIGNATURE_LENGTH   (sizeof(MCTP_SIGNATURE) - 1)
#define MCTP_MIN_MTU            68
#define PCC_DWORD_TYPE          0x0c

struct mctp_pcc_mailbox {
	u32 index;
	struct pcc_mbox_chan *chan;
	struct mbox_client client;
	struct sk_buff_head packets;
};

/* The netdev structure. One of these per PCC adapter. */
struct mctp_pcc_ndev {
	/* spinlock to serialize access to queue that holds a copy of the
	 * sk_buffs that are also in the ring buffers of the mailbox.
	 */
	spinlock_t lock;
	struct net_device *ndev;
	struct acpi_device *acpi_device;
	struct mctp_pcc_mailbox inbox;
	struct mctp_pcc_mailbox outbox;
};

static void *mctp_pcc_rx_alloc(struct mbox_client *c, int size)
{
	struct mctp_pcc_ndev *mctp_pcc_ndev;
	struct mctp_pcc_mailbox *box;
	struct sk_buff *skb;

	mctp_pcc_ndev =	container_of(c, struct mctp_pcc_ndev, inbox.client);
	box = &mctp_pcc_ndev->inbox;

	if (size > mctp_pcc_ndev->ndev->mtu)
		return NULL;
	skb = netdev_alloc_skb(mctp_pcc_ndev->ndev, size);
	if (!skb)
		return NULL;
	skb_put(skb, size);
	skb->protocol = htons(ETH_P_MCTP);

	spin_lock(&mctp_pcc_ndev->lock);
	skb_queue_head(&box->packets, skb);
	spin_unlock(&mctp_pcc_ndev->lock);

	return skb->data;
}

static void mctp_pcc_client_rx_callback(struct mbox_client *c, void *buffer)
{
	struct mctp_pcc_ndev *mctp_pcc_ndev;
	struct pcc_header pcc_header;
	struct sk_buff *skb = NULL;
	struct mctp_skb_cb *cb;

	mctp_pcc_ndev = container_of(c, struct mctp_pcc_ndev, inbox.client);
	if (!buffer) {
		dev_dstats_rx_dropped(mctp_pcc_ndev->ndev);
		return;
	}

	spin_lock(&mctp_pcc_ndev->lock);
	skb_queue_walk(&mctp_pcc_ndev->inbox.packets, skb) {
		if (skb->data != buffer)
			continue;
		skb_unlink(skb, &mctp_pcc_ndev->inbox.packets);
		break;
	}
	spin_unlock(&mctp_pcc_ndev->lock);

	if (skb) {
		dev_dstats_rx_add(mctp_pcc_ndev->ndev, skb->len);
		skb_reset_mac_header(skb);
		skb_pull(skb, sizeof(pcc_header));
		skb_reset_network_header(skb);
		cb = __mctp_cb(skb);
		cb->halen = 0;
		netif_rx(skb);
	}
}

static void mctp_pcc_tx_done(struct mbox_client *c, void *mssg, int r)
{
	struct mctp_pcc_ndev *mctp_pcc_ndev;
	struct mctp_pcc_mailbox *box;
	struct sk_buff *skb = NULL;

	mctp_pcc_ndev = container_of(c, struct mctp_pcc_ndev, outbox.client);
	box = container_of(c, struct mctp_pcc_mailbox, client);
	spin_lock(&mctp_pcc_ndev->lock);
	skb_queue_walk(&box->packets, skb) {
		if (skb->data == mssg) {
			skb_unlink(skb, &box->packets);
			break;
		}
	}
	spin_unlock(&mctp_pcc_ndev->lock);

	if (skb)
		dev_consume_skb_any(skb);
}

static netdev_tx_t mctp_pcc_tx(struct sk_buff *skb, struct net_device *ndev)
{
	struct mctp_pcc_ndev *mpnd = netdev_priv(ndev);
	struct pcc_header *pcc_header;
	int len = skb->len;
	int rc;

	rc = skb_cow_head(skb, sizeof(*pcc_header));
	if (rc) {
		dev_dstats_tx_dropped(ndev);
		kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	pcc_header = skb_push(skb, sizeof(*pcc_header));
	pcc_header->signature = PCC_SIGNATURE | mpnd->outbox.index;
	pcc_header->flags = PCC_CMD_COMPLETION_NOTIFY;
	memcpy(&pcc_header->command, MCTP_SIGNATURE, MCTP_SIGNATURE_LENGTH);
	pcc_header->length = len + MCTP_SIGNATURE_LENGTH;

	spin_lock(&mpnd->lock);
	skb_queue_head(&mpnd->outbox.packets, skb);
	spin_unlock(&mpnd->lock);

	rc = mbox_send_message(mpnd->outbox.chan->mchan, skb->data);

	if (rc < 0) {
		skb_unlink(skb, &mpnd->outbox.packets);
		return NETDEV_TX_BUSY;
	}

	dev_dstats_tx_add(ndev, len);
	return NETDEV_TX_OK;
}

static void drain_packets(struct sk_buff_head *list)
{
	struct sk_buff *skb;

	while (!skb_queue_empty(list)) {
		skb = skb_dequeue(list);
		dev_consume_skb_any(skb);
	}
}

static int mctp_pcc_ndo_open(struct net_device *ndev)
{
	struct mctp_pcc_ndev *mctp_pcc_ndev =
	    netdev_priv(ndev);
	struct mctp_pcc_mailbox *outbox =
	    &mctp_pcc_ndev->outbox;
	struct mctp_pcc_mailbox *inbox =
	    &mctp_pcc_ndev->inbox;
	int mctp_pcc_mtu;

	outbox->chan = pcc_mbox_request_channel(&outbox->client, outbox->index);
	if (IS_ERR(outbox->chan))
		return PTR_ERR(outbox->chan);

	inbox->chan = pcc_mbox_request_channel(&inbox->client, inbox->index);
	if (IS_ERR(inbox->chan)) {
		pcc_mbox_free_channel(outbox->chan);
		return PTR_ERR(inbox->chan);
	}

	mctp_pcc_ndev->inbox.chan->rx_alloc = mctp_pcc_rx_alloc;
	mctp_pcc_ndev->outbox.chan->manage_writes = true;

	/* There is no clean way to pass the MTU to the callback function
	 * used for registration, so set the values ahead of time.
	 */
	mctp_pcc_mtu = mctp_pcc_ndev->outbox.chan->shmem_size -
		sizeof(struct pcc_header);
	ndev->mtu = MCTP_MIN_MTU;
	ndev->max_mtu = mctp_pcc_mtu;
	ndev->min_mtu = MCTP_MIN_MTU;

	return 0;
}

static int mctp_pcc_ndo_stop(struct net_device *ndev)
{
	struct mctp_pcc_ndev *mctp_pcc_ndev =
	    netdev_priv(ndev);
	struct mctp_pcc_mailbox *outbox =
	    &mctp_pcc_ndev->outbox;
	struct mctp_pcc_mailbox *inbox =
	    &mctp_pcc_ndev->inbox;

	pcc_mbox_free_channel(outbox->chan);
	pcc_mbox_free_channel(inbox->chan);

	spin_lock(&mctp_pcc_ndev->lock);
	drain_packets(&mctp_pcc_ndev->outbox.packets);
	drain_packets(&mctp_pcc_ndev->inbox.packets);
	spin_unlock(&mctp_pcc_ndev->lock);
	return 0;
}

static const struct net_device_ops mctp_pcc_netdev_ops = {
	.ndo_open = mctp_pcc_ndo_open,
	.ndo_stop = mctp_pcc_ndo_stop,
	.ndo_start_xmit = mctp_pcc_tx,

};

static void mctp_pcc_setup(struct net_device *ndev)
{
	ndev->type = ARPHRD_MCTP;
	ndev->hard_header_len = 0;
	ndev->tx_queue_len = 0;
	ndev->flags = IFF_NOARP;
	ndev->netdev_ops = &mctp_pcc_netdev_ops;
	ndev->needs_free_netdev = true;
	ndev->pcpu_stat_type = NETDEV_PCPU_STAT_DSTATS;
}

struct mctp_pcc_lookup_context {
	int index;
	u32 inbox_index;
	u32 outbox_index;
};

static acpi_status lookup_pcct_indices(struct acpi_resource *ares,
				       void *context)
{
	struct mctp_pcc_lookup_context *luc = context;
	struct acpi_resource_address32 *addr;

	if (ares->type != PCC_DWORD_TYPE)
		return AE_OK;

	addr = ACPI_CAST_PTR(struct acpi_resource_address32, &ares->data);
	switch (luc->index) {
	case 0:
		luc->outbox_index = addr[0].address.minimum;
		break;
	case 1:
		luc->inbox_index = addr[0].address.minimum;
		break;
	}
	luc->index++;
	return AE_OK;
}

static void mctp_cleanup_netdev(void *data)
{
	struct net_device *ndev = data;

	mctp_unregister_netdev(ndev);
}

static int mctp_pcc_initialize_mailbox(struct device *dev,
				       struct mctp_pcc_mailbox *box, u32 index)
{
	box->index = index;
	skb_queue_head_init(&box->packets);
	box->client.dev = dev;
	return 0;
}

static int mctp_pcc_driver_add(struct acpi_device *acpi_dev)
{
	struct mctp_pcc_lookup_context context = {0};
	struct mctp_pcc_ndev *mctp_pcc_ndev;
	struct device *dev = &acpi_dev->dev;
	struct net_device *ndev;
	acpi_handle dev_handle;
	acpi_status status;
	char name[32];
	int rc;

	dev_dbg(dev, "Adding mctp_pcc device for HID %s\n",
		acpi_device_hid(acpi_dev));
	dev_handle = acpi_device_handle(acpi_dev);
	status = acpi_walk_resources(dev_handle, "_CRS", lookup_pcct_indices,
				     &context);
	if (!ACPI_SUCCESS(status)) {
		dev_err(dev, "FAILURE to lookup PCC indexes from CRS\n");
		return -EINVAL;
	}

	snprintf(name, sizeof(name), "mctpipcc%d", context.inbox_index);
	ndev = alloc_netdev(sizeof(*mctp_pcc_ndev), name, NET_NAME_PREDICTABLE,
			    mctp_pcc_setup);
	if (!ndev)
		return -ENOMEM;

	mctp_pcc_ndev = netdev_priv(ndev);
	spin_lock_init(&mctp_pcc_ndev->lock);

	/* inbox initialization */
	rc = mctp_pcc_initialize_mailbox(dev, &mctp_pcc_ndev->inbox,
					 context.inbox_index);
	if (rc)
		goto free_netdev;

	mctp_pcc_ndev->inbox.client.rx_callback = mctp_pcc_client_rx_callback;

	/* outbox initialization */
	rc = mctp_pcc_initialize_mailbox(dev, &mctp_pcc_ndev->outbox,
					 context.outbox_index);
	if (rc)
		goto free_netdev;

	mctp_pcc_ndev->outbox.client.tx_done = mctp_pcc_tx_done;
	mctp_pcc_ndev->acpi_device = acpi_dev;
	mctp_pcc_ndev->ndev = ndev;
	acpi_dev->driver_data = mctp_pcc_ndev;

	/* ndev needs to be freed before the iomemory (mapped above) gets
	 * unmapped,  devm resources get freed in reverse to the order they
	 * are added.
	 */
	rc = mctp_register_netdev(ndev, NULL, MCTP_PHYS_BINDING_PCC);
	if (rc)
		goto free_netdev;

	return devm_add_action_or_reset(dev, mctp_cleanup_netdev, ndev);
free_netdev:
	free_netdev(ndev);
	return rc;
}

static const struct acpi_device_id mctp_pcc_device_ids[] = {
	{ "DMT0001" },
	{}
};

static struct acpi_driver mctp_pcc_driver = {
	.name = "mctp_pcc",
	.class = "Unknown",
	.ids = mctp_pcc_device_ids,
	.ops = {
		.add = mctp_pcc_driver_add,
	},
};

module_acpi_driver(mctp_pcc_driver);

MODULE_DEVICE_TABLE(acpi, mctp_pcc_device_ids);

MODULE_DESCRIPTION("MCTP PCC ACPI device");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Adam Young <admiyo@os.amperecomputing.com>");
