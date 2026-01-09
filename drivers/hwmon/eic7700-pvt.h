/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ESWIN EIC7700 Process, Voltage, Temperature sensor driver
 *
 * Copyright 2026, Beijing ESWIN Computing Technology Co., Ltd.
 */
#ifndef __HWMON_EIC7700_PVT_H__
#define __HWMON_EIC7700_PVT_H__

#include <linux/completion.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/time.h>

/* ESWIN EIC7700 PVT registers and their bitfields */
#define PVT_TRIM		0x04
#define PVT_TRIM_G		GENMASK(4, 0)
#define PVT_TRIM_O		GENMASK(13, 8)
#define PVT_MODE		0x08
#define PVT_MODE_MASK		GENMASK(2, 0)
#define PVT_CTRL_MODE_TEMP	0x0
#define PVT_CTRL_MODE_VOLT	0x4
#define PVT_CTRL_MODE_LVT	0x1
#define PVT_CTRL_MODE_ULVT	0x2
#define PVT_CTRL_MODE_SVT	0x3
#define PVT_ENA			0x0c
#define PVT_ENA_EN		BIT(0)
#define PVT_INT			0x10
#define PVT_INT_STAT		BIT(0)
#define PVT_INT_CLR		BIT(1)
#define PVT_DATA		0x14
#define PVT_DATA_OUT		GENMASK(9, 0)

/*
 * PVT sensors-related limits and default values
 * @PVT_TEMP_CHS: Number of temperature hwmon channels.
 * @PVT_VOLT_CHS: Number of voltage hwmon channels.
 * @PVT_TRIM_DEF: Default temperature sensor trim value (set a proper value
 *		  when one is determined for ESWIN EIC7700 SoC).
 * @PVT_TOUT_MIN: Minimal timeout between samples in nanoseconds.
 */
#define PVT_TEMP_CHS		1
#define PVT_VOLT_CHS		4
#define PVT_TRIM_DEF		0
#define PVT_TOUT_MIN		(NSEC_PER_SEC / 3000)

/*
 * enum pvt_sensor_type - ESWIN EIC7700 PVT sensor types (correspond to each PVT
 *			  sampling mode)
 * @PVT_SENSOR*: helpers to traverse the sensors in loops.
 * @PVT_TEMP: PVT Temperature sensor.
 * @PVT_VOLT: PVT Voltage sensor.
 * @PVT_LVT: PVT Low-Voltage threshold sensor.
 * @PVT_HVT: PVT High-Voltage threshold sensor.
 * @PVT_SVT: PVT Standard-Voltage threshold sensor.
 */
enum pvt_sensor_type {
	PVT_SENSOR_FIRST,
	PVT_TEMP = PVT_SENSOR_FIRST,
	PVT_VOLT,
	PVT_LVT,
	PVT_ULVT,
	PVT_SVT,
	PVT_SENSOR_LAST = PVT_SVT,
	PVT_SENSORS_NUM
};

/*
 * struct pvt_sensor_info - ESWIN EIC7700 PVT sensor informational structure
 * @channel: Sensor channel ID.
 * @label: hwmon sensor label.
 * @mode: PVT mode corresponding to the channel.
 * @type: Sensor type.
 */
struct pvt_sensor_info {
	int channel;
	const char *label;
	u32 mode;
	enum hwmon_sensor_types type;
};

#define PVT_SENSOR_INFO(_ch, _label, _type, _mode)	\
	{						\
		.channel = _ch,				\
		.label = _label,			\
		.mode = PVT_CTRL_MODE_ ##_mode,		\
		.type = _type,				\
	}

/*
 * struct pvt_cache - PVT sensors data cache
 * @data: data cache in raw format.
 * @conversion: data conversion completion.
 */
struct pvt_cache {
	u32 data;
	struct completion conversion;
};

/*
 * struct pvt_hwmon - Eswin EIC7700 PVT private data
 * @dev: device structure of the PVT platform device.
 * @hwmon: hwmon device structure.
 * @regs: pointer to the Eswin EIC7700 PVT registers region.
 * @irq: PVT events IRQ number.
 * @clk: PVT core clock (1.2MHz).
 * @rst: pointer to the reset descriptor.
 * @iface_mtx: Generic interface mutex (used to lock the data conversion
 *	       interface).
 * @sensor: current PVT sensor the data conversion is being performed for.
 * @cache: data cache descriptor.
 * @sensor_info: pvt sensor info.
 */
struct pvt_hwmon {
	struct device *dev;
	struct device *hwmon;
	void __iomem *regs;
	int irq;
	struct clk *clk;
	struct reset_control *rst;
	struct mutex iface_mtx;		/* protects data conversion interface */
	enum pvt_sensor_type sensor;
	struct pvt_cache cache[PVT_SENSORS_NUM];
	const struct pvt_sensor_info *sensor_info;
};

#endif /* __HWMON_EIC7700_PVT_H__ */
