// SPDX-License-Identifier: GPL-2.0-or-later

/* Hint sysfs interface */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/hint.h>
#include <linux/sysfs.h>

struct hint_handler {
	const char *name;
	struct device dev;
	int minor;
	struct mutex lock; /* Prevents parallel calls to class device. */
	unsigned long idle_choices[BITS_TO_LONGS(HINT_IDLE_LAST)];
	enum hint_idle_option idle;
	const struct hint_ops *ops;
};

#define to_hint_handler(d)	(container_of(d, struct hint_handler, dev))

static const char * const hint_idle_names[] = {
	[HINT_IDLE_SNOOZE]   = "snooze",
	[HINT_IDLE_RESUME]   = "resume",
	[HINT_IDLE_INACTIVE] = "inactive",
	[HINT_IDLE_ACTIVE]   = "active",
};
static_assert(ARRAY_SIZE(hint_idle_names) == HINT_IDLE_LAST);

static ssize_t name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hint_handler *handler = to_hint_handler(dev);

	return sysfs_emit(buf, "%s\n", handler->name);
}
static DEVICE_ATTR_RO(name);

static ssize_t idle_choices_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct hint_handler *handler = to_hint_handler(dev);
	int i, len = 0;

	for_each_set_bit(i, handler->idle_choices, HINT_IDLE_LAST) {
		if (len == 0)
			len += sysfs_emit_at(buf, len, "%s", hint_idle_names[i]);
		else
			len += sysfs_emit_at(buf, len, " %s", hint_idle_names[i]);
	}
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}
static DEVICE_ATTR_RO(idle_choices);

static ssize_t idle_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	enum hint_idle_option idle = HINT_IDLE_LAST;
	struct hint_handler *handler = to_hint_handler(dev);
	int err;

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &handler->lock) {
		if (!handler->ops->idle_get)
			return handler->idle;

		err = handler->ops->idle_get(dev, &idle);
		if (err)
			return err;

		if (WARN_ON(idle >= HINT_IDLE_LAST))
			return -EINVAL;
	}

	return sysfs_emit(buf, "%s\n", hint_idle_names[idle]);
}

static ssize_t idle_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct hint_handler *handler = to_hint_handler(dev);
	int index, ret;

	index = sysfs_match_string(hint_idle_names, buf);
	if (index < 0)
		return -EINVAL;

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &handler->lock) {
		if (!test_bit(index, handler->idle_choices))
			return -EOPNOTSUPP;

		if (handler->ops->idle_set) {
			ret = handler->ops->idle_set(dev, index);
			if (ret)
				return ret;
		}
		handler->idle = index;
	}

	return count;
}
static DEVICE_ATTR_RW(idle);

static umode_t hint_attr_is_visible(struct kobject *kobj,
				    struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct hint_handler *handler = to_hint_handler(dev);

	if ((attr == &dev_attr_idle.attr ||
	     attr == &dev_attr_idle_choices.attr) &&
	    bitmap_empty(handler->idle_choices, HINT_IDLE_LAST))
		return 0;

	return attr->mode;
}

static struct attribute *hint_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_idle_choices.attr,
	&dev_attr_idle.attr,
	NULL
};

static const struct attribute_group hint_group = {
	.attrs      = hint_attrs,
	.is_visible = hint_attr_is_visible,
};

static const struct attribute_group *hint_groups[] = {
	&hint_group,
	NULL,
};

static void hint_device_release(struct device *dev)
{
	struct hint_handler *handler = to_hint_handler(dev);

	kfree(handler);
}

static const struct class hint_class = {
	.name = "hint",
	.dev_groups = hint_groups,
	.dev_release = hint_device_release,
};

/**
 * hint_register - Creates and registers a hint class device
 * @dev: Parent device
 * @name: Name of the class device
 * @drvdata: Driver data that will be attached to the class device
 * @ops: Hint probes and getters/setters
 *
 * Return: pointer to the new class device on success, ERR_PTR on failure
 */
struct device *hint_register(struct device *dev, const char *name,
			     void *drvdata,
			     const struct hint_ops *ops)
{
	struct device *adev;
	int minor, err;

	/* Sanity check */
	if (WARN_ON_ONCE(!dev || !name || !ops))
		return ERR_PTR(-EINVAL);

	struct hint_handler *handler __free(kfree) = kzalloc_obj(*handler);
	if (!handler)
		return ERR_PTR(-ENOMEM);

	/*
	 * Hint probes
	 */

	if (ops->idle_probe) {
		err = ops->idle_probe(drvdata, handler->idle_choices);
		if (err) {
			dev_err(dev, "idle state hint probe failed\n");
			return ERR_PTR(err);
		}
		handler->idle =
			find_first_bit(handler->idle_choices, HINT_IDLE_LAST);
	}

	/* create class interface for handler */
	handler->name = name;
	handler->ops = ops;
	handler->minor = minor;
	handler->dev.class = &hint_class;
	handler->dev.parent = dev;
	mutex_init(&handler->lock);
	dev_set_drvdata(&handler->dev, drvdata);
	dev_set_name(&handler->dev, name, handler->minor);

	adev = &no_free_ptr(handler)->dev;
	err = device_register(adev);
	if (err) {
		put_device(adev);
		return ERR_PTR(err);
	}

	return adev;
}
EXPORT_SYMBOL_GPL(hint_register);

/**
 * hint_remove - Unregisters a hint class device
 * @dev: Class device
 */
void hint_remove(struct device *dev)
{
	struct hint_handler *handler;

	if (IS_ERR_OR_NULL(dev))
		return;

	handler = to_hint_handler(dev);

	guard(mutex)(&handler->lock);

	device_unregister(&handler->dev);
}
EXPORT_SYMBOL_GPL(hint_remove);

static void devm_hint_release(struct device *dev, void *res)
{
	struct device **adev = res;

	hint_remove(*adev);
}

/**
 * devm_hint_register - Device managed version of hint_register
 * @dev: Parent device
 * @name: Name of the class device
 * @drvdata: Driver data that will be attached to the class device
 * @ops: Activity operations
 *
 * Return: pointer to the new class device on success, ERR_PTR on failure
 */
struct device *devm_hint_register(struct device *dev, const char *name,
				  void *drvdata, const struct hint_ops *ops)
{
	struct device *adev;
	struct device **dr;

	dr = devres_alloc(devm_hint_release, sizeof(*dr), GFP_KERNEL);
	if (!dr)
		return ERR_PTR(-ENOMEM);

	adev = hint_register(dev, name, drvdata, ops);
	if (IS_ERR(adev)) {
		devres_free(dr);
		return adev;
	}

	*dr = adev;
	devres_add(dev, dr);

	return adev;
}
EXPORT_SYMBOL_GPL(devm_hint_register);

static int __init hint_init(void)
{
	return class_register(&hint_class);
}

/*
 * Required for s2idle to be able to register hints.
 * module_init() would run after it tries to register the device.
 */
postcore_initcall(hint_init);

MODULE_AUTHOR("Antheas Kapenekakis <lkml@antheas.dev>");
MODULE_DESCRIPTION("Activity sysfs interface");
MODULE_LICENSE("GPL");
