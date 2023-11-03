/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2021, IBM Corp. */

#ifndef __KCS_BMC_CONSUMER_H__
#define __KCS_BMC_CONSUMER_H__

#include <linux/irqreturn.h>
#include <linux/module.h>

#include "kcs_bmc.h"

struct kcs_bmc_driver;

/**
 * struct kcs_bmc_client_ops - Callbacks operating on a client instance
 * @event: A notification to the client that the device has an active interrupt
 */
struct kcs_bmc_client_ops {
	irqreturn_t (*event)(struct kcs_bmc_client *client);
};

/**
 * struct kcs_bmc_client - Associates a KCS protocol implementation with a KCS device
 * @ops: A set of callbacks for handling client events
 * @drv: The KCS protocol implementation associated with the client instance
 * @dev: The KCS device instance associated with the client instance
 * @entry: A list node for the KCS core to track KCS client instances
 *
 * A ``struct kcs_bmc_client`` should be created for each device added via
 * &kcs_bmc_driver_ops.add_device
 */
struct kcs_bmc_client {
	const struct kcs_bmc_client_ops *ops;

	struct kcs_bmc_driver *drv;
	struct kcs_bmc_device *dev;
	struct list_head entry;
};

/**
 * struct kcs_bmc_driver_ops - KCS device lifecycle operations for a KCS protocol driver
 * @add_device: A new device has appeared, a client instance is to be created
 * @remove_device: A known device has been removed - a client instance should be destroyed
 */
struct kcs_bmc_driver_ops {
	struct kcs_bmc_client *(*add_device)(struct kcs_bmc_driver *drv,
					     struct kcs_bmc_device *dev);
	void (*remove_device)(struct kcs_bmc_client *client);
};

/**
 * kcs_bmc_client_init() - Initialise an instance of &struct kcs_bmc_client
 * @client: The &struct kcs_bmc_client instance of interest, usually embedded in
 *          an instance-private struct
 * @ops: The &struct kcs_bmc_client_ops relevant for @client
 * @drv: The &struct kcs_bmc_driver instance relevant for @client
 * @dev: The &struct kcs_bmc_device instance relevant for @client
 *
 * It's intended that kcs_bmc_client_init() is invoked in the @add_device
 * callback for the protocol driver where the protocol-private data is
 * initialised for the new device instance. The function is provided to make
 * sure that all required fields are initialised.
 *
 * Context: Any context
 */
static inline void kcs_bmc_client_init(struct kcs_bmc_client *client,
				       const struct kcs_bmc_client_ops *ops,
				       struct kcs_bmc_driver *drv,
				       struct kcs_bmc_device *dev)
{
	client->ops = ops;
	client->drv = drv;
	client->dev = dev;
}

/**
 * struct kcs_bmc_driver - An implementation of a protocol run over a KCS channel
 * @entry: A list node for the KCS core to track KCS protocol drivers
 * @ops: A set of callbacks for handling device lifecycle events for the protocol driver
 */
struct kcs_bmc_driver {
	struct list_head entry;

	const struct kcs_bmc_driver_ops *ops;
};

/**
 * kcs_bmc_register_driver() - Register a KCS protocol driver with the KCS subsystem
 * @drv: The &struct kcs_bmc_driver instance to register
 *
 * Generally only invoked on module init.
 *
 * Context: Process context. Takes and releases the internal KCS subsystem mutex.
 *
 * Return: 0 on succes.
 */
int kcs_bmc_register_driver(struct kcs_bmc_driver *drv);

/**
 * kcs_bmc_unregister_driver() - Unregister a KCS protocol driver with the KCS
 *                               subsystem
 * @drv: The &struct kcs_bmc_driver instance to unregister
 *
 * Generally only invoked on module exit.
 *
 * Context: Process context. Takes and releases the internal KCS subsystem mutex.
 */
void kcs_bmc_unregister_driver(struct kcs_bmc_driver *drv);

/**
 * module_kcs_bmc_driver() - Helper macro for registering a module KCS protocol driver
 * @__kcs_bmc_driver: A ``struct kcs_bmc_driver``
 *
 * Helper macro for KCS protocol drivers which do not do anything special in
 * module init/exit. This eliminates a lot of boilerplate. Each module may only
 * use this macro once, and calling it replaces module_init() and module_exit()
 */
#define module_kcs_bmc_driver(__kcs_bmc_driver) \
	module_driver(__kcs_bmc_driver, kcs_bmc_register_driver, \
		kcs_bmc_unregister_driver)

/**
 * kcs_bmc_enable_device() - Prepare a KCS device for active use
 * @client: The client whose KCS device should be enabled
 *
 * A client should enable its associated KCS device when the userspace
 * interface for the client is "opened" in some fashion. Enabling the KCS device
 * associates the client with the device and enables interrupts on the hardware.
 *
 * Context: Process context. Takes and releases ``client->dev->lock``
 *
 * Return: 0 on success, or -EBUSY if a client is already associated with the
 *         device
 */
int kcs_bmc_enable_device(struct kcs_bmc_client *client);

/**
 * kcs_bmc_disable_device() - Remove a KCS device from active use
 * @client: The client whose KCS device should be disabled
 *
 * A client should disable its associated KCS device when the userspace
 * interface for the client is "closed" in some fashion. The client is
 * disassociated from the device iff it was the active client. If the client is
 * disassociated then interrupts are disabled on the hardware.
 *
 * Context: Process context. Takes and releases ``client->dev->lock``.
 */
void kcs_bmc_disable_device(struct kcs_bmc_client *client);

/**
 * kcs_bmc_read_data() - Read the Input Data Register (IDR) on a KCS device
 * @client: The client whose device's IDR should be read
 *
 * Must only be called on a client that is currently active on its associated
 * device.
 *
 * Context: Any context. Any spinlocks taken are also released.
 *
 * Return: The value of IDR
 */
u8 kcs_bmc_read_data(struct kcs_bmc_client *client);

/**
 * kcs_bmc_write_data() - Write the Output Data Register (ODR) on a KCS device
 * @client: The client whose device's ODR should be written
 * @data: The value to write to ODR
 *
 * Must only be called on a client that is currently active on its associated
 * device.
 *
 * Context: Any context. Any spinlocks taken are also released.
 */
void kcs_bmc_write_data(struct kcs_bmc_client *client, u8 data);

/**
 * kcs_bmc_read_status() - Read the Status Register (STR) on a KCS device
 * @client: The client whose device's STR should be read
 *
 * Must only be called on a client that is currently active on its associated
 * device.
 *
 * Context: Any context. Any spinlocks taken are also released.
 *
 * Return: The value of STR
 */
u8 kcs_bmc_read_status(struct kcs_bmc_client *client);

/**
 * kcs_bmc_write_status() - Write the Status Register (STR) on a KCS device
 * @client: The client whose device's STR should be written
 * @data: The value to write to STR
 *
 * Must only be called on a client that is currently active on its associated
 * device.
 *
 * Context: Any context. Any spinlocks taken are also released.
 */
void kcs_bmc_write_status(struct kcs_bmc_client *client, u8 data);

/**
 * kcs_bmc_update_status() - Update Status Register (STR) on a KCS device
 * @client: The client whose device's STR should be updated
 * @mask: A bit-mask defining the field in STR that should be updated
 * @val: The new value of the field, specified in the position of the bit-mask
 *       defined by @mask
 *
 * Must only be called on a client that is currently active on its associated
 * device.
 *
 * Context: Any context. Any spinlocks taken are also released.
 */
void kcs_bmc_update_status(struct kcs_bmc_client *client, u8 mask, u8 val);
#endif
