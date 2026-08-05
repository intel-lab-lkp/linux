// SPDX-License-Identifier: GPL-2.0+
/* Framework for MDIO devices, other than PHYs.
 *
 * Copyright (c) 2016 Andrew Lunn <andrew@lunn.ch>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mdio.h>
#include <linux/mii.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unistd.h>
#include <linux/property.h>
#include "phylib-internal.h"

/**
 * mdio_device_register_reset - Read and initialize the reset properties of
 *				an mdio device
 * @mdiodev: mdio_device structure
 *
 * Return: Zero if successful, negative error code on failure
 */
static int mdio_device_register_reset(struct mdio_device *mdiodev)
{
	struct reset_control *reset;
	int err;

	/* Deassert the optional reset signal */
	mdiodev->reset_gpio = gpiod_get_optional(&mdiodev->dev,
						 "reset", GPIOD_OUT_LOW);
	if (IS_ERR(mdiodev->reset_gpio)) {
		err = PTR_ERR(mdiodev->reset_gpio);
		mdiodev->reset_gpio = NULL;
		return err;
	}

	if (mdiodev->reset_gpio)
		gpiod_set_consumer_name(mdiodev->reset_gpio, "PHY reset");

	reset = reset_control_get_optional_exclusive(&mdiodev->dev, "phy");
	if (IS_ERR(reset)) {
		gpiod_put(mdiodev->reset_gpio);
		mdiodev->reset_gpio = NULL;
		return PTR_ERR(reset);
	}

	mdiodev->reset_ctrl = reset;

	/* Read optional firmware properties */
	device_property_read_u32(&mdiodev->dev, "reset-assert-us",
				 &mdiodev->reset_assert_delay);
	device_property_read_u32(&mdiodev->dev, "reset-deassert-us",
				 &mdiodev->reset_deassert_delay);

	return 0;
}

/**
 * mdio_device_unregister_reset - uninitialize the reset properties of
 *				  an mdio device
 * @mdiodev: mdio_device structure
 */
static void mdio_device_unregister_reset(struct mdio_device *mdiodev)
{
	gpiod_put(mdiodev->reset_gpio);
	mdiodev->reset_gpio = NULL;
	reset_control_put(mdiodev->reset_ctrl);
	mdiodev->reset_ctrl = NULL;
	mdiodev->reset_assert_delay = 0;
	mdiodev->reset_deassert_delay = 0;
}

void mdio_device_reset(struct mdio_device *mdiodev, int value)
{
	unsigned int d;

	if (!mdiodev->reset_gpio && !mdiodev->reset_ctrl)
		return;

	if (mdiodev->reset_state == value)
		return;

	if (mdiodev->reset_gpio)
		gpiod_set_value_cansleep(mdiodev->reset_gpio, value);

	if (mdiodev->reset_ctrl) {
		if (value)
			reset_control_assert(mdiodev->reset_ctrl);
		else
			reset_control_deassert(mdiodev->reset_ctrl);
	}

	d = value ? mdiodev->reset_assert_delay : mdiodev->reset_deassert_delay;
	if (d)
		fsleep(d);

	mdiodev->reset_state = value;
}
EXPORT_SYMBOL(mdio_device_reset);

void mdio_device_free(struct mdio_device *mdiodev)
{
	put_device(&mdiodev->dev);
}
EXPORT_SYMBOL(mdio_device_free);

static void mdio_device_release(struct device *dev)
{
	fwnode_handle_put(dev->fwnode);
	kfree(to_mdio_device(dev));
}

static int __mdio_device_remove(struct mdio_device *mdiodev, bool dynamic);

struct mdio_device *mdio_device_create(struct mii_bus *bus, int addr)
{
	struct mdio_device *mdiodev;

	/* We allocate the device, and initialize the default values */
	mdiodev = kzalloc_obj(*mdiodev);
	if (!mdiodev)
		return ERR_PTR(-ENOMEM);

