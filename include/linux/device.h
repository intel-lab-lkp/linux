// SPDX-License-Identifier: GPL-2.0
/*
 * device.h - generic, centralized driver model
 *
 * Copyright (c) 2001-2003 Patrick Mochel <mochel@osdl.org>
 * Copyright (c) 2004-2009 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2008-2009 Novell Inc.
 *
 * See Documentation/driver-api/driver-model/ for more information.
 */

#ifndef _DEVICE_H_
#define _DEVICE_H_

#include <linux/dev_printk.h>
#include <linux/energy_model.h>
#include <linux/ioport.h>
#include <linux/kobject.h>
#include <linux/klist.h>
#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/atomic.h>
#include <linux/uidgid.h>
#include <linux/gfp.h>
#include <linux/device/bus.h>
#include <linux/device/class.h>
#include <linux/device/devlink.h>
#include <linux/device/devres.h>
#include <linux/device/driver.h>
#include <linux/device/types.h>
#include <linux/cleanup.h>

struct device_driver;
struct module;
struct class;
struct subsys_private;
struct device_node;
struct fwnode_handle;

/**
 * struct subsys_interface - interfaces to device functions
 * @name:       name of the device function
 * @subsys:     subsystem of the devices to attach to
 * @node:       the list of functions registered at the subsystem
 * @add_dev:    device hookup to device function handler
 * @remove_dev: device hookup to device function handler
 *
 * Simple interfaces attached to a subsystem. Multiple interfaces can
 * attach to a subsystem and its devices. Unlike drivers, they do not
 * exclusively claim or control devices. Interfaces usually represent
 * a specific functionality of a subsystem/class of devices.
 */
struct subsys_interface {
	const char *name;
	const struct bus_type *subsys;
	struct list_head node;
	int (*add_dev)(struct device *dev, struct subsys_interface *sif);
	void (*remove_dev)(struct device *dev, struct subsys_interface *sif);
};

int subsys_interface_register(struct subsys_interface *sif);
void subsys_interface_unregister(struct subsys_interface *sif);

int subsys_system_register(const struct bus_type *subsys,
			   const struct attribute_group **groups);
int subsys_virtual_register(const struct bus_type *subsys,
			    const struct attribute_group **groups);

/**
 * struct device_attribute - Interface for exporting device attributes.
 * @attr: sysfs attribute definition.
 * @show: Show handler.
 * @store: Store handler.
 */
struct device_attribute {
	struct attribute	attr;
	ssize_t (*show)(struct device *dev, struct device_attribute *attr,
			char *buf);
	ssize_t (*store)(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count);
};

/**
 * struct dev_ext_attribute - Exported device attribute with extra context.
 * @attr: Exported device attribute.
 * @var: Pointer to context.
 */
struct dev_ext_attribute {
	struct device_attribute attr;
	void *var;
};

ssize_t device_show_ulong(struct device *dev, struct device_attribute *attr,
			  char *buf);
ssize_t device_store_ulong(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count);
ssize_t device_show_int(struct device *dev, struct device_attribute *attr,
			char *buf);
ssize_t device_store_int(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count);
ssize_t device_show_bool(struct device *dev, struct device_attribute *attr,
			char *buf);
ssize_t device_store_bool(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count);
ssize_t device_show_string(struct device *dev, struct device_attribute *attr,
			   char *buf);

/**
 * DEVICE_ATTR - Define a device attribute.
 * @_name: Attribute name.
 * @_mode: File mode.
 * @_show: Show handler. Optional, but mandatory if attribute is readable.
 * @_store: Store handler. Optional, but mandatory if attribute is writable.
 *
 * Convenience macro for defining a struct device_attribute.
 *
 * For example, ``DEVICE_ATTR(foo, 0644, foo_show, foo_store);`` expands to:
 *
 * .. code-block:: c
 *
 *	struct device_attribute dev_attr_foo = {
 *		.attr	= { .name = "foo", .mode = 0644 },
 *		.show	= foo_show,
 *		.store	= foo_store,
 *	};
 */
#define DEVICE_ATTR(_name, _mode, _show, _store) \
	struct device_attribute dev_attr_##_name = __ATTR(_name, _mode, _show, _store)

/**
 * DEVICE_ATTR_PREALLOC - Define a preallocated device attribute.
 * @_name: Attribute name.
 * @_mode: File mode.
 * @_show: Show handler. Optional, but mandatory if attribute is readable.
 * @_store: Store handler. Optional, but mandatory if attribute is writable.
 *
 * Like DEVICE_ATTR(), but ``SYSFS_PREALLOC`` is set on @_mode.
 */
