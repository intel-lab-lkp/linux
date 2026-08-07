// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minisforum UM780 XTX (F7BSD) embedded-controller hwmon driver.
 *
 * Copyright (C) 2026 Sebastián Peyrott <speyrott@gmail.com>
 */

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/jiffies.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#define UM780XTX_EC_TEMP_SYS	0x05
#define UM780XTX_EC_TEMP_CPU	0x09
#define UM780XTX_EC_CPU_PROFILE	0x2f
#define UM780XTX_EC_SYS_POINT1	0x31
#define UM780XTX_EC_SYS_POINT2	0x34
#define UM780XTX_EC_SYS_POINT3	0x37

#define UM780XTX_EC_PROFILE_B1	0xb1
#define UM780XTX_EC_PROFILE_B2	0xb2

#define UM780XTX_EC_FAN1_LO	0xb6
#define UM780XTX_EC_FAN1_HI	0xb7
#define UM780XTX_EC_FAN2_LO	0xb9
#define UM780XTX_EC_FAN2_HI	0xba

#define UM780XTX_EC_MAX_RPM	9000
#define UM780XTX_EC_RPM_RETRIES	3
#define UM780XTX_PWM_SYS_LOW	40
#define UM780XTX_PWM_SYS_HIGH	102
#define UM780XTX_RESUME_DELAY_MIN_MS	1000
#define UM780XTX_RESUME_DELAY_MAX_MS	60000

static struct platform_device *um780xtx_pdev;

struct um780xtx_data {
	struct device *dev;
	/* Serializes EC transactions and cached state updates. */
	struct mutex lock;
	struct delayed_work resume_work;
	u8 cached_profile;
	u8 cached_sys_point1;
	u8 cached_sys_point2;
	bool profile_valid;
	bool sys_points_valid;
};

static unsigned int resume_restore_delay_ms = 1000;

static int um780xtx_set_resume_delay(const char *val,
				     const struct kernel_param *kp)
{
	unsigned int delay;
	int ret;

	ret = kstrtouint(val, 0, &delay);
	if (ret)
		return ret;
	if (delay && (delay < UM780XTX_RESUME_DELAY_MIN_MS ||
		      delay > UM780XTX_RESUME_DELAY_MAX_MS))
		return -EINVAL;

	return param_set_uint(val, kp);
}

static const struct kernel_param_ops um780xtx_resume_delay_ops = {
	.set = um780xtx_set_resume_delay,
	.get = param_get_uint,
};

module_param_cb(resume_restore_delay_ms, &um780xtx_resume_delay_ops,
		&resume_restore_delay_ms, 0644);
MODULE_PARM_DESC(resume_restore_delay_ms,
		 "Delay before one post-resume state check; 0 disables (milliseconds)");

static const struct dmi_system_id um780xtx_dmi_table[] = {
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR,
					"Micro Computer (HK) Tech Limited"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Venus series"),
			DMI_EXACT_MATCH(DMI_BOARD_VENDOR,
					"Shenzhen Meigao Electronic Equipment Co.,Ltd"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "F7BSD"),
		},
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, um780xtx_dmi_table);

static bool um780xtx_firmware_match(void)
{
	const char *board_version = dmi_get_system_info(DMI_BOARD_VERSION);
	const char *bios_version = dmi_get_system_info(DMI_BIOS_VERSION);

	return board_version && bios_version &&
		!strcmp(board_version, "1.1") && !strcmp(bios_version, "1.06");
}

static int um780xtx_oem_read(u8 command, u8 *value)
{
	return ec_transaction(command, NULL, 0, value, 1);
}

