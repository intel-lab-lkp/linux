// SPDX-License-Identifier: GPL-2.0-or-later

/* Platform profile sysfs interface */

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/platform_profile.h>
#include <linux/sysfs.h>

static struct platform_profile_handler *cur_profile;
static LIST_HEAD(platform_profile_handler_list);
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

static const struct class platform_profile_class = {
	.name = "platform-profile",
};

static bool platform_profile_is_registered(void)
{
	lockdep_assert_held(&profile_lock);
	return !list_empty(&platform_profile_handler_list);
}

static bool platform_profile_is_class_device(struct device *dev)
{
	return dev && dev->class == &platform_profile_class;
}

static unsigned long platform_profile_get_choices(struct device *dev)
{
	struct platform_profile_handler *handler;
	unsigned long aggregate = 0;
	int i;

	lockdep_assert_held(&profile_lock);
	list_for_each_entry(handler, &platform_profile_handler_list, list) {
		unsigned long individual = 0;

		/* if called from a class attribute then only match that one */
		if (platform_profile_is_class_device(dev) && handler->dev != dev->parent)
			continue;
		for_each_set_bit(i, handler->choices, PLATFORM_PROFILE_LAST)
			individual |= BIT(i);
		if (!aggregate)
			aggregate = individual;
		else
			aggregate &= individual;
	}

	return aggregate;
}

static int platform_profile_get_active(struct device *dev, enum platform_profile_option *profile)
{
	struct platform_profile_handler *handler;
	enum platform_profile_option active = PLATFORM_PROFILE_LAST;
	enum platform_profile_option val;
	int err;

	lockdep_assert_held(&profile_lock);
	list_for_each_entry(handler, &platform_profile_handler_list, list) {
		if (platform_profile_is_class_device(dev) && handler->dev != dev->parent)
			continue;
		err = handler->profile_get(handler, &val);
		if (err) {
			pr_err("Failed to get profile for handler %s\n", handler->name);
			return err;
		}

		if (WARN_ON(val >= PLATFORM_PROFILE_LAST))
			return -EINVAL;

		/*
		 * If the profiles are different for class devices then this must
		 * show "custom" to legacy sysfs interface
		 */
		if (active != val && active != PLATFORM_PROFILE_LAST) {
			*profile = PLATFORM_PROFILE_CUSTOM;
			return 0;
		}
		active = val;
	}

	*profile = active;

	return 0;
}

static ssize_t platform_profile_choices_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	unsigned long choices;
	int len = 0;
	int i;

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &profile_lock)
		choices = platform_profile_get_choices(dev);

	for_each_set_bit(i, &choices, PLATFORM_PROFILE_LAST) {
		if (len == 0)
			len += sysfs_emit_at(buf, len, "%s", profile_names[i]);
		else
			len += sysfs_emit_at(buf, len, " %s", profile_names[i]);
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
		if (!platform_profile_is_registered())
			return -ENODEV;
		err = platform_profile_get_active(dev, &profile);
		if (err)
			return err;
	}

	return sysfs_emit(buf, "%s\n", profile_names[profile]);
}

static ssize_t platform_profile_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct platform_profile_handler *handler;
	unsigned long choices;
	int err, i;

	/* Scan for a matching profile */
	i = sysfs_match_string(profile_names, buf);
	if (i < 0)
		return -EINVAL;

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &profile_lock) {
		if (!platform_profile_is_registered())
			return -ENODEV;

		/* don't allow setting custom to legacy sysfs interface */
		if (!platform_profile_is_class_device(dev) &&
		     i == PLATFORM_PROFILE_CUSTOM) {
			pr_warn("Custom profile not supported for legacy sysfs interface\n");
			return -EINVAL;
		}

		/* Check that applicable handlers support this profile choice */
		choices = platform_profile_get_choices(dev);
		if (!test_bit(i, &choices))
			return -EOPNOTSUPP;

		list_for_each_entry(handler, &platform_profile_handler_list, list) {
			if (platform_profile_is_class_device(dev) &&
			    handler->dev != dev->parent)
				continue;
			err = handler->profile_set(handler, i);
			if (err) {
				pr_err("Failed to set profile for handler %s\n", handler->name);
				break;
			}
		}
		if (err) {
			list_for_each_entry_continue_reverse(handler, &platform_profile_handler_list, list) {
				if (platform_profile_is_class_device(dev) &&
				    handler->dev != dev->parent)
					continue;
				if (handler->profile_set(handler, PLATFORM_PROFILE_BALANCED))
					pr_err("Failed to revert profile for handler %s\n",
					       handler->name);
			}
			return err;
		}
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
	sysfs_notify(acpi_kobj, NULL, "platform_profile");
}
EXPORT_SYMBOL_GPL(platform_profile_notify);

