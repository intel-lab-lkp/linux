// SPDX-License-Identifier: GPL-2.0
/*
 * mctp_pcc.c - Driver for MCTP over PCC.
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
#include <net/pkt_sched.h>

#define SPDM_VERSION_OFFSET 1
#define SPDM_REQ_RESP_OFFSET 2
#define MCTP_PAYLOAD_LENGTH 256
#define MCTP_CMD_LENGTH 4
#define MCTP_PCC_VERSION     0x1 /* DSP0253 defines a single version: 1 */
#define MCTP_SIGNATURE "MCTP"
#define SIGNATURE_LENGTH 4
#define MCTP_HEADER_LENGTH 12
#define MCTP_MIN_MTU 68
#define PCC_MAGIC 0x50434300

struct mctp_pcc_hdr {
	u32 signature;
	u32  flags;
	u32 length;
	char mctp_signature[4];
};

struct mctp_pcc_packet {
	struct mctp_pcc_hdr pcc_header;
	union {
		struct mctp_hdr     mctp_header;
		unsigned char header_data[sizeof(struct mctp_hdr)];
	};
	unsigned char payload[MCTP_PAYLOAD_LENGTH];
};

struct mctp_pcc_hw_addr {
	int inbox_index;
	int outbox_index;
};

/* The netdev structure. One of these per PCC adapter. */
struct mctp_pcc_ndev {
	struct list_head head;
	/* spinlock to serialize access from netdev to pcc buffer*/
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
	void (*cleanup_channel)(struct pcc_mbox_chan *in_chan);
};

static struct list_head mctp_pcc_ndevs;

static struct mctp_pcc_packet *mctp_pcc_extract_data(struct sk_buff *old_skb,
						     void *buffer, int outbox_index)
{
	struct mctp_pcc_packet *mpp;

	mpp = buffer;
	writel(PCC_MAGIC | outbox_index, &mpp->pcc_header.signature);
	writel(0x1, &mpp->pcc_header.flags);
	memcpy_toio(mpp->pcc_header.mctp_signature, MCTP_SIGNATURE, SIGNATURE_LENGTH);
	writel(old_skb->len + SIGNATURE_LENGTH,  &mpp->pcc_header.length);
	memcpy_toio(mpp->header_data,    old_skb->data, old_skb->len);
	return mpp;
}

static void mctp_pcc_client_rx_callback(struct mbox_client *c, void *)
{
	struct sk_buff *skb;
	struct mctp_pcc_packet *mpp;
	struct mctp_skb_cb *cb;
	int data_len;
	unsigned long buf_ptr_val;
	struct mctp_pcc_ndev *mctp_pcc_dev = container_of(c, struct mctp_pcc_ndev, inbox_client);
	void *skb_buf;
	u32 flags;

	mpp = (struct mctp_pcc_packet *)mctp_pcc_dev->pcc_comm_inbox_addr;
	buf_ptr_val = (unsigned long)mpp;
	data_len = readl(&mpp->pcc_header.length) + MCTP_HEADER_LENGTH;
	skb = netdev_alloc_skb(mctp_pcc_dev->mdev.dev, data_len);
	if (!skb) {
		mctp_pcc_dev->mdev.dev->stats.rx_dropped++;
		return;
	}
	skb->protocol = htons(ETH_P_MCTP);
	skb_buf = skb_put(skb, data_len);
	memcpy_fromio(skb_buf, mpp, data_len);
	skb_reset_mac_header(skb);
	skb_pull(skb, sizeof(struct mctp_pcc_hdr));
	skb_reset_network_header(skb);
	cb = __mctp_cb(skb);
	cb->halen = 0;
	skb->dev =  mctp_pcc_dev->mdev.dev;
	netif_rx(skb);

	flags = readl(&mpp->pcc_header.flags);
	mctp_pcc_dev->in_chan->ack_rx = (flags & 1) > 0;
}

static netdev_tx_t mctp_pcc_tx(struct sk_buff *skb, struct net_device *ndev)
{
	unsigned char *buffer;
	struct mctp_pcc_ndev *mpnd;
	struct mctp_pcc_packet  *mpp;
	unsigned long flags;
	int rc;

	netif_stop_queue(ndev);
	ndev->stats.tx_bytes += skb->len;
	mpnd = (struct mctp_pcc_ndev *)netdev_priv(ndev);
	spin_lock_irqsave(&mpnd->lock, flags);
	buffer =  mpnd->pcc_comm_outbox_addr;
	mpp = mctp_pcc_extract_data(skb, mpnd->pcc_comm_outbox_addr, mpnd->hw_addr.outbox_index);
	rc = mpnd->out_chan->mchan->mbox->ops->send_data(mpnd->out_chan->mchan, mpp);
	spin_unlock_irqrestore(&mpnd->lock, flags);

	dev_consume_skb_any(skb);
	netif_start_queue(ndev);
	if (!rc)
		return NETDEV_TX_OK;
	return NETDEV_TX_BUSY;
}

static const struct net_device_ops mctp_pcc_netdev_ops = {
	.ndo_start_xmit = mctp_pcc_tx,
	.ndo_uninit = NULL
};

static void  mctp_pcc_setup(struct net_device *ndev)
{
	ndev->type = ARPHRD_MCTP;
	ndev->hard_header_len = 0;
	ndev->addr_len = sizeof(struct mctp_pcc_hw_addr);
	ndev->tx_queue_len = DEFAULT_TX_QUEUE_LEN;
	ndev->flags = IFF_NOARP;
	ndev->netdev_ops = &mctp_pcc_netdev_ops;
	ndev->needs_free_netdev = true;
}

