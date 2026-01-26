// SPDX-License-Identifier: GPL-2.0-or-later
#include "asm-generic/errno-base.h"
#include "asm-generic/errno.h"
#include "linux/err.h"
#include "linux/input-event-codes.h"
#include "linux/input.h"
#include "linux/input/sparse-keymap.h"
#include "linux/power_supply.h"
#include "linux/stddef.h"
#include "linux/string.h"
#include <linux/cleanup.h>
#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/platform_profile.h>
#include <linux/sysfs.h>
#include <linux/wmi.h>

#define DRV_NAME "tongfang-mifs-wmi"
#define TONGFANG_MIFS_GUID "B60BFB48-3E5B-49E4-A0E9-8CFFE1B3434B"
#define TONGFANG_EVENT_GUID "46C93E13-EE9B-4262-8488-563BCA757FEF"
#define WMI_BUFFER_SIZE 32

static char *profile_state_path = "/sys/firmware/acpi/platform_profile_last";
module_param(profile_state_path, charp, 0644);
MODULE_PARM_DESC(profile_state_path, "Path to store profile state");

enum tongfang_mifs_operation {
	WMI_METHOD_GET = 250,
	WMI_METHOD_SET = 251,
};

enum tongang_mifs_function {
	WMI_FN_SYSTEM_PER_MODE = 8,
	WMI_FN_GPU_MODE = 9,
	WMI_FN_KBD_TYPE = 10,
	WMI_FN_FN_LOCK = 11,
	WMI_FN_TP_LOCK = 12,
	WMI_FN_FAN_SPEEDS = 13,
	WMI_FN_RGB_KB_MODE = 16,
	WMI_FN_RGB_KB_COLOR = 17,
	WMI_FN_RGB_KB_BRIGHTNESS = 18,
	WMI_FN_SYSTEM_AC_TYPE = 19,
	WMI_FN_MAX_FAN_SWITCH = 20,
	WMI_FN_MAX_FAN_SPEED = 21,
	WMI_FN_CPU_THERMOMETER = 22,
	WMI_FN_CPU_POWER = 23,
};

enum tongfang_system_ac_mode {
	WMI_SYSTEM_AC_TYPEC = 1,
	WMI_SYSTEM_AC_CIRCULARHOLE = 2, /* Unknown type, this is unused in the original driver */
};

enum tongfang_mifs_power_profile {
	WMI_PP_BALANCED = 0,
	WMI_PP_PERFORMANCE = 1,
	WMI_PP_QUIET = 2,
	WMI_PP_FULL_SPEED = 3,
};

enum tongfang_mifs_event_id {
	WMI_EVENT_RESERVED_1 = 1,
	WMI_EVENT_RESERVED_2 = 2,
	WMI_EVENT_RESERVED_3 = 3,
	WMI_EVENT_AIRPLANE_MODE = 4,
	WMI_EVENT_KBD_BRIGHTNESS = 5,
	WMI_EVENT_TOUCHPAD_STATE = 6,
	WMI_EVENT_FNLOCK_STATE = 7,
	WMI_EVENT_KBD_MODE = 8,
	WMI_EVENT_CAPSLOCK_STATE = 9,
	WMI_EVENT_CALCULATOR_START = 11,
	WMI_EVENT_BROWSER_START = 12,
	WMI_EVENT_NUMLOCK_STATE = 13,
	WMI_EVENT_SCROLLLOCK_STATE = 14,
	WMI_EVENT_PERFORMANCE_PLAN = 15,
	WMI_EVENT_FN_J = 16,
	WMI_EVENT_FN_F = 17,
	WMI_EVENT_FN_0 = 18,
	WMI_EVENT_FN_1 = 19,
	WMI_EVENT_FN_2 = 20,
	WMI_EVENT_FN_3 = 21,
	WMI_EVENT_FN_4 = 22,
	WMI_EVENT_FN_5 = 24,
	WMI_EVENT_REFRESH_RATE = 25,
	WMI_EVENT_CPU_FAN_SPEED = 26,
	WMI_EVENT_GPU_FAN_SPEED = 32,
	WMI_EVENT_WIN_KEY_LOCK = 33,
	WMI_EVENT_RESERVED_23 = 34,
	WMI_EVENT_OPEN_APP = 35,
};

enum tongfang_mifs_event_type {
	WMI_EVENT_TYPE_HOTKEY = 1,
};

