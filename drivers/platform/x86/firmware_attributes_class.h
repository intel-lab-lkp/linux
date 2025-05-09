/* SPDX-License-Identifier: GPL-2.0 */

/* Firmware attributes class helper module */

#ifndef FW_ATTR_CLASS_H
#define FW_ATTR_CLASS_H

#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

extern const struct class firmware_attributes_class;

/**
 * struct fwat_device - The firmware-attributes device
 * @dev: The class device.
 * @attrs_kobj: The "attributes" root kobject.
 * @groups: Sysfs groups attached to the @attrs_kobj.
 * @auto_groups: Sysgs groups generated from &struct fwat_attr_config attached.
 * to the @attrs_kobj
 */
struct fwat_device {
	struct device *dev;
	struct kobject attrs_kobj;
	const struct attribute_group **groups;
	const struct attribute_group **auto_groups;
};

#define to_fwat_device(_k)	container_of_const(_k, struct fwat_device, attrs_kobj)

/**
 * struct fwat_attribute - The firmware-attributes's custom attribute
 * @attr: Embedded struct attribute.
 * @aux: Auxiliary number defined by the user.
 * @show: Show method called by the "attributes" kobject's ktype.
 * @store: Store method called by the "attributes" kobject's ktype.
 */
struct fwat_attribute {
	struct attribute attr;
	ssize_t (*show)(struct device *dev, const struct fwat_attribute *attr,
			char *buf);
	ssize_t (*store)(struct device *dev, const struct fwat_attribute *attr,
			 const char *buf, size_t count);
};

#define to_fwat_attribute(_a) container_of_const(_a, struct fwat_attribute, attr)

enum fwat_attr_type {
	fwat_type_integer,
	fwat_type_boolean,
	fwat_type_string,
	fwat_type_enumeration,
};

enum fwat_property {
	FWAT_PROP_DISPLAY_NAME,
	FWAT_PROP_LANGUAGE_CODE,
	FWAT_PROP_DEFAULT,

	FWAT_INT_PROP_MIN,
	FWAT_INT_PROP_MAX,
	FWAT_INT_PROP_INCREMENT,

	FWAT_STR_PROP_MIN,
	FWAT_STR_PROP_MAX,

	FWAT_ENUM_PROP_POSSIBLE_VALUES,
};

struct fwat_attr_config;

/**
 * struct fwat_attr_ops - Operations for a firmware *attribute*
 * @prop_read: Callback for retrieving each configured property of an attribute.
 * @integer_read: Callback for reading the current_value of an attribute of
 *                type *integer*.
 * @integer_write: Callback for writing the current_value of an attribute of
 *                 type *integer*.
 * @boolean_read: Callback for reading the current_value of an attribute of type
 *                *boolean*.
 * @boolean_write: Callback for writing the current_value of an attribute of
 *                 type *boolean*.
 * @string_read: Callback for reading the current_value of an attribute of type
 *               *string*.
 * @string_write: Callback for writing the current_value of an attribute of type
 *                *string*.
 * @enumeration_read: Callback for reading the current_value of an attribute of
 *                    type *enumeration*.
 * @enumeration_write: Callback for writing the current_value of an attribute
 *                     of type *enumeration*.
 */
struct fwat_attr_ops {
	ssize_t (*prop_read)(struct device *dev, long aux,
			     enum fwat_property prop, const char *buf);
	union {
		struct {
			int (*integer_read)(struct device *dev, long aux,
					    long *val);
			int (*integer_write)(struct device *dev, long aux,
					     long val);
		};
		struct {
			int (*boolean_read)(struct device *dev, long aux,
					    bool *val);
			int (*boolean_write)(struct device *dev, long aux,
					     bool val);
		};
		struct {
			int (*string_read)(struct device *dev, long aux,
					   const char **str);
			int (*string_write)(struct device *dev, long aux,
					    const char *str);
		};
		struct {
			int (*enumeration_read)(struct device *dev, long aux,
						const char **str);
			int (*enumeration_write)(struct device *dev, long aux,
						 const char *str);
		};
	};
};

/**
 * struct fwat_attr_config - Configuration for a single firmware *attribute*
 * @name: Name of the sysfs group associated with this *attribute*.
 * @type: Type of this *attribute*.
 * @aux: Auxiliary number defined by the user, which will be passed to
 *       read/write callbacks.
 * @ops: Operations for this *attribute*.
 * @props: Array of properties of this *attribute*.
 * @num_props: Size of the props array.
 */
struct fwat_attr_config {
	const char *name;
	enum fwat_attr_type type;
	long aux;
	const struct fwat_attr_ops *ops;
	const enum fwat_property *props;
	size_t num_props;
};

/**
 * DEFINE_SIMPLE_FWAT_OPS() - Define static &struct fwat_attr_ops for a simple
 *                            *attribute* with no properties, i.e. No
 *                            &fwat_attr_ops.read_prop callback
 * @_name: Prefix of the `read` and `write` callbacks.
 * @_type: Firmware *attribute* type.
 *
 * Example:
 *
 * static int example_read(...) {...}
 * static int example_write(...) {...}
 *
 * DEFINE_SIMPLE_FWAT_OPS(example, ...);
 */