#define DEVICE_ATTR_PREALLOC(_name, _mode, _show, _store) \
	struct device_attribute dev_attr_##_name = \
		__ATTR_PREALLOC(_name, _mode, _show, _store)

/**
 * DEVICE_ATTR_RW - Define a read-write device attribute.
 * @_name: Attribute name.
 *
 * Like DEVICE_ATTR(), but @_mode is 0644, @_show is <_name>_show,
 * and @_store is <_name>_store.
 */
#define DEVICE_ATTR_RW(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RW(_name)

/**
 * DEVICE_ATTR_ADMIN_RW - Define an admin-only read-write device attribute.
 * @_name: Attribute name.
 *
 * Like DEVICE_ATTR_RW(), but @_mode is 0600.
 */
#define DEVICE_ATTR_ADMIN_RW(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RW_MODE(_name, 0600)

/**
 * DEVICE_ATTR_RO - Define a readable device attribute.
 * @_name: Attribute name.
 *
 * Like DEVICE_ATTR(), but @_mode is 0444 and @_show is <_name>_show.
 */
#define DEVICE_ATTR_RO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RO(_name)

/**
 * DEVICE_ATTR_ADMIN_RO - Define an admin-only readable device attribute.
 * @_name: Attribute name.
 *
 * Like DEVICE_ATTR_RO(), but @_mode is 0400.
 */
#define DEVICE_ATTR_ADMIN_RO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RO_MODE(_name, 0400)

/**
 * DEVICE_ATTR_WO - Define an admin-only writable device attribute.
 * @_name: Attribute name.
 *
 * Like DEVICE_ATTR(), but @_mode is 0200 and @_store is <_name>_store.
 */
#define DEVICE_ATTR_WO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_WO(_name)

/**
 * DEVICE_ULONG_ATTR - Define a device attribute backed by an unsigned long.
 * @_name: Attribute name.
 * @_mode: File mode.
 * @_var: Identifier of unsigned long.
 *
 * Like DEVICE_ATTR(), but @_show and @_store are automatically provided
 * such that reads and writes to the attribute from userspace affect @_var.
 */
#define DEVICE_ULONG_ATTR(_name, _mode, _var) \
	struct dev_ext_attribute dev_attr_##_name = \
		{ __ATTR(_name, _mode, device_show_ulong, device_store_ulong), &(_var) }

/**
 * DEVICE_INT_ATTR - Define a device attribute backed by an int.
 * @_name: Attribute name.
 * @_mode: File mode.
 * @_var: Identifier of int.
 *
 * Like DEVICE_ULONG_ATTR(), but @_var is an int.
 */
#define DEVICE_INT_ATTR(_name, _mode, _var) \
	struct dev_ext_attribute dev_attr_##_name = \
		{ __ATTR(_name, _mode, device_show_int, device_store_int), &(_var) }

/**
 * DEVICE_BOOL_ATTR - Define a device attribute backed by a bool.
 * @_name: Attribute name.
 * @_mode: File mode.
 * @_var: Identifier of bool.
 *
 * Like DEVICE_ULONG_ATTR(), but @_var is a bool.
 */
#define DEVICE_BOOL_ATTR(_name, _mode, _var) \
	struct dev_ext_attribute dev_attr_##_name = \
		{ __ATTR(_name, _mode, device_show_bool, device_store_bool), &(_var) }

/**
 * DEVICE_STRING_ATTR_RO - Define a device attribute backed by a r/o string.
 * @_name: Attribute name.
 * @_mode: File mode.
 * @_var: Identifier of string.
 *
 * Like DEVICE_ULONG_ATTR(), but @_var is a string. Because the length of the
 * string allocation is unknown, the attribute must be read-only.
 */
#define DEVICE_STRING_ATTR_RO(_name, _mode, _var) \
	struct dev_ext_attribute dev_attr_##_name = \
		{ __ATTR(_name, (_mode) & ~0222, device_show_string, NULL), (_var) }

#define DEVICE_ATTR_IGNORE_LOCKDEP(_name, _mode, _show, _store) \
	struct device_attribute dev_attr_##_name =		\
		__ATTR_IGNORE_LOCKDEP(_name, _mode, _show, _store)

int device_create_file(struct device *device,
		       const struct device_attribute *entry);
void device_remove_file(struct device *dev,
			const struct device_attribute *attr);
bool device_remove_file_self(struct device *dev,
			     const struct device_attribute *attr);
int __must_check device_create_bin_file(struct device *dev,
					const struct bin_attribute *attr);
