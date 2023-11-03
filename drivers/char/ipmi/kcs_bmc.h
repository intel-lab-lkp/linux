/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2015-2018, Intel Corporation.
 */

#ifndef __KCS_BMC_H__
#define __KCS_BMC_H__

#include <linux/list.h>
#include <linux/module.h>
#include <linux/spinlock.h>

/**
 * DOC: KCS subsystem structure
 *
 * The KCS subsystem is split into three components:
 *
 * 1. &struct kcs_bmc_device
 * 2. &struct kcs_bmc_driver
 * 3. &struct kcs_bmc_client
 *
 * ``struct kcs_bmc_device`` (device) represents a driver instance for a
 * particular KCS device. ``struct kcs_bmc_device``` abstracts away the device
 * specifics allowing for device-independent implementation of protocols over
 * KCS.
 *
 * ``struct kcs_bmc_driver`` (driver) represents an implementation of a KCS
 * protocol. Implementations of a protocol either expose this behaviour out to
 * userspace via a character device, or provide the glue into another kernel
 * subsystem.
 *
 * ``struct kcs_bmc_client`` (client) associates a ``struct kcs_bmc_device``
 * instance (``D``) with a &struct kcs_bmc_driver instance (``P``). An instance
 * of each protocol implementation is associated with each device, yielding
 * ``D*P`` client instances.
 *
 * A device may only have one active client at a time. A client becomes active
 * on its associated device whenever userspace "opens" its interface in some
 * fashion, for example, opening a character device. If the device associated
 * with a client already has an active client then an error is propagated.
 */

#define KCS_BMC_EVENT_TYPE_OBE	BIT(0)
#define KCS_BMC_EVENT_TYPE_IBF	BIT(1)

#define KCS_BMC_STR_OBF		BIT(0)
#define KCS_BMC_STR_IBF		BIT(1)
#define KCS_BMC_STR_CMD_DAT	BIT(3)

/* IPMI 2.0 - 9.5, KCS Interface Registers
 * @idr: Input Data Register
 * @odr: Output Data Register
 * @str: Status Register
 */
struct kcs_ioreg {
	u32 idr;
	u32 odr;
	u32 str;
};

struct kcs_bmc_device_ops;
struct kcs_bmc_client;

/**
 * struct kcs_bmc_device - An abstract representation of a KCS device
 * @entry: A list node for the KCS core to track KCS device instances
 * @dev: The kernel device object for the KCS hardware
 * @channel: The IPMI channel number for the KCS device
 * @ops: A set of callbacks for providing abstract access to the KCS hardware
 * @lock: Protects accesses to, and operations on &kcs_bmc_device.client
 * @client: The client instance, if any, currently associated with the device
 */
struct kcs_bmc_device {
	struct list_head entry;

	struct device *dev;
	u32 channel;

	struct kcs_ioreg ioreg;

	const struct kcs_bmc_device_ops *ops;

	spinlock_t lock;
	struct kcs_bmc_client *client;
};

#endif /* __KCS_BMC_H__ */
