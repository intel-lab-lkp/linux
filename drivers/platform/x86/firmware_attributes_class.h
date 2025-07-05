/* SPDX-License-Identifier: GPL-2.0 */

/* Firmware attributes class helper module */

#ifndef FW_ATTR_CLASS_H
#define FW_ATTR_CLASS_H

#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/list.h>

extern const struct class firmware_attributes_class;

/**
 * struct fwat_device - The firmware-attributes device
 * @dev: The class device.
 * @attrs_kobj: The "attributes" root kobject.
 * @groups: Sysfs groups attached to the @attrs_kobj.
 */
struct fwat_device {
	struct device dev;
	struct kset *attrs_kset;
	const struct attribute_group **groups;
};

#define to_fwat_device(_d)	container_of_const(_d, struct fwat_device, dev)

enum fwat_group_type {
	fwat_group_boolean,
	fwat_group_enumeration,
	fwat_group_integer,
	fwat_group_string,
};

enum fwat_bool_attrs {
	fwat_bool_current_value,
	fwat_bool_default_value,
	fwat_bool_attrs_last
};

#define FWAT_BOOL_CURRENT_VALUE			BIT(fwat_bool_current_value)
#define FWAT_BOOL_DEFAULT_VALUE			BIT(fwat_bool_default_value)
#define FWAT_BOOL_ALL_ATTRS			GENMASK(fwat_bool_attrs_last, 0)

enum fwat_enum_attrs {
	fwat_enum_current_value,
	fwat_enum_default_value,
	fwat_enum_possible_values,
	fwat_enum_attrs_last
};

#define FWAT_ENUM_CURRENT_VALUE			BIT(fwat_enum_current_value)
#define FWAT_ENUM_DEFAULT_VALUE			BIT(fwat_enum_default_value)
#define FWAT_ENUM_POSSIBLE_VALUES		BIT(fwat_enum_possible_values)
#define FWAT_ENUM_ALL_ATTRS			GENMASK(fwat_enum_attrs_last, 0)

enum fwat_int_attrs {
	fwat_int_current_value,
	fwat_int_default_value,
	fwat_int_min_value,
	fwat_int_max_value,
	fwat_int_scalar_increment,
	fwat_int_attrs_last
};

#define FWAT_INT_CURRENT_VALUE			BIT(fwat_int_current_value)
#define FWAT_INT_DEFAULT_VALUE			BIT(fwat_int_default_value)
#define FWAT_INT_MIN_VALUE			BIT(fwat_int_min_value)
#define FWAT_INT_MAX_VALUE			BIT(fwat_int_max_value)
#define FWAT_INT_SCALAR_INCREMENT		BIT(fwat_int_scalar_increment)
#define FWAT_INT_ALL_ATTRS			GENMASK(fwat_int_attrs_last, 0)

enum fwat_str_attrs {
	fwat_str_current_value,
	fwat_str_default_value,
	fwat_str_min_length,
	fwat_str_max_length,
	fwat_str_attrs_last
};

#define FWAT_STR_CURRENT_VALUE			BIT(fwat_str_current_value)
#define FWAT_STR_DEFAULT_VALUE			BIT(fwat_str_default_value)
#define FWAT_STR_MIN_LENGTH			BIT(fwat_str_min_length)
#define FWAT_STR_MAX_LENGTH			BIT(fwat_str_max_length)
#define FWAT_STR_ALL_ATTRS			GENMASK(fwat_str_attrs_last, 0)

static_assert(fwat_bool_current_value == 0);
static_assert(fwat_enum_current_value == 0);
static_assert(fwat_int_current_value == 0);
static_assert(fwat_str_current_value == 0);

/**
 * struct fwat_group_data - Data struct common between group types
 * @id: Group ID defined by the user.
 * @name: Name of the group.
 * @display_name: Name showed in the display_name attribute. (Optional)
 * @language_code: Language code showed in the display_name_language_code
 *                 attribute. (Optional)
 * @mode: Mode for the current_value attribute. All other attributes will have
 *        0444 permissions.
 * @fattrs: Bitmap of selected attributes for this group type.
 * @show_override: Custom show method for attributes in this group, except for
 *		   the current_value attribute, for which the a `read` callback
 *		   will still be used. (Optional)
 *
 * NOTE: This struct is not meant to be defined directly. It is supposed to be
 * embedded and defined as part of fwat_[type]_data structs.
 */
struct fwat_group_data {
	long id;
	umode_t mode;
	const char *name;
	const char *display_name;
	const char *language_code;
	unsigned long fattrs;
	ssize_t (*show_override)(struct device *dev, int type, char *buf);
};

/**
 * struct fwat_bool_data - Data struct for the boolean group type
 * @read: Read callback for the current_value attribute.
 * @write: Write callback for the current_value attribute.
 * @default_val: Default value.
 * @group: Group data.
 */