void device_remove_bin_file(struct device *dev,
			    const struct bin_attribute *attr);

/**
 * device_iommu_mapped - Returns true when the device DMA is translated
 *			 by an IOMMU
 * @dev: Device to perform the check on
 */
static inline bool device_iommu_mapped(struct device *dev)
{
	return (dev->iommu_group != NULL);
}

/* Get the wakeup routines, which depend on struct device */
#include <linux/pm_wakeup.h>

/**
 * dev_name - Return a device's name.
 * @dev: Device with name to get.
 * Return: The kobject name of the device, or its initial name if unavailable.
 */
static inline const char *dev_name(const struct device *dev)
{
	/* Use the init name until the kobject becomes available */
	if (dev->init_name)
		return dev->init_name;

	return kobject_name(&dev->kobj);
}

/**
 * dev_bus_name - Return a device's bus/class name, if at all possible
 * @dev: struct device to get the bus/class name of
 *
 * Will return the name of the bus/class the device is attached to.  If it is
 * not attached to a bus/class, an empty string will be returned.
 */
static inline const char *dev_bus_name(const struct device *dev)
{
	return dev->bus ? dev->bus->name : (dev->class ? dev->class->name : "");
}

__printf(2, 3) int dev_set_name(struct device *dev, const char *name, ...);

#ifdef CONFIG_NUMA
static inline int dev_to_node(struct device *dev)
{
	return dev->numa_node;
}
static inline void set_dev_node(struct device *dev, int node)
{
	dev->numa_node = node;
}
#else
static inline int dev_to_node(struct device *dev)
{
	return NUMA_NO_NODE;
}
static inline void set_dev_node(struct device *dev, int node)
{
}
#endif

static inline void *dev_get_drvdata(const struct device *dev)
{
	return dev->driver_data;
}

static inline void dev_set_drvdata(struct device *dev, void *data)
{
	dev->driver_data = data;
}

static inline struct pm_subsys_data *dev_to_psd(struct device *dev)
{
	return dev ? dev->power.subsys_data : NULL;
}

static inline unsigned int dev_get_uevent_suppress(const struct device *dev)
{
	return dev->kobj.uevent_suppress;
}

static inline void dev_set_uevent_suppress(struct device *dev, int val)
{
	dev->kobj.uevent_suppress = val;
}

static inline int device_is_registered(struct device *dev)
{
	return dev->kobj.state_in_sysfs;
}

static inline void device_enable_async_suspend(struct device *dev)
{
	if (!dev->power.is_prepared)
		dev->power.async_suspend = true;
}

static inline void device_disable_async_suspend(struct device *dev)
{
	if (!dev->power.is_prepared)
		dev->power.async_suspend = false;
}

static inline bool device_async_suspend_enabled(struct device *dev)
{
	return !!dev->power.async_suspend;
}

static inline bool device_pm_not_required(struct device *dev)
{
	return dev->power.no_pm;
}

static inline void device_set_pm_not_required(struct device *dev)
{
	dev->power.no_pm = true;
#ifdef CONFIG_PM
	dev->power.no_callbacks = true;
#endif
}

static inline void dev_pm_syscore_device(struct device *dev, bool val)
{
#ifdef CONFIG_PM_SLEEP
	dev->power.syscore = val;
#endif
}

static inline void dev_pm_set_driver_flags(struct device *dev, u32 flags)
{
	dev->power.driver_flags = flags;
}

static inline bool dev_pm_test_driver_flags(struct device *dev, u32 flags)
{
	return !!(dev->power.driver_flags & flags);
}

static inline bool dev_pm_smart_suspend(struct device *dev)
{
#ifdef CONFIG_PM_SLEEP
	return dev->power.smart_suspend;
#else
	return false;
#endif
}

/*
 * dev_pm_set_strict_midlayer - Update the device's power.strict_midlayer flag
 * @dev: Target device.
 * @val: New flag value.
 *
 * When set, power.strict_midlayer means that the middle layer power management
 * code (typically, a bus type or a PM domain) does not expect its runtime PM
 * suspend callback to be invoked at all during system-wide PM transitions and
 * it does not expect its runtime PM resume callback to be invoked at any point
 * when runtime PM is disabled for the device during system-wide PM transitions.
 */
static inline void dev_pm_set_strict_midlayer(struct device *dev, bool val)
{
#ifdef CONFIG_PM_SLEEP
	dev->power.strict_midlayer = val;
#endif
}

static inline bool dev_pm_strict_midlayer_is_set(struct device *dev)
{
#ifdef CONFIG_PM_SLEEP
	return dev->power.strict_midlayer;
#else
	return false;
#endif
}

