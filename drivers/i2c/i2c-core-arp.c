// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SMBus Address Resolution Protocol
 *
 * Copyright (C) 2026 Intel Corporation
 */

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/dev_printk.h>
#include <linux/device/devres.h>
#include <linux/idr.h>
#include <linux/i2c.h>
#include <linux/i2c-smbus.h>
#include <linux/list.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "i2c-core.h"

#define I2C_SMBUS_DEFAULT_ADDR	0x61

#define ARP_CMD_PREPARE_TO_ARP	0x01
#define ARP_CMD_RESET_DEVICE	0x02
#define ARP_CMD_GET_UDID	0x03
#define ARP_CMD_ASSIGN_ADDRESS	0x04

struct get_udid_data {
	struct i2c_arp_udid	udid;
	u8			target_address;
} __packed;

struct arp_work {
	struct work_struct	work;
	struct i2c_adapter	*adapter;
	u8			target;
};

struct i2c_smbus_arp {
	struct list_head	clients;
	struct mutex		lock; /* ARP controller lock */
};

/* SMBus Specification Appendix C. */
static const DECLARE_BITMAP(reserved_addrs, (1 << 7)) = {
	[BIT_WORD(0)] = GENMASK(12, 0) |	/* SMBus v1.1 */
			BIT_MASK(0x28) |	/* PMBus ZONE READ */
			BIT_MASK(0x2c) |	/* Reserved */
			BIT_MASK(0x2d) |	/* Reserved */
			BIT_MASK(0x55),		/* PMBus ZONE WRITE */

	[BIT_WORD(64)] = BIT_MASK(0x40) |	/* Reserved */
			 BIT_MASK(0x41) |	/* Reserved */
			 BIT_MASK(0x42) |	/* Reserved */
			 BIT_MASK(0x43) |	/* Reserved */
			 BIT_MASK(0x44) |	/* Reserved */
			 BIT_MASK(0x48) |	/* Prototype Address */
			 BIT_MASK(0x49) |	/* Prototype Address */
			 BIT_MASK(0x4a) |	/* Prototype Address */
			 BIT_MASK(0x4b) |	/* Prototype Address */
			 BIT_MASK(0x61) |	/* SMBus Default Address */
			 BIT_MASK(0x78) |	/* 10-bit target addressing */
			 BIT_MASK(0x79) |	/* 10-bit target addressing */
			 BIT_MASK(0x7a) |	/* 10-bit target addressing */
			 BIT_MASK(0x7b) |	/* 10-bit target addressing */
			 BIT_MASK(0x7c) |	/* Reserved */
			 BIT_MASK(0x7d) |	/* Reserved */
			 BIT_MASK(0x7e) |	/* Reserved */
			 BIT_MASK(0x7f)		/* Reserved */
};

const struct smbus_device_id *i2c_smbus_match_id(const struct i2c_client *client,
						 const struct smbus_device_id *id)
{
	struct i2c_arp_udid *udid;

	if (!(id && client && client->udid))
		return NULL;

	udid = client->udid;

	while (id->vendor) {
		if ((id->vendor == SMBUS_ANY_ID || id->vendor == udid->vendor) &&
		    (id->device == SMBUS_ANY_ID || id->device == udid->device) &&
		    /* The interface is split into "version" and "protocol". */
		    (id->interface == SMBUS_ANY_ID ||
		      /* minimum version must be supported */
		     (((id->interface & 0xf) <= (udid->interface & 0xf)) &&
		      /* the protocol must match */
		      ((id->interface & 0xf0) == (udid->interface & 0xf0)))) &&
		    (id->subvendor == SMBUS_ANY_ID || id->subvendor == udid->subvendor) &&
		    (id->subdevice == SMBUS_ANY_ID || id->subdevice == udid->subdevice) &&
		    (id->vendor_specific_id == SMBUS_ANY_VENDOR_SPECIFIC_ID ||
		     id->vendor_specific_id == udid->vendor_specific_id))
			return id;
		id++;
	}

	return NULL;
}

static int i2c_smbus_arp_get_udid(struct i2c_adapter *adapter, u8 target,
				  struct get_udid_data *get_udid)
{
	union i2c_smbus_data data;
	u8 command;
	int ret;

	if (target)
		command = (target << 1) | 1;
	else
		command = ARP_CMD_GET_UDID;

	ret = i2c_smbus_xfer(adapter, I2C_SMBUS_DEFAULT_ADDR,
			     I2C_CLIENT_PEC, I2C_SMBUS_READ,
			     command, I2C_SMBUS_BLOCK_DATA, &data);
	if (ret)
		return ret;

	memcpy(get_udid, &data.block[1], sizeof(*get_udid));

	return 0;
}

static int i2c_smbus_arp_verify_address(struct i2c_adapter *adapter,
					struct get_udid_data *get_udid)
{
	u8 addr_type = FIELD_GET(ARP_CAP_ADDRESS_TYPE_MASK,
				 get_udid->udid.capabilities);
	u8 addr = get_udid->target_address;

	if (addr_type == ARP_CAP_ADDRESS_TYPE_FIXED)
		return 0;

	/* Find a free address if necessary. */
	if (addr == 0xff || i2c_check_addr_busy(adapter, addr)) {
		for_each_clear_bit(addr, reserved_addrs, 128)
			if (!i2c_check_addr_busy(adapter, addr))
				break;
		if (addr == 128)
			return -EBUSY;

		get_udid->target_address = addr;
	}

	return 0;
}

