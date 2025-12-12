// SPDX-License-Identifier: GPL-2.0-only
/*
 * eio_fan
 * ============
 * Thermal zone driver for Advantech EIO embedded controller's smart
 * fan mechanism.
 *
 * We create a sysfs 'name' of the zone, point out where the fan is. Such as
 * CPU0, SYS3, etc.
 *
 * The sysfs 'fan_mode' can be one of 'Stop', 'Full', 'Manual' or 'Auto'.
 * If 'Manual'. You can control fan speed via sysfs 'PWM'.
 * If it is 'Auto'. It enables the smart fan mechanism as below.
 *
 * In EIO chip. The smart fan has 3 trips. When the temperature is:
 * - Over Temp High(trip0), the Fan runs at the fan PWM High.
 * - Between Temp Low and Temp High(trip1 - trip0), the fan PWM value slopes
 *   from PWM Low to PWM High.
 * - Between Temp Stop and Temp Low(trip2 - trip1), the fan PWM is PWM low.
 * - Below Temp Stop, the fan stopped.
 *
 * (PWM)|
 *	|
 * High |............................. ______________
 * (Max)|			      /:
 *	|			     / :
 *	|			    /  :
 *	|			   /   :
 *	|			  /    :
 *	|			 /     :
 *	|			/      :
 *	|		       /       :
 *  Low	|.......... __________/	       :
 *	|	    |	      :	       :
 *	|	    |	      :	       :
 *    0	+===========+---------+--------+-------------
 *	0	   Stop	     Low      High	(Temp)
 *
 * Copyright (C) 2025 Advantech Corporation. All rights reserved.
 */

#include <linux/ctype.h>
#include <linux/errno.h>
#include <linux/mfd/core.h>
#include <linux/mfd/eio.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/thermal.h>

#define CMD_FAN_WRITE		0x24
#define CMD_FAN_READ		0x25
#define FAN_MAX			0x04

#define CMD_THERM_WRITE		0x10
#define CMD_THERM_READ		0x11
#define THERM_MAX		0x04
#define THERM_MULTI		100

#define CTRL_STATE		0x00
#define CTRL_TYPE		0x01
#define CTRL_CTRL		0x02
#define CTRL_ERROR		0x04
#define CTRL_VALUE		0x10
#define CTRL_INVERT		0x11
#define CTRL_FREQ		0x12
#define CTRL_THERM_HIGH		0x13
#define CTRL_THERM_LOW		0x14
#define CTRL_THERM_STOP		0x15
#define CTRL_PWM_HIGH		0x16
#define CTRL_PWM_LOW		0x17
#define CTRL_THERM_SRC		0x20

#define CTRLMODE_STOP		0x00
#define CTRLMODE_FULL		0x01
#define CTRLMODE_MANUAL		0x02
#define CTRLMODE_AUTO		0x03

#define DUTY_MAX		100
#define UNIT_PER_TEMP		10
#define NAME_SIZE		4

#define TRIP_HIGH		0
#define TRIP_LOW		1
#define TRIP_STOP		2
#define TRIP_NUM		3

/* Bitfields inside CTRL_CTRL */
#define FAN_MODE_MASK           GENMASK(1, 0)
#define FAN_SCM_BIT             BIT(2)
#define FAN_FRAME_BIT           BIT(3)
#define FAN_SRC_MASK            GENMASK(7, 4)

#define FAN_SRC(val)            (((int)(val)) >> 4)

#ifndef DECI_KELVIN_TO_MILLI_CELSIUS
#define DECI_KELVIN_TO_MILLI_CELSIUS(t)  ((((t) - 2731) * 100))
#endif

#ifndef MILLI_CELSIUS_TO_DECI_KELVIN
#define MILLI_CELSIUS_TO_DECI_KELVIN(t)  ((((t) / 100) + 2731))
#endif

static const u8 pmc_len[CTRL_THERM_SRC + 1] = {
/*      0, 1, 2, 3, 4, 5, 6, 7, 8, 9, a, b, c, d, e, f */
	1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	1, 1, 4, 2, 2, 2, 1, 1, 2, 2, 2, 0, 0, 0, 0, 0,
	1,
};

static const char fan_name[0x20][NAME_SIZE + 1] = {
	"CPU0", "CPU1", "CPU2", "CPU3", "SYS0", "SYS1", "SYS2", "SYS3",
	"", "", "", "", "", "", "", "",
	"", "", "", "", "", "", "", "",
	"", "", "", "", "OEM0", "OEM1", "OEM2", "OEM3",
};