static inline void device_lock(struct device *dev)
{
	mutex_lock(&dev->mutex);
}

static inline int device_lock_interruptible(struct device *dev)
{
	return mutex_lock_interruptible(&dev->mutex);
}

static inline int device_trylock(struct device *dev)
{
	return mutex_trylock(&dev->mutex);
}

static inline void device_unlock(struct device *dev)
{
	mutex_unlock(&dev->mutex);
}

DEFINE_GUARD(device, struct device *, device_lock(_T), device_unlock(_T))

static inline void device_lock_assert(struct device *dev)
{
	lockdep_assert_held(&dev->mutex);
}

static inline bool dev_has_sync_state(struct device *dev)
{
	if (!dev)
		return false;
	if (dev->driver && dev->driver->sync_state)
		return true;
	if (dev->bus && dev->bus->sync_state)
		return true;
	return false;
}

static inline int dev_set_drv_sync_state(struct device *dev,
					 void (*fn)(struct device *dev))
{
	if (!dev || !dev->driver)
		return 0;
	if (dev->driver->sync_state && dev->driver->sync_state != fn)
		return -EBUSY;
	if (!dev->driver->sync_state)
		dev->driver->sync_state = fn;
	return 0;
}

static inline void dev_set_removable(struct device *dev,
				     enum device_removable removable)
{
	dev->removable = removable;
}

static inline bool dev_is_removable(struct device *dev)
{
	return dev->removable == DEVICE_REMOVABLE;
}

static inline bool dev_removable_is_valid(struct device *dev)
{
	return dev->removable != DEVICE_REMOVABLE_NOT_SUPPORTED;
}

/*
 * High level routines for use by the bus drivers
 */
int __must_check device_register(struct device *dev);
void device_unregister(struct device *dev);
void device_initialize(struct device *dev);
int __must_check device_add(struct device *dev);
void device_del(struct device *dev);

DEFINE_FREE(device_del, struct device *, if (_T) device_del(_T))

int device_for_each_child(struct device *parent, void *data,
			  device_iter_t fn);
int device_for_each_child_reverse(struct device *parent, void *data,
				  device_iter_t fn);
int device_for_each_child_reverse_from(struct device *parent,
				       struct device *from, void *data,
				       device_iter_t fn);
struct device *device_find_child(struct device *parent, const void *data,
				 device_match_t match);
/**
 * device_find_child_by_name - device iterator for locating a child device.
 * @parent: parent struct device
 * @name: name of the child device
 *
 * This is similar to the device_find_child() function above, but it
 * returns a reference to a device that has the name @name.
 *
 * NOTE: you will need to drop the reference with put_device() after use.
 */
static inline struct device *device_find_child_by_name(struct device *parent,
						       const char *name)
{
	return device_find_child(parent, name, device_match_name);
}

/**
 * device_find_any_child - device iterator for locating a child device, if any.
 * @parent: parent struct device
 *
 * This is similar to the device_find_child() function above, but it
 * returns a reference to a child device, if any.
 *
 * NOTE: you will need to drop the reference with put_device() after use.
 */
static inline struct device *device_find_any_child(struct device *parent)
{
	return device_find_child(parent, NULL, device_match_any);
}

int device_rename(struct device *dev, const char *new_name);
int device_move(struct device *dev, struct device *new_parent,
		enum dpm_order dpm_order);
int device_change_owner(struct device *dev, kuid_t kuid, kgid_t kgid);

static inline bool device_supports_offline(struct device *dev)
{
	return dev->bus && dev->bus->offline && dev->bus->online;
}

#define __device_lock_set_class(dev, name, key)                        \
do {                                                                   \
	struct device *__d2 __maybe_unused = dev;                      \
	lock_set_class(&__d2->mutex.dep_map, name, key, 0, _THIS_IP_); \
} while (0)

/**
 * device_lock_set_class - Specify a temporary lock class while a device
 *			   is attached to a driver
 * @dev: device to modify
 * @key: lock class key data
 *
 * This must be called with the device_lock() already held, for example
 * from driver ->probe(). Take care to only override the default
 * lockdep_no_validate class.
 */