static int create_mctp_pcc_netdev(struct acpi_device *acpi_dev,
				  struct device *dev, int inbox_index,
				  int outbox_index)
{
	int rc;
	int mctp_pcc_mtu;
	char name[32];
	struct net_device *ndev;
	struct mctp_pcc_ndev *mctp_pcc_dev;
	struct mctp_pcc_hw_addr physical_link_addr;

	snprintf(name, sizeof(name), "mctpipcc%x", inbox_index);
	ndev = alloc_netdev(sizeof(struct mctp_pcc_ndev), name, NET_NAME_ENUM, mctp_pcc_setup);
	if (!ndev)
		return -ENOMEM;
	mctp_pcc_dev = (struct mctp_pcc_ndev *)netdev_priv(ndev);
	INIT_LIST_HEAD(&mctp_pcc_dev->head);
	spin_lock_init(&mctp_pcc_dev->lock);

	mctp_pcc_dev->outbox_client.tx_prepare = NULL;
	mctp_pcc_dev->outbox_client.tx_done = NULL;
	mctp_pcc_dev->hw_addr.inbox_index = inbox_index;
	mctp_pcc_dev->hw_addr.outbox_index = outbox_index;
	mctp_pcc_dev->inbox_client.rx_callback = mctp_pcc_client_rx_callback;
	mctp_pcc_dev->cleanup_channel = pcc_mbox_free_channel;
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

/* There is no clean way to pass the MTU to the callback function
 * used for registration, so set the values ahead of time.
 */
	mctp_pcc_mtu = mctp_pcc_dev->out_chan->shmem_size -
		sizeof(struct mctp_pcc_hdr);
	ndev->mtu = mctp_pcc_mtu;
	ndev->max_mtu = mctp_pcc_mtu;
	ndev->min_mtu = MCTP_MIN_MTU;

	physical_link_addr.inbox_index =
		htonl(mctp_pcc_dev->hw_addr.inbox_index);
	physical_link_addr.outbox_index =
		htonl(mctp_pcc_dev->hw_addr.outbox_index);
	dev_addr_set(ndev, (const u8 *)&physical_link_addr);
	rc = register_netdev(ndev);
	if (rc)
		goto cleanup_in_channel;
	list_add_tail(&mctp_pcc_dev->head, &mctp_pcc_ndevs);
	return 0;
cleanup_in_channel:
	mctp_pcc_dev->cleanup_channel(mctp_pcc_dev->in_chan);
cleanup_out_channel:
	mctp_pcc_dev->cleanup_channel(mctp_pcc_dev->out_chan);
free_netdev:
	unregister_netdev(ndev);
	free_netdev(ndev);
	return rc;
}

struct lookup_context {
	int index;
	int inbox_index;
	int outbox_index;
};

static acpi_status lookup_pcct_indices(struct acpi_resource *ares, void *context)
{
	struct acpi_resource_address32 *addr;
	struct lookup_context *luc = context;

	switch (ares->type) {
	case 0x0c:
	case 0x0a:
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

static int mctp_pcc_driver_add(struct acpi_device *adev)
{
	int inbox_index;
	int outbox_index;
	acpi_handle dev_handle;
	acpi_status status;
	struct lookup_context context = {0, 0, 0};

	dev_info(&adev->dev, "Adding mctp_pcc device for HID  %s\n", acpi_device_hid(adev));
	dev_handle = acpi_device_handle(adev);
	status = acpi_walk_resources(dev_handle, "_CRS", lookup_pcct_indices, &context);
	if (ACPI_SUCCESS(status)) {
		inbox_index = context.inbox_index;
		outbox_index = context.outbox_index;
		return create_mctp_pcc_netdev(adev, &adev->dev, inbox_index, outbox_index);
	}
	dev_err(&adev->dev, "FAILURE to lookup PCC indexes from CRS");
	return -EINVAL;
};

/* pass in adev=NULL to remove all devices
 */
static void mctp_pcc_driver_remove(struct acpi_device *adev)
{
	struct mctp_pcc_ndev *mctp_pcc_dev = NULL;
	struct list_head *ptr;
	struct list_head *tmp;

	list_for_each_safe(ptr, tmp, &mctp_pcc_ndevs) {
		mctp_pcc_dev = list_entry(ptr, struct mctp_pcc_ndev, head);
		if (!adev || mctp_pcc_dev->acpi_device == adev) {
			struct net_device *ndev;

			mctp_pcc_dev->cleanup_channel(mctp_pcc_dev->out_chan);
			mctp_pcc_dev->cleanup_channel(mctp_pcc_dev->in_chan);
			ndev = mctp_pcc_dev->mdev.dev;
			if (ndev)
				mctp_unregister_netdev(ndev);
			list_del(ptr);
			if (adev)
				break;
		}
	}
};

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
		.notify = NULL,
	},
	.owner = THIS_MODULE,

};

static int __init mctp_pcc_mod_init(void)
{
	int rc;

	pr_info("initializing MCTP over PCC\n");
	INIT_LIST_HEAD(&mctp_pcc_ndevs);
	rc = acpi_bus_register_driver(&mctp_pcc_driver);
	if (rc < 0)
		ACPI_DEBUG_PRINT((ACPI_DB_ERROR, "Error registering driver\n"));
	return rc;
}

static __exit void mctp_pcc_mod_exit(void)
{
	pr_info("Removing MCTP over PCC transport driver\n");
	mctp_pcc_driver_remove(NULL);
	acpi_bus_unregister_driver(&mctp_pcc_driver);
}

module_init(mctp_pcc_mod_init);
module_exit(mctp_pcc_mod_exit);

MODULE_DEVICE_TABLE(acpi, mctp_pcc_device_ids);

MODULE_DESCRIPTION("MCTP PCC device");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Adam Young <admiyo@os.amperecomputing.com>");
