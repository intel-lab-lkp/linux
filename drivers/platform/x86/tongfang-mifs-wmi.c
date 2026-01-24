// SPDX-License-Identifier: GPL-2.0-or-later
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
#define WMI_BUFFER_SIZE 32

enum wmi_method_type {
	WMI_METHOD_GET = 250,
	WMI_METHOD_SET = 251,
};

enum wmi_method_name {
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

struct tongfang_mifs_wmi_data {
	struct wmi_device *wdev;
	struct mutex lock;
	struct led_classdev kbd_led;
	struct device *hwmon_dev;
};

static int tongfang_mifs_wmi_call(struct tongfang_mifs_wmi_data *data, u8 type,
				  u8 method, u8 *payload, size_t payload_len,
				  u8 *out_data)
{
	struct acpi_buffer input = { 0, NULL };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	u8 *buffer;
	acpi_status status;
	int ret = 0;

	if (!data)
		return -EINVAL;

	buffer = kzalloc(WMI_BUFFER_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	buffer[1] = type;
	buffer[3] = method;

	if (payload && payload_len > 0) {
		size_t copy_len =
			min_t(size_t, payload_len, WMI_BUFFER_SIZE - 4);
		memcpy(&buffer[4], payload, copy_len);
	}

	input.length = WMI_BUFFER_SIZE;
	input.pointer = buffer;

	status = wmi_evaluate_method(TONGFANG_MIFS_GUID, 0, 1, &input, &output);
	if (ACPI_FAILURE(status)) {
		ret = -EIO;
		goto out_free_in;
	}

	obj = output.pointer;
	if (!obj) {
		ret = -ENODATA;
		goto out_free_in;
	}

	if (obj->type == ACPI_TYPE_BUFFER &&
	    obj->buffer.length >= WMI_BUFFER_SIZE) {
		if (out_data)
			memcpy(out_data, obj->buffer.pointer, WMI_BUFFER_SIZE);
	} else {
		ret = -EINVAL;
	}

	kfree(output.pointer);
out_free_in:
	kfree(buffer);
	return ret;
}

static int laptop_profile_get(struct device *dev,
			      enum platform_profile_option *profile)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(dev);
	u8 result[WMI_BUFFER_SIZE];
	int ret;

	if (!data)
		return -EINVAL;

	mutex_lock(&data->lock);
	ret = tongfang_mifs_wmi_call(data, WMI_METHOD_GET,
				     WMI_FN_SYSTEM_PER_MODE, NULL, 0, result);
	mutex_unlock(&data->lock);

	if (ret)
		return ret;

	switch (result[4]) {
	case 0:
		*profile = PLATFORM_PROFILE_BALANCED;
		break;
	case 1:
		*profile = PLATFORM_PROFILE_BALANCED_PERFORMANCE;
		break;
	case 2:
		*profile = PLATFORM_PROFILE_LOW_POWER;
		break;
	case 3:
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
	u8 val;
	int ret;

	if (!data)
		return -EINVAL;

	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		val = 2;
		break;
	case PLATFORM_PROFILE_BALANCED:
		val = 0;
		break;
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
		val = 1;
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		val = 3; /* Fullspeed */
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&data->lock);
	ret = tongfang_mifs_wmi_call(data, WMI_METHOD_SET,
				     WMI_FN_SYSTEM_PER_MODE, &val, 1, NULL);
	mutex_unlock(&data->lock);
	return ret;
}

static int platform_profile_probe(void *drvdata, unsigned long *choices)
{
	__set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	__set_bit(PLATFORM_PROFILE_BALANCED, choices);
	__set_bit(PLATFORM_PROFILE_BALANCED_PERFORMANCE, choices);
	__set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);
	return 0;
}

static struct platform_profile_ops laptop_profile_ops = {
	.probe = platform_profile_probe,
	.profile_get = laptop_profile_get,
	.profile_set = laptop_profile_set,
};