static int um780xtx_read_rpm(u8 command_hi, u8 command_lo, long *rpm)
{
	u8 hi_before;
	u8 hi_after;
	u8 lo;
	unsigned int value;
	int attempt;
	int ret;

	for (attempt = 0; attempt < UM780XTX_EC_RPM_RETRIES; attempt++) {
		ret = um780xtx_oem_read(command_hi, &hi_before);
		if (ret)
			return ret;
		ret = um780xtx_oem_read(command_lo, &lo);
		if (ret)
			return ret;
		ret = um780xtx_oem_read(command_hi, &hi_after);
		if (ret)
			return ret;
		if (hi_before != hi_after)
			continue;

		value = (hi_after << 8) | lo;
		if (value > UM780XTX_EC_MAX_RPM)
			continue;

		*rpm = value;
		return 0;
	}

	return -EAGAIN;
}

static ssize_t pwm1_enable_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct um780xtx_data *data = dev_get_drvdata(dev);
	u8 profile;
	int ret;

	mutex_lock(&data->lock);
	ret = ec_read(UM780XTX_EC_CPU_PROFILE, &profile);
	mutex_unlock(&data->lock);
	if (ret)
		return ret;
	if (profile == UM780XTX_EC_PROFILE_B1)
		return sysfs_emit(buf, "2\n");
	if (profile == UM780XTX_EC_PROFILE_B2)
		return sysfs_emit(buf, "3\n");

	return -EOPNOTSUPP;
}

static ssize_t pwm1_enable_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct um780xtx_data *data = dev_get_drvdata(dev);
	unsigned long mode;
	u8 expected;
	u8 profile;
	int ret;

	ret = kstrtoul(buf, 10, &mode);
	if (ret)
		return ret;
	if (mode != 2 && mode != 3)
		return -EINVAL;
	expected = mode == 2 ? UM780XTX_EC_PROFILE_B1 : UM780XTX_EC_PROFILE_B2;

	mutex_lock(&data->lock);
	ret = ec_transaction(expected, NULL, 0, NULL, 0);
	if (ret)
		goto out_unlock;
	ret = ec_read(UM780XTX_EC_CPU_PROFILE, &profile);
	if (ret)
		goto out_unlock;
	if (profile != expected) {
		ret = -EIO;
	} else {
		data->cached_profile = profile;
		data->profile_valid = true;
	}

out_unlock:
	mutex_unlock(&data->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(pwm1_enable);

static ssize_t pwm2_enable_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "2\n");
}
static DEVICE_ATTR_RO(pwm2_enable);

static ssize_t pwm2_auto_channels_temp_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	return sysfs_emit(buf, "1\n");
}
static DEVICE_ATTR_RO(pwm2_auto_channels_temp);

static const u8 um780xtx_sys_point_offsets[] = {
	UM780XTX_EC_SYS_POINT1,
	UM780XTX_EC_SYS_POINT2,
};

static ssize_t um780xtx_sys_point_temp_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct um780xtx_data *data = dev_get_drvdata(dev);
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	u8 value;
	int ret;

	mutex_lock(&data->lock);
	ret = ec_read(um780xtx_sys_point_offsets[sattr->index], &value);
	mutex_unlock(&data->lock);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", value * 1000);
}

static ssize_t um780xtx_sys_point_temp_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct um780xtx_data *data = dev_get_drvdata(dev);
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	u8 points[3];
	u8 readback;
	long value;
	int ret;

	ret = kstrtol(buf, 10, &value);
	if (ret)
		return ret;
	if (value < 0 || value > 255000 || value % 1000)
		return -EINVAL;

	mutex_lock(&data->lock);
	ret = ec_read(UM780XTX_EC_SYS_POINT1, &points[0]);
	if (ret)
		goto out_unlock;
	ret = ec_read(UM780XTX_EC_SYS_POINT2, &points[1]);
	if (ret)
		goto out_unlock;
	ret = ec_read(UM780XTX_EC_SYS_POINT3, &points[2]);
	if (ret)
		goto out_unlock;

	points[sattr->index] = value / 1000;
	if (points[0] >= points[1] || points[1] >= points[2]) {
		ret = -EINVAL;
		goto out_unlock;
	}

	ret = ec_write(um780xtx_sys_point_offsets[sattr->index],
		       points[sattr->index]);
	if (ret)
		goto out_unlock;
	ret = ec_read(um780xtx_sys_point_offsets[sattr->index], &readback);
	if (ret)
		goto out_unlock;
	if (readback != points[sattr->index]) {
		ret = -EIO;
	} else {
		data->cached_sys_point1 = points[0];
		data->cached_sys_point2 = points[1];
		data->sys_points_valid = true;
	}