static int i2c_smbus_arp_assign_address(struct i2c_adapter *adapter,
					struct get_udid_data *get_udid)
{
	union i2c_smbus_data data;
	int ret;

	ret = i2c_smbus_arp_verify_address(adapter, get_udid);
	if (ret)
		return ret;

	data.block[0] = sizeof(*get_udid);
	memcpy(&data.block[1], get_udid, data.block[0]);

	ret = i2c_smbus_xfer(adapter, I2C_SMBUS_DEFAULT_ADDR,
			     I2C_CLIENT_PEC, I2C_SMBUS_WRITE,
			     ARP_CMD_ASSIGN_ADDRESS, I2C_SMBUS_BLOCK_DATA, &data);
	if (ret)
		return -EAGAIN;

	return 0;
}

static void i2c_smbus_arp_remove_client(void *udid)
{
	kfree(udid);
}

static int i2c_smbus_arp_new_client(struct i2c_adapter *adapter,
				    struct get_udid_data *data)
{
	struct i2c_board_info info = { };
	struct i2c_client *client;
	int ret;

	info.udid = kmemdup(&data->udid, sizeof(data->udid), GFP_KERNEL);
	if (!info.udid)
		return -ENOMEM;

	info.addr = data->target_address;
	info.flags = I2C_CLIENT_HOST_NOTIFY;

	if (data->udid.capabilities & ARP_CAP_PEC_SUPPORTED)
		info.flags |= I2C_CLIENT_PEC;

	sprintf(info.type, "%d:arp-%zu", i2c_adapter_id(adapter),
		list_count_nodes(&adapter->arp->clients));
	info.dev_name = info.type;

	client = i2c_new_client_device(adapter, &info);
	if (IS_ERR(client)) {
		kfree(info.udid);
		return PTR_ERR(client);
	}

	ret = devm_add_action_or_reset(&client->dev, i2c_smbus_arp_remove_client, info.udid);
	if (ret)
		return ret;

	list_add_tail(&client->detected, &adapter->arp->clients);

	return 0;
}

static void i2c_smbus_arp_work(struct work_struct *work)
{
	struct arp_work *awork = container_of(work, struct arp_work, work);
	struct i2c_adapter *adapter = awork->adapter;
	u8 target = awork->target;
	struct get_udid_data data;
	int ret;

	mutex_lock(&adapter->arp->lock);

	do {
		if (i2c_smbus_arp_get_udid(adapter, target, &data))
			break;

		ret = i2c_smbus_arp_assign_address(adapter, &data);
		if (ret == -EAGAIN)
			continue;
		if (ret < 0) {
			dev_warn(&adapter->dev, "out of addresses\n");
			break;
		}

		if (i2c_smbus_arp_new_client(adapter, &data))
			break;
	} while (!target);

	mutex_unlock(&adapter->arp->lock);
	kfree(awork);
}

/**
 * i2c_smbus_arp_detect - Schedule detection and registration of ARP devices
 * @adapter: ARP Controller
 * @target_address: Address for directed ARP commands
 *
 * Registers ARP-capable devices attached to @adapter. If @target_address is
 * supplied, directed Get UDID command will be used. Otherwise, if
 * @target_address is 0, the general Get UDID command is used until there are no
 * more responses.
 *
 * Returns 0 on success or errno.
 */
int i2c_smbus_arp_detect(struct i2c_adapter *adapter, u8 target_address)
{
	struct arp_work *awork;

	if (!adapter->arp)
		return -ENXIO;

	awork = kzalloc(sizeof(*awork), GFP_KERNEL);
	if (!awork)
		return -ENOMEM;

	INIT_WORK(&awork->work, i2c_smbus_arp_work);
	awork->target = target_address;
	awork->adapter = adapter;

	queue_work(system_long_wq, &awork->work);

	return 0;
}

/**
 * i2c_smbus_arp_probe - Declare adapter as ARP controller
 * @adapter: ARP Controller
 *
 * Declare @adapter as the ARP controller with the Prepare to ARP command, and
 * then detect all available ARP devices with the general Get UDID command.
 *
 * Returns 0 on success or errno.
 */
int i2c_smbus_arp_probe(struct i2c_adapter *adapter)
{
	int ret;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE | I2C_FUNC_SMBUS_BLOCK_DATA))
		return 0;

	adapter->arp = devm_kzalloc(&adapter->dev, sizeof(*adapter->arp), GFP_KERNEL);
	if (!adapter->arp)
		return -ENOMEM;

	mutex_init(&adapter->arp->lock);
	INIT_LIST_HEAD(&adapter->arp->clients);

	/* Broadcast "Prepare to ARP" command. */
	ret = i2c_smbus_xfer(adapter, I2C_SMBUS_DEFAULT_ADDR, I2C_CLIENT_PEC,
			     I2C_SMBUS_WRITE, ARP_CMD_PREPARE_TO_ARP,
			     I2C_SMBUS_BYTE, NULL);
	if (ret)
		return 0;

	return i2c_smbus_arp_detect(adapter, 0);
}

/**
 * i2c_smbus_arp_remove - Unregister all ARP devices
 * @adapter: ARP Controller
 *
 * Unregister all ARP devices attached to @adapter.
 */
void i2c_smbus_arp_remove(struct i2c_adapter *adapter)
{
	struct i2c_client *client, *next;

	if (!adapter->arp)
		return;

	mutex_lock(&adapter->arp->lock);

	list_for_each_entry_safe(client, next, &adapter->arp->clients, detected) {
		list_del(&client->detected);
		i2c_unregister_device(client);
	}

	mutex_unlock(&adapter->arp->lock);
}
