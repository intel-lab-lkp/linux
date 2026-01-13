/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * i2c-smbus.h - SMBus extensions to the I2C protocol
 *
 * Copyright (C) 2010-2019 Jean Delvare <jdelvare@suse.de>
 */

#ifndef _LINUX_I2C_SMBUS_H
#define _LINUX_I2C_SMBUS_H

#include <linux/i2c.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>


/**
 * i2c_smbus_alert_setup - platform data for the smbus_alert i2c client
 * @irq: IRQ number, if the smbus_alert driver should take care of interrupt
 *		handling
 *
 * If irq is not specified, the smbus_alert driver doesn't take care of
 * interrupt handling. In that case it is up to the I2C bus driver to either
 * handle the interrupts or to poll for alerts.
 */
struct i2c_smbus_alert_setup {
	int			irq;
};

struct i2c_client *i2c_new_smbus_alert_device(struct i2c_adapter *adapter,
					      struct i2c_smbus_alert_setup *setup);
int i2c_handle_smbus_alert(struct i2c_client *ara);

#if IS_ENABLED(CONFIG_I2C_SMBUS) && IS_ENABLED(CONFIG_I2C_SLAVE)
struct i2c_client *i2c_new_slave_host_notify_device(struct i2c_adapter *adapter);
void i2c_free_slave_host_notify_device(struct i2c_client *client);
#else
static inline struct i2c_client *i2c_new_slave_host_notify_device(struct i2c_adapter *adapter)
{
	return ERR_PTR(-ENOSYS);
}
static inline void i2c_free_slave_host_notify_device(struct i2c_client *client)
{
}
#endif

#if IS_ENABLED(CONFIG_I2C_SMBUS) && IS_ENABLED(CONFIG_DMI)
void i2c_register_spd_write_disable(struct i2c_adapter *adap);
void i2c_register_spd_write_enable(struct i2c_adapter *adap);
#else
static inline void i2c_register_spd_write_disable(struct i2c_adapter *adap) { }
static inline void i2c_register_spd_write_enable(struct i2c_adapter *adap) { }
#endif

/**
 * struct i2c_arp_udid - ARP Unique Device Identifier
 * @capabilities:
 * @version:
 * @vendor:
 * @device:
 * @interface:
 * @subvendor:
 * @subdevice:
 * @vendor_specific_id:
 */
struct i2c_arp_udid {
	u8 capabilities;
#define ARP_CAP_PEC_SUPPORTED				BIT(0)
#define ARP_CAP_ADDRESS_TYPE_MASK			GENMASK(7, 6)
#define   ARP_CAP_ADDRESS_TYPE_FIXED			0
#define   ARP_CAP_ADDRESS_TYPE_DYNAMIC_PERSISTENT	1
#define   ARP_CAP_ADDRESS_TYPE_DYNAMIC_VOLATILE		2
#define   ARP_CAP_ADDRESS_TYPE_RANDOM_NUMBER		3
	u8 version;
	u16 vendor;
	u16 device;
	u16 interface;
	u16 subvendor;
	u16 subdevice;
	u32 vendor_specific_id;
} __packed;

/**
 * SMBUS_DEVICE - macro used to describe a specific SMBus device
 * @v: the 16 bit UDID Vendor ID
 * @d: the 16 bit UDID Device ID
 *
 * This macro is used to create a struct smbus_device_id that matches a
 * specific device. The interface, subvendor and subdevice fields will be set to
 * SMBUS_ANY_ID.
 */
#define SMBUS_DEVICE(v, d) \
	.vendor = (v), .device = (d), \
	.interface = SMBUS_ANY_ID, \
	.subvendor = SMBUS_ANY_ID, .subdevice = SMBUS_ANY_ID, \
	.vendor_specific_id = SMBUS_ANY_VENDOR_SPECIFIC_ID,

/**
 * SMBUS_INTERFACE - macro used to describe a specific SMBus interface
 * @i: the 16 bit UDID interface
 *
 * This macro is used to create a struct smbus_device_id that matches a
 * specific interface. The vendor, device, subvendor and subdevice fields will be
 * set to SMBUS_ANY_ID.
 */
#define SMBUS_INTERFACE(i) \
	.vendor = SMBUS_ANY_ID, .device = SMBUS_ANY_ID, \
	.interface = (i), \
	.subvendor = SMBUS_ANY_ID, .subdevice = SMBUS_ANY_ID, \
	.vendor_specific_id = SMBUS_ANY_VENDOR_SPECIFIC_ID,

/* The version field in the interface - these are used as the minimum. */
#define SMBUS_INTERFACE_SMBUS_V2_0	0x4
#define SMBUS_INTERFACE_SMBUS_V3_0	0x5

/* The interfaces defined in the SMBus specification. */
#define SMBUS_INTERFACE_OEM	SMBUS_INTERFACE(BIT(4) | SMBUS_INTERFACE_SMBUS_V2_0)
#define SMBUS_INTERFACE_ASF	SMBUS_INTERFACE(BIT(5) | SMBUS_INTERFACE_SMBUS_V2_0)
#define SMBUS_INTERFACE_IPMI	SMBUS_INTERFACE(BIT(6) | SMBUS_INTERFACE_SMBUS_V2_0)
#define SMBUS_INTERFACE_ZONE	SMBUS_INTERFACE(BIT(7) | SMBUS_INTERFACE_SMBUS_V3_0)

#endif /* _LINUX_I2C_SMBUS_H */