enum tongfang_wmi_device_type {
	TONGFANG_WMI_CONTROL = 0,
	TONGFANG_WMI_EVENT = 1,
};

struct tongfang_mifs_input {
	u8 reserved1;
	u8 operation;
	u8 reserved2;
	u8 function;
	u8 payload[28];
} __packed;

struct tongfang_mifs_output {
	u8 reserved1;
	u8 operation;
	u8 reserved2;
	u8 function;
	u8 data[28];
} __packed;

struct tongfang_mifs_event {
	u8 event_type;
	u8 event_id;
	u8 value_low; /* For most events, this is the value */
	u8 value_high; /* For fan speed events, combined with value_low */
	u8 reserved[4];
} __packed;

struct tongfang_mifs_wmi_data {
	struct wmi_device *wdev;
	struct mutex lock; /* Protects WMI calls */
	struct led_classdev kbd_led;
	struct input_dev *input_dev;
	enum platform_profile_option saved_profile;
};

static int tongfang_mifs_wmi_call(struct tongfang_mifs_wmi_data *data,
				  const struct tongfang_mifs_input *input,
				  struct tongfang_mifs_output *output)
{
	struct wmi_buffer in_buf, out_buf;
	int ret;

	guard(mutex)(&data->lock);

	in_buf.length = sizeof(*input);
	in_buf.data = (void *)input;

	if (output) {
		out_buf.length = sizeof(*output);
		out_buf.data = output;
	}

	ret = wmidev_invoke_method(data->wdev, 0, 1, &in_buf,
				   output ? &out_buf : NULL);

	return ret;
}

static bool is_ac_online(void)
{
	struct power_supply *psy;
	union power_supply_propval val;
	bool online = false;

	psy = power_supply_get_by_name("ADP1");
	if (!psy)
		return false;

	if (!power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE, &val))
		online = (val.intval == 1);

	power_supply_put(psy);
	return online;
}

static int laptop_profile_get(struct device *dev,
			      enum platform_profile_option *profile)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(dev);
	struct tongfang_mifs_input input = {
		.reserved1 = 0,
		.operation = WMI_METHOD_GET,
		.reserved2 = 0,
		.function = WMI_FN_SYSTEM_PER_MODE,
	};
	struct tongfang_mifs_output result;
	int ret;

	ret = tongfang_mifs_wmi_call(data, &input, &result);

	if (ret)
		return ret;

	switch (result.data[0]) {
	case WMI_PP_BALANCED:
		*profile = PLATFORM_PROFILE_BALANCED;
		break;
	case WMI_PP_PERFORMANCE:
		*profile = PLATFORM_PROFILE_BALANCED_PERFORMANCE;
		break;
	case WMI_PP_QUIET:
		*profile = PLATFORM_PROFILE_LOW_POWER;
		break;
	case WMI_PP_FULL_SPEED:
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break; /* Fullspeed */
	default:
		return -EINVAL;
	}
	return 0;
}


static int laptop_profile_set(struct device *dev,
			      enum platform_profile_option profile)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(dev);
	struct tongfang_mifs_input input = {
		.reserved1 = 0,
		.operation = WMI_METHOD_SET,
		.reserved2 = 0,
		.function = WMI_FN_SYSTEM_PER_MODE,
	};
	struct tongfang_mifs_output ac_type_res;

	u8 val;
	int ret;

	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		val = WMI_PP_QUIET;
		break;
	case PLATFORM_PROFILE_BALANCED:
		val = WMI_PP_BALANCED;
		break;
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
		val = WMI_PP_PERFORMANCE;
		fallthrough;
	case PLATFORM_PROFILE_PERFORMANCE:
		/* Check if Typec power is not connected for full-speed/performance mode */
		input.operation = WMI_METHOD_GET;
		input.function = WMI_FN_SYSTEM_AC_TYPE;
		ret = tongfang_mifs_wmi_call(data, &input, &ac_type_res);
		if (ret)
			return ret;

		/* Full-speed/performance mode requires DC power (not USB-C) */
		if (ac_type_res.data[0] == WMI_SYSTEM_AC_TYPEC || !is_ac_online())
			return -EOPNOTSUPP;

		if (!val)
			val = WMI_PP_FULL_SPEED;

		/* Restore operation and function for the actual SET call */
		input.operation = WMI_METHOD_SET;
		input.function = WMI_FN_SYSTEM_PER_MODE;
		break;
	default:
		return -EOPNOTSUPP;
	}

	input.payload[0] = val;

	ret = tongfang_mifs_wmi_call(data, &input, NULL);
	return ret;
}

