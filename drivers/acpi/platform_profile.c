// SPDX-License-Identifier: GPL-2.0-or-later

/* Platform profile sysfs interface */

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/mutex.h>
#include <linux/platform_profile.h>
#include <linux/sysfs.h>

static DEFINE_MUTEX(profile_lock);

static const char * const profile_names[] = {
	[PLATFORM_PROFILE_LOW_POWER] = "low-power",
	[PLATFORM_PROFILE_COOL] = "cool",
	[PLATFORM_PROFILE_QUIET] = "quiet",
	[PLATFORM_PROFILE_BALANCED] = "balanced",
	[PLATFORM_PROFILE_BALANCED_PERFORMANCE] = "balanced-performance",
	[PLATFORM_PROFILE_PERFORMANCE] = "performance",
	[PLATFORM_PROFILE_CUSTOM] = "custom",
};
static_assert(ARRAY_SIZE(profile_names) == PLATFORM_PROFILE_LAST);

static DEFINE_IDR(platform_profile_minor_idr);

/**
 * _commmon_choices_show - Show the available profile choices
 * @choices: The available profile choices
 * @buf: The buffer to write to
 * Return: The number of bytes written
 */
static ssize_t _commmon_choices_show(unsigned long choices, char *buf)
{
	int i, len = 0;

	for_each_set_bit(i, &choices, PLATFORM_PROFILE_LAST) {
		if (len == 0)
			len += sysfs_emit_at(buf, len, "%s", profile_names[i]);
		else
			len += sysfs_emit_at(buf, len, " %s", profile_names[i]);
	}
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}

/**
 * _get_class_choices - Get the available profile choices for a class device
 * @dev: The class device
 * Return: The available profile choices
 */
static int _get_class_choices(struct device *dev, unsigned long *choices)
{
	struct platform_profile_handler *handler;
	int i;

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &profile_lock) {
		handler = dev_get_drvdata(dev);
		for_each_set_bit(i, handler->choices, PLATFORM_PROFILE_LAST)
			*choices |= BIT(i);
	}

	return 0;
}

/**
 * _store_class_profile - Set the profile for a class device
 * @dev: The class device
 * @data: The profile to set
 */
static int _store_class_profile(struct device *dev, void *data)
{
	enum platform_profile_option profile;
	unsigned long choices;
	int *i = (int *)data;
	int err;

	err = _get_class_choices(dev, &choices);
	if (err)
		return err;

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &profile_lock) {
		struct platform_profile_handler *handler;

		if (!test_bit(*i, &choices))
			return -EOPNOTSUPP;

		handler = dev_get_drvdata(dev);
		err = handler->profile_get(handler, &profile);
		if (err)
			return err;

		err = handler->profile_set(handler, *i);
		if (err) {
			int recover_err;

			dev_err(dev, "Failed to set profile: %d\n", err);
			recover_err = handler->profile_set(handler, profile);
			if (recover_err)
				dev_err(dev, "Failed to reset profile: %d\n", recover_err);
		}
		sysfs_notify(&handler->class_dev->kobj, NULL, "platform_profile");
		kobject_uevent(&handler->class_dev->kobj, KOBJ_CHANGE);
	}

	sysfs_notify(acpi_kobj, NULL, "platform_profile");
	return err ? err : 0;
}

/**
 * get_class_profile - Show the current profile for a class device
 * @dev: The class device
 * @profile: The profile to return
 * Return: 0 on success, -errno on failure
 */
static int get_class_profile(struct device *dev,
			     enum platform_profile_option *profile)
{
	struct platform_profile_handler *handler;
	enum platform_profile_option val;
	int err;

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &profile_lock) {
		handler = dev_get_drvdata(dev);
		err = handler->profile_get(handler, &val);
		if (err) {
			pr_err("Failed to get profile for handler %s\n", handler->name);
			return err;
		}
	}

	if (WARN_ON(val >= PLATFORM_PROFILE_LAST))
		return -EINVAL;
	*profile = val;

	return 0;
}

/**
 * name_show - Show the name of the profile handler
 * @dev: The device
 * @attr: The attribute
 * @buf: The buffer to write to
 * Return: The number of bytes written
 */
