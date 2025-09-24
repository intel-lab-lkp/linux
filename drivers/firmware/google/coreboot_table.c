// SPDX-License-Identifier: GPL-2.0-only
/*
 * coreboot_table.c
 *
 * Module providing coreboot table access. This module creates a new bus type
 * ("coreboot") and a platform driver. The driver finds the coreboot table in
 * memory (via ACPI or Device Tree), parses it, and creates a new device on the
 * coreboot bus for each entry in the table. Other drivers can then register
 * with the coreboot bus to interact with these specific entries.
 *
 * Copyright 2017 Google Inc.
 * Copyright 2017 Samuel Holland <samuel@sholland.org>
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "coreboot_table.h"

/* Helper macro to get the coreboot_device from a struct device. */
#define CB_DEV(d) container_of(d, struct coreboot_device, dev)
/* Helper macro to get the coreboot_driver from a const struct device_driver. */
#define CB_DRV(d) container_of_const(d, struct coreboot_driver, drv)

/**
 * coreboot_bus_match - Match a coreboot device with a coreboot driver.
 * @dev: The coreboot device.
 * @drv: The coreboot device driver.
 *
 * This function is called by the driver core to determine if a driver can
 * handle a specific device. It iterates through the driver's ID table and
 * compares the coreboot entry tag of the device with the tags supported by
 * the driver.
 *
 * Return: 1 if the device's tag matches an ID in the driver's table,
 * 0 otherwise.
 */
static int coreboot_bus_match(struct device *dev, const struct device_driver *drv)
{
	struct coreboot_device *device = CB_DEV(dev);
	const struct coreboot_driver *driver = CB_DRV(drv);
	const struct coreboot_device_id *id;

	/* If the driver doesn't have an ID table, it can't match any device. */
	if (!driver->id_table)
		return 0;

	/* Iterate over the driver's supported tags. */
	for (id = driver->id_table; id->tag; id++) {
		if (device->entry.tag == id->tag)
			return 1; /* Found a match. */
	}

	return 0; /* No match found. */
}

/**
 * coreboot_bus_probe - Probe a coreboot device.
 * @dev: The coreboot device that was matched with a driver.
 *
 * This function is called by the driver core after a successful match.
 * It simply calls the driver's specific probe function, if it exists.
 *
 * Return: 0 on success, or an error code from the driver's probe function.
 */
static int coreboot_bus_probe(struct device *dev)
{
	int ret = -ENODEV;
	struct coreboot_device *device = CB_DEV(dev);
	struct coreboot_driver *driver = CB_DRV(dev->driver);

	if (driver->probe)
		ret = driver->probe(device);

	return ret;
}

/**
 * coreboot_bus_remove - Remove a coreboot device.
 * @dev: The coreboot device to remove.
 *
 * This function is called by the driver core when the device is being removed.
 * It calls the driver's specific remove function, if it exists.
 */
static void coreboot_bus_remove(struct device *dev)
{
	struct coreboot_device *device = CB_DEV(dev);
	struct coreboot_driver *driver = CB_DRV(dev->driver);

	if (driver->remove)
		driver->remove(device);
}

/**
 * coreboot_bus_uevent - Generate a uevent for a coreboot device.
 * @dev: The device for which to generate the event.
 * @env: The uevent environment variables.
 *
 * This function adds a MODALIAS environment variable to the uevent.
 * The MODALIAS is used by userspace (e.g., udev) to automatically load the
 * appropriate driver module for this device. The alias is formatted as
 * "coreboot:t<tag>", where <tag> is the 8-digit hexadecimal tag of the
 * coreboot table entry.
 *
 * Return: 0 on success, or an error code from add_uevent_var().
 */
static int coreboot_bus_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	struct coreboot_device *device = CB_DEV(dev);
	u32 tag = device->entry.tag;

	return add_uevent_var(env, "MODALIAS=coreboot:t%08X", tag);
}

/*
 * The bus_type structure for the coreboot bus.
 * This defines the bus's name and its core operations (match, probe, etc.).
 */
static const struct bus_type coreboot_bus_type = {
	.name		= "coreboot",
	.match		= coreboot_bus_match,
	.probe		= coreboot_bus_probe,
	.remove		= coreboot_bus_remove,
	.uevent		= coreboot_bus_uevent,
};

