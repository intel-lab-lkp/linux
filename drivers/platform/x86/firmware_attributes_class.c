// SPDX-License-Identifier: GPL-2.0-or-later

/* Firmware attributes class helper module */

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/kdev_t.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/string_choices.h>
#include <linux/firmware_attributes_class.h>

#define FWAT_TYPE_NONE				-1

#define to_fwat_bool_data(_c) \
	container_of_const(_c, struct fwat_bool_data, group)
#define to_fwat_enum_data(_c) \
	container_of_const(_c, struct fwat_enum_data, group)
#define to_fwat_int_data(_c) \
	container_of_const(_c, struct fwat_int_data, group)
#define to_fwat_str_data(_c) \
	container_of_const(_c, struct fwat_str_data, group)

struct fwat_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct fwat_attribute *attr,
			char *buf);
	ssize_t (*store)(struct kobject *kobj, struct fwat_attribute *attr,
			 const char *buf, size_t count);
	int type;
};

#define to_fwat_attribute(_a) \
	container_of_const(_a, struct fwat_attribute, attr)

#define __FWAT_ATTR(_name, _mode, _show, _store, _type) \
	{								\
		.attr = { .name = __stringify(_name), .mode = _mode },	\
		.show = _show, .store = _store, .type = _type,		\
	}

#define FWAT_ATTR_RO(_prefix, _name, _show, _type) \
	static struct fwat_attribute fwat_##_prefix##_##_name##_attr = \
		__FWAT_ATTR(_name, 0444, _show, NULL, _type)

#define FWAT_ATTR_RW(_prefix, _name, _show, _store, _type) \
	static struct fwat_attribute fwat_##_prefix##_##_name##_attr = \
		__FWAT_ATTR(_name, 0644, _show, _store, _type)

struct fwat_group {
	const struct fwat_group_data *data;
	struct device *dev;
	struct kobject kobj;
};

#define kobj_to_fwat_group(_k) \
	container_of_const(_k, struct fwat_group, kobj)

const struct class firmware_attributes_class = {
	.name = "firmware-attributes",
};
EXPORT_SYMBOL_GPL(firmware_attributes_class);

static const char * const fwat_type_labels[] = {
	[fwat_group_boolean]			= "boolean",
	[fwat_group_enumeration]		= "enumeration",
	[fwat_group_integer]			= "integer",
	[fwat_group_string]			= "string",
};

static void fwat_device_release(struct device *dev)
{
	struct fwat_device *fadev = to_fwat_device(dev);

	kfree(fadev);
}

static ssize_t
type_show(struct kobject *kobj, struct fwat_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", fwat_type_labels[attr->type]);
}

static ssize_t
display_name_show(struct kobject *kobj, struct fwat_attribute *attr, char *buf)
{
	struct fwat_group *group = kobj_to_fwat_group(kobj);
	const char *disp_name = group->data->display_name;

	if (!disp_name)
		return -EOPNOTSUPP;

	return sysfs_emit(buf, "%s\n", disp_name);
}

static ssize_t
display_name_language_code_show(struct kobject *kobj, struct fwat_attribute *attr,
				char *buf)
{
	struct fwat_group *group = kobj_to_fwat_group(kobj);
	const char *lang_code = group->data->language_code;

	if (!lang_code)
		return -EOPNOTSUPP;

	return sysfs_emit(buf, "%s\n", lang_code);
}

