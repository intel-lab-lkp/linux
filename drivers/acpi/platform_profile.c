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

static ssize_t platform_profile_choices_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct platform_profile_handler *handler;
	unsigned long seen = 0;
	int len = 0;
	int i;

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &profile_lock) {
		list_for_each_entry(handler, &platform_profile_handler_list, list) {
			for_each_set_bit(i, handler->choices, PLATFORM_PROFILE_LAST) {
				if (seen & BIT(i))
					continue;
				if (len == 0)
					len += sysfs_emit_at(buf, len, "%s", profile_names[i]);
				else
					len += sysfs_emit_at(buf, len, " %s", profile_names[i]);
				seen |= BIT(i);
			}
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
	struct platform_profile_handler *handler;
	int err;


	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &profile_lock) {
		if (!platform_profile_is_registered())
			return -ENODEV;
		list_for_each_entry(handler, &platform_profile_handler_list, list) {
			err = handler->profile_get(handler, &profile);
			if (err)
				return err;
		}
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
	struct platform_profile_handler *handler;
	enum platform_profile_option profile;
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
		list_for_each_entry(handler, &platform_profile_handler_list, list) {
			if (!test_bit(i, handler->choices))
				return -EOPNOTSUPP;

			/* save the profile so that it can be reverted if necessary */
			err = handler->profile_get(handler, &profile);
			if (err)
				return err;
		}

		list_for_each_entry(handler, &platform_profile_handler_list, list) {
			err = handler->profile_set(handler, i);
			if (err) {
				pr_err("Failed to set profile for handler %s\n", handler->name);
				break;
			}
		}
		if (err) {
			list_for_each_entry_continue_reverse(handler, &platform_profile_handler_list, list) {
				if (handler->profile_set(handler, profile))
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
	struct platform_profile_handler *handler;
	enum platform_profile_option profile;
	enum platform_profile_option next = PLATFORM_PROFILE_LAST;
	enum platform_profile_option next2 = PLATFORM_PROFILE_LAST;
	int err;

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &profile_lock) {
		/* first pass, make sure all handlers agree on the definition of "next" profile */
		list_for_each_entry(handler, &platform_profile_handler_list, list) {

			err = handler->profile_get(handler, &profile);
			if (err)
				return err;

			if (next == PLATFORM_PROFILE_LAST)
				next = find_next_bit_wrap(handler->choices,
							  PLATFORM_PROFILE_LAST,
							  profile + 1);
			else
				next2 = find_next_bit_wrap(handler->choices,
							   PLATFORM_PROFILE_LAST,
							   profile + 1);

			if (WARN_ON(next == PLATFORM_PROFILE_LAST))
				return -EINVAL;

			if (next2 == PLATFORM_PROFILE_LAST)
				continue;

			if (next != next2) {
				pr_warn("Next profile to cycle to is ambiguous between platform_profile handlers\n");
				return -EINVAL;
			}
			next = next2;
		}

		/*
		 * Second pass: apply "next" to each handler
		 * If any failures occur unwind and revert all back to the original profile
		 */
		list_for_each_entry(handler, &platform_profile_handler_list, list) {
			err = handler->profile_set(handler, next);
			if (err) {
				pr_err("Failed to set profile for handler %s\n", handler->name);
				break;
			}
		}
		if (err) {
			list_for_each_entry_continue_reverse(handler, &platform_profile_handler_list, list) {
				err = handler->profile_set(handler, profile);
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

	guard(mutex)(&profile_lock);

	/* Sanity check the profile handler field are set */
	if (!pprof || bitmap_empty(pprof->choices, PLATFORM_PROFILE_LAST) ||
		!pprof->profile_set || !pprof->profile_get)
		return -EINVAL;

	if (!platform_profile_is_registered()) {
		err = sysfs_create_group(acpi_kobj, &platform_profile_group);
		if (err)
			return err;
	}
	list_add_tail(&pprof->list, &platform_profile_handler_list);

	return 0;
}
EXPORT_SYMBOL_GPL(platform_profile_register);

int platform_profile_remove(struct platform_profile_handler *pprof)
{
	guard(mutex)(&profile_lock);

	list_del(&pprof->list);

	if (!platform_profile_is_registered())
		sysfs_remove_group(acpi_kobj, &platform_profile_group);

	return 0;
}
EXPORT_SYMBOL_GPL(platform_profile_remove);

MODULE_AUTHOR("Mark Pearson <markpearson@lenovo.com>");
MODULE_DESCRIPTION("ACPI platform profile sysfs interface");
MODULE_LICENSE("GPL");
