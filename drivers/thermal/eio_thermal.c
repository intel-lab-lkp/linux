// SPDX-License-Identifier: GPL-2.0-only
/*
 * eio_thermal
 * ================
 * Thermal zone driver for Advantech EIO embedded controller's thermal
 * protect mechanism.
 *
 * In EIO chip. The smart fan has 3 trips. While the temperature:
 * - Touch Trip0: Shutdown --> Cut off the power.
 * - Touch Trip1: Poweroff --> Send the power button signal.
 * - between Trip2 and Trip1: Throttle --> Intermittently hold the CPU.
 *
 *			  PowerOff    Shutdown
 *			      ^	         ^
 *	      Throttle	      |		 |
 *		 |	      |	         |
 *	+--------+------------+----------+---------
 *	0       trip2	     trip1      trip0  (Temp)
 *
 * Copyright (C) 2025 Advantech Corporation. All rights reserved.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/mfd/core.h>
#include <linux/mfd/eio.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/thermal.h>

#define CMD_THERM_WRITE		 0x10
#define CMD_THERM_READ		 0x11
#define THERM_NUM		 0x04
#define UNIT_PER_TEMP		 100

#define CTRL_STATE		 0x00
#define CTRL_TYPE		 0x01
#define CTRL_ERROR		 0x04
#define CTRL_VALUE		 0x10
#define CTRL_MAX		 0x11
#define CTRL_MIN		 0x12
#define CTRL_THROTTLE		 0x20
#define CTRL_THROTTLE_HI	 0x21
#define CTRL_THROTTLE_LO	 0x22
#define CTRL_THROTTLE_DEFAULT	 0x28
#define CTRL_THROTTLE_HI_DEFAULT 0x29
#define CTRL_THROTTLE_LO_DEFAULT 0x2A
#define CTRL_POWEROFF		 0x30
#define CTRL_POWEROFF_HI	 0x31
#define CTRL_POWEROFF_LO	 0x32
#define CTRL_POWEROFF_DEFAULT	 0x38
#define CTRL_POWEROFF_HI_DEFAULT 0x39
#define CTRL_POWEROFF_LO_DEFAULT 0x3A
#define CTRL_SHUTDOWN		 0x40
#define CTRL_SHUTDOWN_HI	 0x41
#define CTRL_SHUTDOWN_LO	 0x42
#define CTRL_SHUTDOWN_DEFAULT	 0x48
#define CTRL_SHUTDOWN_HI_DEFAULT 0x49
#define CTRL_SHUTDOWN_LO_DEFAULT 0x4A
#define CTRL_SB_TSI_STATUS	 0x80
#define CTRL_SB_TSI_ACCESS	 0x81
#define CTRL_WARN_STATUS	 0x90
#define CTRL_WARN_BEEP		 0x91
#define CTRL_WARN_TEMP		 0x92

#define THERM_ERR_NO		 0x00
#define THERM_ERR_CHANNEL	 0x01
#define THERM_ERR_HI		 0x02
#define THERM_ERR_LO		 0x03

#define NAME_SIZE		 5

#define TRIP_NUM		 3
#define TRIP_SHUTDOWN		 0
#define TRIP_POWEROFF		 1
#define TRIP_THROTTLE		 2
/* Beep mechanism no stable. Not supported, yet. */
#define TRIP_BEEP		 3

#define THERMAL_POLLING_DELAY		2000 /* millisecond */
#define THERMAL_PASSIVE_DELAY		1000

#define DECI_KELVIN_TO_MILLI_CELSIUS(t) (((t) - 2731) * 100)
#define MILLI_CELSIUS_TO_DECI_KELVIN(t) (((t) / 100) + 2731)

#define THERM_STS_AVAIL           BIT(0)
#define THERM_STS_THROTTLE_AVAIL  BIT(1)
#define THERM_STS_POWEROFF_AVAIL  BIT(2)
#define THERM_STS_SHUTDOWN_AVAIL  BIT(3)
#define THERM_STS_THROTTLE_EVT    BIT(4)
#define THERM_STS_POWEROFF_EVT    BIT(5)
#define THERM_STS_SHUTDOWN_EVT    BIT(6)
/* BIT(7) reserved */
#define THERM_STS_THROTTLE_ON     BIT(8)
#define THERM_STS_POWEROFF_ON     BIT(9)
#define THERM_STS_SHUTDOWN_ON     BIT(10)
/* BIT(11) reserved */
#define THERM_STS_THROTTLE_LOG    BIT(12)
#define THERM_STS_POWEROFF_LOG    BIT(13)
#define THERM_STS_SHUTDOWN_LOG    BIT(14)