static ssize_t
bool_group_show(struct kobject *kobj, struct fwat_attribute *attr, char *buf)
{
	const struct fwat_group *group = kobj_to_fwat_group(kobj);
	const struct fwat_bool_data *data = to_fwat_bool_data(group->data);
	bool val;
	int ret;

	/* show_override does not affect current_value */
	if (data->group.show_override && attr->type != fwat_bool_current_value)
		return data->group.show_override(group->dev, attr->type, buf);

	switch (attr->type) {
	case fwat_bool_current_value:
		ret = data->read(group->dev, data->group.id, &val);
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
bool_group_store(struct kobject *kobj, struct fwat_attribute *attr, const char *buf,
		 size_t count)
{
	const struct fwat_group *group = kobj_to_fwat_group(kobj);
	const struct fwat_bool_data *data = to_fwat_bool_data(group->data);
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	ret = data->write(group->dev, data->group.id, val);
	if (ret)
		return ret;

	return count;
}

static ssize_t
enum_group_show(struct kobject *kobj, struct fwat_attribute *attr, char *buf)
{
	const struct fwat_group *group = kobj_to_fwat_group(kobj);
	const struct fwat_enum_data *data = to_fwat_enum_data(group->data);
	int val_idx, sz = 0;
	int ret;

	/* show_override does not affect current_value */
	if (data->group.show_override && attr->type != fwat_enum_current_value)
		return data->group.show_override(group->dev, attr->type, buf);

	switch (attr->type) {
	case fwat_enum_current_value:
		ret = data->read(group->dev, data->group.id, &val_idx);
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
enum_group_store(struct kobject *kobj, struct fwat_attribute *attr, const char *buf,
		 size_t count)
{
	const struct fwat_group *group = kobj_to_fwat_group(kobj);
	const struct fwat_enum_data *data = to_fwat_enum_data(group->data);
	int val_idx;
	int ret;

	val_idx = __sysfs_match_string(data->possible_vals, -1, buf);
	if (val_idx < 0)
		return val_idx;

	ret = data->write(group->dev, data->group.id, val_idx);
	if (ret)
		return ret;

	return count;
}

static ssize_t
int_group_show(struct kobject *kobj, struct fwat_attribute *attr, char *buf)
{
	const struct fwat_group *group = kobj_to_fwat_group(kobj);
	const struct fwat_int_data *data = to_fwat_int_data(group->data);
	long val;
	int ret;

	/* show_override does not affect current_value */
	if (data->group.show_override && attr->type != fwat_int_current_value)
		return data->group.show_override(group->dev, attr->type, buf);

	switch (attr->type) {
	case fwat_int_current_value:
		ret = data->read(group->dev, data->group.id, &val);
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
int_group_store(struct kobject *kobj, struct fwat_attribute *attr, const char *buf,
		size_t count)
{
	const struct fwat_group *group = kobj_to_fwat_group(kobj);
	const struct fwat_int_data *data = to_fwat_int_data(group->data);
	long val;
	int ret;

	ret = kstrtol(buf, 0, &val);
	if (ret)
		return ret;

	ret = data->write(group->dev, data->group.id, val);
	if (ret)
		return ret;

	return count;
}

static ssize_t
str_group_show(struct kobject *kobj, struct fwat_attribute *attr, char *buf)
{
	const struct fwat_group *group = kobj_to_fwat_group(kobj);
	const struct fwat_str_data *data = to_fwat_str_data(group->data);
	const char *val;
	long len;
	int ret;

	/* show_override does not affect current_value */
	if (data->group.show_override && attr->type != fwat_bool_current_value)
		return data->group.show_override(group->dev, attr->type, buf);

	switch (attr->type) {
	case fwat_str_current_value:
		ret = data->read(group->dev, data->group.id, &val);
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
str_group_store(struct kobject *kobj, struct fwat_attribute *attr, const char *buf,
		size_t count)
{
	const struct fwat_group *group = kobj_to_fwat_group(kobj);
	const struct fwat_str_data *data = to_fwat_str_data(group->data);
	int ret;

	ret = data->write(group->dev, data->group.id, buf);
	if (ret)
		return ret;

	return count;
}

FWAT_ATTR_RO(all, display_name, display_name_show, FWAT_TYPE_NONE);
FWAT_ATTR_RO(all, display_name_language_code, display_name_language_code_show, FWAT_TYPE_NONE);

FWAT_ATTR_RO(bool, type, type_show, fwat_group_boolean);
FWAT_ATTR_RW(bool, current_value, bool_group_show, bool_group_store, fwat_bool_current_value);
FWAT_ATTR_RO(bool, default_value, bool_group_show, fwat_bool_default_value);

FWAT_ATTR_RO(enum, type, type_show, fwat_group_enumeration);
FWAT_ATTR_RW(enum, current_value, enum_group_show, enum_group_store, fwat_enum_current_value);
FWAT_ATTR_RO(enum, default_value, enum_group_show, fwat_enum_default_value);
FWAT_ATTR_RO(enum, possible_values, enum_group_show, fwat_enum_possible_values);

FWAT_ATTR_RO(int, type, type_show, fwat_group_integer);
FWAT_ATTR_RW(int, current_value, int_group_show, int_group_store, fwat_int_current_value);
FWAT_ATTR_RO(int, default_value, int_group_show, fwat_int_default_value);
FWAT_ATTR_RO(int, min_value, int_group_show, fwat_int_min_value);
FWAT_ATTR_RO(int, max_value, int_group_show, fwat_int_max_value);
FWAT_ATTR_RO(int, scalar_increment, int_group_show, fwat_int_scalar_increment);

FWAT_ATTR_RO(str, type, type_show, fwat_group_string);
FWAT_ATTR_RW(str, current_value, str_group_show, str_group_store, fwat_int_current_value);
FWAT_ATTR_RO(str, default_value, str_group_show, fwat_str_default_value);
FWAT_ATTR_RO(str, min_length, str_group_show, fwat_str_min_length);
FWAT_ATTR_RO(str, max_length, str_group_show, fwat_str_max_length);

static struct attribute *fwat_bool_attrs[] = {
	&fwat_bool_type_attr.attr,
	&fwat_all_display_name_attr.attr,
	&fwat_all_display_name_language_code_attr.attr,
	&fwat_bool_current_value_attr.attr,
	&fwat_bool_default_value_attr.attr,
	NULL
};

static struct attribute *fwat_enum_attrs[] = {
	&fwat_enum_type_attr.attr,
	&fwat_all_display_name_attr.attr,
	&fwat_all_display_name_language_code_attr.attr,
	&fwat_enum_current_value_attr.attr,
	&fwat_enum_default_value_attr.attr,
	&fwat_enum_possible_values_attr.attr,
	NULL
};

static struct attribute *fwat_int_attrs[] = {
	&fwat_int_type_attr.attr,
	&fwat_all_display_name_attr.attr,
	&fwat_all_display_name_language_code_attr.attr,
	&fwat_int_current_value_attr.attr,
	&fwat_int_default_value_attr.attr,
	&fwat_int_min_value_attr.attr,
	&fwat_int_max_value_attr.attr,
	&fwat_int_scalar_increment_attr.attr,
	NULL
};

static struct attribute *fwat_str_attrs[] = {
	&fwat_str_type_attr.attr,
	&fwat_all_display_name_attr.attr,
	&fwat_all_display_name_language_code_attr.attr,
	&fwat_str_current_value_attr.attr,
	&fwat_str_default_value_attr.attr,
	&fwat_str_min_length_attr.attr,
	&fwat_str_max_length_attr.attr,
	NULL
};

static umode_t fwat_attr_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	struct fwat_attribute *fwat_attr = to_fwat_attribute(attr);
	struct fwat_group *group = kobj_to_fwat_group(kobj);
	const struct fwat_group_data *data = group->data;

	/* The `type` attribute is always first */
	if (n == 0)
		return attr->mode;

	if (attr == &fwat_all_display_name_attr.attr)
		return data->display_name ? attr->mode : 0;

	if (attr == &fwat_all_display_name_language_code_attr.attr)
		return data->language_code ? attr->mode : 0;

	/* The `current_value` attribute always has type == 0 */
	if (!fwat_attr->type)
		return data->mode;

	return test_bit(fwat_attr->type, &data->fattrs) ? attr->mode : 0;
}

static umode_t fwat_group_visible(struct kobject *kobj)
{
	return true;
}

DEFINE_SYSFS_GROUP_VISIBLE(fwat);

static const struct attribute_group fwat_bool_group = {
	.attrs = fwat_bool_attrs,
	.is_visible = SYSFS_GROUP_VISIBLE(fwat),
};
__ATTRIBUTE_GROUPS(fwat_bool);

static const struct attribute_group fwat_enum_group = {
	.attrs = fwat_enum_attrs,
	.is_visible = SYSFS_GROUP_VISIBLE(fwat),
};
__ATTRIBUTE_GROUPS(fwat_enum);

static const struct attribute_group fwat_int_group = {
	.attrs = fwat_int_attrs,
	.is_visible = SYSFS_GROUP_VISIBLE(fwat),
};
__ATTRIBUTE_GROUPS(fwat_int);

static const struct attribute_group fwat_str_group = {
	.attrs = fwat_str_attrs,
	.is_visible = SYSFS_GROUP_VISIBLE(fwat),
};
__ATTRIBUTE_GROUPS(fwat_str);

static ssize_t
fwat_attr_sysfs_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct fwat_attribute *fwat_attr = to_fwat_attribute(attr);

	if (!fwat_attr->show)
		return -EOPNOTSUPP;

	return fwat_attr->show(kobj, fwat_attr, buf);
}

static ssize_t
fwat_attr_sysfs_store(struct kobject *kobj, struct attribute *attr, const char *buf,
		      size_t count)
{
	struct fwat_attribute *fwat_attr = to_fwat_attribute(attr);

	if (!fwat_attr->show)
		return -EOPNOTSUPP;

	return fwat_attr->store(kobj, fwat_attr, buf, count);
}

static void fwat_group_release(struct kobject *kobj)
{
	struct fwat_group *group = kobj_to_fwat_group(kobj);

	kfree(group);
}

static const struct sysfs_ops fwat_attr_sysfs_ops = {
	.show = fwat_attr_sysfs_show,
	.store = fwat_attr_sysfs_store,
};

static const struct kobj_type fwat_boolean_ktype = {
	.sysfs_ops = &fwat_attr_sysfs_ops,
	.release = fwat_group_release,
	.default_groups = fwat_bool_groups,
};

static const struct kobj_type fwat_enumeration_ktype = {
	.sysfs_ops = &fwat_attr_sysfs_ops,
	.release = fwat_group_release,
	.default_groups = fwat_enum_groups,
};

static const struct kobj_type fwat_integer_ktype = {
	.sysfs_ops = &fwat_attr_sysfs_ops,
	.release = fwat_group_release,
	.default_groups = fwat_int_groups,
};

static const struct kobj_type fwat_string_ktype = {
	.sysfs_ops = &fwat_attr_sysfs_ops,
	.release = fwat_group_release,
	.default_groups = fwat_str_groups,
};

static int __fwat_create_group(struct fwat_device *fadev, const struct kobj_type *ktype,
			       const struct fwat_group_data *data)
{
	struct fwat_group *group;
	int ret;

	group = kzalloc(sizeof(*group), GFP_KERNEL);
	if (!group)
		return -ENOMEM;

	group->dev = &fadev->dev;
	group->data = data;

	group->kobj.kset = fadev->attrs_kset;
	ret = kobject_init_and_add(&group->kobj, ktype, NULL, "%s", data->name);
	if (ret) {
		kobject_put(&group->kobj);
		return ret;
	}

	kobject_uevent(&group->kobj, KOBJ_ADD);

	return 0;
}

static void fwat_remove_auto_groups(struct fwat_device *fadev)
{
	struct kobject *pos, *n;

	list_for_each_entry_safe(pos, n, &fadev->attrs_kset->list, entry)
		kobject_put(pos);
}

int fwat_create_bool_group(struct fwat_device *fadev, const struct fwat_bool_data *data)
{
	return __fwat_create_group(fadev, &fwat_boolean_ktype, &data->group);
}
EXPORT_SYMBOL_GPL(fwat_create_bool_group);

int fwat_create_enum_group(struct fwat_device *fadev, const struct fwat_enum_data *data)
{
	return __fwat_create_group(fadev, &fwat_enumeration_ktype, &data->group);
}
EXPORT_SYMBOL_GPL(fwat_create_enum_group);

int fwat_create_int_group(struct fwat_device *fadev, const struct fwat_int_data *data)
{
	return __fwat_create_group(fadev, &fwat_integer_ktype, &data->group);
}
EXPORT_SYMBOL_GPL(fwat_create_int_group);

int fwat_create_str_group(struct fwat_device *fadev, const struct fwat_str_data *data)
{
	return __fwat_create_group(fadev, &fwat_string_ktype, &data->group);
}
EXPORT_SYMBOL_GPL(fwat_create_str_group);

/**
 * fwat_device_register - Create and register a firmware-attributes class
 *			  device
 * @parent: Parent device
 * @name: Name of the class device
 * @drvdata: Drvdata of the class device
 * @groups: Extra groups for the "attributes" directory
 *
 * Return: pointer to the new fwat_device on success, ERR_PTR on failure
 */
struct fwat_device *
fwat_device_register(struct device *parent, const char *name, void *drvdata,
		     const struct attribute_group **groups)
{
	struct fwat_device *fadev;
	int ret;

	if (!parent || !name)
		return ERR_PTR(-EINVAL);

	fadev = kzalloc(sizeof(*fadev), GFP_KERNEL);
	if (!fadev)
		return ERR_PTR(-ENOMEM);

	fadev->groups = groups;
	fadev->dev.class = &firmware_attributes_class;
	fadev->dev.parent = parent;
	fadev->dev.release = fwat_device_release;
	dev_set_drvdata(&fadev->dev, drvdata);
	ret = dev_set_name(&fadev->dev, "%s", name);
	if (ret) {
		kfree(fadev);
		return ERR_PTR(ret);
	}
	ret = device_register(&fadev->dev);
	if (ret)
		return ERR_PTR(ret);

	fadev->attrs_kset = kset_create_and_add("attributes", NULL, &fadev->dev.kobj);
	if (!fadev->attrs_kset) {
		ret = -ENOMEM;
		goto out_device_unregister;
	}

	ret = sysfs_create_groups(&fadev->attrs_kset->kobj, groups);
	if (ret)
		goto out_kset_unregister;

	return fadev;

out_kset_unregister:
	kset_unregister(fadev->attrs_kset);

out_device_unregister:
	device_unregister(&fadev->dev);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(fwat_device_register);

void fwat_device_unregister(struct fwat_device *fadev)
{
	if (!fadev)
		return;

	fwat_remove_auto_groups(fadev);
	sysfs_remove_groups(&fadev->attrs_kset->kobj, fadev->groups);
	kset_unregister(fadev->attrs_kset);
	device_unregister(&fadev->dev);
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
