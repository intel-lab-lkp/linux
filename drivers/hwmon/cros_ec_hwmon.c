// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  ChromeOS EC driver for hwmon
 *
 *  Copyright (C) 2024 Thomas Weißschuh <linux@weissschuh.net>
 */

#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/math.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/platform_data/cros_ec_commands.h>
#include <linux/platform_data/cros_ec_proto.h>
#include <linux/types.h>
#include <linux/units.h>

#define DRV_NAME	"cros-ec-hwmon"

enum cros_ec_hwmon_fan_mode {
	cros_ec_hwmon_fan_mode_full = 0,
	cros_ec_hwmon_fan_mode_manual = 1,
	cros_ec_hwmon_fan_mode_auto = 2,
};

struct cros_ec_hwmon_priv {
	struct cros_ec_device *cros_ec;
	const char *temp_sensor_names[EC_TEMP_SENSOR_ENTRIES + EC_TEMP_SENSOR_B_ENTRIES];
	u8 usable_fans;
	bool has_fan_pwm;
	u8 fan_pwm[EC_FAN_SPEED_ENTRIES];
	enum cros_ec_hwmon_fan_mode fan_mode[EC_FAN_SPEED_ENTRIES];
};

static int cros_ec_hwmon_read_fan_speed(struct cros_ec_device *cros_ec, u8 index, u16 *speed)
{
	int ret;
	__le16 __speed;

	ret = cros_ec_cmd_readmem(cros_ec, EC_MEMMAP_FAN + index * 2, 2, &__speed);
	if (ret < 0)
		return ret;

	*speed = le16_to_cpu(__speed);
	return 0;
}

static int cros_ec_hwmon_read_fan_target(struct cros_ec_device *cros_ec, u16 *speed)
{
	struct ec_response_pwm_get_fan_rpm resp;
	int ret;

	ret = cros_ec_cmd(cros_ec, 0, EC_CMD_PWM_GET_FAN_TARGET_RPM,
			  NULL, 0, &resp, sizeof(resp));
	if (ret < 0)
		return ret;

	*speed = resp.rpm;
	return 0;
}

static int cros_ec_hwmon_set_fan_auto(struct cros_ec_device *cros_ec, u8 index)
{
	struct ec_params_auto_fan_ctrl_v1 req = {
		.fan_idx = index,
	};

	return cros_ec_cmd(cros_ec, 1, EC_CMD_THERMAL_AUTO_FAN_CTRL,
			   &req, sizeof(req), NULL, 0);
}

static int cros_ec_hwmon_set_fan_duty_cycle(struct cros_ec_device *cros_ec, u8 index, u8 percent)
{
	struct ec_params_pwm_set_fan_duty_v1 req = {
		.fan_idx = index,
		.percent = percent,
	};

	return cros_ec_cmd(cros_ec, 1, EC_CMD_PWM_SET_FAN_DUTY,
			   &req, sizeof(req), NULL, 0);
}

static int cros_ec_hwmon_read_temp(struct cros_ec_device *cros_ec, u8 index, u8 *temp)
{
	unsigned int offset;
	int ret;

	if (index < EC_TEMP_SENSOR_ENTRIES)
		offset = EC_MEMMAP_TEMP_SENSOR + index;
	else
		offset = EC_MEMMAP_TEMP_SENSOR_B + index - EC_TEMP_SENSOR_ENTRIES;

	ret = cros_ec_cmd_readmem(cros_ec, offset, 1, temp);
	if (ret < 0)
		return ret;
	return 0;
}

static bool cros_ec_hwmon_is_error_fan(u16 speed)
{
	return speed == EC_FAN_SPEED_NOT_PRESENT || speed == EC_FAN_SPEED_STALLED;
}

static bool cros_ec_hwmon_is_error_temp(u8 temp)
{
	return temp == EC_TEMP_SENSOR_NOT_PRESENT     ||
	       temp == EC_TEMP_SENSOR_ERROR           ||
	       temp == EC_TEMP_SENSOR_NOT_POWERED     ||
	       temp == EC_TEMP_SENSOR_NOT_CALIBRATED;
}

static long cros_ec_hwmon_temp_to_millicelsius(u8 temp)
{
	return kelvin_to_millicelsius((((long)temp) + EC_TEMP_SENSOR_OFFSET));
}