struct eio_fan_trip {
	u8 trip_ctl;
};

struct eio_fan_dev {
	struct device *mfd;
	struct device *dev;
	u8 id;
	struct thermal_zone_device *tzd;
	struct eio_fan_trip trip_priv[TRIP_NUM];
};

static int timeout;
module_param(timeout, int, 0444);
MODULE_PARM_DESC(timeout, "Set PMC command timeout value.\n");

static int pmc_write(struct device *mfd, u8 ctrl, u8 id, void *data)
{
	if (ctrl >= ARRAY_SIZE(pmc_len))
		return -EINVAL;

	struct pmc_op op = {
		.cmd       = CMD_FAN_WRITE,
		.control   = ctrl,
		.device_id = id,
		.size	   = pmc_len[ctrl],
		.payload   = (u8 *)data,
		.timeout   = timeout,
	};
	return eio_core_pmc_operation(mfd, &op);
}

static int pmc_read(struct device *mfd, u8 ctrl, u8 id, void *data)
{
	struct pmc_op op = {
		.cmd       = CMD_FAN_READ,
		.control   = ctrl,
		.device_id = id,
		.size	   = pmc_len[ctrl],
		.payload   = (u8 *)data,
		.timeout   = timeout,
	};
	return eio_core_pmc_operation(mfd, &op);
}

static int pmc_read_therm(struct device *mfd, u8 ctrl, u8 id, void *data)
{
	struct pmc_op op = {
		.cmd       = CMD_THERM_READ,
		.control   = ctrl,
		.device_id = id,
		.size	   = 2,
		.payload   = (u8 *)data,
		.timeout   = timeout,
	};
	return eio_core_pmc_operation(mfd, &op);
}

static int eio_fan_get_temp(struct thermal_zone_device *tzd, int *temp)
{
	struct eio_fan_dev *fan = thermal_zone_device_priv(tzd);
	struct device *mfd = fan->mfd;
	u8 ch = fan->id;
	int sensor = 0;
	u16 val = 0;
	int ret;

	ret = pmc_read(mfd, CTRL_CTRL, ch, &sensor);
	if (ret)
		return ret;

	ret = pmc_read_therm(mfd, CTRL_VALUE, (u8)FAN_SRC(sensor), &val);
	if (ret)
		return ret;

	*temp = DECI_KELVIN_TO_MILLI_CELSIUS(val);
	return 0;
}

static int eio_fan_set_trip_temp(struct thermal_zone_device *tzd,
				 const struct thermal_trip *trip, int temp)
{
	struct eio_fan_dev *fan = thermal_zone_device_priv(tzd);
	const struct eio_fan_trip *fan_trip = trip->priv;
	u8 ctl = CTRL_THERM_HIGH + fan_trip->trip_ctl;
	u16 val;

	if (temp < 1000)
		return -EINVAL;

	val = MILLI_CELSIUS_TO_DECI_KELVIN(temp);
	return pmc_write(fan->mfd, ctl, fan->id, &val);
}

static bool eio_fan_should_bind(struct thermal_zone_device *tzd,
				const struct thermal_trip *trip,
				struct thermal_cooling_device *cdev,
				struct cooling_spec *spec)
{
	struct eio_fan_dev *tz_fan  = thermal_zone_device_priv(tzd);
	struct eio_fan_dev *cd_fan  = cdev->devdata;

	if (!tz_fan || !cd_fan)
		return false;

	if (tz_fan->mfd != cd_fan->mfd || tz_fan->id != cd_fan->id)
		return false;

	return true;
}

static const struct thermal_zone_device_ops zone_ops = {
	.get_temp = eio_fan_get_temp,
	.set_trip_temp = eio_fan_set_trip_temp,
	.should_bind   = eio_fan_should_bind,
};

static int eio_fan_get_max_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	*state = 100;
	return 0;
}