int platform_profile_cycle(void)
{
	enum platform_profile_option next = PLATFORM_PROFILE_LAST;
	struct platform_profile_handler *handler;
	enum platform_profile_option profile;
	unsigned long choices;
	int err;

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &profile_lock) {
		err = platform_profile_get_active(NULL, &profile);
		if (err)
			return err;

		choices = platform_profile_get_choices(NULL);

		next = find_next_bit_wrap(&choices,
					  PLATFORM_PROFILE_LAST,
					  profile + 1);

		list_for_each_entry(handler, &platform_profile_handler_list, list) {
			err = handler->profile_set(handler, next);
			if (err) {
				pr_err("Failed to set profile for handler %s\n", handler->name);
				break;
			}
		}
		if (err) {
			list_for_each_entry_continue_reverse(handler, &platform_profile_handler_list, list) {
				err = handler->profile_set(handler, PLATFORM_PROFILE_BALANCED);
				if (err)
					pr_err("Failed to revert profile for handler %s\n", handler->name);
			}
		}
	}

	sysfs_notify(acpi_kobj, NULL, "platform_profile");

	return 0;
}
EXPORT_SYMBOL_GPL(platform_profile_cycle);

int platform_profile_register(struct platform_profile_handler *pprof)
{
	bool registered;
	int err;

	/* Sanity check the profile handler */
	if (!pprof || bitmap_empty(pprof->choices, PLATFORM_PROFILE_LAST) ||
	    !pprof->profile_set || !pprof->profile_get) {
		pr_err("platform_profile: handler is invalid\n");
		return -EINVAL;
	}
	if (!test_bit(PLATFORM_PROFILE_BALANCED, pprof->choices)) {
		pr_err("platform_profile: handler does not support balanced profile\n");
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

	registered = platform_profile_is_registered();
	if (!registered) {
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
					 MKDEV(0, pprof->minor), NULL, "platform-profile-%s",
					 pprof->name);
	if (IS_ERR(pprof->class_dev)) {
		err = PTR_ERR(pprof->class_dev);
		goto cleanup_legacy;
	}
	err = sysfs_create_group(&pprof->class_dev->kobj, &platform_profile_group);
	if (err)
		goto cleanup_device;

	list_add_tail(&pprof->list, &platform_profile_handler_list);
	sysfs_notify(acpi_kobj, NULL, "platform_profile");

	cur_profile = pprof;
	return 0;

cleanup_device:
	device_destroy(&platform_profile_class, MKDEV(0, pprof->minor));

cleanup_legacy:
	if (!registered)
		sysfs_remove_group(acpi_kobj, &platform_profile_group);
cleanup_class:
	if (!registered)
		class_unregister(&platform_profile_class);

	return err;
}
EXPORT_SYMBOL_GPL(platform_profile_register);

int platform_profile_remove(struct platform_profile_handler *pprof)
{
	guard(mutex)(&profile_lock);

	list_del(&pprof->list);

	cur_profile = NULL;

	sysfs_notify(acpi_kobj, NULL, "platform_profile");

	sysfs_remove_group(&pprof->class_dev->kobj, &platform_profile_group);
	device_destroy(&platform_profile_class, MKDEV(0, pprof->minor));

	if (!platform_profile_is_registered())
		sysfs_remove_group(acpi_kobj, &platform_profile_group);

	return 0;
}
EXPORT_SYMBOL_GPL(platform_profile_remove);

MODULE_AUTHOR("Mark Pearson <markpearson@lenovo.com>");
MODULE_DESCRIPTION("ACPI platform profile sysfs interface");
MODULE_LICENSE("GPL");