out_unlock:
	mutex_unlock(&data->lock);
	return ret ? ret : count;
}

static SENSOR_DEVICE_ATTR_RW(pwm2_auto_point1_temp,
			     um780xtx_sys_point_temp, 0);
static SENSOR_DEVICE_ATTR_RW(pwm2_auto_point2_temp,
			     um780xtx_sys_point_temp, 1);

static ssize_t pwm2_auto_point1_pwm_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%d\n", UM780XTX_PWM_SYS_LOW);
}
static DEVICE_ATTR_RO(pwm2_auto_point1_pwm);

static ssize_t pwm2_auto_point2_pwm_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%d\n", UM780XTX_PWM_SYS_HIGH);
}
static DEVICE_ATTR_RO(pwm2_auto_point2_pwm);

static struct attribute *um780xtx_extra_attrs[] = {
	&dev_attr_pwm1_enable.attr,
	&dev_attr_pwm2_enable.attr,
	&dev_attr_pwm2_auto_channels_temp.attr,
	&sensor_dev_attr_pwm2_auto_point1_temp.dev_attr.attr,
	&dev_attr_pwm2_auto_point1_pwm.attr,
	&sensor_dev_attr_pwm2_auto_point2_temp.dev_attr.attr,
	&dev_attr_pwm2_auto_point2_pwm.attr,
	NULL
};

static const struct attribute_group um780xtx_extra_group = {
	.attrs = um780xtx_extra_attrs,
};

static const struct attribute_group *um780xtx_extra_groups[] = {
	&um780xtx_extra_group,
	NULL
};

static umode_t um780xtx_is_visible(const void *data,
				   enum hwmon_sensor_types type,
				   u32 attr, int channel)
{
	if (type == hwmon_temp && channel < 2 &&
	    (attr == hwmon_temp_input || attr == hwmon_temp_label))
		return 0444;
	if (type == hwmon_fan && channel < 2 &&
	    (attr == hwmon_fan_input || attr == hwmon_fan_label))
		return 0444;
	return 0;
}

static int um780xtx_read(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long *value)
{
	struct um780xtx_data *data = dev_get_drvdata(dev);
	u8 raw;
	int ret;

	if (type == hwmon_temp && attr == hwmon_temp_input && channel < 2) {
		mutex_lock(&data->lock);
		ret = ec_read(channel ? UM780XTX_EC_TEMP_CPU :
			      UM780XTX_EC_TEMP_SYS, &raw);
		mutex_unlock(&data->lock);
		if (ret)
			return ret;
		*value = raw * 1000L;
		return 0;
	}
	if (type == hwmon_fan && attr == hwmon_fan_input && channel < 2) {
		mutex_lock(&data->lock);
		if (!channel)
			ret = um780xtx_read_rpm(UM780XTX_EC_FAN1_HI,
						UM780XTX_EC_FAN1_LO, value);
		else
			ret = um780xtx_read_rpm(UM780XTX_EC_FAN2_HI,
						UM780XTX_EC_FAN2_LO, value);
		mutex_unlock(&data->lock);
		return ret;
	}
	return -EOPNOTSUPP;
}

static int um780xtx_read_string(struct device *dev,
				enum hwmon_sensor_types type,
				u32 attr, int channel, const char **str)
{
	static const char * const temp_labels[] = {
		"SYS fan control temperature",
		"CPU fan control temperature",
	};
	static const char * const fan_labels[] = { "CPU fan", "SYS fan" };

	if (channel >= 2)
		return -EOPNOTSUPP;
	if (type == hwmon_temp && attr == hwmon_temp_label)
		*str = temp_labels[channel];
	else if (type == hwmon_fan && attr == hwmon_fan_label)
		*str = fan_labels[channel];
	else
		return -EOPNOTSUPP;
	return 0;
}

