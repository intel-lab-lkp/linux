// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Bluetooth HCI over Greybus.
 *
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/greybus.h>
#include <linux/skbuff.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#define GREYBUS_VENDOR_SILABS	0xBEEF
#define GREYBUS_PRODUCT_EFX	0xCAFE

#define GB_BLE_TRANSFER		0x01

struct gb_ble {
	struct gb_connection *conn;
	struct hci_dev *hdev;
	struct sk_buff_head txq;
};

static int gb_ble_open(struct hci_dev *hdev)
{
	struct gb_ble *ble = hci_get_drvdata(hdev);

	skb_queue_head_init(&ble->txq);

	return gb_connection_enable(ble->conn);
}

static int gb_ble_close(struct hci_dev *hdev)
{
	struct gb_ble *ble = hci_get_drvdata(hdev);

	gb_connection_disable(ble->conn);

	return 0;
}

static void gb_ble_xfer_done(struct gb_operation *operation)
{
	struct sk_buff *skb = gb_operation_get_data(operation);

	kfree_skb(skb);
}

static int gb_ble_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct gb_ble *ble = hci_get_drvdata(hdev);
	int ret;

	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);

	ret = gb_operation_unidirectional_async_timeout(ble->conn,
							gb_ble_xfer_done, skb,
							GB_BLE_TRANSFER,
							skb->data, skb->len + 1,
							GB_OPERATION_TIMEOUT_DEFAULT);

	return -ENOMEM;
}

static int gb_ble_request_handler(struct gb_operation *operation)
{
	struct gb_ble *ble = gb_connection_get_data(operation->connection);
	struct device *dev = &operation->connection->bundle->dev;
	struct sk_buff *skb;
	unsigned int skb_len;

	switch (operation->type) {
	case GB_BLE_TRANSFER:
		/* Must be unidirectional as AP is not responding to this request. */
		if (!gb_operation_is_unidirectional(operation))
			return -EINVAL;

		if (operation->request->payload_size < 2)
			return -EINVAL;

		skb_len = operation->request->payload_size - 1;
		skb = bt_skb_alloc(skb_len, GFP_KERNEL);
		if (!skb)
			return -ENOMEM;

		/* Prepare HCI SKB and pass it to upper layer */
		hci_skb_pkt_type(skb) = ((u8 *)operation->request->payload)[0];
		memcpy(skb_put(skb, skb_len), &(((u8 *)operation->request->payload)[1]), skb_len);
		hci_skb_expect(skb) = skb_len;

		hci_recv_frame(ble->hdev, skb);

		break;
	default:
		dev_err(dev, "unsupported request: %u\n", operation->type);
		return -EINVAL;
	}

	return 0;
}

static int gb_ble_probe(struct gb_bundle *bundle,
			const struct greybus_bundle_id *id)
{
	struct greybus_descriptor_cport *cport_desc;
	struct gb_connection *connection;
	struct gb_ble *ble;
	int err;

	if (bundle->num_cports != 1)
		return -ENODEV;

	cport_desc = &bundle->cport_desc[0];
	if (cport_desc->protocol_id != GREYBUS_PROTOCOL_VENDOR)
		return -ENODEV;

	ble = kzalloc(sizeof(*ble), GFP_KERNEL);
	if (!ble) {
		err = -ENOMEM;
		goto alloc_ble_fail;
	}

	greybus_set_drvdata(bundle, ble);

	connection = gb_connection_create(bundle, le16_to_cpu(cport_desc->id),
					  gb_ble_request_handler);
	if (IS_ERR(connection)) {
		err = PTR_ERR(connection);
		goto connection_create_fail;
	}

	gb_connection_set_data(connection, ble);
	ble->conn = connection;
	ble->hdev = hci_alloc_dev();
	if (!ble->hdev) {
		err = -ENOMEM;
		goto alloc_hdev_fail;
	}

	hci_set_drvdata(ble->hdev, ble);
	ble->hdev->open = gb_ble_open;
	ble->hdev->close = gb_ble_close;
	ble->hdev->send = gb_ble_send;

	err = hci_register_dev(ble->hdev);
	if (err)
		goto register_hdev_fail;

	return 0;

register_hdev_fail:
	hci_free_dev(ble->hdev);
alloc_hdev_fail:
	gb_connection_destroy(connection);
connection_create_fail:
	kfree(ble);
alloc_ble_fail:
	return err;
}

static void gb_ble_disconnect(struct gb_bundle *bundle)
{
	struct gb_ble *ble = greybus_get_drvdata(bundle);

	hci_unregister_dev(ble->hdev);
	hci_free_dev(ble->hdev);

	/*
	 * The connection should be disabled by now as unregistering the HCI device
	 * calls its close callback, so it should be safe to destroy the connection.
	 */
	gb_connection_destroy(ble->conn);

	kfree(ble);
}

static const struct greybus_bundle_id gb_silabs_ble_id_table[] = {
	{ GREYBUS_DEVICE(GREYBUS_VENDOR_SILABS, GREYBUS_PRODUCT_EFX, 1) },
	{ }
};
MODULE_DEVICE_TABLE(greybus, gb_silabs_ble_id_table);

static struct greybus_driver gb_silabs_ble_driver = {
	.name		= "silabs-ble",
	.probe		= gb_ble_probe,
	.disconnect	= gb_ble_disconnect,
	.id_table	= gb_silabs_ble_id_table,
};

static int silabs_ble_init(void)
{
	return greybus_register(&gb_silabs_ble_driver);
}
module_init(silabs_ble_init);

static void __exit silabs_ble_exit(void)
{
	greybus_deregister(&gb_silabs_ble_driver);
}
module_exit(silabs_ble_exit);

MODULE_DESCRIPTION("Bluetooth Driver for Silicon Labs EFx devices over Greybus");
MODULE_LICENSE("GPL");
