// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * witrn - Driver for WITRN USB charging testers
 *
 * Copyright (C) 2026 Rong Zhang <i@rong.moe>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/jiffies.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/units.h>
#include <linux/workqueue.h>

#include "hwmon-fp.h"

#define DRIVER_NAME		"witrn"
#define WITRN_EP_CMD_OUT	0x01
#define WITRN_EP_DATA_IN	0x81

#define WITRN_REPORT_SZ		64

/* flags */
#define WITRN_HID_OPENED	0

/*
 * The device sends reports every 10ms (100Hz!) once it's opened, which is
 * really annoying and produces a lot of irq noise.
 *
 * Unfortunately, the device doesn't provide any command to start/stop reporting
 * on demand -- it simply spams reports blindly. The only way to stop reporting
 * is to close the HID device (i.e., to stop IN URB (re)submission).
 *
 * Let's close the HID device if the device has not been accessed for a while.
 */
#define PAUSE_TIMEOUT		secs_to_jiffies(8)
#define UP_TO_DATE_TIMEOUT	msecs_to_jiffies(100)

enum witrn_report_type {
	WITRN_PD		= 0xfe,
	WITRN_SENSOR		= 0xff,
};

struct witrn_sensor {
	__le16	record_threshold;	/* mA */
	__le32	record_charge;		/* Ah (float) */
	__le32	record_energy;		/* Wh (float) */
	__le32	record_time;		/* s */
	__le32	uptime;			/* s */
	__le32	vdp;			/* V (float) */
	__le32	vdm;			/* V (float) */
	u8	__unknown[4];
	__le32	temp_ntc;		/* Celsius (float) */
	__le32	vbus;			/* V (float) */
	__le32	ibus;			/* A (float) */
	u8	record_group;		/* 0: group 1 on device, ... */
	u8	vcc1;			/* dV */
	u8	vcc2;			/* dV */
} __packed;

struct witrn_report {
	u8	report_type;
	u8	__unknown_0[11];

	struct witrn_sensor sensor;

	u8	__unknown_1[7];
} __packed;
static_assert(sizeof(struct witrn_report) == WITRN_REPORT_SZ);

struct witrn_priv {
	struct device *hwmon_dev;
	struct hid_device *hdev;

	struct work_struct pause_work;

	unsigned long flags;

	spinlock_t lock; /* Protects members below */

	struct completion completion;
	unsigned long last_update; /* jiffies */
	unsigned long last_access; /* jiffies */

	struct witrn_sensor sensor;
};

static inline bool sensor_is_outdated(struct witrn_priv *priv)
{
	return time_after(jiffies, priv->last_update + UP_TO_DATE_TIMEOUT);
}

static inline bool hwmon_is_inactive(struct witrn_priv *priv)
{
	return time_after(jiffies, priv->last_access + PAUSE_TIMEOUT);
}

/* ======== HID ======== */

static int witrn_open_hid(struct witrn_priv *priv)
{
	int ret;

	if (test_and_set_bit(WITRN_HID_OPENED, &priv->flags))
		return 0; /* Already opened */

	hid_dbg(priv->hdev, "opening hid hw\n");

	ret = hid_hw_open(priv->hdev);
	if (ret) {
		hid_err(priv->hdev, "hid hw open failed with %d\n", ret);
		clear_bit(WITRN_HID_OPENED, &priv->flags);
	}

	return ret;
}

static void witrn_close_hid(struct witrn_priv *priv)
{
	if (!test_and_clear_bit(WITRN_HID_OPENED, &priv->flags))
		return; /* Already closed */

	hid_dbg(priv->hdev, "closing hid hw\n");

	hid_hw_close(priv->hdev);
}

static void witrn_pause_hid(struct work_struct *work)
{
	struct witrn_priv *priv = container_of(work, struct witrn_priv, pause_work);

	scoped_guard(spinlock, &priv->lock) {
		/* Double check. Condition may change after being scheduled. */
		if (!hwmon_is_inactive(priv))
			return;
	}

	witrn_close_hid(priv);
}