static u8 pmc_len[] = {
/*      0, 1, 2, 3, 4, 5, 6, 7, 8, 9, a, b, c, d, e, f */
/* 0 */	2, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 1 */	2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 2 */	1, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 3 */	1, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 4 */	1, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 5 */	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 6 */	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 7 */	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 8 */	1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 9 */	2, 1, 2,
};

static char therm_name[0x20][NAME_SIZE + 1] = {
	"CPU0", "CPU1", "CPU2", "CPU3", "SYS0", "SYS1", "SYS2", "SYS3",
	"AUX0", "AUX1", "AUX2", "AUX3", "DIMM0", "DIMM1", "DIMM2", "DIMM3",
	"PCH", "VGA", "", "", "", "", "", "",
	"", "", "", "", "OEM0", "OEM1", "OEM2", "OEM3",
};

static const u8 ctrl_map[] = {
	CTRL_SHUTDOWN, CTRL_POWEROFF, CTRL_THROTTLE
};

struct eio_thermal_dev {
	struct device *mfd;
	struct device *dev;
	u8 ch;
	u8 name;
};

struct eio_trip_dev {
	struct device *mfd;
	u8 ch;
	u8 idx;
};

static int timeout;
module_param(timeout, int, 0444);
MODULE_PARM_DESC(timeout, "Set PMC command timeout value.\n");

static int pmc_write(struct device *mfd, u8 ctrl, u8 dev_id, void *data)
{
	if (ctrl >= ARRAY_SIZE(pmc_len))
		return -EINVAL;

	struct pmc_op op = {
		.cmd       = CMD_THERM_WRITE,
		.control   = ctrl,
		.device_id = dev_id,
		.payload   = (u8 *)data,
		.size      = pmc_len[ctrl],
		.timeout   = timeout,
	};

	return eio_core_pmc_operation(mfd, &op);
}

static int pmc_read(struct device *mfd, u8 ctrl, u8 dev_id, void *data)
{
	if (ctrl >= ARRAY_SIZE(pmc_len))
		return -EINVAL;

	struct pmc_op op = {
		.cmd       = CMD_THERM_READ,
		.control   = ctrl,
		.device_id = dev_id,
		.payload   = (u8 *)data,
		.size      = pmc_len[ctrl],
		.timeout   = timeout,
	};

	return eio_core_pmc_operation(mfd, &op);
}

static int eio_tz_get_temp(struct thermal_zone_device *tzd, int *temp)
{
	struct eio_thermal_dev *eio_thermal = thermal_zone_device_priv(tzd);
	u16 val = 0;
	int ret;

	ret = pmc_read(eio_thermal->mfd, CTRL_VALUE, eio_thermal->ch, &val);
	if (ret)
		return ret;

	*temp = DECI_KELVIN_TO_MILLI_CELSIUS(val);
	return 0;
}

static int eio_tz_set_trip_temp(struct thermal_zone_device *tzd,
				const struct thermal_trip *trip, int temp)
{
	struct eio_thermal_dev *eio_thermal = thermal_zone_device_priv(tzd);
	const u8 ctl = (uintptr_t)trip->priv;
	u16 val;

	if (temp < 1000)
		return -EINVAL;

	val = MILLI_CELSIUS_TO_DECI_KELVIN(temp);
	return pmc_write(eio_thermal->mfd, ctl, eio_thermal->ch, &val);
}

static int eio_tz_change_mode(struct thermal_zone_device *tzd,
			      enum thermal_device_mode mode)
{
	struct eio_thermal_dev *eio_thermal = thermal_zone_device_priv(tzd);
	int trip;
	int ret = 0;

	for (trip = 0; trip < TRIP_NUM; trip++) {
		ret = pmc_write(eio_thermal->mfd, ctrl_map[trip], eio_thermal->ch, &mode);
		if (ret)
			dev_err(eio_thermal->dev, "Error when %s trip num %d\n",
				mode == THERMAL_DEVICE_ENABLED ? "enabling" : "disabling",
				trip);
	}

	return ret;
}

static struct thermal_zone_device_ops zone_ops = {
	.get_temp = eio_tz_get_temp,
	.set_trip_temp = eio_tz_set_trip_temp,
	.change_mode   = eio_tz_change_mode,
};

static struct thermal_zone_params zone_params = {
	.no_hwmon      = true,
};