struct fwat_bool_data {
	int (*read)(struct device *dev, long id, bool *val);
	int (*write)(struct device *dev, long id, bool val);
	bool default_val;
	struct fwat_group_data group;
};

/**
 * struct fwat_enum_data - Data struct for the enumeration group type
 * @read: Read callback for the current_value attribute.
 * @write: Write callback for the current_value attribute.
 * @default_idx: Index of the default value in the @possible_vals array.
 * @possible_vals: Array of possible value strings for this group type.
 * @group: Group data.
 *
 * NOTE: The `val_idx` argument in the @write callback is guaranteed to be a
 *       valid (within bounds) index. However, the user is in charge of writing
 *       valid indexes to the `*val_idx` argument of the @read callback.
 *       Failing to do so may result in an OOB access.
 */
struct fwat_enum_data {
	int (*read)(struct device *dev, long id, int *val_idx);
	int (*write)(struct device *dev, long id, int val_idx);
	int default_idx;
	const char * const *possible_vals;
	struct fwat_group_data group;
};

/**
 * struct fwat_int_data - Data struct for the integer group type
 * @read: Read callback for the current_value attribute.
 * @write: Write callback for the current_value attribute.
 * @default_val: Default value.
 * @min_val: Minimum value.
 * @max_val: Maximum value.
 * @increment: Scalar increment for this value.
 * @group: Group data.
 *
 * NOTE: The @min_val, @max_val, @increment constraints are merely informative.
 *       These values are not enforced in any of the callbacks.
 */
struct fwat_int_data {
	int (*read)(struct device *dev, long id, long *val);
	int (*write)(struct device *dev, long id, long val);
	long default_val;
	long min_val;
	long max_val;
	long increment;
	struct fwat_group_data group;
};

/**
 * struct fwat_str_data - Data struct for the string group type
 * @read: Read callback for the current_value attribute.
 * @write: Write callback for the current_value attribute.
 * @default_val: Default value.
 * @min_len: Minimum string length.
 * @max_len: Maximum string length.
 * @group: Group data.
 *
 * NOTE: The @min_len, @max_len constraints are merely informative. These
 *       values are not enforced in any of the callbacks.
 */
struct fwat_str_data {
	int (*read)(struct device *dev, long id, const char **buf);
	int (*write)(struct device *dev, long id, const char *buf);
	const char *default_val;
	long min_len;
	long max_len;
	struct fwat_group_data group;
};

#define __FWAT_GROUP(_name, _disp_name, _mode, _fattrs) \
	{ .name = __stringify(_name), .display_name = _disp_name, .mode = _mode, .fattrs = _fattrs }

/**
 * DEFINE_FWAT_BOOL_GROUP - Convenience macro to quickly define an static
 *                          struct fwat_bool_data instance
 * @_name: Name of the group.
 * @_disp_name: Name showed in the display_name attribute. (Optional)
 * @_def_val: Default value.
 * @_mode: Mode for the current_value attribute. All other attributes will have
 *         0444 permissions.
 * @_fattrs: Bitmap of selected attributes for this group type.
 *
 * `read` and `write` callbacks are required to be already defined as
 * `_name##_read` and `_name##_write` respectively.
 */
#define DEFINE_FWAT_BOOL_GROUP(_name, _disp_name, _def_val, _mode, _fattrs) \
	static const struct fwat_bool_data _name##_group_data = {	\
		.read = _name##_read,					\
		.write = _name##_write,					\
		.default_val = _def_val,				\
		.group = __FWAT_GROUP(_name, _disp_name, _mode, _fattrs), \
	}

/**
 * DEFINE_FWAT_ENUM_GROUP - Convenience macro to quickly define an static
 *                          struct fwat_enum_data instance
 * @_name: Name of the group.
 * @_disp_name: Name showed in the display_name attribute. (Optional)
 * @_def_idx: Index of the default value in the @_poss_vals array.
 * @_poss_vals: Array of possible value strings for this group type.
 * @_mode: Mode for the current_value attribute. All other attributes will have
 *         0444 permissions.
 * @_fattrs: Bitmap of selected attributes for this group type.
 *
 * `read` and `write` callbacks are required to be already defined as
 * `_name##_read` and `_name##_write` respectively.
 *
 * NOTE: The `val_idx` argument in the `write` callback is guaranteed to be a
 *       valid (within bounds) index. However, the user is in charge of writing
 *       valid indexes to the `*val_idx` argument of the `read` callback.
 *       Failing to do so may result in an OOB access.
 */
#define DEFINE_FWAT_ENUM_GROUP(_name, _disp_name, _poss_vals, _def_idx, _mode, _fattrs) \
	static const struct fwat_enum_data _name##_group_data = {	\
		.read = _name##_read,					\
		.write = _name##_write,					\
		.default_idx = _def_idx,				\
		.possible_vals = _poss_vals,				\
		.group = __FWAT_GROUP(_name, _disp_name, _mode, _fattrs), \
	}

