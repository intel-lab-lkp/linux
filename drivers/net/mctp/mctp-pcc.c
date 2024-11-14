// SPDX-License-Identifier: GPL-2.0
/*
 * mctp-pcc.c - Driver for MCTP over PCC.
 * Copyright (c) 2024, Ampere Computing LLC
 */

/* Implelmentation of MCTP over PCC DMTF Specification 256
 * https://www.dmtf.org/sites/default/files/standards/documents/DSP0256_2.0.0WIP50.pdf
 */

#include <linux/acpi.h>
#include <linux/if_arp.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>
#include <linux/string.h>

#include <acpi/acpi_bus.h>
#include <acpi/acpi_drivers.h>
#include <acpi/acrestyp.h>
#include <acpi/actbl.h>
#include <net/mctp.h>
#include <net/mctpdevice.h>
#include <acpi/pcc.h>

#define MCTP_PAYLOAD_LENGTH     256
#define MCTP_CMD_LENGTH         4
#define MCTP_PCC_VERSION        0x1 /* DSP0253 defines a single version: 1 */
#define MCTP_SIGNATURE          "MCTP"
#define MCTP_SIGNATURE_LENGTH   (sizeof(MCTP_SIGNATURE) - 1)
#define MCTP_HEADER_LENGTH      12
#define MCTP_MIN_MTU            68
#define PCC_MAGIC               0x50434300
#define PCC_HEADER_FLAG_REQ_INT 0x1
#define PCC_HEADER_FLAGS        PCC_HEADER_FLAG_REQ_INT
#define PCC_DWORD_TYPE          0x0c

struct mctp_pcc_hdr {
	u32 signature;
	u32 flags;
	u32 length;
	char mctp_signature[MCTP_SIGNATURE_LENGTH];
};

struct mctp_pcc_mailbox {
	u32 index;
	struct pcc_mbox_chan *chan;
	struct mbox_client client;
};

struct mctp_pcc_hw_addr {
	__be32 parent_id;
	__be16 inbox_id;
	__be16 outbox_id;
};

/* The netdev structure. One of these per PCC adapter. */
struct mctp_pcc_ndev {
	/* spinlock to serialize access to PCC outbox buffer and registers
	 * Note that what PCC calls registers are memory locations, not CPU
	 * Registers.  They include the fields used to synchronize access
	 * between the OS and remote endpoints.
	 *
	 * Only the Outbox needs a spinlock, to prevent multiple
	 * sent packets triggering multiple attempts to over write
	 * the outbox.  The Inbox buffer is controlled by the remote
	 * service and a spinlock would have no effect.
	 */
	spinlock_t lock;
	struct mctp_dev mdev;
	struct acpi_device *acpi_device;
	struct mctp_pcc_mailbox inbox;
	struct mctp_pcc_mailbox outbox;
};

static void mctp_pcc_client_rx_callback(struct mbox_client *c, void *buffer)
{
	struct mctp_pcc_ndev *mctp_pcc_dev;
	struct mctp_pcc_hdr mctp_pcc_hdr;
	struct mctp_skb_cb *cb;
	struct sk_buff *skb;
	void *skb_buf;
	u32 data_len;

	mctp_pcc_dev = container_of(c, struct mctp_pcc_ndev, inbox.client);
	memcpy_fromio(&mctp_pcc_hdr, mctp_pcc_dev->inbox.chan->shmem,
		      sizeof(struct mctp_pcc_hdr));
	data_len = mctp_pcc_hdr.length + MCTP_HEADER_LENGTH;

	if (data_len > mctp_pcc_dev->mdev.dev->mtu) {
		mctp_pcc_dev->mdev.dev->stats.rx_dropped++;
		return;
	}

	skb = netdev_alloc_skb(mctp_pcc_dev->mdev.dev, data_len);
	if (!skb) {
		mctp_pcc_dev->mdev.dev->stats.rx_dropped++;
		return;
	}
	mctp_pcc_dev->mdev.dev->stats.rx_packets++;
	mctp_pcc_dev->mdev.dev->stats.rx_bytes += data_len;
	skb->protocol = htons(ETH_P_MCTP);
	skb_buf = skb_put(skb, data_len);
	memcpy_fromio(skb_buf, mctp_pcc_dev->inbox.chan->shmem, data_len);

	skb_reset_mac_header(skb);
	skb_pull(skb, sizeof(struct mctp_pcc_hdr));
	skb_reset_network_header(skb);
	cb = __mctp_cb(skb);
	cb->halen = 0;
	netif_rx(skb);
}

static netdev_tx_t mctp_pcc_tx(struct sk_buff *skb, struct net_device *ndev)
{
	struct mctp_pcc_ndev *mpnd = netdev_priv(ndev);
	struct mctp_pcc_hdr *mctp_pcc_header;
	void __iomem *buffer;
	unsigned long flags;
	int len = skb->len;

	ndev->stats.tx_bytes += skb->len;
	ndev->stats.tx_packets++;

	mctp_pcc_header = skb_push(skb, sizeof(struct mctp_pcc_hdr));
	mctp_pcc_header->signature = PCC_MAGIC | mpnd->outbox.index;
	mctp_pcc_header->flags = PCC_HEADER_FLAGS;
	memcpy(mctp_pcc_header->mctp_signature, MCTP_SIGNATURE,
	       MCTP_SIGNATURE_LENGTH);
	mctp_pcc_header->length = len + MCTP_SIGNATURE_LENGTH;
	spin_lock_irqsave(&mpnd->lock, flags);
	buffer = mpnd->outbox.chan->shmem;
	memcpy_toio(buffer, skb->data, skb->len);
	mbox_send_message(mpnd->outbox.chan->mchan, NULL);
	spin_unlock_irqrestore(&mpnd->lock, flags);
	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;
}

