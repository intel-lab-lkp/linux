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

#include <linux/device_types.h>
#include <linux/dev_printk.h>
#include <linux/energy_model.h>
#include <linux/ioport.h>
#include <linux/kobject_types.h>
#include <linux/klist.h>
#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/uidgid_types.h>
#include <linux/gfp_types.h>
#include <linux/overflow.h>
#include <linux/device/bus.h>
#include <linux/device/class.h>
#include <linux/device/driver.h>
#include <linux/cleanup.h>
#include <linux/sysfs.h> // for __ATTR_*

struct device;
struct device_private;
struct device_driver;
struct driver_private;
struct module;
struct class;
struct subsys_private;
struct device_node;
struct fwnode_handle;
struct iommu_group;
struct dev_pin_info;
struct dev_iommu;
struct msi_device_data;

int subsys_interface_register(struct subsys_interface *sif);
void subsys_interface_unregister(struct subsys_interface *sif);

int subsys_system_register(const struct bus_type *subsys,
			   const struct attribute_group **groups);
int subsys_virtual_register(const struct bus_type *subsys,
			    const struct attribute_group **groups);

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

/* device resource management */
typedef void (*dr_release_t)(struct device *dev, void *res);
typedef int (*dr_match_t)(struct device *dev, void *res, void *match_data);

void *__devres_alloc_node(dr_release_t release, size_t size, gfp_t gfp,
			  int nid, const char *name) __malloc;
#define devres_alloc(release, size, gfp) \
	__devres_alloc_node(release, size, gfp, NUMA_NO_NODE, #release)
#define devres_alloc_node(release, size, gfp, nid) \
	__devres_alloc_node(release, size, gfp, nid, #release)

void devres_for_each_res(struct device *dev, dr_release_t release,
			 dr_match_t match, void *match_data,
			 void (*fn)(struct device *, void *, void *),
			 void *data);
void devres_free(void *res);
void devres_add(struct device *dev, void *res);
void *devres_find(struct device *dev, dr_release_t release,
		  dr_match_t match, void *match_data);
void *devres_get(struct device *dev, void *new_res,
		 dr_match_t match, void *match_data);
void *devres_remove(struct device *dev, dr_release_t release,
		    dr_match_t match, void *match_data);
int devres_destroy(struct device *dev, dr_release_t release,
		   dr_match_t match, void *match_data);
int devres_release(struct device *dev, dr_release_t release,
		   dr_match_t match, void *match_data);

/* devres group */
void * __must_check devres_open_group(struct device *dev, void *id, gfp_t gfp);
void devres_close_group(struct device *dev, void *id);
void devres_remove_group(struct device *dev, void *id);
int devres_release_group(struct device *dev, void *id);

/* managed devm_k.alloc/kfree for device drivers */
void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp) __alloc_size(2);
void *devm_krealloc(struct device *dev, void *ptr, size_t size,
		    gfp_t gfp) __must_check __realloc_size(3);
__printf(3, 0) char *devm_kvasprintf(struct device *dev, gfp_t gfp,
				     const char *fmt, va_list ap) __malloc;
__printf(3, 4) char *devm_kasprintf(struct device *dev, gfp_t gfp,
				    const char *fmt, ...) __malloc;
static inline void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp)
{
	return devm_kmalloc(dev, size, gfp | __GFP_ZERO);
}
static inline void *devm_kmalloc_array(struct device *dev,
				       size_t n, size_t size, gfp_t flags)
{
	size_t bytes;

	if (unlikely(check_mul_overflow(n, size, &bytes)))
		return NULL;

	return devm_kmalloc(dev, bytes, flags);
}
static inline void *devm_kcalloc(struct device *dev,
				 size_t n, size_t size, gfp_t flags)
{
	return devm_kmalloc_array(dev, n, size, flags | __GFP_ZERO);
}
static inline __realloc_size(3, 4) void * __must_check
devm_krealloc_array(struct device *dev, void *p, size_t new_n, size_t new_size, gfp_t flags)
{
	size_t bytes;

	if (unlikely(check_mul_overflow(new_n, new_size, &bytes)))
		return NULL;

	return devm_krealloc(dev, p, bytes, flags);
}

void devm_kfree(struct device *dev, const void *p);
char *devm_kstrdup(struct device *dev, const char *s, gfp_t gfp) __malloc;
const char *devm_kstrdup_const(struct device *dev, const char *s, gfp_t gfp);
void *devm_kmemdup(struct device *dev, const void *src, size_t len, gfp_t gfp)
	__realloc_size(3);

unsigned long devm_get_free_pages(struct device *dev,
				  gfp_t gfp_mask, unsigned int order);
void devm_free_pages(struct device *dev, unsigned long addr);

#ifdef CONFIG_HAS_IOMEM
void __iomem *devm_ioremap_resource(struct device *dev,
				    const struct resource *res);
void __iomem *devm_ioremap_resource_wc(struct device *dev,
				       const struct resource *res);

void __iomem *devm_of_iomap(struct device *dev,
			    struct device_node *node, int index,
			    resource_size_t *size);
#else

static inline
void __iomem *devm_ioremap_resource(struct device *dev,
				    const struct resource *res)
{
	return ERR_PTR(-EINVAL);
}

static inline
void __iomem *devm_ioremap_resource_wc(struct device *dev,
				       const struct resource *res)
{
	return ERR_PTR(-EINVAL);
}

static inline
void __iomem *devm_of_iomap(struct device *dev,
			    struct device_node *node, int index,
			    resource_size_t *size)
{
	return ERR_PTR(-EINVAL);
}

#endif

/* allows to add/remove a custom action to devres stack */
void devm_remove_action(struct device *dev, void (*action)(void *), void *data);
void devm_release_action(struct device *dev, void (*action)(void *), void *data);

