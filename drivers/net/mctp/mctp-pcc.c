// SPDX-License-Identifier: GPL-2.0
/*
 * mctp-pcc.c - Driver for MCTP over PCC.
 * Copyright (c) 2024, Ampere Computing LLC
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

#define SPDM_VERSION_OFFSET	1
#define SPDM_REQ_RESP_OFFSET	2
#define MCTP_PAYLOAD_LENGTH	256
#define MCTP_CMD_LENGTH		4
#define MCTP_PCC_VERSION	0x1 /* DSP0253 defines a single version: 1 */
#define MCTP_SIGNATURE		"MCTP"
#define SIGNATURE_LENGTH	4
#define MCTP_HEADER_LENGTH	12
#define MCTP_MIN_MTU		68
#define PCC_MAGIC		0x50434300
#define PCC_HEADER_FLAG_REQ_INT	0x1
#define PCC_HEADER_FLAGS	PCC_HEADER_FLAG_REQ_INT
#define PCC_DWORD_TYPE		0x0c
#define PCC_ACK_FLAG_MASK	0x1

struct mctp_pcc_hdr {
	u32 signature;
	u32 flags;
	u32 length;
	char mctp_signature[4];
};

struct mctp_pcc_hw_addr {
	u32 inbox_index;
	u32 outbox_index;
};

/* The netdev structure. One of these per PCC adapter. */
struct mctp_pcc_ndev {
	struct list_head next;
	/* spinlock to serialize access to pcc buffer and registers*/
	spinlock_t lock;
	struct mctp_dev mdev;
	struct acpi_device *acpi_device;
	struct pcc_mbox_chan *in_chan;
	struct pcc_mbox_chan *out_chan;
	struct mbox_client outbox_client;
	struct mbox_client inbox_client;
	void __iomem *pcc_comm_inbox_addr;
	void __iomem *pcc_comm_outbox_addr;
	struct mctp_pcc_hw_addr hw_addr;
};

static struct list_head mctp_pcc_ndevs = LIST_HEAD_INIT(mctp_pcc_ndevs);