static int witrn_raw_event(struct hid_device *hdev, struct hid_report *report,
			   u8 *data, int size)
{
	struct witrn_priv *priv = hid_get_drvdata(hdev);
	const struct witrn_report *wreport;
	bool do_pause = false;

	/* HIDRAW has opened the device while we are pausing. */
	if (!test_bit(WITRN_HID_OPENED, &priv->flags))
		return 0;

	if (size < WITRN_REPORT_SZ) {
		hid_dbg(hdev, "report size mismatch: %d < %d\n", size, WITRN_REPORT_SZ);
		return 0;
	}

	wreport = (const struct witrn_report *)data;
	if (wreport->report_type != WITRN_SENSOR) {
		hid_dbg(hdev, "report ignored with type 0x%02x", wreport->report_type);
		return 0;
	}

	scoped_guard(spinlock, &priv->lock) {
		priv->last_update = jiffies;
		do_pause = hwmon_is_inactive(priv);

		memcpy(&priv->sensor, &wreport->sensor, sizeof(wreport->sensor));
		complete(&priv->completion);
	}

	if (do_pause)
		schedule_work(&priv->pause_work);

	return 0;
}

/* ======== HWMON ======== */

static int witrn_collect_sensor(struct witrn_priv *priv, struct witrn_sensor *sensor)
{
	int ret;

	scoped_guard(spinlock, &priv->lock) {
		priv->last_access = jiffies;

		if (!sensor_is_outdated(priv)) {
			memcpy(sensor, &priv->sensor, sizeof(priv->sensor));
			return 0;
		}

		reinit_completion(&priv->completion);
	}

	ret = witrn_open_hid(priv);
	if (ret)
		return ret;

	ret = wait_for_completion_interruptible_timeout(&priv->completion,
							UP_TO_DATE_TIMEOUT);
	if (ret == 0)
		return -ETIMEDOUT;
	else if (ret < 0)
		return ret;

	scoped_guard(spinlock, &priv->lock)
		memcpy(sensor, &priv->sensor, sizeof(priv->sensor));

	return 0;
}

#define SECS_PER_HOUR		3600ULL
#define WITRN_SCALE_IN_VCC	(HWMON_FP_SCALE_IN / DECI)		/* dV to mV */
#define WITRN_SCALE_CHARGE	(HWMON_FP_SCALE_CURR * SECS_PER_HOUR)	/* Ah to mC(mAs) */
#define WITRN_SCALE_ENERGY	(HWMON_FP_SCALE_ENERGY * SECS_PER_HOUR)	/* Wh to uJ(uWs) */

