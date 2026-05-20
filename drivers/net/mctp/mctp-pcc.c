// SPDX-License-Identifier: GPL-2.0
/*
 * mctp-pcc.c - Driver for MCTP over PCC.
 * Copyright (c) 2024-2026, Ampere Computing LLC
 *
 */

/* Implementation of MCTP over PCC DMTF Specification DSP0256
 * https://www.dmtf.org/sites/default/files/standards/documents/DSP0292_1.0.0WIP50.pdf
 */

#include <linux/acpi.h>
#include <linux/hrtimer.h>
#include <linux/if_arp.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>
#include <linux/string.h>

#include <acpi/acpi_bus.h>
#include <acpi/acpi_drivers.h>
#include <acpi/acrestyp.h>
#include <acpi/actbl.h>
#include <acpi/pcc.h>
#include <net/mctp.h>
#include <net/mctpdevice.h>

#define MCTP_SIGNATURE          "MCTP"
#define MCTP_SIGNATURE_LENGTH   (sizeof(MCTP_SIGNATURE) - 1)
#define MCTP_MIN_MTU            68
#define PCC_HEADER_SIZE         sizeof(struct acpi_pcct_ext_pcc_shared_memory)
#define MCTP_PCC_MIN_SIZE       (PCC_HEADER_SIZE + MCTP_MIN_MTU)
#define PCC_EXTRA_LEN           (PCC_HEADER_SIZE - sizeof(pcc_header.command))
struct mctp_pcc_mailbox {
	u32 index;
	struct pcc_mbox_chan *chan;
	struct mbox_client client;
};

/* The netdev structure. One of these per PCC adapter. */
struct mctp_pcc_ndev {
	struct net_device *ndev;
	struct acpi_device *acpi_device;
	struct mctp_pcc_mailbox inbox;
	struct mctp_pcc_mailbox outbox;
};

static void mctp_pcc_client_rx_callback(struct mbox_client *cl, void *mssg)
{
	struct acpi_pcct_ext_pcc_shared_memory pcc_header;
	struct mctp_pcc_ndev *mctp_pcc_ndev;
	struct mctp_pcc_mailbox *inbox;
	struct mctp_skb_cb *cb;
	struct sk_buff *skb;
	u32 header_length;
	int size;

	mctp_pcc_ndev = container_of(cl, struct mctp_pcc_ndev, inbox.client);
	inbox = &mctp_pcc_ndev->inbox;
	memcpy_fromio(&pcc_header, inbox->chan->shmem, sizeof(pcc_header));

	// The message must at least have the PCC command indicating it is an MCTP
	// message followed by the MCTP header, or we have a malformed message.
	// This may be run on big endian system, but the data in the buffer is
	// explicitly little endian.
	header_length = le32_to_cpu(pcc_header.length);

	if (header_length < sizeof(pcc_header.command) + sizeof(struct mctp_hdr))
		goto error;

	// If the reported size is larger than the shared memory minus headers,
	// something is wrong and treat the buffer as corrupted data.
	if (header_length > inbox->chan->shmem_size - PCC_EXTRA_LEN)
		goto error;

	if (memcmp(&pcc_header.command, MCTP_SIGNATURE, MCTP_SIGNATURE_LENGTH) != 0)
		goto error;

	size = header_length + PCC_EXTRA_LEN;
	skb = netdev_alloc_skb(mctp_pcc_ndev->ndev, size);
	if (!skb)
		goto error;

	skb_put(skb, size);
	skb->protocol = htons(ETH_P_MCTP);
	memcpy_fromio(skb->data, inbox->chan->shmem, size);
	dev_dstats_rx_add(mctp_pcc_ndev->ndev, size);
	skb_pull(skb, sizeof(pcc_header));
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	cb = __mctp_cb(skb);
	cb->halen = 0;
	netif_rx(skb);
	return;

error:
	dev_dstats_rx_dropped(mctp_pcc_ndev->ndev);
}

