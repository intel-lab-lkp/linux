// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/device.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include "internal.h"

static const char *
console_effective_loglevel_source_str(const struct console *con)
{
	enum loglevel_source source;
	const char *str;
	int con_level;
	int cookie;

	cookie = console_srcu_read_lock();
	con_level = console_srcu_read_loglevel(con);
	console_srcu_read_unlock(cookie);
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
	int cookie;

	cookie = console_srcu_read_lock();
	con_level = console_srcu_read_loglevel(con);
	console_srcu_read_unlock(cookie);
	return sysfs_emit(buf, "%d\n", console_effective_loglevel(con_level));
}

static DEVICE_ATTR_RO(effective_loglevel);

static ssize_t loglevel_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct console *con = dev_get_drvdata(dev);
	int con_level;
	int cookie;

	cookie = console_srcu_read_lock();
	con_level = console_srcu_read_loglevel(con);
	console_srcu_read_unlock(cookie);
	return sysfs_emit(buf, "%d\n", con_level);
}

static ssize_t loglevel_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t size)
{
	struct console *con = dev_get_drvdata(dev);
	ssize_t ret;
	int level;
	int cookie;

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
	cookie = console_srcu_read_lock();
	WRITE_ONCE(con->level, level);
	console_srcu_read_unlock(cookie);

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

static void console_classdev_release(struct device *dev)
{
	kfree(dev);
}

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
	dev_set_name(con->classdev, "%s%d", con->name, con->index);
	dev_set_drvdata(con->classdev, con);
	con->classdev->release = console_classdev_release;
	con->classdev->class = &console_class;

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
	if (err)
		return;

	/*
	 * Take console_list_lock() before exposing the class globally.
	 * This ensures register_console() (which holds the lock) cannot
	 * see the class until it's fully initialized with dev_groups.
	 */
	console_list_lock();
	console_class_registered = true;
	cookie = console_srcu_read_lock();
	for_each_console_srcu(con)
		console_register_device(con);
	console_srcu_read_unlock(cookie);
	console_list_unlock();
}