	mdiodev->dev.release = mdio_device_release;
	mdiodev->dev.parent = &bus->dev;
	mdiodev->dev.bus = &mdio_bus_type;
	mdiodev->device_free = mdio_device_free;
	mdiodev->device_remove = __mdio_device_remove;
	mdiodev->bus = bus;
	mdiodev->addr = addr;
	mdiodev->reset_state = -1;

	dev_set_name(&mdiodev->dev, PHY_ID_FMT, bus->id, addr);

	device_initialize(&mdiodev->dev);

	return mdiodev;
}
EXPORT_SYMBOL(mdio_device_create);

/**
 * mdio_device_register - Register the mdio device on the MDIO bus
 * @mdiodev: mdio_device structure to be added to the MDIO bus
 *
 * Return: Zero if successful, negative error code on failure
 */
int mdio_device_register(struct mdio_device *mdiodev)
{
	int err;

	dev_dbg(&mdiodev->dev, "%s\n", __func__);

	err = mdiobus_register_device(mdiodev);
	if (err)
		return err;

	err = device_add(&mdiodev->dev);
	if (err)
		pr_err("MDIO %d failed to add\n", mdiodev->addr);

	return mdiobus_registration_done(mdiodev, err);
}
EXPORT_SYMBOL(mdio_device_register);

static int __mdio_device_remove(struct mdio_device *mdiodev, bool dynamic)
{
	int err;

	err = mdiobus_begin_remove(mdiodev, dynamic);
	if (err)
		return err;

	device_del(&mdiodev->dev);
	mdiobus_finish_remove(mdiodev, dynamic);

	return 0;
}

/**
 * mdio_device_remove - Remove a previously registered mdio device from the
 *			MDIO bus
 * @mdiodev: mdio_device structure to remove
 *
 * This doesn't free the mdio_device itself, it merely reverses the effects
 * of mdio_device_register(). Use mdio_device_free() to free the device
 * after calling this function.
 */
void mdio_device_remove(struct mdio_device *mdiodev)
{
	__mdio_device_remove(mdiodev, false);
}
EXPORT_SYMBOL(mdio_device_remove);

int mdiobus_register_device(struct mdio_device *mdiodev)
{
	struct mii_bus *bus = mdiodev->bus;
	int err;

	mutex_lock(&bus->mdio_map_lock);
	if (bus->state != MDIOBUS_REGISTERING &&
	    bus->state != MDIOBUS_REGISTERED) {
		err = -ENODEV;
		goto out_unlock;
	}
	if (bus->mdio_map_removing) {
		err = -EBUSY;
		goto out_unlock;
	}

	if (bus->mdio_map[mdiodev->addr] ||
	    bus->mdio_map_pending & BIT(mdiodev->addr)) {
		err = -EBUSY;
		goto out_unlock;
	}

	bus->mdio_map_pending |= BIT(mdiodev->addr);
	bus->mdio_map_ops++;
	mutex_unlock(&bus->mdio_map_lock);

	if (mdiodev->flags & MDIO_DEVICE_FLAG_PHY) {
		err = mdio_device_register_reset(mdiodev);
		if (err) {
			mdiobus_registration_done(mdiodev, err);
			return err;
		}

		/* Assert the reset signal */
		mdio_device_reset(mdiodev, 1);
	}

	return 0;

out_unlock:
	mutex_unlock(&bus->mdio_map_lock);
	return err;
}

/**
 * mdiobus_device_change_begin - start changing devices on a registered bus
 * @bus: MDIO bus that will be scanned or changed
 * @removing: whether to start an exclusive removal transaction
 *
 * Return: zero on success or a negative error code when the bus is unavailable
 */