static netdev_tx_t mctp_pcc_tx(struct sk_buff *skb, struct net_device *ndev)
{
	struct acpi_pcct_ext_pcc_shared_memory *pcc_header;
	struct mctp_pcc_ndev *mpnd = netdev_priv(ndev);
	int len = skb->len;

	if (skb_cow_head(skb, sizeof(*pcc_header)))
		goto error;

	pcc_header = skb_push(skb, sizeof(*pcc_header));
	pcc_header->signature = PCC_SIGNATURE | mpnd->outbox.index;
	pcc_header->flags = PCC_CMD_COMPLETION_NOTIFY;
	memcpy(&pcc_header->command, MCTP_SIGNATURE, MCTP_SIGNATURE_LENGTH);
	pcc_header->length = len + MCTP_SIGNATURE_LENGTH;

	if (skb->len > mpnd->outbox.chan->shmem_size)
		goto error;

	if (mbox_send_message(mpnd->outbox.chan->mchan, skb) < 0) {
		netif_stop_queue(ndev);
		/*
		 * There is a possibility that the mailbox was cleared on
		 * another thread between the failed send attempt and
		 * stopping the queue.  If that is the case, and we don't restart
		 * the queue, it will remain permanently stopped.  To test,
		 * try submitting the message again. If successful, restart the
		 * queue.
		 */
		if (mbox_send_message(mpnd->outbox.chan->mchan, skb) >= 0) {
			netif_wake_queue(ndev);
		} else {
			// Remove the header in case it gets sent again
			skb_pull(skb, sizeof(*pcc_header));
			return NETDEV_TX_BUSY;
		}
	}
	return NETDEV_TX_OK;

error:
	dev_dstats_tx_dropped(ndev);
	kfree_skb(skb);
	return NETDEV_TX_OK;
}

static void mctp_pcc_tx_prepare(struct mbox_client *cl, void *mssg)
{
	struct mctp_pcc_ndev *mctp_pcc_ndev;
	struct mctp_pcc_mailbox *outbox;
	struct sk_buff *skb = mssg;

	mctp_pcc_ndev = container_of(cl, struct mctp_pcc_ndev, outbox.client);
	outbox = &mctp_pcc_ndev->outbox;

	/* The PCC Mailbox typically does not make use of the mssg pointer
	 * The mctp-over pcc driver is the only client that uses it.
	 * This value should always be non-null; it is possible
	 * that a change in the Mailbox level will break that assumption.
	 */
	if (!skb) {
		netdev_warn_once(mctp_pcc_ndev->ndev,
				 "%s called with null message.\n", __func__);
		return;
	}
	memcpy_toio(outbox->chan->shmem, skb->data, skb->len);
}

static void mctp_pcc_tx_done(struct mbox_client *c, void *mssg, int rc)
{
	struct mctp_pcc_ndev *mctp_pcc_ndev;
	struct pcpu_dstats *dstats;
	struct sk_buff *skb = mssg;
	unsigned long flags;

	/*
	 * If there is a packet in flight during driver cleanup
	 * It may have been freed already.
	 */
	if (!mssg)
		return;
	mctp_pcc_ndev = container_of(c, struct mctp_pcc_ndev, outbox.client);

	/* Use an IRQ safe update as this is called from HARD IRQ instead of
	 * dev_dstats_tx_add(mctp_pcc_ndev->ndev, skb->len);
	 */
	dstats = this_cpu_ptr(mctp_pcc_ndev->ndev->dstats);
	flags = u64_stats_update_begin_irqsave(&dstats->syncp);

	if (rc) {
		u64_stats_inc(&dstats->tx_drops);
	} else {
		u64_stats_inc(&dstats->tx_packets);
		u64_stats_add(&dstats->tx_bytes, skb->len);
	}
	u64_stats_update_end_irqrestore(&dstats->syncp, flags);
	dev_consume_skb_any(skb);
	netif_wake_queue(mctp_pcc_ndev->ndev);
}

static int mctp_pcc_open(struct net_device *ndev)
{
	struct mctp_pcc_ndev *mctp_pcc_ndev = netdev_priv(ndev);
	struct mctp_pcc_mailbox *outbox, *inbox;

	outbox = &mctp_pcc_ndev->outbox;
	inbox = &mctp_pcc_ndev->inbox;

	outbox->chan = pcc_mbox_request_channel(&outbox->client, outbox->index);
	if (IS_ERR(outbox->chan))
		return PTR_ERR(outbox->chan);
	if (outbox->chan->shmem_size < MCTP_PCC_MIN_SIZE) {
		pcc_mbox_free_channel(outbox->chan);
		return -EINVAL;
	}

	inbox->client.rx_callback = mctp_pcc_client_rx_callback;
	inbox->chan = pcc_mbox_request_channel(&inbox->client, inbox->index);
	if (IS_ERR(inbox->chan)) {
		pcc_mbox_free_channel(outbox->chan);
		return PTR_ERR(inbox->chan);
	}
	if (inbox->chan->shmem_size < MCTP_PCC_MIN_SIZE) {
		pcc_mbox_free_channel(outbox->chan);
		pcc_mbox_free_channel(inbox->chan);
		return -EINVAL;
	}
	return 0;
}

