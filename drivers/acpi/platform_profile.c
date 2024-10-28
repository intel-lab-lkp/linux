// SPDX-License-Identifier: GPL-2.0-or-later

/* Platform profile sysfs interface */

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/platform_profile.h>
#include <linux/sysfs.h>

static LIST_HEAD(platform_profile_handler_list);
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

static bool platform_profile_is_registered(void)
{
	struct list_head *pos;
	int count = 0;

	list_for_each(pos, &platform_profile_handler_list)
		count++;
	return count > 0;
}

/* expected to be called under mutex */
static unsigned long platform_profile_get_choices(void)
{
	struct platform_profile_handler *handler;
	unsigned long seen = 0;
	int i;

	list_for_each_entry(handler, &platform_profile_handler_list, list) {
		for_each_set_bit(i, handler->choices, PLATFORM_PROFILE_LAST) {
			if (seen & BIT(i))
				continue;
			seen |= BIT(i);
		}
	}

	return seen;
}

/* expected to be called under mutex */
static int platform_profile_get_active(enum platform_profile_option *profile)
{
	struct platform_profile_handler *handler;
	enum platform_profile_option active = PLATFORM_PROFILE_LAST;
	enum platform_profile_option active2 = PLATFORM_PROFILE_LAST;
	int err;

	list_for_each_entry(handler, &platform_profile_handler_list, list) {
		if (active == PLATFORM_PROFILE_LAST)
			err = handler->profile_get(handler, &active);
		else
			err = handler->profile_get(handler, &active2);
		if (err) {
			pr_err("Failed to get profile for handler %s\n", handler->name);
			return err;
		}

		if (WARN_ON(active == PLATFORM_PROFILE_LAST))
			return -EINVAL;
		if (active2 == PLATFORM_PROFILE_LAST)
			continue;

		if (active != active2) {
			pr_warn("Profile handlers don't agree on current profile\n");
			return -EINVAL;
		}
		active = active2;
	}

	/* Check that profile is valid index */
	if (WARN_ON((active < 0) || (active >= ARRAY_SIZE(profile_names))))
		return -EIO;

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
		choices = platform_profile_get_choices();

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
		err = platform_profile_get_active(&profile);
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
	if (i < 0) {
		return -EINVAL;
	}

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &profile_lock) {
		if (!platform_profile_is_registered())
			return -ENODEV;

		/* Check that all handlers support this profile choice */
		choices = platform_profile_get_choices();
		if (!test_bit(i, &choices))
			return -EOPNOTSUPP;

		list_for_each_entry(handler, &platform_profile_handler_list, list) {
			err = handler->profile_set(handler, i);
			if (err) {
				pr_err("Failed to set profile for handler %s\n", handler->name);
				break;
			}
		}
		if (err) {
			list_for_each_entry_continue_reverse(handler, &platform_profile_handler_list, list) {
				if (handler->profile_set(handler, PLATFORM_PROFILE_BALANCED))
					pr_err("Failed to revert profile for handler %s\n", handler->name);
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
	guard(mutex)(&profile_lock);
	if (!platform_profile_is_registered())
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
		err = platform_profile_get_active(&profile);
		if (err)
			return err;

		choices = platform_profile_get_choices();

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
	int err;

	/* Sanity check the profile handler field are set and balanced is supported */
	if (!pprof || bitmap_empty(pprof->choices, PLATFORM_PROFILE_LAST) ||
		!pprof->profile_set || !pprof->profile_get) {
		pr_err("platform_profile: handler is invalid\n");
		return -EINVAL;
	}
	if (!test_bit(PLATFORM_PROFILE_BALANCED, pprof->choices)) {
		pr_err("platform_profile: handler does not support balanced profile\n");
		return -EINVAL;
	}

	guard(mutex)(&profile_lock);

	if (!platform_profile_is_registered()) {
		err = sysfs_create_group(acpi_kobj, &platform_profile_group);
		if (err)
			return err;
	}
	list_add_tail(&pprof->list, &platform_profile_handler_list);
	sysfs_notify(acpi_kobj, NULL, "platform_profile");

	return 0;
}
EXPORT_SYMBOL_GPL(platform_profile_register);

int platform_profile_remove(struct platform_profile_handler *pprof)
{
	guard(mutex)(&profile_lock);

	list_del(&pprof->list);

	sysfs_notify(acpi_kobj, NULL, "platform_profile");

	if (!platform_profile_is_registered())
		sysfs_remove_group(acpi_kobj, &platform_profile_group);

	return 0;
}
EXPORT_SYMBOL_GPL(platform_profile_remove);

MODULE_AUTHOR("Mark Pearson <markpearson@lenovo.com>");
MODULE_DESCRIPTION("ACPI platform profile sysfs interface");
MODULE_LICENSE("GPL");