static int witrn_read_in(const struct witrn_sensor *sensor, u32 attr, int channel, long *val)
{
	switch (attr) {
	case hwmon_in_input:
		switch (channel) {
		case 0:
			return hwmon_fp_float_to_long(le32_to_cpu(sensor->vbus),
						      HWMON_FP_SCALE_IN, val);
		case 1:
			return hwmon_fp_float_to_long(le32_to_cpu(sensor->vdp),
						      HWMON_FP_SCALE_IN, val);
		case 2:
			return hwmon_fp_float_to_long(le32_to_cpu(sensor->vdm),
						      HWMON_FP_SCALE_IN, val);
		case 3:
			*val = sensor->vcc1 * WITRN_SCALE_IN_VCC;
			return 0;
		case 4:
			*val = sensor->vcc2 * WITRN_SCALE_IN_VCC;
			return 0;
		default:
			return -EOPNOTSUPP;
		}
	case hwmon_in_average:
		switch (channel) {
		case 0:
			return hwmon_fp_div_to_long(le32_to_cpu(sensor->record_energy),
						    le32_to_cpu(sensor->record_charge),
						    HWMON_FP_SCALE_IN, true, val);
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static int witrn_read_curr(const struct witrn_sensor *sensor, u32 attr, int channel, long *val)
{
	int ret;

	switch (attr) {
	case hwmon_curr_input:
		switch (channel) {
		case 0:
			return hwmon_fp_float_to_long(le32_to_cpu(sensor->ibus),
						      HWMON_FP_SCALE_CURR, val);
		default:
			return -EOPNOTSUPP;
		}
	case hwmon_curr_average:
		switch (channel) {
		case 0: {
			s64 record_time = le32_to_cpu(sensor->record_time);
			s64 capacity; /* mC(mAs) */

			if (record_time == 0) {
				*val = 0;
				return 0;
			}

			ret = hwmon_fp_float_to_s64(le32_to_cpu(sensor->record_charge),
						    WITRN_SCALE_CHARGE, &capacity);
			if (ret)
				return ret;

			/* mC(mAs) / s = mA */
			*val = hwmon_fp_s64_to_long(capacity / record_time);
			return 0;
		}
		default:
			return -EOPNOTSUPP;
		}
	case hwmon_curr_rated_min:
		switch (channel) {
		case 0:
			*val = le16_to_cpu(sensor->record_threshold); /* already in mA */
			return 0;
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static int witrn_read_power(const struct witrn_sensor *sensor, u32 attr, int channel, long *val)
{
	int ret;

	switch (attr) {
	case hwmon_power_input:
		switch (channel) {
		case 0:
			/*
			 * The device provides 1e-5 precision.
			 *
			 * Though userspace programs can calculate (VBUS * IBUS)
			 * themselves, this channel is provided for convenience
			 * and accuracy.
			 *
			 * E.g., when VBUS = 5.00049V and IBUS = 0.50049A,
			 * userspace calculates 5.000V * 0.500A = 2.500000W,
			 * while this channel reports 2.502695W.
			 */
			return hwmon_fp_mul_to_long(le32_to_cpu(sensor->vbus),
						    le32_to_cpu(sensor->ibus),
						    HWMON_FP_SCALE_POWER, val);
		default:
			return -EOPNOTSUPP;
		}
	case hwmon_power_average:
		switch (channel) {
		case 0: {
			s64 record_time = le32_to_cpu(sensor->record_time);
			s64 energy; /* uJ(uWs) */

			if (record_time == 0) {
				*val = 0;
				return 0;
			}

			ret = hwmon_fp_float_to_s64(le32_to_cpu(sensor->record_energy),
						    WITRN_SCALE_ENERGY, &energy);
			if (ret)
				return ret;

			/* uJ(uWs) / s = uW */
			*val = hwmon_fp_s64_to_long(energy / record_time);
			return 0;
		}
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static int witrn_read_temp(const struct witrn_sensor *sensor, u32 attr, int channel, long *val)
{
	int ret;

	switch (attr) {
	case hwmon_temp_input:
		switch (channel) {
		case 0:
			ret = hwmon_fp_float_to_long(le32_to_cpu(sensor->temp_ntc),
						     HWMON_FP_SCALE_TEMP, val);

			/*
			 * The thermistor (NTC, B=3435, T0=25°C, R0=10kohm) is an optional
			 * addon. When it's missing, an extremely cold temperature
			 * (-50°C - -80°C) is reported as the device deduced a very large
			 * resistance value (~500Kohm - ~5Mohm).
			 *
			 * We choose -40°C (~250kohm) as the threshold to determine whether
			 * the thermistor is connected.
			 *
			 * The addon can be connected to the device after the device being
			 * connected to the PC, so we can't use is_visible to hide it.
			 */
			if (!ret && *val < -40L * (long)HWMON_FP_SCALE_TEMP)
				return -EXDEV;

			return ret;
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static int witrn_read_energy(const struct witrn_sensor *sensor, u32 attr, int channel, s64 *val)
{
	switch (attr) {
	case hwmon_energy_input:
		switch (channel) {
		case 0:
			return hwmon_fp_float_to_s64(le32_to_cpu(sensor->record_energy),
						     WITRN_SCALE_ENERGY, val);
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static int witrn_read(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long *val)
{
	struct witrn_priv *priv = dev_get_drvdata(dev);
	struct witrn_sensor sensor;
	int ret;

	ret = witrn_collect_sensor(priv, &sensor);
	if (ret)
		return ret;

	switch (type) {
	case hwmon_in:
		return witrn_read_in(&sensor, attr, channel, val);
	case hwmon_curr:
		return witrn_read_curr(&sensor, attr, channel, val);
	case hwmon_power:
		return witrn_read_power(&sensor, attr, channel, val);
	case hwmon_temp:
		return witrn_read_temp(&sensor, attr, channel, val);
	case hwmon_energy64:
		return witrn_read_energy(&sensor, attr, channel, (s64 *)val);
	default:
		return -EOPNOTSUPP;
	}
}

static int witrn_read_string(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, const char **str)
{
	static const char * const in_labels[] = {
		"VBUS",
		"D+",
		"D-",
		"CC1",
		"CC2",
	};
	static const char * const curr_labels[] = {
		"IBUS", /* VBUS current */
	};
	static const char * const power_labels[] = {
		"PBUS", /* VBUS power */
	};
	static const char * const energy_labels[] = {
		"EBUS", /* VBUS energy */
	};
	static const char * const temp_labels[] = {
		"Thermistor",
	};

	if (type == hwmon_in && attr == hwmon_in_label &&
	    channel < ARRAY_SIZE(in_labels)) {
		*str = in_labels[channel];
	} else if (type == hwmon_curr && attr == hwmon_curr_label &&
		   channel < ARRAY_SIZE(curr_labels)) {
		*str = curr_labels[channel];
	} else if (type == hwmon_power && attr == hwmon_power_label &&
		   channel < ARRAY_SIZE(power_labels)) {
		*str = power_labels[channel];
	} else if (type == hwmon_energy64 && attr == hwmon_energy_label &&
		   channel < ARRAY_SIZE(energy_labels)) {
		*str = energy_labels[channel];
	} else if (type == hwmon_temp && attr == hwmon_temp_label &&
		   channel < ARRAY_SIZE(temp_labels)) {
		*str = temp_labels[channel];
	} else {
		return -EOPNOTSUPP;
	}

	return 0;
}

static const struct hwmon_channel_info *const witrn_info[] = {
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LABEL | HWMON_I_AVERAGE,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_LABEL | HWMON_C_AVERAGE | HWMON_C_RATED_MIN),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_LABEL | HWMON_P_AVERAGE),
	HWMON_CHANNEL_INFO(energy64,
			   HWMON_E_INPUT | HWMON_E_LABEL),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static const struct hwmon_ops witrn_hwmon_ops = {
	.visible = 0444, /* Nothing is tunable from PC :-( */
	.read = witrn_read,
	.read_string = witrn_read_string,
};

static const struct hwmon_chip_info witrn_chip_info = {
	.ops = &witrn_hwmon_ops,
	.info = witrn_info,
};

enum witrn_attr_channel {
	ATTR_CHARGE,
	ATTR_RECORD_GROUP,
	ATTR_RECORD_TIME,
	ATTR_UPTIME,
};

static ssize_t witrn_attr_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	enum witrn_attr_channel channel = to_sensor_dev_attr(attr)->index;
	struct witrn_priv *priv = dev_get_drvdata(dev);
	struct witrn_sensor sensor;
	int ret;
	s64 val;

	ret = witrn_collect_sensor(priv, &sensor);
	if (ret)
		return ret;

	switch (channel) {
	case ATTR_CHARGE:
		ret = hwmon_fp_float_to_s64(le32_to_cpu(sensor.record_charge),
					    WITRN_SCALE_CHARGE, &val);
		if (ret)
			return ret;
		break;
	case ATTR_RECORD_GROUP:
		/* +1 to match the index displayed on the meter. */
		val = sensor.record_group + 1;
		break;
	case ATTR_RECORD_TIME:
		val = le32_to_cpu(sensor.record_time);
		break;
	case ATTR_UPTIME:
		val = le32_to_cpu(sensor.uptime);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return sysfs_emit(buf, "%lld\n", val);
}

static ssize_t witrn_attr_label_show(struct device *dev, struct device_attribute *attr,
				     char *buf)
{
	enum witrn_attr_channel channel = to_sensor_dev_attr(attr)->index;
	const char *str;

	switch (channel) {
	case ATTR_CHARGE:
		str = "CBUS"; /* VBUS charge */
		break;
	default:
		return -EOPNOTSUPP;
	}

	return sysfs_emit(buf, "%s\n", str);
}

static SENSOR_DEVICE_ATTR_RO(charge1_input, witrn_attr, ATTR_CHARGE);
static SENSOR_DEVICE_ATTR_RO(charge1_label, witrn_attr_label, ATTR_CHARGE);
static SENSOR_DEVICE_ATTR_RO(record_group, witrn_attr, ATTR_RECORD_GROUP);
static SENSOR_DEVICE_ATTR_RO(record_time, witrn_attr, ATTR_RECORD_TIME);
static SENSOR_DEVICE_ATTR_RO(uptime, witrn_attr, ATTR_UPTIME);

static struct attribute *witrn_attrs[] = {
	&sensor_dev_attr_charge1_input.dev_attr.attr,
	&sensor_dev_attr_charge1_label.dev_attr.attr,
	&sensor_dev_attr_record_group.dev_attr.attr,
	&sensor_dev_attr_record_time.dev_attr.attr,
	&sensor_dev_attr_uptime.dev_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(witrn);

static int witrn_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct device *parent = &hdev->dev;
	struct witrn_priv *priv;
	int ret;

	priv = devm_kzalloc(parent, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hdev = hdev;
	hid_set_drvdata(hdev, priv);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid parse failed with %d\n", ret);
		return ret;
	}

	/* Enable HIDRAW so existing user-space tools can continue to work. */
	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "hid hw start failed with %d\n", ret);
		return ret;
	}

	spin_lock_init(&priv->lock);
	init_completion(&priv->completion);

	INIT_WORK(&priv->pause_work, witrn_pause_hid);

	priv->last_access = jiffies;
	priv->last_update = priv->last_access - UP_TO_DATE_TIMEOUT - 1;
	clear_bit(WITRN_HID_OPENED, &priv->flags);

	ret = witrn_open_hid(priv);
	if (ret) {
		hid_hw_stop(hdev);
		return ret;
	}

	priv->hwmon_dev = hwmon_device_register_with_info(parent, DRIVER_NAME, priv,
							  &witrn_chip_info, witrn_groups);
	if (IS_ERR(priv->hwmon_dev)) {
		witrn_close_hid(priv);
		hid_hw_stop(hdev);
		return PTR_ERR(priv->hwmon_dev);
	}

	return 0;
}

static void witrn_remove(struct hid_device *hdev)
{
	struct witrn_priv *priv = hid_get_drvdata(hdev);

	hwmon_device_unregister(priv->hwmon_dev);

	witrn_close_hid(priv);

	/* Cancel it after closing HID so that it won't be rescheduled. */
	cancel_work_sync(&priv->pause_work);

	hid_hw_stop(hdev);
}

static const struct hid_device_id witrn_id_table[] = {
	{ HID_USB_DEVICE(0x0716, 0x5060) },	/* WITRN K2 USB-C tester */
	{ }
};

MODULE_DEVICE_TABLE(hid, witrn_id_table);

static struct hid_driver witrn_driver = {
	.name = DRIVER_NAME,
	.id_table = witrn_id_table,
	.probe = witrn_probe,
	.remove = witrn_remove,
	.raw_event = witrn_raw_event,
};

static int __init witrn_init(void)
{
	return hid_register_driver(&witrn_driver);
}

static void __exit witrn_exit(void)
{
	hid_unregister_driver(&witrn_driver);
}

/* When compiled into the kernel, initialize after the HID bus */
late_initcall(witrn_init);
module_exit(witrn_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rong Zhang <i@rong.moe>");
MODULE_DESCRIPTION("WITRN USB tester driver");