#define DEFINE_SIMPLE_FWAT_OPS(_name, _type)		\
	static const struct fwat_attr_ops _name##_ops = { \
		._type##_read = _name##_read,		\
		._type##_write = _name##_write,		\
	}

/**
 * DEFINE_FWAT_OPS() - Define static &struct fwat_attr_ops with all callbacks.
 * @_name: Prefix of the `read` and `write` callbacks.
 * @_type: Firmware *attribute* type.
 *
 * Example:
 *
 * static int example_read(...) {...}
 * static int example_write(...) {...}
 * static int example_prop_read(...) {...}
 *
 * DEFINE_FWAT_OPS(example, ...);
 */
#define DEFINE_FWAT_OPS(_name, _type)			\
	static const struct fwat_attr_ops _name##_ops = { \
		.prop_read = _name##_prop_read,		\
		._type##_read = _name##_read,		\
		._type##_write = _name##_write,		\
	}

/**
 * FWAT_CONFIG() - Configuration pointer for a single firmware *attribute*.
 * @_name: String name of this *attribute*.
 * @_type: Firmware *attribute* type.
 * @_ops: Pointer to &struct fwat_attr_ops.
 * @_props: Pointer to a enum fwat_property array.
 * @_num_props: Size of the @_props array.
 *
 * This is a convenience macro to quickly construct a &struct fwat_attr_config
 * array, which will be passed to &struct fwat_dev_config.
 *
 * Example:
 *
 * static int example_read(...) {...}
 * static int example_write(...) {...}
 * static int example_prop_read(...) {...}
 *
 * DEFINE_FWAT_OPS(example, ...);
 *
 * static const enum fwat_property props[] = {...};
 *
 * static const struct fwat_attr_config * const attrs_config[] = {
 *	FWAT_CONFIG(example, ..., &example_fwat_ops, props,
 *		    ARRAY_SIZE(props)),
 *	...
 *	NULL
 * };
 *
 * static const struct fwat_dev_config fdev_config = {
 *	.attrs_config = attrs_config,
 *	...
 * }
 */
#define FWAT_CONFIG(_name, _type, _ops, _props, _num_props) \
	(&(const struct fwat_attr_config) {	\
		.name = _name,				\
		.type = fwat_type_##_type,		\
		.ops = _ops,				\
		.num_props = _num_props,		\
		.props = _props,			\
	})

/**
 * FWAT_CONFIG_AUX() - Configuration pointer for a single firmware *attribute*
 *                     with an auxiliary number defined by the user
 * @_name: String name of this *attribute*.
 * @_type: Firmware *attribute* type.
 * @_aux: Auxiliary number defined by the user.
 * @_ops: Pointer to &struct fwat_attr_ops.
 * @_props: Pointer to a enum fwat_property array.
 * @_num_props: Size of the @_props array.
 *
 * This is a convenience macro to quickly construct a &struct fwat_attr_config
 * array, which will be passed to &struct fwat_dev_config.
 *
 * Example:
 *
 * static int example_read(...) {...}
 * static int example_write(...) {...}
 * static int example_prop_read(...) {...}
 *
 * DEFINE_FWAT_OPS(example, ...);
 *
 * static const enum fwat_property props[] = {...};
 *
 * static const struct fwat_attr_config * const config[] = {
 *	FWAT_CONFIG_AUX(example, ..., n, &example_fwat_ops, props,
 *			ARRAY_SIZE(props)),
 *	...
 *	NULL
 * };
 *
 * static const struct fwat_dev_config fdev_config = {
 *	.attrs_config = attrs_config,
 *	...
 * }
 */
#define FWAT_CONFIG_AUX(_name, _type, _aux, _ops, _props, _num_props) \
	(&(const struct fwat_attr_config) {		\
		.name = _name,				\
		.type = fwat_type_##_type,		\
		.aux = _aux,				\
		.ops = _ops,				\
		.num_props = _num_props,		\
		.props = _props,			\
	})

/**
 * struct fwat_dev_config - Configuration for this devices's
 *                          &fwat_device.auto_groups
 * @attrs_config: NULL terminated &struct fwat_attr_config array.
 * @is_visible: Optional visibility callback to determine the visibility
 *              of each auto_group.
 */
struct fwat_dev_config {
	const struct fwat_attr_config *const *attrs_config;
	bool (*is_visible)(struct device *dev, const struct fwat_attr_config *config);
};

struct fwat_device * __must_check
fwat_device_register(struct device *parent, const char *name, void *data,
		     const struct fwat_dev_config *config,
		     const struct attribute_group **groups);

void fwat_device_unregister(struct fwat_device *fwadev);

struct fwat_device * __must_check
devm_fwat_device_register(struct device *parent, const char *name, void *data,
			  const struct fwat_dev_config *config,
			  const struct attribute_group **groups);

#endif /* FW_ATTR_CLASS_H */