static int platform_profile_probe(void *drvdata, unsigned long *choices)
{
	set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_BALANCED_PERFORMANCE, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);

	return 0;
}

static int tongfang_mifs_wmi_suspend(struct device *dev)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(dev);
	enum platform_profile_option profile;
	int ret;

	ret = laptop_profile_get(dev, &profile);
	if (ret == 0)
		data->saved_profile = profile;

	return 0;
}

static int tongfang_mifs_wmi_resume(struct device *dev)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(dev);

	if (data->saved_profile != PLATFORM_PROFILE_LAST) {
		dev_dbg(dev, "Resuming, restoring profile %d\n",
			data->saved_profile);
		return laptop_profile_set(dev, data->saved_profile);
	}

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(tongfang_mifs_wmi_pm_ops,
				tongfang_mifs_wmi_suspend,
				tongfang_mifs_wmi_resume);

static const struct platform_profile_ops laptop_profile_ops = {
	.probe = platform_profile_probe,
	.profile_get = laptop_profile_get,
	.profile_set = laptop_profile_set,
};

static const char *const fan_labels[] = {
	"CPU", /* 0 */
	"GPU", /* 1 */
	"SYS", /* 2 */
};

static int laptop_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long *val)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(dev);
	struct tongfang_mifs_input input = {
		.reserved1 = 0,
		.operation = WMI_METHOD_GET,
		.reserved2 = 0,
	};
	struct tongfang_mifs_output res;
	int ret;

	switch (type) {
	case hwmon_temp:
		input.function = WMI_FN_CPU_THERMOMETER;
		ret = tongfang_mifs_wmi_call(data, &input, &res);
		if (!ret)
			*val = res.data[0] * 1000;
		break;
	case hwmon_fan:
		input.function = WMI_FN_FAN_SPEEDS;
		ret = tongfang_mifs_wmi_call(data, &input, &res);
		if (ret)
			break;

		switch (channel) {
		case 0: /* CPU */
			*val = (res.data[1] << 8) | res.data[0];
			break;
		case 1: /* GPU */
			*val = (res.data[3] << 8) | res.data[2];
			break;
		case 2: /* SYS */
			*val = (res.data[7] << 8) | res.data[6];
			break;
		default:
			ret = -EINVAL;
			break;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int laptop_hwmon_read_string(struct device *dev,
				    enum hwmon_sensor_types type, u32 attr,
				    int channel, const char **str)
{
	if (type == hwmon_fan && attr == hwmon_fan_label) {
		if (channel >= 0 && channel < ARRAY_SIZE(fan_labels)) {
			*str = fan_labels[channel];
			return 0;
		}
	}
	return -EINVAL;
}

static const struct hwmon_channel_info *laptop_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	NULL
};

static const struct hwmon_ops laptop_hwmon_ops = {
	.visible = 0444,
	.read = laptop_hwmon_read,
	.read_string = laptop_hwmon_read_string,
};

static const struct hwmon_chip_info laptop_chip_info = {
	.ops = &laptop_hwmon_ops,
	.info = laptop_hwmon_info,
};

static int laptop_kbd_led_set(struct led_classdev *led_cdev,
			      enum led_brightness value)
{
	struct tongfang_mifs_wmi_data *data =
		container_of(led_cdev, struct tongfang_mifs_wmi_data, kbd_led);
	struct tongfang_mifs_input input = {
		.reserved1 = 0,
		.operation = WMI_METHOD_SET,
		.reserved2 = 0,
		.function = WMI_FN_RGB_KB_BRIGHTNESS,
	};
	int ret;

	input.payload[0] = (u8)value;

	ret = tongfang_mifs_wmi_call(data, &input, NULL);
	return ret;
}

static enum led_brightness laptop_kbd_led_get(struct led_classdev *led_cdev)
{
	struct tongfang_mifs_wmi_data *data =
		container_of(led_cdev, struct tongfang_mifs_wmi_data, kbd_led);
	struct tongfang_mifs_input input = {
		.reserved1 = 0,
		.operation = WMI_METHOD_GET,
		.reserved2 = 0,
		.function = WMI_FN_RGB_KB_BRIGHTNESS,
	};
	struct tongfang_mifs_output res;
	u8 level_val;
	int ret;

	ret = tongfang_mifs_wmi_call(data, &input, &res);
	if (ret)
		return ret;

	level_val = res.data[0];

	return level_val;
}

static const char *const gpu_mode_strings[] = {
	"hybrid",
	"discrete",
	"uma",
};

/* GPU Mode: 0:Hybrid, 1:Discrete, 2:UMA */
static ssize_t gpu_mode_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(dev);
	struct tongfang_mifs_input input = {
		.reserved1 = 0,
		.operation = WMI_METHOD_GET,
		.reserved2 = 0,
		.function = WMI_FN_GPU_MODE,
	};
	struct tongfang_mifs_output res;
	u8 mode_val;
	int ret;
	const char *mode_str;

	ret = tongfang_mifs_wmi_call(data, &input, &res);

	if (ret)
		return ret;

	mode_val = res.data[0];

	if (mode_val < ARRAY_SIZE(gpu_mode_strings)) {
		mode_str = gpu_mode_strings[mode_val];
	} else {
		// fallback: If the value is unknown, report the raw number in wmi
		return sysfs_emit(buf, "%d\n", mode_val);
	}

	return sysfs_emit(buf, "%s\n", mode_str);
}