static const struct hwmon_ops um780xtx_hwmon_ops = {
	.is_visible = um780xtx_is_visible,
	.read = um780xtx_read,
	.read_string = um780xtx_read_string,
};

static const struct hwmon_channel_info * const um780xtx_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	NULL
};

static const struct hwmon_chip_info um780xtx_chip_info = {
	.ops = &um780xtx_hwmon_ops,
	.info = um780xtx_info,
};

static int um780xtx_write_sys_point(u8 offset, u8 value)
{
	u8 readback;
	int ret;

	ret = ec_write(offset, value);
	if (ret)
		return ret;
	ret = ec_read(offset, &readback);
	if (ret)
		return ret;

	return readback == value ? 0 : -EIO;
}

static int um780xtx_restore_sys_points(struct um780xtx_data *data,
				       u8 current_point1,
				       u8 current_point2, u8 point3)
{
	u8 point1 = data->cached_sys_point1;
	u8 point2 = data->cached_sys_point2;
	int ret;

	if (point1 >= point2 || point2 >= point3)
		return -EINVAL;

	/* Keep strict ordering valid after each individual EC write. */
	if (point1 >= current_point2) {
		ret = um780xtx_write_sys_point(UM780XTX_EC_SYS_POINT2, point2);
		if (ret)
			return ret;
		return um780xtx_write_sys_point(UM780XTX_EC_SYS_POINT1,
						 point1);
	}

	ret = um780xtx_write_sys_point(UM780XTX_EC_SYS_POINT1, point1);
	if (ret)
		return ret;
	return um780xtx_write_sys_point(UM780XTX_EC_SYS_POINT2, point2);
}

static void um780xtx_cache_state(struct um780xtx_data *data)
{
	u8 profile;
	u8 point1;
	u8 point2;
	u8 point3;
	int profile_ret;
	int sys_ret;

	mutex_lock(&data->lock);
	profile_ret = ec_read(UM780XTX_EC_CPU_PROFILE, &profile);
	if (!profile_ret &&
	    (profile == UM780XTX_EC_PROFILE_B1 ||
	     profile == UM780XTX_EC_PROFILE_B2)) {
		data->cached_profile = profile;
		data->profile_valid = true;
	}

	sys_ret = ec_read(UM780XTX_EC_SYS_POINT1, &point1);
	if (!sys_ret)
		sys_ret = ec_read(UM780XTX_EC_SYS_POINT2, &point2);
	if (!sys_ret)
		sys_ret = ec_read(UM780XTX_EC_SYS_POINT3, &point3);
	if (!sys_ret && point1 < point2 && point2 < point3) {
		data->cached_sys_point1 = point1;
		data->cached_sys_point2 = point2;
		data->sys_points_valid = true;
	}
	mutex_unlock(&data->lock);

	if (profile_ret)
		dev_warn(data->dev, "failed to cache initial CPU profile: %d\n",
			 profile_ret);
	if (sys_ret)
		dev_warn(data->dev, "failed to cache initial SYS curve: %d\n",
			 sys_ret);
}

