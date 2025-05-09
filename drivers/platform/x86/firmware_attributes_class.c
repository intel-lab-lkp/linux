// SPDX-License-Identifier: GPL-2.0-or-later

/* Firmware attributes class helper module */

#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include "firmware_attributes_class.h"

#define to_fwat_attribute_ext(_a) container_of_const(_a, struct fwat_attribute_ext, attr)

struct fwat_attribute_ext {
	struct fwat_attribute attr;
	enum fwat_property prop;
	const struct fwat_attr_config *config;
};

const struct class firmware_attributes_class = {
	.name = "firmware-attributes",
};
EXPORT_SYMBOL_GPL(firmware_attributes_class);

static const char * const fwat_type_labels[] = {
	[fwat_type_integer]		= "integer",
	[fwat_type_string]		= "string",
	[fwat_type_enumeration]		= "enumeration",
};

static const char * const fwat_prop_labels[] = {
	[FWAT_PROP_DISPLAY_NAME]		= "display_name",
	[FWAT_PROP_LANGUAGE_CODE]		= "display_name_language_code",
	[FWAT_PROP_DEFAULT]			= "default",

	[FWAT_INT_PROP_MIN]			= "min_value",
	[FWAT_INT_PROP_MAX]			= "max_value",
	[FWAT_INT_PROP_INCREMENT]		= "scalar_increment",

	[FWAT_STR_PROP_MIN]			= "min_length",
	[FWAT_STR_PROP_MAX]			= "max_length",

	[FWAT_ENUM_PROP_POSSIBLE_VALUES]	= "possible_values",
};

static ssize_t
fwat_type_show(struct device *dev, const struct fwat_attribute *attr, char *buf)
{
	const struct fwat_attribute_ext *ext = to_fwat_attribute_ext(attr);
	const struct fwat_attr_config *config = ext->config;

	return sysfs_emit(buf, "%s\n", fwat_type_labels[config->type]);
}

static ssize_t
fwat_property_show(struct device *dev, const struct fwat_attribute *attr, char *buf)
{
	const struct fwat_attribute_ext *ext = to_fwat_attribute_ext(attr);
	const struct fwat_attr_config *config = ext->config;

	if (!config->ops->prop_read)
		return -EOPNOTSUPP;

	return config->ops->prop_read(dev, config->aux, ext->prop, buf);
}

static ssize_t
fwat_current_value_show(struct device *dev, const struct fwat_attribute *attr, char *buf)
{
	const struct fwat_attribute_ext *ext = to_fwat_attribute_ext(attr);
	const struct fwat_attr_config *config = ext->config;
	const char *str;
	long int_val;
	int ret;

	switch (config->type) {
	case fwat_type_integer:
		ret = config->ops->integer_read(dev, config->aux, &int_val);
		if (ret)
			return ret;

		return sysfs_emit(buf, "%ld\n", int_val);
	case fwat_type_string:
		ret = config->ops->string_read(dev, config->aux, &str);
		if (ret)
			return ret;

		return sysfs_emit(buf, "%s\n", str);
	case fwat_type_enumeration:
		ret = config->ops->enumeration_read(dev, config->aux, &str);
		if (ret)
			return ret;

		return sysfs_emit(buf, "%s\n", str);
	default:
		return -EOPNOTSUPP;
	}
}

