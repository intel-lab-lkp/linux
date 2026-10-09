/* SPDX-License-Identifier: GPL-2.0-only */
/*
    hwmon.h - part of lm_sensors, Linux kernel modules for hardware monitoring

    This file declares helper functions for the sysfs class "hwmon",
    for use by sensors drivers.

    Copyright (C) 2005 Mark M. Hoffman <mhoffman@lightlink.com>

*/

#ifndef _HWMON_H_
#define _HWMON_H_

#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <uapi/linux/hwmon.h>

struct device;
struct attribute_group;

/**
 * struct hwmon_ops - hwmon device operations
 * @visible:	Static visibility. If non-zero, 'is_visible' is ignored.
 * @is_visible: Callback to return attribute visibility. Mandatory unless
 *		'visible' is non-zero.
 *		Parameters are:
 *		@const void *drvdata:
 *			Pointer to driver-private data structure passed
 *			as argument to hwmon_device_register_with_info().
 *		@type:	Sensor type
 *		@attr:	Sensor attribute
 *		@channel:
 *			Channel number
 *		The function returns the file permissions.
 *		If the return value is 0, no attribute will be created.
 * @read:	Read callback for data attributes. Mandatory if readable
 *		data attributes are present.
 *		Parameters are:
 *		@dev:	Pointer to hardware monitoring device
 *		@type:	Sensor type
 *		@attr:	Sensor attribute
 *		@channel:
 *			Channel number
 *		@val:	Pointer to returned value
 *		The function returns 0 on success or a negative error number.
 * @read_string:
 *		Read callback for string attributes. Mandatory if string
 *		attributes are present.
 *		Parameters are:
 *		@dev:	Pointer to hardware monitoring device
 *		@type:	Sensor type
 *		@attr:	Sensor attribute
 *		@channel:
 *			Channel number
 *		@str:	Pointer to returned string
 *		The function returns 0 on success or a negative error number.
 * @write:	Write callback for data attributes. Mandatory if writeable
 *		data attributes are present.
 *		Parameters are:
 *		@dev:	Pointer to hardware monitoring device
 *		@type:	Sensor type
 *		@attr:	Sensor attribute
 *		@channel:
 *			Channel number
 *		@val:	Value to write
 *		The function returns 0 on success or a negative error number.
 */
struct hwmon_ops {
	umode_t visible;
	umode_t (*is_visible)(const void *drvdata, enum hwmon_sensor_types type,
			      u32 attr, int channel);
	int (*read)(struct device *dev, enum hwmon_sensor_types type,
		    u32 attr, int channel, long *val);
	int (*read_string)(struct device *dev, enum hwmon_sensor_types type,
		    u32 attr, int channel, const char **str);
	int (*write)(struct device *dev, enum hwmon_sensor_types type,
		     u32 attr, int channel, long val);
};

/**
 * struct hwmon_channel_info - Channel information
 * @type:	Channel type.
 * @config:	Pointer to NULL-terminated list of channel parameters.
 *		Use for per-channel attributes.
 */
struct hwmon_channel_info {
	enum hwmon_sensor_types type;
	const u32 *config;
};

#define HWMON_CHANNEL_INFO(stype, ...)		\
	(&(const struct hwmon_channel_info) {	\
		.type = hwmon_##stype,		\
		.config = (const u32 []) {	\
			__VA_ARGS__, 0		\
		}				\
	})

/**
 * struct hwmon_chip_info - Chip configuration
 * @ops:	Pointer to hwmon operations.
 * @info:	Null-terminated list of channel information.
 */
struct hwmon_chip_info {
	const struct hwmon_ops *ops;
	const struct hwmon_channel_info * const *info;
};

/* hwmon_device_register() is deprecated */
struct device *hwmon_device_register(struct device *dev);

/*
 * hwmon_device_register_with_groups() and
 * devm_hwmon_device_register_with_groups() are deprecated.
 */
struct device *
hwmon_device_register_with_groups(struct device *dev, const char *name,
				  void *drvdata,
				  const struct attribute_group **groups);
struct device *
devm_hwmon_device_register_with_groups(struct device *dev, const char *name,
				       void *drvdata,
				       const struct attribute_group **groups);
struct device *
hwmon_device_register_with_info(struct device *dev,
				const char *name, void *drvdata,
				const struct hwmon_chip_info *info,
				const struct attribute_group **extra_groups);
struct device *
hwmon_device_register_for_thermal(struct device *dev, const char *name,
				  void *drvdata);
struct device *
devm_hwmon_device_register_with_info(struct device *dev,
				const char *name, void *drvdata,
				const struct hwmon_chip_info *info,
				const struct attribute_group **extra_groups);

void hwmon_device_unregister(struct device *dev);

int hwmon_notify_event(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel);

char *hwmon_sanitize_name(const char *name);
char *devm_hwmon_sanitize_name(struct device *dev, const char *name);

void hwmon_lock(struct device *dev);
void hwmon_unlock(struct device *dev);

DEFINE_GUARD(hwmon_lock, struct device *, hwmon_lock(_T), hwmon_unlock(_T))

/**
 * hwmon_is_bad_char - Is the char invalid in a hwmon name
 * @ch: the char to be considered
 *
 * hwmon_is_bad_char() can be used to determine if the given character
 * may not be used in a hwmon name.
 *
 * Returns true if the char is invalid, false otherwise.
 */
static inline bool hwmon_is_bad_char(const char ch)
{
	switch (ch) {
	case '-':
	case '*':
	case ' ':
	case '\t':
	case '\n':
		return true;
	default:
		return false;
	}
}

#endif