static int eio_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ch;

	if (!dev_get_drvdata(dev->parent)) {
		dev_err(dev, "eio_core not present\n");
		return -ENODEV;
	}

	for (ch = 0; ch < THERM_NUM; ch++) {
		u16 state = 0;
		u8  name  = 0;
		u16 hi_shutdown = 0, hi_poweroff = 0, hi_throttle = 0;
		int t_shutdown = 0, t_poweroff = 0, t_throttle = 0;
		struct thermal_trip trips[TRIP_NUM];
		int ntrips = 0;
		struct eio_thermal_dev *eio_th;
		struct thermal_zone_device *tzd;

		if (pmc_read(dev->parent, CTRL_STATE, (u8)ch, &state) ||
		    pmc_read(dev->parent, CTRL_TYPE,  (u8)ch, &name)) {
			dev_info(dev, "thermal%d: PMC read error\n", ch);
			continue;
		}

		if (!(state & THERM_STS_AVAIL) ||
		    !((state & THERM_STS_THROTTLE_AVAIL) ||
		      (state & THERM_STS_POWEROFF_AVAIL) ||
		      (state & THERM_STS_SHUTDOWN_AVAIL))) {
			dev_info(dev, "thermal%d: firmware not activated\n", ch);
			continue;
		}

		if (name >= ARRAY_SIZE(therm_name) || !therm_name[name][0]) {
			dev_info(dev, "thermal%d: unknown sensor name idx=%u\n", ch, name);
			continue;
		}

		/* Throttle starts a 1C increase it */
		int throttle_temp = MILLI_CELSIUS_TO_DECI_KELVIN(60000);

		pmc_write(dev->parent, CTRL_THROTTLE_HI, (u8)ch, &throttle_temp);

		pmc_read(dev->parent, CTRL_SHUTDOWN_HI, (u8)ch, &hi_shutdown);
		pmc_read(dev->parent, CTRL_POWEROFF_HI, (u8)ch, &hi_poweroff);
		pmc_read(dev->parent, CTRL_THROTTLE_HI, (u8)ch, &hi_throttle);

		t_shutdown = DECI_KELVIN_TO_MILLI_CELSIUS(hi_shutdown);
		t_poweroff = DECI_KELVIN_TO_MILLI_CELSIUS(hi_poweroff);
		t_throttle = DECI_KELVIN_TO_MILLI_CELSIUS(hi_throttle);

		ntrips = 0;
		if (hi_shutdown) {
			trips[ntrips].type = THERMAL_TRIP_CRITICAL;
			trips[ntrips].temperature = t_shutdown;
			trips[ntrips].flags = THERMAL_TRIP_FLAG_RW_TEMP;
			trips[ntrips].priv  = THERMAL_INT_TO_TRIP_PRIV(TRIP_SHUTDOWN),
			ntrips++;
		}
		if (hi_poweroff) {
			trips[ntrips].type = THERMAL_TRIP_HOT;
			trips[ntrips].temperature = t_poweroff;
			trips[ntrips].flags = THERMAL_TRIP_FLAG_RW_TEMP;
			trips[ntrips].priv  = THERMAL_INT_TO_TRIP_PRIV(TRIP_POWEROFF),
			ntrips++;
		}
		if (hi_throttle) {
			trips[ntrips].type = THERMAL_TRIP_PASSIVE;
			trips[ntrips].temperature = t_throttle;
			trips[ntrips].flags = THERMAL_TRIP_FLAG_RW_TEMP;
			trips[ntrips].priv  = THERMAL_INT_TO_TRIP_PRIV(TRIP_THROTTLE),
			ntrips++;
		}
		if (!ntrips) {
			dev_info(dev, "thermal%d: no valid trips\n", ch);
			continue;
		}

		eio_th = devm_kzalloc(dev, sizeof(*eio_th), GFP_KERNEL);
		if (!eio_th)
			return -ENOMEM;
		eio_th->ch = (u8)ch;
		eio_th->mfd = dev->parent;
		eio_th->dev = dev;

		tzd = thermal_zone_device_register_with_trips(therm_name[name],
							      trips,
							      ntrips,
							      eio_th,
							      &zone_ops,
							      &zone_params,
							      THERMAL_PASSIVE_DELAY,
							      THERMAL_POLLING_DELAY);
		if (IS_ERR(tzd))
			return PTR_ERR(tzd);
		/* Make sure zones start disabled */
		thermal_zone_device_disable(tzd);

		dev_info(dev, "%s thermal up (ch=%d)\n", therm_name[name], ch);
	}

	return 0;
}

static struct platform_driver eio_thermal_driver = {
	.probe  = eio_thermal_probe,
	.driver = {
		.name = "eio_thermal",
	},
};
module_platform_driver(eio_thermal_driver);

MODULE_AUTHOR("Wenkai Chung <wenkai.chung@advantech.com.tw>");
MODULE_AUTHOR("Ramiro Oliveira <ramiro.oliveira@advantech.com>");
MODULE_DESCRIPTION("Thermal driver for Advantech EIO embedded controller");
MODULE_LICENSE("GPL");