static void
mctp_pcc_net_stats(struct net_device *net_dev,
		   struct rtnl_link_stats64 *stats)
{
	stats->rx_errors = 0;
	stats->rx_packets = net_dev->stats.rx_packets;
	stats->tx_packets = net_dev->stats.tx_packets;
	stats->rx_dropped = 0;
	stats->tx_bytes = net_dev->stats.tx_bytes;
	stats->rx_bytes = net_dev->stats.rx_bytes;
}

static const struct net_device_ops mctp_pcc_netdev_ops = {
	.ndo_start_xmit = mctp_pcc_tx,
	.ndo_get_stats64 = mctp_pcc_net_stats,
};

static void  mctp_pcc_setup(struct net_device *ndev)
{
	ndev->type = ARPHRD_MCTP;
	ndev->hard_header_len = 0;
	ndev->tx_queue_len = 0;
	ndev->flags = IFF_NOARP;
	ndev->netdev_ops = &mctp_pcc_netdev_ops;
	ndev->needs_free_netdev = true;
}

struct mctp_pcc_lookup_context {
	int index;
	u32 inbox_index;
	u32 outbox_index;
};

static acpi_status lookup_pcct_indices(struct acpi_resource *ares,
				       void *context)
{
	struct  mctp_pcc_lookup_context *luc = context;
	struct acpi_resource_address32 *addr;

	switch (ares->type) {
	case PCC_DWORD_TYPE:
		break;
	default:
		return AE_OK;
	}

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

static void mctp_cleanup_channel(void *data)
{
	struct pcc_mbox_chan *chan = data;

	pcc_mbox_free_channel(chan);
}

static int mctp_pcc_initialize_mailbox(struct device *dev,
				       struct mctp_pcc_mailbox *box, u32 index)
{
	int ret;

	box->index = index;
	box->chan = pcc_mbox_request_channel(&box->client, index);
	if (IS_ERR(box->chan))
		return PTR_ERR(box->chan);
	devm_add_action_or_reset(dev, mctp_cleanup_channel, box->chan);
	ret = pcc_mbox_ioremap(box->chan->mchan);
	if (ret)
		return -EINVAL;
	return 0;
}

static int mctp_pcc_driver_add(struct acpi_device *acpi_dev)
{
	struct mctp_pcc_lookup_context context = {0, 0, 0};
	struct mctp_pcc_ndev *mctp_pcc_ndev;
	struct device *dev = &acpi_dev->dev;
	struct net_device *ndev;
	acpi_handle dev_handle;
	acpi_status status;
	int mctp_pcc_mtu;
	char name[32];
	int rc;

	dev_dbg(dev, "Adding mctp_pcc device for HID  %s\n",
		acpi_device_hid(acpi_dev));
	dev_handle = acpi_device_handle(acpi_dev);
	status = acpi_walk_resources(dev_handle, "_CRS", lookup_pcct_indices,
				     &context);
	if (!ACPI_SUCCESS(status)) {
		dev_err(dev, "FAILURE to lookup PCC indexes from CRS");
		return -EINVAL;
	}

	//inbox initialization
	snprintf(name, sizeof(name), "mctpipcc%d", context.inbox_index);
	ndev = alloc_netdev(sizeof(struct mctp_pcc_ndev), name, NET_NAME_ENUM,
			    mctp_pcc_setup);
	if (!ndev)
		return -ENOMEM;

	mctp_pcc_ndev = netdev_priv(ndev);
	rc =  devm_add_action_or_reset(dev, mctp_cleanup_netdev, ndev);
	if (rc)
		goto cleanup_netdev;
	spin_lock_init(&mctp_pcc_ndev->lock);

	rc = mctp_pcc_initialize_mailbox(dev, &mctp_pcc_ndev->inbox,
					 context.inbox_index);
	if (rc)
		goto cleanup_netdev;
	mctp_pcc_ndev->inbox.client.rx_callback = mctp_pcc_client_rx_callback;

	//outbox initialization
	rc = mctp_pcc_initialize_mailbox(dev, &mctp_pcc_ndev->outbox,
					 context.outbox_index);
	if (rc)
		goto cleanup_netdev;

	mctp_pcc_ndev->acpi_device = acpi_dev;
	mctp_pcc_ndev->inbox.client.dev = dev;
	mctp_pcc_ndev->outbox.client.dev = dev;
	mctp_pcc_ndev->mdev.dev = ndev;
	acpi_dev->driver_data = mctp_pcc_ndev;

	/* There is no clean way to pass the MTU to the callback function
	 * used for registration, so set the values ahead of time.
	 */
	mctp_pcc_mtu = mctp_pcc_ndev->outbox.chan->shmem_size -
		sizeof(struct mctp_pcc_hdr);
	ndev->mtu = MCTP_MIN_MTU;
	ndev->max_mtu = mctp_pcc_mtu;
	ndev->min_mtu = MCTP_MIN_MTU;

	/* ndev needs to be freed before the iomemory (mapped above) gets
	 * unmapped,  devm resources get freed in reverse to the order they
	 * are added.
	 */
	rc = register_netdev(ndev);
	return rc;
cleanup_netdev:
	free_netdev(ndev);
	return rc;
}

static const struct acpi_device_id mctp_pcc_device_ids[] = {
	{ "DMT0001"},
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