int mdiobus_device_change_begin(struct mii_bus *bus, bool removing)
{
	int err = 0;

	mutex_lock(&bus->mdio_map_lock);
	if (bus->state != MDIOBUS_REGISTERED) {
		err = -ENODEV;
	} else if (bus->mdio_map_removing ||
		   (removing && bus->mdio_map_ops)) {
		err = -EBUSY;
	} else {
		bus->mdio_map_ops++;
		if (removing)
			bus->mdio_map_removing = true;
	}
	mutex_unlock(&bus->mdio_map_lock);

	return err;
}
EXPORT_SYMBOL_GPL(mdiobus_device_change_begin);

static void mdiobus_operation_done_locked(struct mii_bus *bus)
{
	lockdep_assert_held(&bus->mdio_map_lock);

	if (WARN_ON_ONCE(!bus->mdio_map_ops))
		return;
	bus->mdio_map_ops--;
	if (!bus->mdio_map_ops)
		wake_up_all(&bus->mdio_map_wait);
}

/**
 * mdiobus_device_change_end - finish changing devices on an MDIO bus
 * @bus: MDIO bus previously passed to mdiobus_device_change_begin()
 * @removing: value passed to mdiobus_device_change_begin()
 */
void mdiobus_device_change_end(struct mii_bus *bus, bool removing)
{
	mutex_lock(&bus->mdio_map_lock);
	if (removing) {
		WARN_ON_ONCE(!bus->mdio_map_removing);
		bus->mdio_map_removing = false;
	}
	mdiobus_operation_done_locked(bus);
	mutex_unlock(&bus->mdio_map_lock);
}
EXPORT_SYMBOL_GPL(mdiobus_device_change_end);

static void mdiobus_operation_done(struct mii_bus *bus)
{
	mutex_lock(&bus->mdio_map_lock);
	mdiobus_operation_done_locked(bus);
	mutex_unlock(&bus->mdio_map_lock);
}

static void mdiobus_unpublish_device(struct mdio_device *mdiodev)
{
	struct mii_bus *bus = mdiodev->bus;

	lockdep_assert_held(&bus->mdio_map_lock);

	if (bus->mdio_map[mdiodev->addr] == mdiodev)
		WRITE_ONCE(bus->mdio_map[mdiodev->addr], NULL);
	if (mdiodev->dev.of_node)
		of_node_clear_flag(mdiodev->dev.of_node, OF_POPULATED);
}

int mdiobus_registration_done(struct mdio_device *mdiodev, int err)
{
	struct mii_bus *bus = mdiodev->bus;

	mutex_lock(&bus->mdio_map_lock);
	if (WARN_ON_ONCE(!(bus->mdio_map_pending & BIT(mdiodev->addr))))
		goto out_unlock;

	if (err) {
		mdiobus_unpublish_device(mdiodev);
	} else {
		WARN_ON_ONCE(bus->mdio_map[mdiodev->addr]);
		/* Teardown waits for this operation before consuming the map. */
		smp_store_release(&bus->mdio_map[mdiodev->addr], mdiodev);
	}

	bus->mdio_map_pending &= ~BIT(mdiodev->addr);

out_unlock:
	mutex_unlock(&bus->mdio_map_lock);
	if (err) {
		if (mdiodev->flags & MDIO_DEVICE_FLAG_PHY) {
			mdio_device_reset(mdiodev, 1);
			mdio_device_unregister_reset(mdiodev);
		}
	}
	mdiobus_operation_done(bus);

	return err;
}

int mdiobus_begin_remove(struct mdio_device *mdiodev, bool dynamic)
{
	struct mii_bus *bus = mdiodev->bus;
	struct phy_device *phydev = NULL;
	int err = 0;

	mutex_lock(&bus->mdio_map_lock);
	if (dynamic && bus->state != MDIOBUS_REGISTERED) {
		err = -ENODEV;
		goto out_unlock;
	}
	if (bus->mdio_map_pending & BIT(mdiodev->addr)) {
		err = -EBUSY;
		goto out_unlock;
	}

	if (bus->mdio_map[mdiodev->addr] != mdiodev) {
		err = -ENODEV;
		goto out_unlock;
	}

	if (dynamic && mdiodev->flags & MDIO_DEVICE_FLAG_PHY) {
		phydev = to_phy_device(&mdiodev->dev);
		if (phydev->attached) {
			err = -EBUSY;
			goto out_unlock;
		}
	}

	mdiobus_unpublish_device(mdiodev);

	if (dynamic && mdiodev->flags & MDIO_DEVICE_FLAG_PHY) {
		mdio_device_get(mdiodev);
		list_add_tail(&phydev->retired_node,
			      &bus->mdio_map_retired_phys);
	}

out_unlock:
	mutex_unlock(&bus->mdio_map_lock);
	return err;
}

