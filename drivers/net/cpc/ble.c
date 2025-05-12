// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Bluetooth HCI over CPC.
 *
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/skbuff.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "ble.h"
#include "cpc.h"

struct cpc_ble {
	struct cpc_endpoint *ep;
	struct hci_dev *hdev;
	struct sk_buff_head txq;
};

static int cpc_ble_open(struct hci_dev *hdev)
{
	struct cpc_ble *ble = hci_get_drvdata(hdev);

	skb_queue_head_init(&ble->txq);

	return cpc_endpoint_connect(ble->ep);
}

static int cpc_ble_close(struct hci_dev *hdev)
{
	struct cpc_ble *ble = hci_get_drvdata(hdev);

	cpc_endpoint_disconnect(ble->ep);

	skb_queue_purge(&ble->txq);

	return 0;
}

static int cpc_ble_flush(struct hci_dev *hdev)
{
	struct cpc_ble *ble = hci_get_drvdata(hdev);

	skb_queue_purge(&ble->txq);

	return 0;
}

static int cpc_ble_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct cpc_ble *ble = hci_get_drvdata(hdev);

	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);

	return cpc_endpoint_write(ble->ep, skb);
}

static void cpc_ble_rx_frame(struct cpc_endpoint *ep, struct sk_buff *skb)
{
	struct cpc_ble *ble = cpc_endpoint_get_drvdata(ep);

	hci_skb_pkt_type(skb) = *((u8 *)skb_pull_data(skb, 1));
	hci_skb_expect(skb) = skb->len;

	hci_recv_frame(ble->hdev, skb);
}

static struct cpc_endpoint_ops cpc_ble_ops = {
	.rx = cpc_ble_rx_frame,
};

static int cpc_ble_probe(struct cpc_endpoint *ep)
{
	struct cpc_ble *ble;
	int err;

	ble = kzalloc(sizeof(*ble), GFP_KERNEL);
	if (!ble) {
		err = -ENOMEM;
		goto alloc_ble_fail;
	}

	cpc_endpoint_set_ops(ep, &cpc_ble_ops);
	cpc_endpoint_set_drvdata(ep, ble);

	ble->ep = ep;
	ble->hdev = hci_alloc_dev();
	if (!ble->hdev) {
		err = -ENOMEM;
		goto alloc_hdev_fail;
	}

	hci_set_drvdata(ble->hdev, ble);
	ble->hdev->open = cpc_ble_open;
	ble->hdev->close = cpc_ble_close;
	ble->hdev->flush = cpc_ble_flush;
	ble->hdev->send = cpc_ble_send;

	err = hci_register_dev(ble->hdev);
	if (err)
		goto register_hdev_fail;

	return 0;

register_hdev_fail:
	hci_free_dev(ble->hdev);
alloc_hdev_fail:
	kfree(ble);
alloc_ble_fail:
	return err;
}

static void cpc_ble_remove(struct cpc_endpoint *ep)
{
	struct cpc_ble *ble = cpc_endpoint_get_drvdata(ep);

	hci_unregister_dev(ble->hdev);
	hci_free_dev(ble->hdev);
	kfree(ble);
}

static struct cpc_driver ble_driver = {
	.driver = {
		.name = CPC_BLUETOOTH_ENDPOINT_NAME,
	},
	.probe = cpc_ble_probe,
	.remove = cpc_ble_remove,
};

/**
 * cpc_ble_drv_register - Register the ble endpoint driver.
 *
 * @return: 0 on success, otherwise a negative error code.
 */
int cpc_ble_drv_register(void)
{
	return cpc_driver_register(&ble_driver);
}

/**
 * cpc_ble_drv_unregister - Unregister the ble endpoint driver.
 */
void cpc_ble_drv_unregister(void)
{
	cpc_driver_unregister(&ble_driver);
}