static int mctp_pcc_stop(struct net_device *ndev)
{
	struct mctp_pcc_ndev *mctp_pcc_ndev;
	unsigned int count, idx;
	struct mbox_chan *chan;
	struct sk_buff *skb;

	mctp_pcc_ndev = netdev_priv(ndev);
	chan = mctp_pcc_ndev->outbox.chan->mchan;
	pcc_mbox_free_channel(mctp_pcc_ndev->inbox.chan);
	scoped_guard(spinlock_irqsave, &chan->lock) {
		skb = chan->active_req;
		chan->active_req = NULL;
		if (skb) {
			dev_dstats_tx_dropped(ndev);
			dev_consume_skb_any(skb);
		}
		while (chan->msg_count > 0) {
			count = chan->msg_count;
			idx = chan->msg_free;
			if (idx >= count)
				idx -= count;
			else
				idx += MBOX_TX_QUEUE_LEN - count;
			skb = chan->msg_data[idx];
			dev_dstats_tx_dropped(ndev);
			dev_consume_skb_any(skb);
			chan->msg_count--;
		}
	}
	pcc_mbox_free_channel(mctp_pcc_ndev->outbox.chan);
	return 0;
}

static const struct net_device_ops mctp_pcc_netdev_ops = {
	.ndo_open = mctp_pcc_open,
	.ndo_stop = mctp_pcc_stop,
	.ndo_start_xmit = mctp_pcc_tx,
};

static void mctp_pcc_setup(struct net_device *ndev)
{
	ndev->type = ARPHRD_MCTP;
	ndev->hard_header_len = sizeof(struct acpi_pcct_ext_pcc_shared_memory);
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

	if (ares->type != ACPI_RESOURCE_TYPE_ADDRESS32)
		return AE_OK;

	addr = ACPI_CAST_PTR(struct acpi_resource_address32, &ares->data);
	switch (luc->index) {
	case 0:
		luc->outbox_index = addr[0].address.minimum;
		break;
	case 1:
		luc->inbox_index = addr[0].address.minimum;
		break;
	default:
		return AE_ERROR;
	}
	luc->index++;
	return AE_OK;
}

static void mctp_cleanup_netdev(void *data)
{
	struct net_device *ndev = data;

	mctp_unregister_netdev(ndev);
}

static int initialize_mtu(struct net_device *ndev)
{
	struct mctp_pcc_ndev *mctp_pcc_ndev;
	struct mctp_pcc_mailbox *outbox;
	struct pcc_mbox_chan *pchan;
	int mctp_pcc_max_mtu;

	mctp_pcc_ndev = netdev_priv(ndev);
	outbox = &mctp_pcc_ndev->outbox;
	pchan = pcc_mbox_request_channel(&outbox->client, outbox->index);
	if (IS_ERR(pchan))
		return PTR_ERR(pchan);
	if (pchan->shmem_size < MCTP_MIN_MTU + sizeof(struct acpi_pcct_ext_pcc_shared_memory)) {
		pcc_mbox_free_channel(pchan);
		return -EINVAL;
	}
	mctp_pcc_max_mtu = pchan->shmem_size - sizeof(struct acpi_pcct_ext_pcc_shared_memory);
	pcc_mbox_free_channel(pchan);

	ndev->mtu = MCTP_MIN_MTU;
	ndev->max_mtu = mctp_pcc_max_mtu;
	ndev->min_mtu = MCTP_MIN_MTU;

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
		dev_err(dev, "FAILED to lookup PCC indexes from CRS\n");
		return -EINVAL;
	}

	/*
	 * Ensure we have exactly 2 channels: an outbox and an inbox.
	 */
	if (context.index != 2)
		return -EINVAL;

	snprintf(name, sizeof(name), "mctppcc%d", context.inbox_index);
	ndev = alloc_netdev(sizeof(*mctp_pcc_ndev), name, NET_NAME_PREDICTABLE,
			    mctp_pcc_setup);
	if (!ndev)
		return -ENOMEM;

	mctp_pcc_ndev = netdev_priv(ndev);

	mctp_pcc_ndev->inbox.index = context.inbox_index;
	mctp_pcc_ndev->inbox.client.dev = dev;
	mctp_pcc_ndev->outbox.index = context.outbox_index;
	mctp_pcc_ndev->outbox.client.dev = dev;

	mctp_pcc_ndev->outbox.client.tx_prepare = mctp_pcc_tx_prepare;
	mctp_pcc_ndev->outbox.client.tx_done = mctp_pcc_tx_done;
	mctp_pcc_ndev->acpi_device = acpi_dev;
	mctp_pcc_ndev->ndev = ndev;
	acpi_dev->driver_data = mctp_pcc_ndev;

	rc = initialize_mtu(ndev);
	if (rc)
		goto free_netdev;

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