static void mctp_pcc_client_rx_callback(struct mbox_client *c, void *buffer)
{
	struct mctp_pcc_ndev *mctp_pcc_dev;
	struct mctp_pcc_hdr mctp_pcc_hdr;
	struct mctp_skb_cb *cb;
	struct sk_buff *skb;
	void *skb_buf;
	u32 data_len;
	u32 flags;

	mctp_pcc_dev = container_of(c, struct mctp_pcc_ndev, inbox_client);
	memcpy_fromio(&mctp_pcc_hdr, mctp_pcc_dev->pcc_comm_inbox_addr,
		      sizeof(struct mctp_pcc_hdr));
	data_len = mctp_pcc_hdr.length + MCTP_HEADER_LENGTH;

	if (data_len > mctp_pcc_dev->mdev.dev->max_mtu) {
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
	memcpy_fromio(skb_buf, mctp_pcc_dev->pcc_comm_inbox_addr, data_len);
	skb_reset_mac_header(skb);
	skb_pull(skb, sizeof(struct mctp_pcc_hdr));
	skb_reset_network_header(skb);
	cb = __mctp_cb(skb);
	cb->halen = 0;
	netif_rx(skb);

	flags = mctp_pcc_hdr.flags;
	mctp_pcc_dev->in_chan->ack_rx = (flags & PCC_ACK_FLAG_MASK) > 0;
}

static netdev_tx_t mctp_pcc_tx(struct sk_buff *skb, struct net_device *ndev)
{
	struct mctp_pcc_hdr pcc_header;
	struct mctp_pcc_ndev *mpnd;
	void __iomem *buffer;
	unsigned long flags;

	ndev->stats.tx_bytes += skb->len;
	ndev->stats.tx_packets++;
	mpnd = netdev_priv(ndev);

	spin_lock_irqsave(&mpnd->lock, flags);
	buffer = mpnd->pcc_comm_outbox_addr;
	pcc_header.signature = PCC_MAGIC | mpnd->hw_addr.outbox_index;
	pcc_header.flags = PCC_HEADER_FLAGS;
	memcpy(pcc_header.mctp_signature, MCTP_SIGNATURE, SIGNATURE_LENGTH);
	pcc_header.length = skb->len + SIGNATURE_LENGTH;
	memcpy_toio(buffer, &pcc_header, sizeof(struct mctp_pcc_hdr));
	memcpy_toio(buffer + sizeof(struct mctp_pcc_hdr), skb->data, skb->len);
	mpnd->out_chan->mchan->mbox->ops->send_data(mpnd->out_chan->mchan,
						    NULL);
	spin_unlock_irqrestore(&mpnd->lock, flags);

	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;
}

static void
mctp_pcc_net_stats(struct net_device *net_dev,
		   struct rtnl_link_stats64 *stats)
{
	struct mctp_pcc_ndev *mpnd;

	mpnd = (struct mctp_pcc_ndev *)netdev_priv(net_dev);
	stats->rx_errors = 0;
	stats->rx_packets = mpnd->mdev.dev->stats.rx_packets;
	stats->tx_packets = mpnd->mdev.dev->stats.tx_packets;
	stats->rx_dropped = 0;
	stats->tx_bytes = mpnd->mdev.dev->stats.tx_bytes;
	stats->rx_bytes = mpnd->mdev.dev->stats.rx_bytes;
}

static const struct net_device_ops mctp_pcc_netdev_ops = {
	.ndo_start_xmit = mctp_pcc_tx,
	.ndo_get_stats64 = mctp_pcc_net_stats,
};

static void  mctp_pcc_setup(struct net_device *ndev)
{
	ndev->type = ARPHRD_MCTP;
	ndev->hard_header_len = 0;
	ndev->addr_len = 0;
	ndev->tx_queue_len = 0;
	ndev->flags = IFF_NOARP;
	ndev->netdev_ops = &mctp_pcc_netdev_ops;
	ndev->needs_free_netdev = true;
}

struct lookup_context {
	int index;
	u32 inbox_index;
	u32 outbox_index;
};

static acpi_status lookup_pcct_indices(struct acpi_resource *ares,
				       void *context)
{
	struct acpi_resource_address32 *addr;
	struct lookup_context *luc = context;

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

static int mctp_pcc_driver_add(struct acpi_device *acpi_dev)
{
	struct lookup_context context = {0, 0, 0};
	struct mctp_pcc_ndev *mctp_pcc_dev;
	struct net_device *ndev;
	acpi_handle dev_handle;
	acpi_status status;
	struct device *dev;
	int mctp_pcc_mtu;
	int outbox_index;
	int inbox_index;
	char name[32];
	int rc;

	dev_dbg(&acpi_dev->dev, "Adding mctp_pcc device for HID  %s\n",
		acpi_device_hid(acpi_dev));
	dev_handle = acpi_device_handle(acpi_dev);
	status = acpi_walk_resources(dev_handle, "_CRS", lookup_pcct_indices,
				     &context);
	if (!ACPI_SUCCESS(status)) {
		dev_err(&acpi_dev->dev, "FAILURE to lookup PCC indexes from CRS");
		return -EINVAL;
	}
	inbox_index = context.inbox_index;
	outbox_index = context.outbox_index;
	dev = &acpi_dev->dev;

	snprintf(name, sizeof(name), "mctpipcc%d", inbox_index);
	ndev = alloc_netdev(sizeof(struct mctp_pcc_ndev), name, NET_NAME_ENUM,
			    mctp_pcc_setup);
	if (!ndev)
		return -ENOMEM;
	mctp_pcc_dev = (struct mctp_pcc_ndev *)netdev_priv(ndev);
	INIT_LIST_HEAD(&mctp_pcc_dev->next);
	spin_lock_init(&mctp_pcc_dev->lock);

	mctp_pcc_dev->hw_addr.inbox_index = inbox_index;
	mctp_pcc_dev->hw_addr.outbox_index = outbox_index;
	mctp_pcc_dev->inbox_client.rx_callback = mctp_pcc_client_rx_callback;
	mctp_pcc_dev->out_chan =
		pcc_mbox_request_channel(&mctp_pcc_dev->outbox_client,
					 outbox_index);
	if (IS_ERR(mctp_pcc_dev->out_chan)) {
		rc = PTR_ERR(mctp_pcc_dev->out_chan);
		goto free_netdev;
	}
	mctp_pcc_dev->in_chan =
		pcc_mbox_request_channel(&mctp_pcc_dev->inbox_client,
					 inbox_index);
	if (IS_ERR(mctp_pcc_dev->in_chan)) {
		rc = PTR_ERR(mctp_pcc_dev->in_chan);
		goto cleanup_out_channel;
	}
	mctp_pcc_dev->pcc_comm_inbox_addr =
		devm_ioremap(dev, mctp_pcc_dev->in_chan->shmem_base_addr,
			     mctp_pcc_dev->in_chan->shmem_size);
	if (!mctp_pcc_dev->pcc_comm_inbox_addr) {
		rc = -EINVAL;
		goto cleanup_in_channel;
	}
	mctp_pcc_dev->pcc_comm_outbox_addr =
		devm_ioremap(dev, mctp_pcc_dev->out_chan->shmem_base_addr,
			     mctp_pcc_dev->out_chan->shmem_size);
	if (!mctp_pcc_dev->pcc_comm_outbox_addr) {
		rc = -EINVAL;
		goto cleanup_in_channel;
	}
	mctp_pcc_dev->acpi_device = acpi_dev;
	mctp_pcc_dev->inbox_client.dev = dev;
	mctp_pcc_dev->outbox_client.dev = dev;
	mctp_pcc_dev->mdev.dev = ndev;
	acpi_dev->driver_data = mctp_pcc_dev;

	/* There is no clean way to pass the MTU
	 * to the callback function used for registration,
	 * so set the values ahead of time.
	 */
	mctp_pcc_mtu = mctp_pcc_dev->out_chan->shmem_size -
		sizeof(struct mctp_pcc_hdr);
	ndev->mtu = MCTP_MIN_MTU;
	ndev->max_mtu = mctp_pcc_mtu;
	ndev->min_mtu = MCTP_MIN_MTU;

	rc = register_netdev(ndev);
	if (rc)
		goto cleanup_in_channel;
	list_add_tail(&mctp_pcc_dev->next, &mctp_pcc_ndevs);
	return 0;

cleanup_in_channel:
	pcc_mbox_free_channel(mctp_pcc_dev->in_chan);
cleanup_out_channel:
	pcc_mbox_free_channel(mctp_pcc_dev->out_chan);
free_netdev:
	unregister_netdev(ndev);
	free_netdev(ndev);
	return rc;
}

static void mctp_pcc_driver_remove(struct acpi_device *adev)
{
	struct list_head *ptr;
	struct list_head *tmp;

	list_for_each_safe(ptr, tmp, &mctp_pcc_ndevs) {
		struct net_device *ndev;
		struct mctp_pcc_ndev *mctp_pcc_dev;

		mctp_pcc_dev = list_entry(ptr, struct mctp_pcc_ndev, next);
		if (mctp_pcc_dev->acpi_device != adev)
			continue;
		pcc_mbox_free_channel(mctp_pcc_dev->out_chan);
		pcc_mbox_free_channel(mctp_pcc_dev->in_chan);
		ndev = mctp_pcc_dev->mdev.dev;
		if (ndev)
			mctp_unregister_netdev(ndev);
		list_del(ptr);
			break;
	}
}

static const struct acpi_device_id mctp_pcc_device_ids[] = {
	{ "DMT0001", 0},
	{ "", 0},
};

static struct acpi_driver mctp_pcc_driver = {
	.name = "mctp_pcc",
	.class = "Unknown",
	.ids = mctp_pcc_device_ids,
	.ops = {
		.add = mctp_pcc_driver_add,
		.remove = mctp_pcc_driver_remove,
	},
	.owner = THIS_MODULE,
};

module_acpi_driver(mctp_pcc_driver);

MODULE_DEVICE_TABLE(acpi, mctp_pcc_device_ids);

MODULE_DESCRIPTION("MCTP PCC device");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Adam Young <admiyo@os.amperecomputing.com>");
