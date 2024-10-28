// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/device.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include "internal.h"

#ifdef CONFIG_PRINTK
static struct class *console_class;

static const char *
console_effective_loglevel_source_str(const struct console *con)
{
	enum loglevel_source source;
	const char *str;

	source = console_effective_loglevel_source(con);

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
		pr_warn("Unhandled console loglevel source: %d", source);
		str = "unknown";
		break;
	}

	return str;
}

static ssize_t loglevel_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct console *con = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(con->level));
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

	if (level == -1)
		goto out;

	if (clamp_loglevel(level) != level)
		return -ERANGE;

out:
	WRITE_ONCE(con->level, level);

	return size;
}

static DEVICE_ATTR_RW(loglevel);

static ssize_t effective_loglevel_source_show(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct console *con = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n",
			  console_effective_loglevel_source_str(con));
}

static DEVICE_ATTR_RO(effective_loglevel_source);

static ssize_t effective_loglevel_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct console *con = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", console_effective_loglevel(con));
}

static DEVICE_ATTR_RO(effective_loglevel);

static ssize_t enabled_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct console *con = dev_get_drvdata(dev);
	int cookie;
	bool enabled;

	cookie = console_srcu_read_lock();
	enabled = console_srcu_read_flags(con) & CON_ENABLED;
	console_srcu_read_unlock(cookie);
	return sysfs_emit(buf, "%d\n", enabled);
}

static DEVICE_ATTR_RO(enabled);

static struct attribute *console_sysfs_attrs[] = {
	&dev_attr_loglevel.attr,
	&dev_attr_effective_loglevel_source.attr,
	&dev_attr_effective_loglevel.attr,
	&dev_attr_enabled.attr,
	NULL,
};

ATTRIBUTE_GROUPS(console_sysfs);

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
	if (IS_ERR_OR_NULL(console_class))
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
	con->classdev->class = console_class;
	if (device_add(con->classdev))
		put_device(con->classdev);
}

void console_setup_class(void)
{
	struct console *con;
	int cookie;

	/*
	 * printk exists for the lifetime of the kernel, it cannot be unloaded,
	 * so we should never end up back in here.
	 */
	if (WARN_ON(console_class))
		return;

	console_class = class_create("console");
	if (!IS_ERR(console_class))
		console_class->dev_groups = console_sysfs_groups;

	cookie = console_srcu_read_lock();
	for_each_console_srcu(con)
		console_register_device(con);
	console_srcu_read_unlock(cookie);
}
#else /* CONFIG_PRINTK */
void console_register_device(struct console *new)
{
}
void console_setup_class(void)
{
}
#endif