static ssize_t name_show(struct device *dev,
			 struct device_attribute *attr,
			 char *buf)
{
	struct platform_profile_handler *handler = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", handler->name);
}

/**
 * choices_show - Show the available profile choices
 * @dev: The device
 * @attr: The attribute
 * @buf: The buffer to write to
 */
static ssize_t choices_show(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	unsigned long choices = 0;
	int err;

	err = _get_class_choices(dev, &choices);
	if (err)
		return err;

	return _commmon_choices_show(choices, buf);
}

/**
 * profile_show - Show the current profile for a class device
 * @dev: The device
 * @attr: The attribute
 * @buf: The buffer to write to
 * Return: The number of bytes written
 */
static ssize_t profile_show(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	enum platform_profile_option profile = PLATFORM_PROFILE_LAST;
	int err;

	err = get_class_profile(dev, &profile);
	if (err)
		return err;

	return sysfs_emit(buf, "%s\n", profile_names[profile]);
}

/**
 * profile_store - Set the profile for a class device
 * @dev: The device
 * @attr: The attribute
 * @buf: The buffer to read from
 * @count: The number of bytes to read
 * Return: The number of bytes read
 */
static ssize_t profile_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	int i, ret;

	i = sysfs_match_string(profile_names, buf);
	if (i < 0)
		return -EINVAL;

	ret = _store_class_profile(dev, (void *)(long)&i);

	return ret ? ret : count;
}

static DEVICE_ATTR_RO(name);
static DEVICE_ATTR_RO(choices);
static DEVICE_ATTR_RW(profile);

static struct attribute *profile_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_choices.attr,
	&dev_attr_profile.attr,
	NULL
};
ATTRIBUTE_GROUPS(profile);

static const struct class platform_profile_class = {
	.name = "platform-profile",
	.dev_groups = profile_groups,
};

/**
 * _aggregate_choices - Aggregate the available profile choices
 * @dev: The device
 * @data: The available profile choices
 * Return: 0 on success, -errno on failure
 */
static int _aggregate_choices(struct device *dev, void *data)
{
	unsigned long *aggregate = data;
	unsigned long choices = 0;
	int err;

	err = _get_class_choices(dev, &choices);
	if (err)
		return err;

	if (!*aggregate)
		*aggregate = choices;
	else
		*aggregate &= choices;

	return 0;
}

/**
 * platform_profile_choices_show - Show the available profile choices for legacy sysfs interface
 * @dev: The device
 * @attr: The attribute
 * @buf: The buffer to write to
 * Return: The number of bytes written
 */
static ssize_t platform_profile_choices_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	unsigned long aggregate = 0;
	int err;

	err = class_for_each_device(&platform_profile_class, NULL,
				    &aggregate, _aggregate_choices);

	return _commmon_choices_show(aggregate, buf);
}

/**
 * _aggregate_profiles - Aggregate the profiles for legacy sysfs interface
 * @dev: The device
 * @data: The profile to return
 * Return: 0 on success, -errno on failure
 */
static int _aggregate_profiles(struct device *dev, void *data)
{
	enum platform_profile_option *profile = data;
	enum platform_profile_option val;
	int err;

	err = get_class_profile(dev, &val);
	if (err)
		return err;

	if (*profile != PLATFORM_PROFILE_LAST && *profile != val)
		*profile = PLATFORM_PROFILE_CUSTOM;
	else
		*profile = val;

	return 0;
}

/**
 * platform_profile_show - Show the current profile for legacy sysfs interface
 * @dev: The device
 * @attr: The attribute
 * @buf: The buffer to write to
 * Return: The number of bytes written
 */
static ssize_t platform_profile_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	enum platform_profile_option profile = PLATFORM_PROFILE_LAST;
	int err;

	err = class_for_each_device(&platform_profile_class, NULL,
				    &profile, _aggregate_profiles);

	return sysfs_emit(buf, "%s\n", profile_names[profile]);
}

/**
 * platform_profile_store - Set the profile for legacy sysfs interface
 * @dev: The device
 * @attr: The attribute
 * @buf: The buffer to read from
 * @count: The number of bytes to read
 * Return: The number of bytes read
 */