int __devm_add_action(struct device *dev, void (*action)(void *), void *data, const char *name);
#define devm_add_action(dev, action, data) \
	__devm_add_action(dev, action, data, #action)

static inline int __devm_add_action_or_reset(struct device *dev, void (*action)(void *),
					     void *data, const char *name)
{
	int ret;

	ret = __devm_add_action(dev, action, data, name);
	if (ret)
		action(data);

	return ret;
}
#define devm_add_action_or_reset(dev, action, data) \
	__devm_add_action_or_reset(dev, action, data, #action)

/**
 * devm_alloc_percpu - Resource-managed alloc_percpu
 * @dev: Device to allocate per-cpu memory for
 * @type: Type to allocate per-cpu memory for
 *
 * Managed alloc_percpu. Per-cpu memory allocated with this function is
 * automatically freed on driver detach.
 *
 * RETURNS:
 * Pointer to allocated memory on success, NULL on failure.
 */
#define devm_alloc_percpu(dev, type)      \
	((typeof(type) __percpu *)__devm_alloc_percpu((dev), sizeof(type), \
						      __alignof__(type)))

void __percpu *__devm_alloc_percpu(struct device *dev, size_t size,
				   size_t align);
void devm_free_percpu(struct device *dev, void __percpu *pdata);

/*
 * Device link flags.
 *
 * STATELESS: The core will not remove this link automatically.
 * AUTOREMOVE_CONSUMER: Remove the link automatically on consumer driver unbind.
 * PM_RUNTIME: If set, the runtime PM framework will use this link.
 * RPM_ACTIVE: Run pm_runtime_get_sync() on the supplier during link creation.
 * AUTOREMOVE_SUPPLIER: Remove the link automatically on supplier driver unbind.
 * AUTOPROBE_CONSUMER: Probe consumer driver automatically after supplier binds.
 * MANAGED: The core tracks presence of supplier/consumer drivers (internal).
 * SYNC_STATE_ONLY: Link only affects sync_state() behavior.
 * INFERRED: Inferred from data (eg: firmware) and not from driver actions.
 */
#define DL_FLAG_STATELESS		BIT(0)
#define DL_FLAG_AUTOREMOVE_CONSUMER	BIT(1)
#define DL_FLAG_PM_RUNTIME		BIT(2)
#define DL_FLAG_RPM_ACTIVE		BIT(3)
#define DL_FLAG_AUTOREMOVE_SUPPLIER	BIT(4)
#define DL_FLAG_AUTOPROBE_CONSUMER	BIT(5)
#define DL_FLAG_MANAGED			BIT(6)
#define DL_FLAG_SYNC_STATE_ONLY		BIT(7)
#define DL_FLAG_INFERRED		BIT(8)
#define DL_FLAG_CYCLE			BIT(9)

#define kobj_to_dev(__kobj)	container_of_const(__kobj, struct device, kobj)

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

static inline struct irq_domain *dev_get_msi_domain(const struct device *dev)
{
#ifdef CONFIG_GENERIC_MSI_IRQ
	return dev->msi.domain;
#else
	return NULL;
#endif
}

static inline void dev_set_msi_domain(struct device *dev, struct irq_domain *d)
{
#ifdef CONFIG_GENERIC_MSI_IRQ
	dev->msi.domain = d;
#endif
}

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

static inline struct device_node *dev_of_node(struct device *dev)
{
	if (!IS_ENABLED(CONFIG_OF) || !dev)
		return NULL;
	return dev->of_node;
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

int device_for_each_child(struct device *dev, void *data,
			  int (*fn)(struct device *dev, void *data));
int device_for_each_child_reverse(struct device *dev, void *data,
				  int (*fn)(struct device *dev, void *data));
struct device *device_find_child(struct device *dev, void *data,
				 int (*match)(struct device *dev, void *data));
struct device *device_find_child_by_name(struct device *parent,
					 const char *name);
struct device *device_find_any_child(struct device *parent);

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
void device_set_of_node_from_dev(struct device *dev, const struct device *dev2);
void device_set_node(struct device *dev, struct fwnode_handle *fwnode);

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
int __must_check device_driver_attach(struct device_driver *drv,
				      struct device *dev);
int __must_check device_bind_driver(struct device *dev);
void device_release_driver(struct device *dev);
int  __must_check device_attach(struct device *dev);
int __must_check driver_attach(struct device_driver *drv);
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

	return device_remove_groups(dev, groups);
}

int __must_check devm_device_add_groups(struct device *dev,
					const struct attribute_group **groups);
int __must_check devm_device_add_group(struct device *dev,
				       const struct attribute_group *grp);

/*
 * Platform "fixup" functions - allow the platform to have their say
 * about devices and actions that the general device layer doesn't
 * know about.
 */
/* Notify platform of device discovery */
extern int (*platform_notify)(struct device *dev);

extern int (*platform_notify_remove)(struct device *dev);


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

/* Device links interface. */
struct device_link *device_link_add(struct device *consumer,
				    struct device *supplier, u32 flags);
void device_link_del(struct device_link *link);
void device_link_remove(void *consumer, struct device *supplier);
void device_links_supplier_sync_state_pause(void);
void device_links_supplier_sync_state_resume(void);

/* Create alias, so I can be autoloaded. */
#define MODULE_ALIAS_CHARDEV(major,minor) \
	MODULE_ALIAS("char-major-" __stringify(major) "-" __stringify(minor))
#define MODULE_ALIAS_CHARDEV_MAJOR(major) \
	MODULE_ALIAS("char-major-" __stringify(major) "-*")

#endif /* _DEVICE_H_ */
