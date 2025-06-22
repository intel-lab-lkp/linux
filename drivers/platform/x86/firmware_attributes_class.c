// SPDX-License-Identifier: GPL-2.0-or-later

/* Firmware attributes class helper module */

#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/kdev_t.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/string_choices.h>
#include "firmware_attributes_class.h"

#define to_fwat_bool_data(_c) \
	container_of_const(_c, struct fwat_bool_data, group)
#define to_fwat_enum_data(_c) \
	container_of_const(_c, struct fwat_enum_data, group)
#define to_fwat_int_data(_c) \
	container_of_const(_c, struct fwat_int_data, group)
#define to_fwat_str_data(_c) \
	container_of_const(_c, struct fwat_str_data, group)

struct fwat_group {
	struct attribute_group group;
	struct list_head node;
};

struct fwat_group_config {
	int type;
	unsigned int total_attrs;
	const char * const *attr_labels;
	ssize_t (*show)(struct device *dev, const struct fwat_attribute *attr,
			char *buf);
	ssize_t (*store)(struct device *dev, const struct fwat_attribute *attr,
			 const char *buf, size_t count);
};

struct fwat_attribute_ext {
	struct fwat_attribute fattr;
	const struct fwat_group_data *data;
	unsigned int type;
};

#define to_fwat_attribute_ext(_f) \
	container_of_const(_f, struct fwat_attribute_ext, fattr)

#define fwat_attribute_ext_init(_name, _mode, _show, _store, _idx, _type, _config) \
	((struct fwat_attribute_ext){				\
		.fattr = __ATTR(_name, _mode, _show, _store),	\
		.config = _config,				\
		.idx = _idx,					\
		.type = _type,					\
	 })

const struct class firmware_attributes_class = {
	.name = "firmware-attributes",
};
EXPORT_SYMBOL_GPL(firmware_attributes_class);

static const char * const fwat_type_labels[] = {
	[FWAT_GROUP_BOOLEAN]				= "boolean",
	[FWAT_GROUP_ENUMERATION]			= "enumeration",
	[FWAT_GROUP_INTEGER]				= "integer",
	[FWAT_GROUP_STRING]				= "string",
};

static const char * const fwat_bool_labels[] = {
	[fwat_bool_current_value]			= "current_value",
	[fwat_bool_default_value]			= "default_value",
};

static const char * const fwat_enum_labels[] = {
	[fwat_enum_current_value]			= "current_value",
	[fwat_enum_default_value]			= "default_value",
	[fwat_enum_possible_values]			= "possible_values",
};

static const char * const fwat_int_labels[] = {
	[fwat_int_current_value]			= "current_value",
	[fwat_int_default_value]			= "default_value",
	[fwat_int_min_value]				= "min_value",
	[fwat_int_max_value]				= "max_value",
	[fwat_int_scalar_increment]			= "scalar_increment",
};

static const char * const fwat_str_labels[] = {
	[fwat_str_current_value]			= "current_value",
	[fwat_str_default_value]			= "default_value",
	[fwat_str_min_length]				= "min_length",
	[fwat_str_max_length]				= "max_length",
};

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

static ssize_t fwat_type_show(struct device *dev, const struct fwat_attribute *attr,
			      char *buf)
{
	const struct fwat_attribute_ext *ext = to_fwat_attribute_ext(attr);

	return sysfs_emit(buf, "%s\n", fwat_type_labels[ext->type]);
}

static ssize_t
fwat_display_name_show(struct device *dev, const struct fwat_attribute *attr,
		       char *buf)
{
	const struct fwat_attribute_ext *ext = to_fwat_attribute_ext(attr);
	const struct fwat_group_data *data = ext->data;

	if (!data->display_name)
		return -EOPNOTSUPP;

	return sysfs_emit(buf, "%s\n", data->display_name);
}

static ssize_t
fwat_language_code_show(struct device *dev, const struct fwat_attribute *attr,
			char *buf)
{
	const struct fwat_attribute_ext *ext = to_fwat_attribute_ext(attr);
	const struct fwat_group_data *data = ext->data;

	if (!data->language_code)
		return -EOPNOTSUPP;

	return sysfs_emit(buf, "%s\n", data->language_code);
}