static void um780xtx_resume_work(struct work_struct *work)
{
	struct um780xtx_data *data =
		container_of(to_delayed_work(work), struct um780xtx_data,
			     resume_work);
	u8 profile;
	u8 readback;
	u8 point1;
	u8 point2;
	u8 point3;
	int profile_ret = 0;
	int sys_ret = 0;

	mutex_lock(&data->lock);
	if (data->profile_valid) {
		profile_ret = ec_read(UM780XTX_EC_CPU_PROFILE, &profile);
		if (!profile_ret && profile != data->cached_profile) {
			profile_ret = ec_transaction(data->cached_profile, NULL, 0,
						     NULL, 0);
			if (!profile_ret)
				profile_ret = ec_read(UM780XTX_EC_CPU_PROFILE,
						      &readback);
			if (!profile_ret && readback != data->cached_profile)
				profile_ret = -EIO;
		}
	}

	if (data->sys_points_valid) {
		sys_ret = ec_read(UM780XTX_EC_SYS_POINT1, &point1);
		if (!sys_ret)
			sys_ret = ec_read(UM780XTX_EC_SYS_POINT2, &point2);
		if (!sys_ret)
			sys_ret = ec_read(UM780XTX_EC_SYS_POINT3, &point3);
		if (!sys_ret &&
		    (point1 != data->cached_sys_point1 ||
		     point2 != data->cached_sys_point2)) {
			sys_ret = um780xtx_restore_sys_points(data, point1, point2, point3);
		}
	}
	mutex_unlock(&data->lock);

	if (profile_ret)
		dev_warn(data->dev, "post-resume CPU profile check failed: %d\n",
			 profile_ret);
	if (sys_ret)
		dev_warn(data->dev, "post-resume SYS curve check failed: %d\n",
			 sys_ret);
}

static void um780xtx_cancel_resume_work(void *arg)
{
	struct um780xtx_data *data = arg;

	cancel_delayed_work_sync(&data->resume_work);
}

static int um780xtx_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct um780xtx_data *data;
	struct device *hwmon;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->dev = dev;
	mutex_init(&data->lock);
	INIT_DELAYED_WORK(&data->resume_work, um780xtx_resume_work);
	platform_set_drvdata(pdev, data);

	um780xtx_cache_state(data);

	hwmon = devm_hwmon_device_register_with_info(dev, "um780xtx_ec", data,
						     &um780xtx_chip_info,
						     um780xtx_extra_groups);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);

	return devm_add_action_or_reset(dev, um780xtx_cancel_resume_work, data);
}

static int um780xtx_suspend(struct device *dev)
{
	struct um780xtx_data *data = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&data->resume_work);
	return 0;
}

static int um780xtx_resume(struct device *dev)
{
	struct um780xtx_data *data = dev_get_drvdata(dev);
	unsigned int delay = READ_ONCE(resume_restore_delay_ms);

	if (!delay)
		return 0;

	mod_delayed_work(system_wq, &data->resume_work,
			 msecs_to_jiffies(delay));
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(um780xtx_pm_ops, um780xtx_suspend,
				um780xtx_resume);

static struct platform_driver um780xtx_driver = {
	.driver = {
		.name = "um780xtx-ec-hwmon",
		.pm = pm_sleep_ptr(&um780xtx_pm_ops),
	},
	.probe = um780xtx_probe,
};

static int __init um780xtx_init(void)
{
	int ret;

	if (!dmi_check_system(um780xtx_dmi_table) ||
	    !um780xtx_firmware_match() || !ec_get_handle())
		return -ENODEV;
	ret = platform_driver_register(&um780xtx_driver);
	if (ret)
		return ret;

	um780xtx_pdev = platform_device_register_simple("um780xtx-ec-hwmon",
							PLATFORM_DEVID_NONE,
						       NULL, 0);
	if (IS_ERR(um780xtx_pdev)) {
		ret = PTR_ERR(um780xtx_pdev);
		platform_driver_unregister(&um780xtx_driver);
		return ret;
	}
	return 0;
}

static void __exit um780xtx_exit(void)
{
	platform_device_unregister(um780xtx_pdev);
	platform_driver_unregister(&um780xtx_driver);
}

module_init(um780xtx_init);
module_exit(um780xtx_exit);

MODULE_AUTHOR("Sebastián Peyrott <speyrott@gmail.com>");
MODULE_DESCRIPTION("Minisforum UM780 XTX EC hwmon driver");
MODULE_LICENSE("GPL");