static ssize_t platform_profile_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	int ret;
	int i;

	/* Scan for a matching profile */
	i = sysfs_match_string(profile_names, buf);
	if (i < 0)
		return -EINVAL;
	if (i == PLATFORM_PROFILE_CUSTOM) {
		pr_warn("Custom profile not supported for legacy sysfs interface\n");
		return -EINVAL;
	}

	ret = class_for_each_device(&platform_profile_class, NULL, &i, _store_class_profile);

	return ret ? ret : count;
}

static DEVICE_ATTR_RO(platform_profile_choices);
static DEVICE_ATTR_RW(platform_profile);

static struct attribute *platform_profile_attrs[] = {
	&dev_attr_platform_profile_choices.attr,
	&dev_attr_platform_profile.attr,
	NULL
};

static const struct attribute_group platform_profile_group = {
	.attrs = platform_profile_attrs
};

void platform_profile_notify(void)
{
	guard(mutex)(&profile_lock);
	if (!class_is_registered(&platform_profile_class))
		return;
	sysfs_notify(acpi_kobj, NULL, "platform_profile");
}
EXPORT_SYMBOL_GPL(platform_profile_notify);

int platform_profile_cycle(void)
{
	enum platform_profile_option next = PLATFORM_PROFILE_LAST;
	enum platform_profile_option profile;
	unsigned long choices;
	int err;

	if (!class_is_registered(&platform_profile_class))
		return -ENODEV;

	err = class_for_each_device(&platform_profile_class, NULL,
				    &profile, _aggregate_profiles);
	if (err)
		return err;

	err = class_for_each_device(&platform_profile_class, NULL,
				    &choices, _aggregate_choices);
	if (err)
		return err;

	next = find_next_bit_wrap(&choices,
				  PLATFORM_PROFILE_LAST,
				  profile + 1);

	err = class_for_each_device(&platform_profile_class, NULL, &next,
				    _store_class_profile);

	if (err)
		return err;

	sysfs_notify(acpi_kobj, NULL, "platform_profile");

	return err;
}
EXPORT_SYMBOL_GPL(platform_profile_cycle);

int platform_profile_register(struct platform_profile_handler *pprof)
{
	int err;

	/* Sanity check the profile handler */
	if (!pprof || bitmap_empty(pprof->choices, PLATFORM_PROFILE_LAST) ||
	    !pprof->profile_set || !pprof->profile_get) {
		pr_err("platform_profile: handler is invalid\n");
		return -EINVAL;
	}
	if (!pprof->dev) {
		pr_err("platform_profile: handler device is not set\n");
		return -EINVAL;
	}

	guard(mutex)(&profile_lock);

	if (!class_is_registered(&platform_profile_class)) {
		/* class for individual handlers */
		err = class_register(&platform_profile_class);
		if (err)
			return err;
		/* legacy sysfs files */
		err = sysfs_create_group(acpi_kobj, &platform_profile_group);
		if (err)
			goto cleanup_class;
	}

	/* create class interface for individual handler */
	pprof->minor = idr_alloc(&platform_profile_minor_idr, pprof, 0, 0, GFP_KERNEL);
	pprof->class_dev = device_create(&platform_profile_class, pprof->dev,
					 MKDEV(0, pprof->minor), NULL, "platform-profile-%d",
					 pprof->minor);
	if (IS_ERR(pprof->class_dev))
		return PTR_ERR(pprof->class_dev);
	dev_set_drvdata(pprof->class_dev, pprof);

	sysfs_notify(acpi_kobj, NULL, "platform_profile");

	return 0;

cleanup_class:
	class_unregister(&platform_profile_class);

	return err;
}
EXPORT_SYMBOL_GPL(platform_profile_register);

int platform_profile_remove(struct platform_profile_handler *pprof)
{
	guard(mutex)(&profile_lock);

	sysfs_notify(acpi_kobj, NULL, "platform_profile");

	device_destroy(&platform_profile_class, MKDEV(0, pprof->minor));

	return 0;
}
EXPORT_SYMBOL_GPL(platform_profile_remove);

MODULE_AUTHOR("Mark Pearson <markpearson@lenovo.com>");
MODULE_DESCRIPTION("ACPI platform profile sysfs interface");
MODULE_LICENSE("GPL");