#ifdef CONFIG_LOCKDEP
#define device_lock_set_class(dev, key)                                    \
do {                                                                       \
	struct device *__d = dev;                                          \
	dev_WARN_ONCE(__d, !lockdep_match_class(&__d->mutex,               \
						&__lockdep_no_validate__), \
		 "overriding existing custom lock class\n");               \
	__device_lock_set_class(__d, #key, key);                           \
} while (0)
#else
#define device_lock_set_class(dev, key) __device_lock_set_class(dev, #key, key)
#endif

/**
 * device_lock_reset_class - Return a device to the default lockdep novalidate state
 * @dev: device to modify
 *
 * This must be called with the device_lock() already held, for example
 * from driver ->remove().
 */
#define device_lock_reset_class(dev) \
do { \
	struct device *__d __maybe_unused = dev;                       \
	lock_set_novalidate_class(&__d->mutex.dep_map, "&dev->mutex",  \
				  _THIS_IP_);                          \
} while (0)

void lock_device_hotplug(void);
void unlock_device_hotplug(void);
int lock_device_hotplug_sysfs(void);
int device_offline(struct device *dev);
int device_online(struct device *dev);

void set_primary_fwnode(struct device *dev, struct fwnode_handle *fwnode);
void set_secondary_fwnode(struct device *dev, struct fwnode_handle *fwnode);
void device_set_node(struct device *dev, struct fwnode_handle *fwnode);
int device_add_of_node(struct device *dev, struct device_node *of_node);
void device_remove_of_node(struct device *dev);
void device_set_of_node_from_dev(struct device *dev, const struct device *dev2);

static inline struct device_node *dev_of_node(struct device *dev)
{
	if (!IS_ENABLED(CONFIG_OF) || !dev)
		return NULL;
	return dev->of_node;
}

static inline int dev_num_vf(struct device *dev)
{
	if (dev->bus && dev->bus->num_vf)
		return dev->bus->num_vf(dev);
	return 0;
}

/*
 * Root device objects for grouping under /sys/devices
 */
struct device *__root_device_register(const char *name, struct module *owner);

/* This is a macro to avoid include problems with THIS_MODULE */
#define root_device_register(name) \
	__root_device_register(name, THIS_MODULE)

void root_device_unregister(struct device *root);

static inline void *dev_get_platdata(const struct device *dev)
{
	return dev->platform_data;
}

/*
 * Manual binding of a device to driver. See drivers/base/bus.c
 * for information on use.
 */
int __must_check device_driver_attach(const struct device_driver *drv,
				      struct device *dev);
int __must_check device_bind_driver(struct device *dev);
void device_release_driver(struct device *dev);
int  __must_check device_attach(struct device *dev);
int __must_check driver_attach(const struct device_driver *drv);
void device_initial_probe(struct device *dev);
int __must_check device_reprobe(struct device *dev);

bool device_is_bound(struct device *dev);

/*
 * Easy functions for dynamically creating devices on the fly
 */
__printf(5, 6) struct device *
device_create(const struct class *cls, struct device *parent, dev_t devt,
	      void *drvdata, const char *fmt, ...);
__printf(6, 7) struct device *
device_create_with_groups(const struct class *cls, struct device *parent, dev_t devt,
			  void *drvdata, const struct attribute_group **groups,
			  const char *fmt, ...);
void device_destroy(const struct class *cls, dev_t devt);

int __must_check device_add_groups(struct device *dev,
				   const struct attribute_group **groups);
void device_remove_groups(struct device *dev,
			  const struct attribute_group **groups);

static inline int __must_check device_add_group(struct device *dev,
					const struct attribute_group *grp)
{
	const struct attribute_group *groups[] = { grp, NULL };

	return device_add_groups(dev, groups);
}

static inline void device_remove_group(struct device *dev,
				       const struct attribute_group *grp)
{
	const struct attribute_group *groups[] = { grp, NULL };

	device_remove_groups(dev, groups);
}

int __must_check devm_device_add_group(struct device *dev,
				       const struct attribute_group *grp);

/*
 * get_device - atomically increment the reference count for the device.
 *
 */
struct device *get_device(struct device *dev);
void put_device(struct device *dev);

DEFINE_FREE(put_device, struct device *, if (_T) put_device(_T))

bool kill_device(struct device *dev);

#ifdef CONFIG_DEVTMPFS
int devtmpfs_mount(void);
#else
static inline int devtmpfs_mount(void) { return 0; }
#endif

/* drivers/base/power/shutdown.c */
void device_shutdown(void);

/* debugging and troubleshooting/diagnostic helpers. */
const char *dev_driver_string(const struct device *dev);

/* Create alias, so I can be autoloaded. */
#define MODULE_ALIAS_CHARDEV(major,minor) \
	MODULE_ALIAS("char-major-" __stringify(major) "-" __stringify(minor))
#define MODULE_ALIAS_CHARDEV_MAJOR(major) \
	MODULE_ALIAS("char-major-" __stringify(major) "-*")

#endif /* _DEVICE_H_ */
