// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/device.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include "internal.h"

/**
 * console_sysfs_read_loglevel - Locklessly read the console specific loglevel
 *				 when accessing the related sysfs interface
 * @con:	struct console pointer of console to read loglevel from
 *
 * Locklessly reading @con->level provides a consistent read value because
 * there is at most one CPU modifying @con->level and that CPU is using only
 * read-modify-write operations to do so.
 *
 * Only use this function to read the loglevel via the related sysfs interface.
 * The sysfs API makes sure that the structure cannot disappear while the
 * interface is used.
 *
 * Context: Sysfs interface for the given console.
 * Return: The current value of the @con->level field.
 */
static inline int console_sysfs_read_loglevel(const struct console *con)
{
	/*
	 * The READ_ONCE() matches the WRITE_ONCE() when @level is modified
	 * for registered consoles.
	 */
	return data_race(READ_ONCE(con->level));
}

/**
 * console_sysfs_write_loglevel - Write the console specific loglevel via
 *				  sysfs interface.
 * @con:	struct console pointer of console to write loglevel to
 * @con_level:	new loglevel value to write
 *
 * Only use this function to write the loglevel via the related sysfs interface.
 * The sysfs API makes sure that the structure cannot disappear while the
 * interface is used.
 *
 * Context: Any context.
 */
static inline void console_sysfs_write_loglevel(struct console *con, int con_level)
{
	/* This matches the READ_ONCE() in console_sysfs_read_loglevel(). */
	WRITE_ONCE(con->level, con_level);
}

/**
 * console_effective_loglevel_source_str - Get string name of loglevel source
 *
 * @con:	The console to query
 *
 * Returns a human-readable string describing the source of the console's
 * effective loglevel (e.g., "local", "global", "ignore_loglevel").
 *
 * Return: String name of the loglevel source
 */
static const char *
console_effective_loglevel_source_str(const struct console *con)
{
	enum loglevel_source source;
	const char *str;
	int con_level;

	con_level = console_sysfs_read_loglevel(con);
	source = console_effective_loglevel_source(con_level);

	switch (source) {
	case LLS_IGNORE_LOGLEVEL:
		str = "ignore_loglevel";
		break;
	case LLS_LOCAL:
		str = "local";
		break;
	case LLS_GLOBAL:
		str = "global";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

static ssize_t effective_loglevel_source_show(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct console *con = dev_get_drvdata(dev);
	const char *str;

	str = console_effective_loglevel_source_str(con);
	return sysfs_emit(buf, "%s\n", str);
}

static DEVICE_ATTR_RO(effective_loglevel_source);

static ssize_t effective_loglevel_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct console *con = dev_get_drvdata(dev);
	int con_level;

	con_level = console_sysfs_read_loglevel(con);
	return sysfs_emit(buf, "%d\n", console_effective_loglevel(con_level));
}

static DEVICE_ATTR_RO(effective_loglevel);

static ssize_t loglevel_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct console *con = dev_get_drvdata(dev);
	int con_level;

	con_level = console_sysfs_read_loglevel(con);
	return sysfs_emit(buf, "%d\n", con_level);
}

static ssize_t loglevel_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t size)
{
	struct console *con = dev_get_drvdata(dev);
	ssize_t ret;
	int level;

	ret = kstrtoint(buf, 10, &level);
	if (ret < 0)
		return ret;

	/* -1 means "use global loglevel" */
	if (level == -1)
		goto out;

	/*
	 * Reject level 0 (KERN_EMERG) - per-console loglevel must be > 0.
	 * Emergency messages should go to all consoles, so they cannot be
	 * filtered per-console.
	 */
	if (level == 0)
		return -ERANGE;

	if (console_clamp_loglevel(level) != level)
		return -ERANGE;

	/*
	 * If the system has a minimum console loglevel set (via sysctl or
	 * kernel parameter), enforce it. This prevents setting per-console
	 * loglevels below the system minimum.
	 */
	if (minimum_console_loglevel > CONSOLE_LOGLEVEL_MIN &&
	    level < minimum_console_loglevel)
		return -ERANGE;

out:
	console_sysfs_write_loglevel(con, level);
	return size;
}

static DEVICE_ATTR_RW(loglevel);

static struct attribute *console_sysfs_attrs[] = {
	&dev_attr_loglevel.attr,
	&dev_attr_effective_loglevel_source.attr,
	&dev_attr_effective_loglevel.attr,
	NULL,
};

ATTRIBUTE_GROUPS(console_sysfs);

static const struct class console_class = {
	.name = "console",
	.dev_groups = console_sysfs_groups,
};
static bool console_class_registered;

/**
 * console_classdev_release - Release callback for console class devices
 *
 * @dev:	The device being released
 *
 * Called when the last reference to a console class device is dropped.
 * Frees the memory allocated for the device structure.
 */
static void console_classdev_release(struct device *dev)
{
	kfree(dev);
}

/**
 * console_register_device - Register a console's sysfs class device
 *
 * @con:	The console to register
 *
 * Creates a sysfs class device for the given console under /sys/class/console/.
 * This enables userspace access to per-console attributes like loglevel.
 *
 * If called before the console class is registered (during early boot),
 * this function returns early and the device will be registered later
 * by console_setup_class().
 */
void console_register_device(struct console *con)
{
	/*
	 * We might be called from register_console() before the class is
	 * registered. If that happens, we'll take care of it in
	 * printk_late_init.
	 */
	if (!console_class_registered)
		return;

	if (WARN_ON(con->classdev))
		return;

	con->classdev = kzalloc(sizeof(struct device), GFP_KERNEL);
	if (!con->classdev)
		return;

	device_initialize(con->classdev);
	con->classdev->release = console_classdev_release;
	con->classdev->class = &console_class;
	dev_set_drvdata(con->classdev, con);
	if (dev_set_name(con->classdev, "%s%d", con->name, con->index)) {
		put_device(con->classdev);
		con->classdev = NULL;
		return;
	}

	/*
	 * This class device exists solely to expose attributes (like loglevel)
	 * and does not control physical power states. Power is managed by the
	 * underlying hardware device. Disable PM entirely to prevent the
	 * creation of confusing and unused power sysfs attributes.
	 */
	device_set_pm_not_required(con->classdev);

	if (device_add(con->classdev)) {
		put_device(con->classdev);
		con->classdev = NULL;
	}
}

/**
 * console_setup_class - Initialize the console sysfs class
 *
 * Registers the console class with sysfs and creates class devices for all
 * currently registered consoles. Called during late init after sysfs is
 * available.
 *
 * Consoles registered before this function is called will have their class
 * devices created here. Consoles registered afterwards will have their
 * devices created by console_register_device() during register_console().
 */
void console_setup_class(void)
{
	struct console *con;
	int cookie;
	int err;

	/*
	 * printk exists for the lifetime of the kernel, it cannot be unloaded,
	 * so we should never end up back in here.
	 */
	if (WARN_ON(console_class_registered))
		return;

	err = class_register(&console_class);
	if (err) {
		pr_err("console: failed to register class: %pe\n", ERR_PTR(err));
		return;
	}

	/*
	 * Take console_list_lock() before exposing the class globally.
	 * This ensures register_console() (which holds the lock) cannot
	 * see the class until it's fully initialised with dev_groups.
	 */
	console_list_lock();
	console_class_registered = true;
	cookie = console_srcu_read_lock();
	for_each_console_srcu(con)
		console_register_device(con);
	console_srcu_read_unlock(cookie);
	console_list_unlock();
}