static int eio_fan_get_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct eio_fan_dev *fan = cdev->devdata;
	int fan_mode = 0;
	u8 duty = 0;
	int ret = 0;

	*state = 0;
	ret = pmc_read(fan->mfd, CTRL_CTRL, fan->id, &fan_mode);
	if (ret)
		return ret;

	switch (fan_mode & FAN_MODE_MASK) {
	case CTRLMODE_STOP:
		*state = 0;
		break;
	case CTRLMODE_FULL:
		*state = 100;
		break;
	case CTRLMODE_AUTO:
		*state = 0;
		ret = 0;
		break;
	case CTRLMODE_MANUAL:
		ret = pmc_read(fan->mfd, CTRL_VALUE, fan->id, &duty);
		if (ret)
			return ret;
		duty = (u8)clamp_val(duty, 0, 100);
		*state = duty;
		break;
	default:
		*state = 0;
		return -EINVAL;
	}
	return 0;
}

static int eio_fan_set_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long state)
{
	struct eio_fan_dev *fan = cdev->devdata;
	u8 ctrl = 0;
	u8 duty;
	int ret;

	ret = pmc_read(fan->mfd, CTRL_CTRL, fan->id, &ctrl);
	if (ret)
		return ret;

	if ((ctrl & FAN_MODE_MASK) != CTRLMODE_MANUAL)
		return -EOPNOTSUPP;

	duty = (u8)clamp_val(state, 0, 100);

	ret = pmc_write(fan->mfd, CTRL_VALUE, fan->id, &duty);

	return ret;
}

static const struct thermal_cooling_device_ops cooling_ops = {
	.get_max_state = eio_fan_get_max_state,
	.get_cur_state = eio_fan_get_cur_state,
	.set_cur_state = eio_fan_set_cur_state,
};

static ssize_t fan_mode_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	static const char * const names[] = { "Stop", "Full", "Manual", "Auto" };
	struct thermal_zone_device *tzd = dev_get_drvdata(dev);
	struct eio_fan_dev *fan = thermal_zone_device_priv(tzd);
	u8 mode = 0;

	int ret = pmc_read(fan->mfd, CTRL_CTRL, fan->id, &mode);

	if (ret)
		return ret;

	return sysfs_emit(buf, "%s\n", names[mode & 0x03]);
}

static ssize_t fan_mode_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	static const char * const names[] = { "Stop", "Full", "Manual", "Auto" };
	struct thermal_zone_device *tzd = dev_get_drvdata(dev);
	struct eio_fan_dev *fan = thermal_zone_device_priv(tzd);
	u8 ctrl, newc;
	int mode_idx, ret;

	for (mode_idx = 0; mode_idx < ARRAY_SIZE(names); mode_idx++) {
		if (strncasecmp(buf, names[mode_idx], strlen(names[mode_idx])))
			continue;

		ret = pmc_read(fan->mfd, CTRL_CTRL, fan->id, &ctrl);
		if (ret)
			return -EIO;

		newc  = ctrl & FAN_SRC_MASK;

		switch (mode_idx) {
		case CTRLMODE_AUTO:
			newc |= FAN_FRAME_BIT;
			newc &= ~FAN_SCM_BIT;
			newc |= CTRLMODE_AUTO;
			break;
		case CTRLMODE_MANUAL:
			newc &= ~FAN_FRAME_BIT;
			newc &= ~FAN_SCM_BIT;
			newc |= CTRLMODE_MANUAL;
			break;
		case CTRLMODE_FULL:
			newc &= ~FAN_FRAME_BIT;
			newc &= ~FAN_SCM_BIT;
			newc |= CTRLMODE_FULL;
			break;
		case CTRLMODE_STOP:
		default:
			newc &= ~FAN_FRAME_BIT;
			newc &= ~FAN_SCM_BIT;
			newc |= CTRLMODE_STOP;
			break;
		}

		ret = pmc_write(fan->mfd, CTRL_CTRL, fan->id, &newc);
		return ret ? ret : count;
	}

	return -EINVAL;
}

static DEVICE_ATTR_RW(fan_mode);