static ssize_t gpu_mode_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(dev);
	struct tongfang_mifs_input input = {
		.reserved1 = 0,
		.operation = WMI_METHOD_SET,
		.reserved2 = 0,
		.function = WMI_FN_GPU_MODE,
	};
	int val;
	int ret;

	val = sysfs_match_string(gpu_mode_strings, buf);

	if (val < 0)
		return -EINVAL;

	input.payload[0] = (u8)val;

	ret = tongfang_mifs_wmi_call(data, &input, NULL);

	if (ret)
		return ret;

	return count;
}

static const char *const kb_mode_strings[] = {
	"off", /* 0 */
	"cyclic", /* 1 */
	"fixed", /* 2 */
	"custom", /* 3 */
};

static ssize_t kb_mode_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(dev);
	struct tongfang_mifs_input input = {
		.reserved1 = 0,
		.operation = WMI_METHOD_GET,
		.reserved2 = 0,
		.function = WMI_FN_RGB_KB_MODE,
	};
	struct tongfang_mifs_output res;
	int ret;
	u8 mode_val;
	const char *mode_str;

	ret = tongfang_mifs_wmi_call(data, &input, &res);

	if (ret)
		return ret;

	mode_val = res.data[0];

	if (mode_val < ARRAY_SIZE(kb_mode_strings)) {
		mode_str = kb_mode_strings[mode_val];
	} else {
		// Fallback for an unexpected/unknown mode
		return sysfs_emit(buf, "%d\n", mode_val);
	}

	return sysfs_emit(buf, "%s\n", mode_str);
}

static ssize_t kb_mode_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(dev);
	struct tongfang_mifs_input input = {
		.reserved1 = 0,
		.operation = WMI_METHOD_SET,
		.reserved2 = 0,
		.function = WMI_FN_RGB_KB_MODE,
	};
	// the wmi value (0, 1, 2 or 3)
	int val;
	int ret;

	if (!data)
		return -EINVAL;

	val = sysfs_match_string(kb_mode_strings, buf);

	if (val < 0)
		return -EINVAL;

	input.payload[0] = (u8)val;

	ret = tongfang_mifs_wmi_call(data, &input, NULL);

	if (ret)
		return ret;

	return count;
}

/* Fan Boost: 0:Normal, 1:Max Speed */
static ssize_t fan_boost_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(dev);
	struct tongfang_mifs_input input = {
		.reserved1 = 0,
		.operation = WMI_METHOD_SET,
		.reserved2 = 0,
		.function = WMI_FN_MAX_FAN_SWITCH,
	};
	u8 payload[2];
	bool val;
	int ret;

	if (!data)
		return -EINVAL;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	payload[0] = 0; /* CPU/GPU Fan */
	payload[1] = val;

	memcpy(input.payload, payload, sizeof(payload));

	ret = tongfang_mifs_wmi_call(data, &input, NULL);

	if (ret)
		return ret;

	return count;
}

static ssize_t profile_persist_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	enum platform_profile_option profile;
	int ret = laptop_profile_get(dev, &profile);

	if (ret)
		return ret;

	return sysfs_emit(buf, "%d\n", profile);
}

static ssize_t profile_persist_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	kobject_uevent(&dev->kobj, KOBJ_CHANGE);
	return count;
}