static ssize_t
boolean_group_show(struct device *dev, const struct fwat_attribute *attr,
		   char *buf)
{
	const struct fwat_attribute_ext *ext = to_fwat_attribute_ext(attr);
	const struct fwat_bool_data *data = to_fwat_bool_data(ext->data);
	bool val;
	int ret;

	switch (ext->type) {
	case fwat_bool_current_value:
		ret = data->read(dev, data->group.id, &val);
		if (ret < 0)
			return ret;
		break;
	case fwat_bool_default_value:
		val = data->default_val;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return sysfs_emit(buf, "%s\n", str_yes_no(val));
}

static ssize_t
enumeration_group_show(struct device *dev, const struct fwat_attribute *attr,
		       char *buf)
{
	const struct fwat_attribute_ext *ext = to_fwat_attribute_ext(attr);
	const struct fwat_enum_data *data = to_fwat_enum_data(ext->data);
	int val_idx, sz = 0;
	int ret;

	switch (ext->type) {
	case fwat_enum_current_value:
		ret = data->read(dev, data->group.id, &val_idx);
		if (ret < 0)
			return ret;
		break;
	case fwat_enum_default_value:
		val_idx = data->default_idx;
		break;
	case fwat_enum_possible_values:
		sz += sysfs_emit_at(buf, sz, "%s", data->possible_vals[0]);
		for (unsigned int i = 1; data->possible_vals[i]; i++)
			sz += sysfs_emit_at(buf, sz, ";%s", data->possible_vals[i]);
		sz += sysfs_emit_at(buf, sz, "\n");
		return sz;
	default:
		return -EOPNOTSUPP;
	}

	return sysfs_emit(buf, "%s\n", data->possible_vals[val_idx]);
}

static ssize_t
integer_group_show(struct device *dev, const struct fwat_attribute *attr,
		   char *buf)
{
	const struct fwat_attribute_ext *ext = to_fwat_attribute_ext(attr);
	const struct fwat_int_data *data = to_fwat_int_data(ext->data);
	long val;
	int ret;

	switch (ext->type) {
	case fwat_int_current_value:
		ret = data->read(dev, data->group.id, &val);
		if (ret < 0)
			return ret;
		break;
	case fwat_int_default_value:
		val = data->default_val;
		break;
	case fwat_int_min_value:
		val = data->min_val;
		break;
	case fwat_int_max_value:
		val = data->max_val;
		break;
	case fwat_int_scalar_increment:
		val = data->increment;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return sysfs_emit(buf, "%ld\n", val);
}

static ssize_t
string_group_show(struct device *dev, const struct fwat_attribute *attr,
		  char *buf)
{
	const struct fwat_attribute_ext *ext = to_fwat_attribute_ext(attr);
	const struct fwat_str_data *data = to_fwat_str_data(ext->data);
	const char *val;
	long len;
	int ret;

	switch (ext->type) {
	case fwat_str_current_value:
		ret = data->read(dev, data->group.id, &val);
		if (ret < 0)
			return ret;
		break;
	case fwat_str_default_value:
		val = data->default_val;
		break;
	case fwat_str_min_length:
		len = data->min_len;
		return sysfs_emit(buf, "%ld\n", len);
	case fwat_str_max_length:
		len = data->max_len;
		return sysfs_emit(buf, "%ld\n", len);
	default:
		return -EOPNOTSUPP;
	}

	return sysfs_emit(buf, "%s\n", val);
}

static ssize_t
boolean_group_store(struct device *dev, const struct fwat_attribute *attr,
		    const char *buf, size_t count)
{
	const struct fwat_attribute_ext *ext = to_fwat_attribute_ext(attr);
	const struct fwat_bool_data *data = to_fwat_bool_data(ext->data);
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	ret = data->write(dev, data->group.id, val);
	if (ret)
		return ret;

	return count;
}

static ssize_t
enumeration_group_store(struct device *dev, const struct fwat_attribute *attr,
			const char *buf, size_t count)
{
	const struct fwat_attribute_ext *ext = to_fwat_attribute_ext(attr);
	const struct fwat_enum_data *data = to_fwat_enum_data(ext->data);
	int val_idx;
	int ret;

	val_idx = __sysfs_match_string(data->possible_vals, -1, buf);
	if (val_idx < 0)
		return val_idx;

	ret = data->write(dev, data->group.id, val_idx);
	if (ret)
		return ret;

	return count;
}

static ssize_t
integer_group_store(struct device *dev, const struct fwat_attribute *attr,
		    const char *buf, size_t count)
{
	const struct fwat_attribute_ext *ext = to_fwat_attribute_ext(attr);
	const struct fwat_int_data *data = to_fwat_int_data(ext->data);
	long val;
	int ret;

	ret = kstrtol(buf, 0, &val);
	if (ret)
		return ret;

	ret = data->write(dev, data->group.id, val);
	if (ret)
		return ret;

	return count;
}

static ssize_t
string_group_store(struct device *dev, const struct fwat_attribute *attr,
		   const char *buf, size_t count)
{
	const struct fwat_attribute_ext *ext = to_fwat_attribute_ext(attr);
	const struct fwat_str_data *data = to_fwat_str_data(ext->data);
	int ret;

	ret = data->write(dev, data->group.id, buf);
	if (ret)
		return ret;

	return count;
}

static int fwat_add_group(struct fwat_device *fadev, const struct fwat_group_data *data,
			  const struct fwat_group_config *config)
{
	struct fwat_attribute_ext *fattrs;
	struct fwat_group *group;
	struct attribute **attrs;
	unsigned int bit, i = 0;
	size_t nattrs;
	int ret;

	nattrs = bitmap_weight(&data->fattrs, config->total_attrs);
	if (data->display_name)
		nattrs++;
	if (data->language_code)
		nattrs++;

	attrs = devm_kcalloc(fadev->dev, sizeof(*attrs), nattrs + 2, GFP_KERNEL);
	if (!attrs)
		return -ENOMEM;
	fattrs = devm_kcalloc(fadev->dev, sizeof(*fattrs), nattrs + 1, GFP_KERNEL);
	if (!fattrs)
		return -ENOMEM;

	fattrs[i].fattr.attr.name = "type";
	fattrs[i].fattr.attr.mode = 0444;
	fattrs[i].fattr.show = fwat_type_show;
	fattrs[i].data = data;
	fattrs[i].type = config->type;
	attrs[i] = &fattrs[i].fattr.attr;
	i++;

	if (data->display_name) {
		fattrs[i].fattr.attr.name = "display_name";
		fattrs[i].fattr.attr.mode = 0444;
		fattrs[i].fattr.show = fwat_display_name_show;
		fattrs[i].data = data;
		fattrs[i].type = config->type;
		attrs[i] = &fattrs[i].fattr.attr;
		i++;
	}

	if (data->language_code) {
		fattrs[i].fattr.attr.name = "display_name_language_code";
		fattrs[i].fattr.attr.mode = 0444;
		fattrs[i].fattr.show = fwat_language_code_show;
		fattrs[i].data = data;
		fattrs[i].type = config->type;
		attrs[i] = &fattrs[i].fattr.attr;
		i++;
	}

	for_each_set_bit(bit, &data->fattrs, config->total_attrs) {
		fattrs[i].fattr.attr.name = config->attr_labels[bit];
		/* current_value is always at bit 0 and uses data->mode */
		fattrs[i].fattr.attr.mode = bit ? 0444 : data->mode;
		fattrs[i].fattr.show = config->show;
		fattrs[i].fattr.store = config->store;
		fattrs[i].data = data;
		fattrs[i].type = bit;
		attrs[i] = &fattrs[i].fattr.attr;
		i++;
	}

	group = devm_kzalloc(fadev->dev, sizeof(*group), GFP_KERNEL);
	if (!group)
		return -ENOMEM;
	group->group.name = data->name;
	group->group.attrs = attrs;
	ret = sysfs_create_group(&fadev->attrs_kobj, &group->group);
	if (ret)
		return ret;
	list_add(&group->node, &fadev->auto_groups);

	kobject_uevent(&fadev->attrs_kobj, KOBJ_CHANGE);

	return 0;
}

static void fwat_remove_auto_groups(struct fwat_device *fadev)
{
	struct fwat_group *pos;

	list_for_each_entry(pos, &fadev->auto_groups, node)
		sysfs_remove_group(&fadev->attrs_kobj, &pos->group);
}

int fwat_create_bool_group(struct fwat_device *fadev, const struct fwat_bool_data *data)
{
	struct fwat_group_config config = {
		.type = FWAT_GROUP_BOOLEAN,
		.total_attrs = fwat_bool_attrs_last,
		.attr_labels = fwat_bool_labels,
		.show = boolean_group_show,
		.store = boolean_group_store,
	};

	return fwat_add_group(fadev, &data->group, &config);
}
EXPORT_SYMBOL_GPL(fwat_create_bool_group);

int fwat_create_enum_group(struct fwat_device *fadev, const struct fwat_enum_data *data)
{
	struct fwat_group_config config = {
		.type = FWAT_GROUP_ENUMERATION,
		.total_attrs = fwat_enum_attrs_last,
		.attr_labels = fwat_enum_labels,
		.show = enumeration_group_show,
		.store = enumeration_group_store,
	};

	return fwat_add_group(fadev, &data->group, &config);
}
EXPORT_SYMBOL_GPL(fwat_create_enum_group);

int fwat_create_int_group(struct fwat_device *fadev, const struct fwat_int_data *data)
{
	struct fwat_group_config config = {
		.type = FWAT_GROUP_INTEGER,
		.total_attrs = fwat_int_attrs_last,
		.attr_labels = fwat_int_labels,
		.show = integer_group_show,
		.store = integer_group_store,
	};

	return fwat_add_group(fadev, &data->group, &config);
}
EXPORT_SYMBOL_GPL(fwat_create_int_group);

int fwat_create_str_group(struct fwat_device *fadev, const struct fwat_str_data *data)
{
	struct fwat_group_config config = {
		.type = FWAT_GROUP_STRING,
		.total_attrs = fwat_str_attrs_last,
		.attr_labels = fwat_str_labels,
		.show = string_group_show,
		.store = string_group_store,
	};

	return fwat_add_group(fadev, &data->group, &config);
}
EXPORT_SYMBOL_GPL(fwat_create_str_group);

/**
 * fwat_device_register - Create and register a firmware-attributes class
 *			  device
 * @parent: Parent device
 * @name: Name of the class device
 * @data: Drvdata of the class device
 * @groups: Extra groups for the class device (Optional)
 *
 * Return: pointer to the new fwat_device on success, ERR_PTR on failure
 */
struct fwat_device *
fwat_device_register(struct device *parent, const char *name, void *data,
		     const struct attribute_group **groups)
{
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

	if (groups) {
		ret = device_add_groups(dev, groups);
		if (ret)
			goto out_kobj_unregister;
	}

	fadev->dev = dev;
	fadev->groups = groups;
	INIT_LIST_HEAD(&fadev->auto_groups);

	kobject_uevent(&fadev->attrs_kobj, KOBJ_ADD);

	return fadev;

out_kobj_unregister:
	kobject_del(&fadev->attrs_kobj);

out_kobj_put:
	kobject_put(&fadev->attrs_kobj);
	device_unregister(dev);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(fwat_device_register);

void fwat_device_unregister(struct fwat_device *fadev)
{
	struct device *dev;

	if (!fadev)
		return;

	dev = fadev->dev;
	fwat_remove_auto_groups(fadev);
	device_remove_groups(dev, fadev->groups);
	kobject_del(&fadev->attrs_kobj);
	kobject_put(&fadev->attrs_kobj);
	device_unregister(dev);
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
 * @data: Drvdata of the class device
 * @groups: Extra groups for the class device (Optional)
 *
 * Device managed version of fwat_device_register().
 *
 * Return: pointer to the new fwat_device on success, ERR_PTR on failure
 */
struct fwat_device *
devm_fwat_device_register(struct device *parent, const char *name, void *data,
			  const struct attribute_group **groups)
{
	struct fwat_device *fadev;
	int ret;

	fadev = fwat_device_register(parent, name, data, groups);
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
