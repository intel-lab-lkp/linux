// SPDX-License-Identifier: GPL-2.0-or-later

/* Platform profile sysfs interface */

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/mutex.h>
#include <linux/platform_profile.h>
#include <linux/sysfs.h>

static struct platform_profile_handler *cur_profile;
static DEFINE_MUTEX(profile_lock);

static const char * const profile_names[] = {
	[PLATFORM_PROFILE_LOW_POWER] = "low-power",
	[PLATFORM_PROFILE_COOL] = "cool",
	[PLATFORM_PROFILE_QUIET] = "quiet",
	[PLATFORM_PROFILE_BALANCED] = "balanced",
	[PLATFORM_PROFILE_BALANCED_PERFORMANCE] = "balanced-performance",
	[PLATFORM_PROFILE_PERFORMANCE] = "performance",
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


static DEVICE_ATTR_RO(name);
static DEVICE_ATTR_RO(choices);
static struct attribute *profile_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_choices.attr,
	NULL
};
ATTRIBUTE_GROUPS(profile);

static const struct class platform_profile_class = {
	.name = "platform-profile",
	.dev_groups = profile_groups,
};

static ssize_t platform_profile_choices_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	int len = 0;
	int i;

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &profile_lock) {
		if (!cur_profile)
			return -ENODEV;

		for_each_set_bit(i, cur_profile->choices, PLATFORM_PROFILE_LAST) {
			if (len == 0)
				len += sysfs_emit_at(buf, len, "%s", profile_names[i]);
			else
				len += sysfs_emit_at(buf, len, " %s", profile_names[i]);
		}
	}
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}

static ssize_t platform_profile_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	enum platform_profile_option profile = PLATFORM_PROFILE_BALANCED;
	int err;

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &profile_lock) {
		if (!cur_profile)
			return -ENODEV;

		err = cur_profile->profile_get(cur_profile, &profile);
		if (err)
			return err;
	}

	/* Check that profile is valid index */
	if (WARN_ON((profile < 0) || (profile >= ARRAY_SIZE(profile_names))))
		return -EIO;

	return sysfs_emit(buf, "%s\n", profile_names[profile]);
}

static ssize_t platform_profile_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	int err, i;

	/* Scan for a matching profile */
	i = sysfs_match_string(profile_names, buf);
	if (i < 0)
		return -EINVAL;

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &profile_lock) {
		if (!cur_profile)
			return -ENODEV;

		/* Check that platform supports this profile choice */
		if (!test_bit(i, cur_profile->choices))
			return -EOPNOTSUPP;

		err = cur_profile->profile_set(cur_profile, i);
		if (err)
			return err;
	}

	sysfs_notify(acpi_kobj, NULL, "platform_profile");
	return count;
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
	if (!cur_profile)
		return;
	if (!class_is_registered(&platform_profile_class))
		return;
	sysfs_notify(acpi_kobj, NULL, "platform_profile");
}
EXPORT_SYMBOL_GPL(platform_profile_notify);

int platform_profile_cycle(void)
{
	enum platform_profile_option profile;
	enum platform_profile_option next;
	int err;

	if (!class_is_registered(&platform_profile_class))
		return -ENODEV;

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &profile_lock) {
		if (!cur_profile)
			return -ENODEV;

		err = cur_profile->profile_get(cur_profile, &profile);
		if (err)
			return err;

		next = find_next_bit_wrap(cur_profile->choices, PLATFORM_PROFILE_LAST,
					  profile + 1);

		if (WARN_ON(next == PLATFORM_PROFILE_LAST))
			return -EINVAL;

		err = cur_profile->profile_set(cur_profile, next);
		if (err)
			return err;
	}

	sysfs_notify(acpi_kobj, NULL, "platform_profile");
	return 0;
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
	/* We can only have one active profile */
	if (cur_profile)
		return -EEXIST;

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

	cur_profile = pprof;
	return 0;

cleanup_class:
	class_unregister(&platform_profile_class);

	return err;
}
EXPORT_SYMBOL_GPL(platform_profile_register);

int platform_profile_remove(struct platform_profile_handler *pprof)
{
	guard(mutex)(&profile_lock);

	sysfs_remove_group(acpi_kobj, &platform_profile_group);

	device_destroy(&platform_profile_class, MKDEV(0, pprof->minor));

	cur_profile = NULL;
	return 0;
}
EXPORT_SYMBOL_GPL(platform_profile_remove);

MODULE_AUTHOR("Mark Pearson <markpearson@lenovo.com>");
MODULE_DESCRIPTION("ACPI platform profile sysfs interface");
MODULE_LICENSE("GPL");