static DEVICE_ATTR_RW(gpu_mode);
static DEVICE_ATTR_RW(kb_mode);
static DEVICE_ATTR_WO(fan_boost);
static DEVICE_ATTR_RW(profile_persist);

static struct attribute *laptop_attrs[] = {
	&dev_attr_gpu_mode.attr,
	&dev_attr_kb_mode.attr,
	&dev_attr_fan_boost.attr,
	&dev_attr_profile_persist.attr,
	NULL,
};
ATTRIBUTE_GROUPS(laptop);

static const struct key_entry tongfang_mifs_wmi_keymap[] = {
	{ KE_KEY, WMI_EVENT_OPEN_APP, { KEY_PROG1 } },
	{ KE_KEY, WMI_EVENT_CALCULATOR_START, { KEY_CALC } },
	{ KE_KEY, WMI_EVENT_BROWSER_START, { KEY_WWW } },
	{ KE_IGNORE, WMI_EVENT_FN_J, { KEY_RESERVED } },
	{ KE_IGNORE, WMI_EVENT_FN_F, { KEY_RESERVED } },
	{ KE_IGNORE, WMI_EVENT_FN_0, { KEY_RESERVED } },
	{ KE_IGNORE, WMI_EVENT_FN_1, { KEY_RESERVED } },
	{ KE_IGNORE, WMI_EVENT_FN_2, { KEY_RESERVED } },
	{ KE_IGNORE, WMI_EVENT_FN_3, { KEY_RESERVED } },
	{ KE_IGNORE, WMI_EVENT_FN_4, { KEY_RESERVED } },
	{ KE_IGNORE, WMI_EVENT_FN_5, { KEY_RESERVED } },
	{ KE_END, 0 }
};

static int tongfang_mifs_wmi_probe(struct wmi_device *wdev, const void *context)
{
	struct tongfang_mifs_wmi_data *drv_data;
	struct device *pp_dev;
	struct device *hwmon_dev;
	enum tongfang_wmi_device_type dev_type =
		(enum tongfang_wmi_device_type)(unsigned long)context;

	int ret;

	drv_data = devm_kzalloc(&wdev->dev, sizeof(*drv_data), GFP_KERNEL);
	if (!drv_data)
		return -ENOMEM;

	drv_data->wdev = wdev;

	ret = devm_mutex_init(&wdev->dev, &drv_data->lock);
	if (ret) {
		dev_err(&wdev->dev, "failed to initialize WMI data lock: %d\n",
			ret);
		return ret;
	}

	dev_set_drvdata(&wdev->dev, drv_data);

	if (dev_type == TONGFANG_WMI_EVENT) {
		/* Register input device for hotkeys */
		drv_data->input_dev = devm_input_allocate_device(&wdev->dev);
		if (!drv_data->input_dev)
			return -ENOMEM;

		drv_data->input_dev->name = "Tongfang MIFS WMI hotkeys";
		drv_data->input_dev->phys = "wmi/input0";
		drv_data->input_dev->id.bustype = BUS_HOST;
		drv_data->input_dev->dev.parent = &wdev->dev;

		ret = sparse_keymap_setup(drv_data->input_dev,
					  tongfang_mifs_wmi_keymap, NULL);
		if (ret) {
			dev_err(&wdev->dev, "Failed to setup sparse keymap\n");
			return ret;
		}

		ret = input_register_device(drv_data->input_dev);
		if (ret) {
			dev_err(&wdev->dev,
				"Failed to register input device\n");
			return ret;
		}

		dev_info(&wdev->dev, "Registered WMI event device\n");

		return 0;
	}

	/* Register platform profile */
	pp_dev = devm_platform_profile_register(&wdev->dev, DRV_NAME, drv_data,
						&laptop_profile_ops);
	if (IS_ERR(pp_dev)) {
		dev_err(&wdev->dev, "Failed to register platform profile\n");
		return PTR_ERR(pp_dev);
	}

	drv_data->saved_profile = PLATFORM_PROFILE_LAST;

	/* Register hwmon */
	hwmon_dev = devm_hwmon_device_register_with_info(
		&wdev->dev, "tongfang_mifs", drv_data, &laptop_chip_info, NULL);
	if (IS_ERR(hwmon_dev)) {
		dev_err(&wdev->dev, "Failed to register hwmon\n");
		return PTR_ERR(hwmon_dev);
	}

	/* Register keyboard LED */
	drv_data->kbd_led.name = "laptop::kbd_backlight";

	drv_data->kbd_led.max_brightness = 3;
	drv_data->kbd_led.brightness_set_blocking = laptop_kbd_led_set;
	drv_data->kbd_led.brightness_get = laptop_kbd_led_get;
	ret = devm_led_classdev_register(&wdev->dev, &drv_data->kbd_led);
	if (ret) {
		dev_err(&wdev->dev, "Failed to register keyboard LED\n");
		return ret;
	}

	return 0;
}

