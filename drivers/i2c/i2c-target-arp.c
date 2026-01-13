// SPDX-License-Identifier: GPL-2.0-only
/*
 * SMBus Address Resolution Protocol target mode test driver
 *
 * Copyright (C) 2026 Intel Corporation
 */
#include <linux/bits.h>
#include <linux/device/devres.h>
#include <linux/i2c.h>
#include <linux/i2c-smbus.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/string.h>

#define ARP_ADDRESS_VALID	BIT(0)
#define ARP_ADDRESS_RESOLVED	BIT(1)

#define I2C_SMBUS_HOST		0x08
#define I2C_SMBUS_DEFAULT_ADDR	0x61

#define ARP_CMD_PREPARE_TO_ARP	0x01
#define ARP_CMD_RESET_DEVICE	0x02
#define ARP_CMD_GET_UDID	0x03
#define ARP_CMD_ASSIGN_ADDRESS	0x04

struct get_udid_data {
	struct i2c_arp_udid	udid;
	u8			target_addr;
} __packed;

struct arp_target {
	struct i2c_client target;
	struct i2c_client *client;

	int read_count;
	int write_count;
	unsigned char buf[24];

	struct get_udid_data data;
	unsigned char flag;
};

static u8 i2c_arp_pec(struct arp_target *arp, bool read)
{
	u8 addr = I2C_SMBUS_DEFAULT_ADDR << 1;
	u8 len = sizeof(arp->data);
	u8 pec = 0;

	pec = i2c_smbus_pec(pec, &addr, 1);
	pec = i2c_smbus_pec(pec, arp->buf, arp->write_count);

	if (read) {
		addr |= 1;
		pec = i2c_smbus_pec(pec, &addr, 1);
		pec = i2c_smbus_pec(pec, &len, 1);
		pec = i2c_smbus_pec(pec, (void *)&arp->data, len);
	}

	return pec;
}

static int i2c_target_arp_cb(struct i2c_client *target, enum i2c_slave_event event, u8 *val)
{
	struct arp_target *arp = container_of(target, struct arp_target, target);

	switch (event) {
	case I2C_SLAVE_READ_PROCESSED:
		if (arp->flag)
			break;

		if (arp->read_count == sizeof(arp->data))
			*val = i2c_arp_pec(arp, 1);
		else
			*val = ((u8 *)&arp->data)[arp->read_count];

		arp->read_count++;
		break;
	case I2C_SLAVE_READ_REQUESTED:
		if (arp->flag)
			break;

		*val = sizeof(arp->data);
		break;
	case I2C_SLAVE_STOP:
		switch (arp->buf[0]) {
		case ARP_CMD_PREPARE_TO_ARP:
			arp->flag = 0;
			break;
		case ARP_CMD_ASSIGN_ADDRESS:
			/* If the UDID matches, this address is for us. */
			if (!memcmp(&arp->buf[2], &arp->data.udid, sizeof(arp->data.udid)))
				arp->flag = ARP_ADDRESS_VALID | ARP_ADDRESS_RESOLVED;
			break;
		default:
			break;
		}

		arp->read_count = 0;
		arp->write_count = 0;
		break;
	case I2C_SLAVE_WRITE_REQUESTED:
		arp->read_count = 0;
		arp->write_count = 0;
		break;
	case I2C_SLAVE_WRITE_RECEIVED:
		arp->buf[arp->write_count] = *val;
		arp->write_count++;
		break;
	}

	return 0;
}

static void i2c_notify_arp_controller(struct i2c_client *client)
{
	union i2c_smbus_data data = { };
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_WORD_DATA))
		return;

	ret = i2c_smbus_xfer(client->adapter, I2C_SMBUS_HOST, client->flags,
			     I2C_SMBUS_WRITE, client->addr,
			     I2C_SMBUS_WORD_DATA, &data);
	if (ret)
		dev_warn(&client->dev, "Failed to Notify ARP Controller (%d)\n", ret);
}

static int i2c_target_arp_probe(struct i2c_client *client)
{
	struct arp_target *arp;
	int ret;

	arp = devm_kzalloc(&client->dev, sizeof(*arp), GFP_KERNEL);
	if (!arp)
		return -ENOMEM;

	arp->client = client;
	i2c_set_clientdata(client, arp);

	arp->target = *client;
	arp->target.addr = I2C_SMBUS_DEFAULT_ADDR;
	arp->target.flags |= I2C_CLIENT_SLAVE;

	/*
	 * The vendor and device ID are left undefined. Some hosts may not
	 * support this. If that's the case, supply values for them.
	 *
	 * arp->data.udid.vendor = 0xXXXX;
	 * arp->data.udid.device = 0xXXXX;
	 *
	 * The capabilities are also 0. That makes this a fixed address device
	 * without support for PEC.
	 */
	arp->data.udid.version = BIT(3); /* UDID Version 1 */
	arp->data.udid.interface = SMBUS_INTERFACE_SMBUS_V2_0;
	arp->data.target_addr = client->addr;

	/*
	 * NOTE: This target is only for the ARP. After the address has been
	 * assigned, another target should be registered for the actual device,
	 * but this test code does _not_ register it.
	 */
	ret = i2c_slave_register(&arp->target, i2c_target_arp_cb);
	if (ret)
		return ret;

	i2c_notify_arp_controller(client);

	return 0;
}

static void i2c_target_arp_remove(struct i2c_client *client)
{
	struct arp_target *arp;

	if (IS_ERR_OR_NULL(client))
		return;

	arp = i2c_get_clientdata(client);
	i2c_slave_unregister(&arp->target);
}

static const struct i2c_device_id i2c_target_arp_id[] = {
	{ "target-arp" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, i2c_target_arp_id);

static struct i2c_driver i2c_target_arp_driver = {
	.driver = {
		.name = "i2c-target-arp",
	},
	.probe = i2c_target_arp_probe,
	.remove = i2c_target_arp_remove,
	.id_table = i2c_target_arp_id,
};
module_i2c_driver(i2c_target_arp_driver);

MODULE_DESCRIPTION("SMBus ARP target mode test driver");
MODULE_LICENSE("GPL");