static int cros_ec_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, long *val)
{
	struct cros_ec_hwmon_priv *priv = dev_get_drvdata(dev);
	int ret = -EOPNOTSUPP;
	u16 speed;
	u8 temp;

	if (type == hwmon_fan) {
		if (attr == hwmon_fan_input) {
			ret = cros_ec_hwmon_read_fan_speed(priv->cros_ec, channel, &speed);
			if (ret == 0) {
				if (cros_ec_hwmon_is_error_fan(speed))
					ret = -ENODATA;
				else
					*val = speed;
			}
		} else if (attr == hwmon_fan_fault) {
			ret = cros_ec_hwmon_read_fan_speed(priv->cros_ec, channel, &speed);
			if (ret == 0)
				*val = cros_ec_hwmon_is_error_fan(speed);

		} else if (attr == hwmon_fan_target) {
			ret = cros_ec_hwmon_read_fan_target(priv->cros_ec, &speed);
			if (ret == 0)
				*val = speed;
		}

	} else if (type == hwmon_pwm) {
		if (attr == hwmon_pwm_input) {
			*val = priv->fan_pwm[channel];
			ret = 0;

		} else if (attr == hwmon_pwm_enable) {
			*val = priv->fan_mode[channel];
			ret = 0;
		}

	} else if (type == hwmon_temp) {
		if (attr == hwmon_temp_input) {
			ret = cros_ec_hwmon_read_temp(priv->cros_ec, channel, &temp);
			if (ret == 0) {
				if (cros_ec_hwmon_is_error_temp(temp))
					ret = -ENODATA;
				else
					*val = cros_ec_hwmon_temp_to_millicelsius(temp);
			}
		} else if (attr == hwmon_temp_fault) {
			ret = cros_ec_hwmon_read_temp(priv->cros_ec, channel, &temp);
			if (ret == 0)
				*val = cros_ec_hwmon_is_error_temp(temp);
		}
	}

	return ret;
}

static int cros_ec_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type,
				     u32 attr, int channel, const char **str)
{
	struct cros_ec_hwmon_priv *priv = dev_get_drvdata(dev);

	if (type == hwmon_temp && attr == hwmon_temp_label) {
		*str = priv->temp_sensor_names[channel];
		return 0;
	}

	return -EOPNOTSUPP;
}

static int cros_ec_hwmon_write_pwm_input(struct cros_ec_hwmon_priv *priv, int channel, long val)
{
	u8 percent;
	int ret;

	if (val < 0 || val > 255)
		return -EINVAL;

	percent = DIV_ROUND_CLOSEST(val * 100, 255);

	if (priv->fan_mode[channel] == cros_ec_hwmon_fan_mode_manual) {
		ret = cros_ec_hwmon_set_fan_duty_cycle(priv->cros_ec, channel, percent);
		if (ret < 0)
			return ret;
	}

	priv->fan_pwm[channel] = percent;
	return 0;
}

static int cros_ec_hwmon_write_pwm_enable(struct cros_ec_hwmon_priv *priv, int channel, long val)
{
	int ret;

	if (val == cros_ec_hwmon_fan_mode_full)
		ret = cros_ec_hwmon_set_fan_duty_cycle(priv->cros_ec, channel, 100);
	else if (val == cros_ec_hwmon_fan_mode_manual)
		ret = cros_ec_hwmon_set_fan_duty_cycle(priv->cros_ec, channel,
						       priv->fan_pwm[channel]);
	else if (val == cros_ec_hwmon_fan_mode_auto)
		ret = cros_ec_hwmon_set_fan_auto(priv->cros_ec, channel);
	else
		return -EINVAL;

	priv->fan_mode[channel] = val;
	return ret;
}

static int cros_ec_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long val)
{
	struct cros_ec_hwmon_priv *priv = dev_get_drvdata(dev);
	int ret = -EOPNOTSUPP;

	if (type == hwmon_pwm) {
		if (attr == hwmon_pwm_input)
			ret = cros_ec_hwmon_write_pwm_input(priv, channel, val);

		else if (attr == hwmon_pwm_enable)
			ret = cros_ec_hwmon_write_pwm_enable(priv, channel, val);
	}

	return ret;
}

static umode_t cros_ec_hwmon_is_visible(const void *data, enum hwmon_sensor_types type,
					u32 attr, int channel)
{
	const struct cros_ec_hwmon_priv *priv = data;
	u16 speed;

	if (type == hwmon_fan) {
		if (attr == hwmon_fan_target &&
		    cros_ec_hwmon_read_fan_target(priv->cros_ec, &speed) == -EOPNOTSUPP)
			return 0;

		if (priv->usable_fans & BIT(channel))
			return 0444;

	} else if (type == hwmon_pwm) {
		if (priv->has_fan_pwm && priv->usable_fans & BIT(channel))
			return 0644;

	} else if (type == hwmon_temp) {
		if (priv->temp_sensor_names[channel])
			return 0444;
	}

	return 0;
}

static const struct hwmon_channel_info * const cros_ec_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_FAULT | HWMON_F_TARGET,
			   HWMON_F_INPUT | HWMON_F_FAULT,
			   HWMON_F_INPUT | HWMON_F_FAULT,
			   HWMON_F_INPUT | HWMON_F_FAULT),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),