void mdiobus_finish_remove(struct mdio_device *mdiodev, bool dynamic)
{
	struct fwnode_handle *fwnode;

	if (mdiodev->flags & MDIO_DEVICE_FLAG_PHY)
		mdio_device_unregister_reset(mdiodev);

	/* Do not keep an overlay node alive with the retired device. */
	if (dynamic) {
		fwnode = dev_fwnode(&mdiodev->dev);
		device_set_node(&mdiodev->dev, NULL);
		fwnode_handle_put(fwnode);
	}
}

/**
 * mdio_probe - probe an MDIO device
 * @dev: device to probe
 *
 * Description: Take care of setting up the mdio_device structure
 * and calling the driver to probe the device.
 *
 * Return: Zero if successful, negative error code on failure
 */
static int mdio_probe(struct device *dev)
{
	struct mdio_device *mdiodev = to_mdio_device(dev);
	struct device_driver *drv = mdiodev->dev.driver;
	struct mdio_driver *mdiodrv = to_mdio_driver(drv);
	int err = 0;

	/* Deassert the reset signal */
	mdio_device_reset(mdiodev, 0);

	if (mdiodrv->probe) {
		err = mdiodrv->probe(mdiodev);
		if (err) {
			/* Assert the reset signal */
			mdio_device_reset(mdiodev, 1);
		}
	}

	return err;
}

static int mdio_remove(struct device *dev)
{
	struct mdio_device *mdiodev = to_mdio_device(dev);
	struct device_driver *drv = mdiodev->dev.driver;
	struct mdio_driver *mdiodrv = to_mdio_driver(drv);

	if (mdiodrv->remove)
		mdiodrv->remove(mdiodev);

	/* Assert the reset signal */
	mdio_device_reset(mdiodev, 1);

	return 0;
}

static void mdio_shutdown(struct device *dev)
{
	struct mdio_device *mdiodev = to_mdio_device(dev);
	struct device_driver *drv = mdiodev->dev.driver;
	struct mdio_driver *mdiodrv = to_mdio_driver(drv);

	if (mdiodrv->shutdown)
		mdiodrv->shutdown(mdiodev);
}

/**
 * mdio_driver_register - register an mdio_driver with the MDIO layer
 * @drv: new mdio_driver to register
 *
 * Return: Zero if successful, negative error code on failure
 */
int mdio_driver_register(struct mdio_driver *drv)
{
	struct mdio_driver_common *mdiodrv = &drv->mdiodrv;
	int retval;

	pr_debug("%s: %s\n", __func__, mdiodrv->driver.name);

	mdiodrv->driver.bus = &mdio_bus_type;
	mdiodrv->driver.probe = mdio_probe;
	mdiodrv->driver.remove = mdio_remove;
	mdiodrv->driver.shutdown = mdio_shutdown;

	retval = driver_register(&mdiodrv->driver);
	if (retval) {
		pr_err("%s: Error %d in registering driver\n",
		       mdiodrv->driver.name, retval);

		return retval;
	}

	return 0;
}
EXPORT_SYMBOL(mdio_driver_register);

void mdio_driver_unregister(struct mdio_driver *drv)
{
	struct mdio_driver_common *mdiodrv = &drv->mdiodrv;

	driver_unregister(&mdiodrv->driver);
}
EXPORT_SYMBOL(mdio_driver_unregister);