static int eio_fan_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	unsigned int fan_id;
	int ret;

	if (!dev_get_drvdata(dev->parent)) {
		dev_err(dev, "eio_core not present\n");
		return -ENODEV;
	}

	for (fan_id = 0; fan_id < FAN_MAX; fan_id++) {
		u8 state = 0, name = 0;
		int trip_hi = 0, trip_lo = 0, trip_stop = 0;
		int pwm_hi = 0, pwm_lo = 0;
		int temps_mc[TRIP_NUM];
		struct eio_fan_dev *fan;
		struct thermal_zone_device *tzd;
		struct thermal_cooling_device *cdev;

		if (pmc_read(dev->parent, CTRL_STATE, fan_id, &state) ||
		    pmc_read(dev->parent, CTRL_TYPE, fan_id, &name) ||
		    pmc_read(dev->parent, CTRL_THERM_HIGH, fan_id, &trip_hi) ||
		    pmc_read(dev->parent, CTRL_THERM_LOW, fan_id, &trip_lo) ||
		    pmc_read(dev->parent, CTRL_THERM_STOP, fan_id, &trip_stop) ||
		    pmc_read(dev->parent, CTRL_PWM_HIGH, fan_id, &pwm_hi) ||
		    pmc_read(dev->parent, CTRL_PWM_LOW, fan_id, &pwm_lo)) {
			dev_info(dev, "fan%u: pmc read error, skipping\n", fan_id);
			continue;
		}

		if (!(state & 0x1)) {
			dev_info(dev, "fan%u: firmware reports disabled\n", fan_id);
			continue;
		}

		if (!fan_name[name][0]) {
			dev_info(dev, "fan%u: unknown name index %u\n", fan_id, name);
			continue;
		}

		temps_mc[TRIP_HIGH] = DECI_KELVIN_TO_MILLI_CELSIUS(trip_hi);
		temps_mc[TRIP_LOW]  = DECI_KELVIN_TO_MILLI_CELSIUS(trip_lo);
		temps_mc[TRIP_STOP] = DECI_KELVIN_TO_MILLI_CELSIUS(trip_stop);

		fan = devm_kzalloc(dev, sizeof(*fan), GFP_KERNEL);
		if (!fan)
			return -ENOMEM;

		fan->mfd = dev->parent;
		fan->id  = (u8)fan_id;

		fan->trip_priv[TRIP_HIGH].trip_ctl = CTRL_THERM_HIGH;
		fan->trip_priv[TRIP_LOW].trip_ctl  = CTRL_THERM_LOW;
		fan->trip_priv[TRIP_STOP].trip_ctl = CTRL_THERM_STOP;

		struct thermal_trip trips[TRIP_NUM] = {
			[TRIP_HIGH] = {
				.type = THERMAL_TRIP_ACTIVE,
				.temperature = DECI_KELVIN_TO_MILLI_CELSIUS(trip_hi),
				.flags = THERMAL_TRIP_FLAG_RW_TEMP,
				.priv = &fan->trip_priv[TRIP_HIGH],
			},
			[TRIP_LOW] = {
				.type = THERMAL_TRIP_ACTIVE,
				.temperature = DECI_KELVIN_TO_MILLI_CELSIUS(trip_lo),
				.flags = THERMAL_TRIP_FLAG_RW_TEMP,
				.priv = &fan->trip_priv[TRIP_LOW],
			},
			[TRIP_STOP] = {
				.type = THERMAL_TRIP_ACTIVE,
				.temperature = DECI_KELVIN_TO_MILLI_CELSIUS(trip_stop),
				.flags = THERMAL_TRIP_FLAG_RW_TEMP,
				.priv = &fan->trip_priv[TRIP_STOP],
			},
		};

		tzd = thermal_zone_device_register_with_trips(fan_name[name],
							      trips, TRIP_NUM,
							      fan,
							      &zone_ops,
							      NULL,
							      0, 0);
		if (IS_ERR(tzd))
			return PTR_ERR(tzd);

		cdev = thermal_cooling_device_register(fan_name[name], fan, &cooling_ops);
		if (IS_ERR(cdev)) {
			thermal_zone_device_unregister(tzd);
			dev_err(dev, "fan%u: cdev register failed: %ld\n",
				fan_id, PTR_ERR(cdev));
			return PTR_ERR(cdev);
		}

		dev_set_drvdata(thermal_zone_device(tzd), tzd);
		ret = device_create_file(thermal_zone_device(tzd), &dev_attr_fan_mode);
		if (ret)
			dev_warn(dev, "Error create thermal zone fan_mode sysfs\n");
	}
	return 0;
}

static struct platform_driver eio_fan_driver = {
	.probe  = eio_fan_probe,
	.driver = {
		.name = "eio_fan",
	},
};

module_platform_driver(eio_fan_driver);

MODULE_AUTHOR("Wenkai Chung <wenkai.chung@advantech.com.tw>");
MODULE_AUTHOR("Ramiro Oliveira <ramiro.oliveira@advantech.com>");
MODULE_DESCRIPTION("Fan driver for Advantech EIO embedded controller");
MODULE_LICENSE("GPL");