static void tongfang_mifs_wmi_notify(struct wmi_device *wdev,
				     union acpi_object *obj)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(&wdev->dev);
	const struct tongfang_mifs_event *event;
	u16 fan_speed;

	if (!obj || obj->type != ACPI_TYPE_BUFFER)
		return;

	if (obj->buffer.length < sizeof(*event))
		return;

	event = (const struct tongfang_mifs_event *)obj->buffer.pointer;

	/* Validate event type */
	if (event->event_type != WMI_EVENT_TYPE_HOTKEY)
		return;

	dev_dbg(&wdev->dev,
		"WMI event: id=0x%02x value_low=0x%02x value_high=0x%02x\n",
		event->event_id, event->value_low, event->value_high);

	switch (event->event_id) {
	case WMI_EVENT_KBD_BRIGHTNESS:
		led_classdev_notify_brightness_hw_changed(&data->kbd_led,
							  event->value_low);
		break;

	case WMI_EVENT_PERFORMANCE_PLAN:
		platform_profile_notify(&wdev->dev);
		break;

	case WMI_EVENT_OPEN_APP:
	case WMI_EVENT_CALCULATOR_START:
	case WMI_EVENT_BROWSER_START:
		if (!sparse_keymap_report_event(data->input_dev,
						event->event_id, 1, true))
			dev_warn(&wdev->dev, "Unknown key pressed: 0x%02x\n",
				 event->event_id);
		break;

	case WMI_EVENT_CPU_FAN_SPEED:
	case WMI_EVENT_GPU_FAN_SPEED:
		/* Fan speed is 16-bit value (value_low is LSB, value_high is MSB) */
		fan_speed = (event->value_high << 8) | event->value_low;
		dev_dbg(&wdev->dev, "Fan speed event: id=%d speed=%u RPM\n",
			event->event_id, fan_speed);
		/* These are informational, hwmon polling will read the actual values */
		break;

	case WMI_EVENT_AIRPLANE_MODE:
	case WMI_EVENT_TOUCHPAD_STATE:
	case WMI_EVENT_FNLOCK_STATE:
	case WMI_EVENT_KBD_MODE:
	case WMI_EVENT_CAPSLOCK_STATE:
	case WMI_EVENT_NUMLOCK_STATE:
	case WMI_EVENT_SCROLLLOCK_STATE:
	case WMI_EVENT_REFRESH_RATE:
	case WMI_EVENT_WIN_KEY_LOCK:
		/* These events are informational or handled by firmware */
		dev_dbg(&wdev->dev, "State change event: id=%d value=%d\n",
			event->event_id, event->value_low);
		break;

	default:
		dev_dbg(&wdev->dev, "Unknown event: id=0x%02x value=0x%02x\n",
			event->event_id, event->value_low);
		break;
	}
}

static const struct wmi_device_id tongfang_mifs_wmi_id_table[] = {
	{ TONGFANG_MIFS_GUID, (void *)TONGFANG_WMI_CONTROL },
	{ TONGFANG_EVENT_GUID, (void *)TONGFANG_WMI_EVENT },
	{}
};
MODULE_DEVICE_TABLE(wmi, tongfang_mifs_wmi_id_table);

static struct wmi_driver tongfang_mifs_wmi_driver = {
	.no_singleton = true,
	.driver = {
		.name = DRV_NAME,
		.dev_groups = laptop_groups,
		.pm = pm_sleep_ptr(&tongfang_mifs_wmi_pm_ops),
	},
	.id_table = tongfang_mifs_wmi_id_table,
	.probe = tongfang_mifs_wmi_probe,
	.notify = tongfang_mifs_wmi_notify,
};

module_wmi_driver(tongfang_mifs_wmi_driver);

MODULE_AUTHOR("Mingyou Chen <qby140326@gmail.com>");
MODULE_DESCRIPTION("Tongfang MIFS (MiInterface) WMI driver");
MODULE_LICENSE("GPL");