static ssize_t
fwat_current_value_store(struct device *dev, const struct fwat_attribute *attr,
			 const char *buf, size_t count)
{
	const struct fwat_attribute_ext *ext = to_fwat_attribute_ext(attr);
	const struct fwat_attr_config *config = ext->config;
	long int_val;
	int ret;

	switch (config->type) {
	case fwat_type_integer:
		ret = kstrtol(buf, 0, &int_val);
		if (ret)
			return ret;

		ret = config->ops->integer_write(dev, config->aux, int_val);
		break;
	case fwat_type_string:
		ret = config->ops->string_write(dev, config->aux, buf);
		break;
	case fwat_type_enumeration:
		ret = config->ops->enumeration_write(dev, config->aux, buf);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return ret ? ret : count;
}

static struct attribute *
fwat_alloc_attr(struct device *dev, const struct fwat_attr_config *config,
		const char *attr_name, umode_t mode, enum fwat_property prop,
		ssize_t (*show)(struct device *dev, const struct fwat_attribute *attr,
				char *buf),
		ssize_t (*store)(struct device *dev, const struct fwat_attribute *attr,
				 const char *buf, size_t count))
{
	struct fwat_attribute_ext *fattr;

	fattr = devm_kzalloc(dev, sizeof(*fattr), GFP_KERNEL);
	if (!fattr)
		return NULL;

	fattr->attr.attr.name = attr_name;
	fattr->attr.attr.mode = mode;
	fattr->attr.show = show;
	fattr->attr.store = store;
	fattr->prop = prop;
	fattr->config = config;
	sysfs_attr_init(&fattr->attr.attr);

	return &fattr->attr.attr;
}

static struct attribute **
fwat_create_attrs(struct device *dev, const struct fwat_attr_config *config)
{
	struct attribute **attrs;
	enum fwat_property prop;
	unsigned int index = 0;

	attrs = devm_kcalloc(dev, config->num_props + 3, sizeof(*attrs), GFP_KERNEL);
	if (!attrs)
		return NULL;

	/*
	 * Create optional attributes
	 */
	for (; index < config->num_props; index++) {
		prop = config->props[index];
		attrs[index] = fwat_alloc_attr(dev, config, fwat_prop_labels[prop],
					       0444, prop, fwat_property_show, NULL);
	}

	/*
	 * Create mandatory attributes
	 */
	attrs[index++] = fwat_alloc_attr(dev, config, "type", 0444, 0, fwat_type_show, NULL);
	attrs[index++] = fwat_alloc_attr(dev, config, "current_value", 0644, 0,
					 fwat_current_value_show, fwat_current_value_store);

	return attrs;
}

static const struct attribute_group *
fwat_create_group(struct device *dev, const struct fwat_attr_config *config)
{
	struct attribute_group *group;
	struct attribute **attrs;

	group = devm_kzalloc(dev, sizeof(*group), GFP_KERNEL);
	if (!group)
		return NULL;

	attrs = fwat_create_attrs(dev, config);
	if (!attrs)
		return NULL;

	group->name = config->name;
	group->attrs = attrs;

	return group;
}

static const struct attribute_group **
fwat_create_auto_groups(struct device *dev, const struct fwat_dev_config *config)
{
	const struct attribute_group **groups;
	const struct attribute_group *grp;
	unsigned int index = 0;
	size_t ngroups = 0;

	while (config->attrs_config[ngroups])
		ngroups++;

	groups = devm_kcalloc(dev, ngroups + 1, sizeof(*groups), GFP_KERNEL);
	if (!groups)
		return NULL;

	for (unsigned int i = 0; i < ngroups; i++) {
		if (config->is_visible &&
		    !config->is_visible(dev, config->attrs_config[i]))
			continue;

		grp = fwat_create_group(dev, config->attrs_config[i]);
		if (!grp)
			return NULL;

		groups[index++] = grp;
	}

	return groups;
}

static ssize_t fwat_attrs_kobj_show(struct kobject *kobj, struct attribute *attr,
				    char *buf)
{
	const struct fwat_attribute *fattr = to_fwat_attribute(attr);
	struct fwat_device *fadev = to_fwat_device(kobj);

	if (!fattr->show)
		return -ENOENT;

	return fattr->show(fadev->dev, fattr, buf);
}

static ssize_t fwat_attrs_kobj_store(struct kobject *kobj, struct attribute *attr,
				     const char *buf, size_t count)
{
	const struct fwat_attribute *fattr = to_fwat_attribute(attr);
	struct fwat_device *fadev = to_fwat_device(kobj);

	if (!fattr->store)
		return -ENOENT;

	return fattr->store(fadev->dev, fattr, buf, count);
}

static const struct sysfs_ops fwat_attrs_kobj_ops = {
	.show	= fwat_attrs_kobj_show,
	.store	= fwat_attrs_kobj_store,
};

static void fwat_attrs_kobj_release(struct kobject *kobj)
{
	struct fwat_device *fadev = to_fwat_device(kobj);

	kfree(fadev);
}

static const struct kobj_type fwat_attrs_ktype = {
	.sysfs_ops	= &fwat_attrs_kobj_ops,
	.release	= fwat_attrs_kobj_release,
};

/**
 * fwat_device_register - Create and register a firmware-attributes class
 *			  device
 * @parent: Parent device
 * @name: Name of the class device
 * @config: Device configuration
 * @data: Drvdata of the class device
 * @groups: Sysfs groups for the custom `fwat_attrs_ktype` kobj_type
 *
 * NOTE: @groups are attached to the .attrs_kobj of the new fwat_device which
 * has a custom ktype, which makes use of `struct fwat_attribute` to embed
 * attributes.
 *
 * Return: pointer to the new fwat_device on success, ERR_PTR on failure
 */
struct fwat_device *
fwat_device_register(struct device *parent, const char *name, void *data,
		     const struct fwat_dev_config *config,
		     const struct attribute_group **groups)
{
	const struct attribute_group **auto_groups;
	struct fwat_device *fadev;
	struct device *dev;
	int ret;

	if (!parent || !name)
		return ERR_PTR(-EINVAL);

	fadev = kzalloc(sizeof(*fadev), GFP_KERNEL);
	if (!fadev)
		return ERR_PTR(-ENOMEM);

	dev = device_create(&firmware_attributes_class, parent, MKDEV(0, 0),
			    data, "%s", name);
	if (IS_ERR(dev)) {
		kfree(fadev);
		return ERR_CAST(dev);
	}

	ret = kobject_init_and_add(&fadev->attrs_kobj, &fwat_attrs_ktype, &dev->kobj,
				   "attributes");
	if (ret)
		goto out_kobj_put;

	if (config) {
		auto_groups = fwat_create_auto_groups(dev, config);
		if (!auto_groups) {
			ret = -ENOMEM;
			goto out_kobj_unregister;
		}

		ret = sysfs_create_groups(&fadev->attrs_kobj, auto_groups);
		if (ret)
			goto out_kobj_unregister;
	}

	if (groups) {
		ret = sysfs_create_groups(&fadev->attrs_kobj, groups);
		if (ret)
			goto out_remove_auto_groups;
	}

	fadev->dev = dev;
	fadev->groups = groups;
	fadev->auto_groups = groups;

	kobject_uevent(&fadev->attrs_kobj, KOBJ_ADD);

	return fadev;

out_remove_auto_groups:
	if (config)
		sysfs_remove_groups(&fadev->attrs_kobj, auto_groups);

out_kobj_unregister:
	kobject_del(&fadev->attrs_kobj);

out_kobj_put:
	kobject_put(&fadev->attrs_kobj);
	device_unregister(dev);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(fwat_device_register);

void fwat_device_unregister(struct fwat_device *fwadev)
{
	if (fwadev->groups)
		sysfs_remove_groups(&fwadev->attrs_kobj, fwadev->groups);
	if (fwadev->auto_groups)
		sysfs_remove_groups(&fwadev->attrs_kobj, fwadev->auto_groups);
	kobject_del(&fwadev->attrs_kobj);
	kobject_put(&fwadev->attrs_kobj);
	device_unregister(fwadev->dev);
}
EXPORT_SYMBOL_GPL(fwat_device_unregister);

static void devm_fwat_device_release(void *data)
{
	struct fwat_device *fadev = data;

	fwat_device_unregister(fadev);
}

/**
 * devm_fwat_device_register - Create and register a firmware-attributes class
 *			       device
 * @parent: Parent device
 * @name: Name of the class device
 * @config: Device configuration
 * @data: Drvdata of the class device
 * @groups: Sysfs groups for the custom `fwat_attrs_ktype` kobj_type
 *
 * Device managed version of fwat_device_register().
 *
 * NOTE: @groups are attached to the .attrs_kobj of the new fwat_device which
 * has a custom ktype, which makes use of `struct fwat_attribute` to embed
 * attributes.
 *
 * Return: pointer to the new fwat_device on success, ERR_PTR on failure
 */
struct fwat_device *
devm_fwat_device_register(struct device *parent, const char *name, void *data,
			  const struct fwat_dev_config *config,
			  const struct attribute_group **groups)
{
	struct fwat_device *fadev;
	int ret;

	fadev = fwat_device_register(parent, name, data, config, groups);
	if (IS_ERR(fadev))
		return fadev;

	ret = devm_add_action_or_reset(parent, devm_fwat_device_release, fadev);
	if (ret)
		return ERR_PTR(ret);

	return fadev;
}
EXPORT_SYMBOL_GPL(devm_fwat_device_register);

static __init int fw_attributes_class_init(void)
{
	return class_register(&firmware_attributes_class);
}
module_init(fw_attributes_class_init);

static __exit void fw_attributes_class_exit(void)
{
	class_unregister(&firmware_attributes_class);
}
module_exit(fw_attributes_class_exit);

MODULE_AUTHOR("Mark Pearson <markpearson@lenovo.com>");
MODULE_AUTHOR("Thomas Weißschuh <linux@weissschuh.net>");
MODULE_AUTHOR("Kurt Borja <kuurtb@gmail.com>");
MODULE_DESCRIPTION("Firmware attributes class helper module");
MODULE_LICENSE("GPL");