/**
 * coreboot_device_release - Release a coreboot_device structure.
 * @dev: The device to be released.
 *
 * This is the release function for coreboot devices. It is called by the
 * driver core when the last reference to the device is dropped. Its job is
_ to free the memory allocated for the coreboot_device.
 */
static void coreboot_device_release(struct device *dev)
{
	struct coreboot_device *device = CB_DEV(dev);

	kfree(device);
}

/**
 * __coreboot_driver_register - Register a coreboot driver.
 * @driver: The coreboot driver to register.
 * @owner: The module that owns this driver.
 *
 * A helper function to register a coreboot driver with the coreboot bus.
 * It sets up the bus and owner fields in the driver's device_driver struct
 * and then calls the generic driver_register function.
 *
 * Return: 0 on success, or an error code from driver_register().
 */
int __coreboot_driver_register(struct coreboot_driver *driver,
			       struct module *owner)
{
	driver->drv.bus = &coreboot_bus_type;
	driver->drv.owner = owner;

	return driver_register(&driver->drv);
}
EXPORT_SYMBOL(__coreboot_driver_register);

/**
 * coreboot_driver_unregister - Unregister a coreboot driver.
 * @driver: The coreboot driver to unregister.
 *
 * Unregisters the given driver from the coreboot bus.
 */
void coreboot_driver_unregister(struct coreboot_driver *driver)
{
	driver_unregister(&driver->drv);
}
EXPORT_SYMBOL(coreboot_driver_unregister);

/**
 * coreboot_table_populate - Parse the coreboot table and create devices.
 * @dev: The parent device (the platform_device for the coreboot table).
 * @ptr: A pointer to the mapped coreboot table in memory.
 *
 * This function iterates through all entries in the coreboot table. For each
 * entry, it allocates a `coreboot_device`, copies the entry data into it,
 * and registers it as a new device on the coreboot bus.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
static int coreboot_table_populate(struct device *dev, void *ptr)
{
	int i, ret;
	void *ptr_entry;
	struct coreboot_device *device;
	struct coreboot_table_entry *entry;
	struct coreboot_table_header *header = ptr;

	/* The first entry is located right after the header. */
	ptr_entry = ptr + header->header_bytes;

	/* Loop through all the entries specified in the header. */
	for (i = 0; i < header->table_entries; i++) {
		entry = ptr_entry;

		/* Basic sanity check for entry size. */
		if (entry->size < sizeof(*entry)) {
			dev_warn(dev, "coreboot table entry too small!\n");
			return -EINVAL;
		}

		/* Allocate memory for our device struct and the raw entry data. */
		device = kzalloc(sizeof(*device) + entry->size, GFP_KERNEL);
		if (!device)
			return -ENOMEM;

		/* Initialize the generic device fields. */
		device->dev.parent = dev;
		device->dev.bus = &coreboot_bus_type;
		device->dev.release = coreboot_device_release;
		/* Copy the raw coreboot table entry data. */
		memcpy(device->raw, ptr_entry, entry->size);

		/* Set a descriptive device name based on the entry tag. */
		switch (device->entry.tag) {
		case LB_TAG_CBMEM_ENTRY:
			/* CBMEM entries have a specific ID we can use for a unique name. */
			dev_set_name(&device->dev, "cbmem-%08x",
				     device->cbmem_entry.id);
			break;
		default:
			/* For other entries, just use a generic numbered name. */
			dev_set_name(&device->dev, "coreboot%d", i);
			break;
		}

		/* Register the new device with the driver core. */
		ret = device_register(&device->dev);
		if (ret) {
			/* If registration fails, clean up and bail out. */
			put_device(&device->dev); /* This will trigger release */
			return ret;
		}

		/* Move to the next entry in the table. */
		ptr_entry += entry->size;
	}

	return 0;
}