/**
 * DEFINE_FWAT_INT_GROUP - Convenience macro to quickly define an static
 *                         struct fwat_int_data instance
 * @_name: Name of the group.
 * @_disp_name: Name showed in the display_name attribute. (Optional)
 * @_def_val: Default value.
 * @_min: Minimum value.
 * @_max: Maximum value.
 * @_inc: Scalar increment for this value.
 * @_mode: Mode for the current_value attribute. All other attributes will have
 *         0444 permissions.
 * @_fattrs: Bitmap of selected attributes for this group type.
 *
 * `read` and `write` callbacks are required to be already defined as
 * `_name##_read` and `_name##_write` respectively.
 *
 * NOTE: The @_min, @_max, @_inc constraints are merely informative. These
 *       values are not enforced in any of the callbacks.
 */
#define DEFINE_FWAT_INT_GROUP(_name, _disp_name, _def_val, _min, _max, _inc, _mode, _fattrs) \
	static const struct fwat_int_data _name##_group_data = {	\
		.read = _name##_read,					\
		.write = _name##_write,					\
		.default_val = _def_val,				\
		.min_val = _min,					\
		.max_val = _max,					\
		.increment = _inc,					\
		.group = __FWAT_GROUP(_name, _disp_name, _mode, _fattrs), \
	}

/**
 * DEFINE_FWAT_STR_GROUP - Convenience macro to quickly define an static
 *                         struct fwat_str_data instance
 * @_name: Name of the group.
 * @_disp_name: Name showed in the display_name attribute. (Optional)
 * @_def_val: Default value.
 * @_min: Minimum string length.
 * @_max: Maximum string length.
 * @_mode: Mode for the current_value attribute. All other attributes will have
 *         0444 permissions.
 * @_fattrs: Bitmap of selected attributes for this group type.
 *
 * `read` and `write` callbacks are required to be already defined as
 * `_name##_read` and `_name##_write` respectively.
 *
 * NOTE: The @_min, @_max constraints are merely informative. These values are
 *       not enforced in any of the callbacks.
 */
#define DEFINE_FWAT_STR_GROUP(_name, _disp_name, _def_val, _min, _max, _mode, _fattrs) \
	static const struct fwat_str_data _name##_group_data = {	\
		.read = _name##_read,					\
		.write = _name##_write,					\
		.default_val = _def_val,				\
		.min_len = _min,					\
		.max_len = _max,					\
		.group = __FWAT_GROUP(_name, _disp_name, _mode, _fattrs), \
	}

int fwat_create_bool_group(struct fwat_device *fadev,
			   const struct fwat_bool_data *data);
int fwat_create_enum_group(struct fwat_device *fadev,
			   const struct fwat_enum_data *data);
int fwat_create_int_group(struct fwat_device *fadev,
			  const struct fwat_int_data *data);
int fwat_create_str_group(struct fwat_device *fadev,
			  const struct fwat_str_data *data);

/**
 * fwat_create_group - Convenience generic macro to create a group
 * @_dev: fwat_device
 * @_data: One of fwat_{bool,enum,int,str}_data instance
 *
 * This macro (and associated functions) creates a sysfs group under the
 * 'attributes' directory, which is located in the class device root directory.
 *
 * See Documentation/ABI/testing/sysfs-class-firmware-attributes for details.
 *
 * The @_data associated with this group may be created either statically,
 * through DEFINE_FWAT_*_GROUP macros or dynamically, in which case the user
 * would have allocate and fill the struct manually. The dynamic approach should
 * be preferred when group constraints and/or visibility is decided dynamically.
 *
 * Example:
 *
 * static int stat_read(...){...};
 * static int stat_write(...){...};
 *
 * DEFINE_FWAT_(BOOL|ENUM|INT|STR)_GROUP(stat, ...);
 *
 * static int create_groups(struct fwat_device *fadev)
 * {
 *	struct fwat_enum_data *dyn_group_data;
 *
 *	dyn_group_data = kzalloc(...);
 *	// Fill the data
 *	...
 *	fwat_create_group(fadev, &stat_group_data);
 *	fwat_create_group(fadev, &dyn_group_data);
 *	fwat_create_group(...);
 *	...
 * }
 *
 * Return: 0 on success, -errno on failure
 */
#define fwat_create_group(_dev, _data) \
	_Generic((_data),							\
		 const struct fwat_bool_data * : fwat_create_bool_group,	\
		 const struct fwat_enum_data * : fwat_create_enum_group,	\
		 const struct fwat_int_data * : fwat_create_int_group,		\
		 const struct fwat_str_data * : fwat_create_str_group)		\
		(_dev, _data)

struct fwat_device * __must_check
fwat_device_register(struct device *parent, const char *name, void *drvdata,
		     const struct attribute_group **groups);

void fwat_device_unregister(struct fwat_device *fwadev);

struct fwat_device * __must_check
devm_fwat_device_register(struct device *parent, const char *name, void *data,
			  const struct attribute_group **groups);

#endif /* FW_ATTR_CLASS_H */