static int laptop_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long *val)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(dev);
	u8 res[WMI_BUFFER_SIZE];
	int ret;

	if (!data)
		return -EINVAL;

	mutex_lock(&data->lock);
	if (type == hwmon_temp) {
		ret = tongfang_mifs_wmi_call(data, WMI_METHOD_GET,
					     WMI_FN_CPU_THERMOMETER, NULL, 0,
					     res);
		if (!ret)
			*val = res[4] * 1000;
	} else if (type == hwmon_fan) {
		ret = tongfang_mifs_wmi_call(data, WMI_METHOD_GET,
					     WMI_FN_FAN_SPEEDS, NULL, 0, res);
		if (!ret) {
			if (channel == 0) /* CPU */
				*val = (res[5] << 8) | res[4];
			else if (channel == 1) /* GPU */
				*val = (res[7] << 8) | res[6];
			else if (channel == 2) /* SYS */
				*val = (res[11] << 8) | res[10];
			else
				ret = -EINVAL;
		}
	} else {
		ret = -EINVAL;
	}
	mutex_unlock(&data->lock);
	return ret;
}

static umode_t laptop_hwmon_is_visible(const void *drvdata,
				       enum hwmon_sensor_types type, u32 attr,
				       int channel)
{
	if (type == hwmon_temp && attr == hwmon_temp_input && channel == 0)
		return 0444;
	if (type == hwmon_fan && attr == hwmon_fan_input && channel < 3)
		return 0444;
	return 0;
}

static const struct hwmon_channel_info *laptop_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT),
	NULL
};

static const struct hwmon_ops laptop_hwmon_ops = {
	.is_visible = laptop_hwmon_is_visible,
	.read = laptop_hwmon_read,
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
	u8 val = (u8)value;
	int ret;

	mutex_lock(&data->lock);
	ret = tongfang_mifs_wmi_call(data, WMI_METHOD_SET,
				     WMI_FN_RGB_KB_BRIGHTNESS, &val, 1, NULL);
	mutex_unlock(&data->lock);
	return ret;
}

/* GPU Mode: 0:Hybrid, 1:Discrete, 2:UMA */
static ssize_t gpu_mode_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(dev);
	u8 result[WMI_BUFFER_SIZE];
	int ret;

	if (!data)
		return -EINVAL;

	mutex_lock(&data->lock);
	ret = tongfang_mifs_wmi_call(data, WMI_METHOD_GET, WMI_FN_GPU_MODE,
				     NULL, 0, result);
	mutex_unlock(&data->lock);

	if (ret)
		return ret;
	return sysfs_emit(buf, "%d\n", result[4]);
}

static ssize_t gpu_mode_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(dev);
	u8 val;
	int ret;

	if (!data)
		return -EINVAL;

	if (kstrtou8(buf, 10, &val) || val > 2)
		return -EINVAL;

	mutex_lock(&data->lock);
	ret = tongfang_mifs_wmi_call(data, WMI_METHOD_SET, WMI_FN_GPU_MODE,
				     &val, 1, NULL);
	mutex_unlock(&data->lock);

	if (ret)
		return ret;

	return count;
}

/* RGB Mode: 0:OFF, 1:Cyclic, 2:Fixed, 3:Custom */
static ssize_t kb_mode_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(dev);
	u8 result[WMI_BUFFER_SIZE];
	int ret;

	if (!data)
		return -EINVAL;

	mutex_lock(&data->lock);
	ret = tongfang_mifs_wmi_call(data, WMI_METHOD_GET, WMI_FN_RGB_KB_MODE,
				     NULL, 0, result);
	mutex_unlock(&data->lock);

	if (ret)
		return ret;
	return sysfs_emit(buf, "%d\n", result[4]);
}

static ssize_t kb_mode_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(dev);
	u8 val;
	int ret;

	if (!data)
		return -EINVAL;

	if (kstrtou8(buf, 10, &val) || val > 3)
		return -EINVAL;

	mutex_lock(&data->lock);
	ret = tongfang_mifs_wmi_call(data, WMI_METHOD_SET, WMI_FN_RGB_KB_MODE,
				     &val, 1, NULL);
	mutex_unlock(&data->lock);

	if (ret)
		return ret;

	return count;
}