#define CROS_EC_HWMON_TEMP_PARAMS (HWMON_T_INPUT | HWMON_T_FAULT | HWMON_T_LABEL)
	HWMON_CHANNEL_INFO(temp,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS,
			   CROS_EC_HWMON_TEMP_PARAMS),
	NULL
};

static const struct hwmon_ops cros_ec_hwmon_ops = {
	.read = cros_ec_hwmon_read,
	.read_string = cros_ec_hwmon_read_string,
	.write = cros_ec_hwmon_write,
	.is_visible = cros_ec_hwmon_is_visible,
};

static const struct hwmon_chip_info cros_ec_hwmon_chip_info = {
	.ops = &cros_ec_hwmon_ops,
	.info = cros_ec_hwmon_info,
};

static void cros_ec_hwmon_probe_temp_sensors(struct device *dev, struct cros_ec_hwmon_priv *priv,
					     u8 thermal_version)
{
	struct ec_params_temp_sensor_get_info req = {};
	struct ec_response_temp_sensor_get_info resp;
	size_t candidates, i, sensor_name_size;
	int ret;
	u8 temp;

	if (thermal_version < 2)
		candidates = EC_TEMP_SENSOR_ENTRIES;
	else
		candidates = ARRAY_SIZE(priv->temp_sensor_names);

	for (i = 0; i < candidates; i++) {
		if (cros_ec_hwmon_read_temp(priv->cros_ec, i, &temp) < 0)
			continue;

		if (temp == EC_TEMP_SENSOR_NOT_PRESENT)
			continue;

		req.id = i;
		ret = cros_ec_cmd(priv->cros_ec, 0, EC_CMD_TEMP_SENSOR_GET_INFO,
				  &req, sizeof(req), &resp, sizeof(resp));
		if (ret < 0)
			continue;

		sensor_name_size = strnlen(resp.sensor_name, sizeof(resp.sensor_name));
		priv->temp_sensor_names[i] = devm_kasprintf(dev, GFP_KERNEL, "%.*s",
							    (int)sensor_name_size,
							    resp.sensor_name);
	}
}

static void cros_ec_hwmon_probe_fans(struct cros_ec_hwmon_priv *priv)
{
	u16 speed;
	size_t i;
	int ret;

	ret = cros_ec_cmd_versions(priv->cros_ec, EC_CMD_PWM_SET_FAN_DUTY);
	if (ret >= 0 && ret & EC_VER_MASK(1))
		priv->has_fan_pwm = 1;

	for (i = 0; i < EC_FAN_SPEED_ENTRIES; i++) {
		ret = cros_ec_hwmon_read_fan_speed(priv->cros_ec, i, &speed);
		if (ret < 0)
			continue;
		if (speed == EC_FAN_SPEED_NOT_PRESENT)
			continue;

		priv->usable_fans |= BIT(i);

		if (priv->has_fan_pwm) {
			priv->fan_mode[i] = cros_ec_hwmon_fan_mode_auto;
			ret = cros_ec_hwmon_set_fan_auto(priv->cros_ec, i);
			if (ret != 0)
				priv->has_fan_pwm = 0;
		}

	}
}

static int cros_ec_hwmon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cros_ec_dev *ec_dev = dev_get_drvdata(dev->parent);
	struct cros_ec_device *cros_ec = ec_dev->ec_dev;
	struct cros_ec_hwmon_priv *priv;
	struct device *hwmon_dev;
	u8 thermal_version;
	int ret;

	ret = cros_ec_cmd_readmem(cros_ec, EC_MEMMAP_THERMAL_VERSION, 1, &thermal_version);
	if (ret < 0)
		return ret;

	/* Covers both fan and temp sensors */
	if (thermal_version == 0)
		return -ENODEV;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->cros_ec = cros_ec;

	cros_ec_hwmon_probe_temp_sensors(dev, priv, thermal_version);
	cros_ec_hwmon_probe_fans(priv);

	hwmon_dev = devm_hwmon_device_register_with_info(dev, "cros_ec", priv,
							 &cros_ec_hwmon_chip_info, NULL);

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct platform_device_id cros_ec_hwmon_id[] = {
	{ DRV_NAME, 0 },
	{}
};

static struct platform_driver cros_ec_hwmon_driver = {
	.driver.name	= DRV_NAME,
	.probe		= cros_ec_hwmon_probe,
	.id_table	= cros_ec_hwmon_id,
};
module_platform_driver(cros_ec_hwmon_driver);

MODULE_DEVICE_TABLE(platform, cros_ec_hwmon_id);
MODULE_DESCRIPTION("ChromeOS EC Hardware Monitoring Driver");
MODULE_AUTHOR("Thomas Weißschuh <linux@weissschuh.net");
MODULE_LICENSE("GPL");