/**
 * coreboot_table_probe - Probe function for the coreboot platform driver.
 * @pdev: The platform device.
 *
 * This function is called when the platform bus finds a device that matches
 * this driver (e.g., via ACPI or Device Tree). It locates the coreboot table
 * in memory, verifies its signature, and then calls coreboot_table_populate()
 * to create devices for each entry.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
static int coreboot_table_probe(struct platform_device *pdev)
{
	resource_size_t len;
	struct coreboot_table_header *header;
	struct resource *res;
	struct device *dev = &pdev->dev;
	void *ptr;
	int ret;

	/* Get the memory region containing the coreboot table. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	len = resource_size(res);
	if (!res->start || !len)
		return -EINVAL;

	/*
	 * First, map only the header to perform a sanity check. This avoids
	 * mapping a potentially huge and invalid memory region.
	 */
	header = memremap(res->start, sizeof(*header), MEMREMAP_WB);
	if (!header)
		return -ENOMEM;

	/* Verify the "LBIO" signature. */
	ret = strncmp(header->signature, "LBIO", sizeof(header->signature));
	if (ret) {
		dev_warn(dev, "coreboot table missing or corrupt!\n");
		memunmap(header);
		return -ENODEV;
	}

	/* Get the full table size from the header. */
	len = header->header_bytes + header->table_bytes;
	memunmap(header);

	/* Now map the entire coreboot table. */
	ptr = memremap(res->start, len, MEMREMAP_WB);
	if (!ptr)
		return -ENOMEM;

	/* Populate the bus with devices from the table entries. */
	ret = coreboot_table_populate(dev, ptr);

	/* Unmap the memory region as it's no longer needed. */
	memunmap(ptr);

	return ret;
}

/**
 * __cb_dev_unregister - Helper function to unregister a device.
 * @dev: The device to unregister.
 * @dummy: Unused data pointer (required by bus_for_each_dev).
 *
 * This function is used as a callback for bus_for_each_dev to unregister
 * a single device.
 *
 * Return: Always 0.
 */
static int __cb_dev_unregister(struct device *dev, void *dummy)
{
	device_unregister(dev);
	return 0;
}

/**
 * coreboot_table_remove - Remove function for the coreboot platform driver.
 * @pdev: The platform device.
 *
 * This function is called when the platform device is being removed. It
 * cleans up by iterating over all devices on the coreboot bus and
 * unregistering them.
 */
static void coreboot_table_remove(struct platform_device *pdev)
{
	bus_for_each_dev(&coreboot_bus_type, NULL, NULL, __cb_dev_unregister);
}

#ifdef CONFIG_ACPI
/* ACPI device IDs that this platform driver can bind to. */
static const struct acpi_device_id cros_coreboot_acpi_match[] = {
	{ "GOOGCB00", 0 }, /* Google Coreboot device */
	{ "BOOT0000", 0 }, /* Coreboot device on older systems */
	{ }
};
MODULE_DEVICE_TABLE(acpi, cros_coreboot_acpi_match);
#endif

#ifdef CONFIG_OF
/* Device Tree compatible strings that this platform driver can bind to. */
static const struct of_device_id coreboot_of_match[] = {
	{ .compatible = "coreboot" },
	{}
};
MODULE_DEVICE_TABLE(of, coreboot_of_match);
#endif

/* The platform_driver structure for the main coreboot table driver. */
static struct platform_driver coreboot_table_driver = {
	.probe = coreboot_table_probe,
	.remove = coreboot_table_remove,
	.driver = {
		.name = "coreboot_table",
		/* Link the ACPI and Device Tree match tables. */
		.acpi_match_table = ACPI_PTR(cros_coreboot_acpi_match),
		.of_match_table = of_match_ptr(coreboot_of_match),
	},
};

/**
 * coreboot_table_driver_init - Module initialization function.
 *
 * This function is called when the module is loaded. It registers the
 * coreboot bus type and then registers the platform driver that finds
 * and parses the coreboot table.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
static int __init coreboot_table_driver_init(void)
{
	int ret;

	/* First, register our new bus type with the kernel. */
	ret = bus_register(&coreboot_bus_type);
	if (ret)
		return ret;

	/* Then, register the platform driver. */
	ret = platform_driver_register(&coreboot_table_driver);
	if (ret) {
		/* If driver registration fails, unregister the bus. */
		bus_unregister(&coreboot_bus_type);
		return ret;
	}

	return 0;
}

/**
 * coreboot_table_driver_exit - Module exit function.
 *
 * This function is called when the module is unloaded. It unregisters the
 * platform driver and the coreboot bus type, cleaning up all resources.
 */
static void __exit coreboot_table_driver_exit(void)
{
	platform_driver_unregister(&coreboot_table_driver);
	bus_unregister(&coreboot_bus_type);
}

/* Register the init and exit functions. */
module_init(coreboot_table_driver_init);
module_exit(coreboot_table_driver_exit);

/* Standard module information. */
MODULE_AUTHOR("Google, Inc.");
MODULE_DESCRIPTION("Module providing coreboot table access");
MODULE_LICENSE("GPL");