/* RGB Color: R G B */
static ssize_t kb_color_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct tongfang_mifs_wmi_data *data = dev_get_drvdata(dev);
	unsigned int r, g, b;
	u8 color_buf[3];
	u8 fixed_mode = 2;
	int ret;

	if (!data)
		return -EINVAL;

	if (sscanf(buf, "%u %u %u", &r, &g, &b) != 3)
		return -EINVAL;
	if (r > 255 || g > 255 || b > 255)
		return -EINVAL;

	color_buf[0] = (u8)r;
	color_buf[1] = (u8)g;
	color_buf[2] = (u8)b;

	mutex_lock(&data->lock);

	ret = tongfang_mifs_wmi_call(data, WMI_METHOD_SET, WMI_FN_RGB_KB_MODE,
				     &fixed_mode, 1, NULL);
	if (ret)
		goto unlock;

	ret = tongfang_mifs_wmi_call(data, WMI_METHOD_SET, WMI_FN_RGB_KB_COLOR,
				     color_buf, 3, NULL);

unlock:
	mutex_unlock(&data->lock);

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
	u8 val, payload[2];
	int ret;

	if (!data)
		return -EINVAL;

	if (kstrtou8(buf, 10, &val) || val > 1)
		return -EINVAL;

	payload[0] = 0; /* CPU/GPU Fan */
	payload[1] = val;

	mutex_lock(&data->lock);
	ret = tongfang_mifs_wmi_call(data, WMI_METHOD_SET,
				     WMI_FN_MAX_FAN_SWITCH, payload, 2, NULL);
	mutex_unlock(&data->lock);

	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(gpu_mode);
static DEVICE_ATTR_RW(kb_mode);
static DEVICE_ATTR_WO(kb_color);
static DEVICE_ATTR_WO(fan_boost);

static struct attribute *laptop_attrs[] = { &dev_attr_gpu_mode.attr,
					    &dev_attr_kb_mode.attr,
					    &dev_attr_kb_color.attr,
					    &dev_attr_fan_boost.attr, NULL };
ATTRIBUTE_GROUPS(laptop);

static int tongfang_mifs_wmi_probe(struct wmi_device *wdev, const void *context)
{
	struct tongfang_mifs_wmi_data *drv_data;
	struct device *pp_dev;
	int ret;

	drv_data = devm_kzalloc(&wdev->dev, sizeof(*drv_data), GFP_KERNEL);
	if (!drv_data)
		return -ENOMEM;

	drv_data->wdev = wdev;
	mutex_init(&drv_data->lock);
	dev_set_drvdata(&wdev->dev, drv_data);

	/* Register platform profile */
	pp_dev = devm_platform_profile_register(&wdev->dev, DRV_NAME, drv_data,
						&laptop_profile_ops);
	if (IS_ERR(pp_dev))
		dev_err(&wdev->dev, "Failed to register platform profile\n");

	/* Register hwmon */
	drv_data->hwmon_dev = devm_hwmon_device_register_with_info(
		&wdev->dev, "tongfang_mifs", drv_data, &laptop_chip_info, NULL);
	if (IS_ERR(drv_data->hwmon_dev))
		dev_err(&wdev->dev, "Failed to register hwmon\n");

	/* Register keyboard LED */
	drv_data->kbd_led.name = "laptop::kbd_backlight";
	drv_data->kbd_led.max_brightness = 3;
	drv_data->kbd_led.brightness_set_blocking = laptop_kbd_led_set;
	ret = devm_led_classdev_register(&wdev->dev, &drv_data->kbd_led);
	if (ret)
		dev_err(&wdev->dev, "Failed to register keyboard LED\n");

	return 0;
}

static const struct wmi_device_id tongfang_mifs_wmi_id_table[] = {
	{ TONGFANG_MIFS_GUID, NULL },
	{}
};
MODULE_DEVICE_TABLE(wmi, tongfang_mifs_wmi_id_table);

static struct wmi_driver tongfang_mifs_wmi_driver = {
	.driver = {
		.name = DRV_NAME,
		.dev_groups = laptop_groups,
	},
	.id_table = tongfang_mifs_wmi_id_table,
	.probe = tongfang_mifs_wmi_probe,
};

module_wmi_driver(tongfang_mifs_wmi_driver);

MODULE_AUTHOR("Mingyou Chen <qby104326@gmail.com>");
MODULE_DESCRIPTION("Tongfang MIFS (MiInterface) WMI driver");
MODULE_LICENSE("GPL");
