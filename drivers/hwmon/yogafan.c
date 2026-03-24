// SPDX-License-Identifier: GPL-2.0-only
/**
 * yoga_fan.c - Lenovo Yoga/Legion Fan Hardware Monitoring Driver
 *
 * Provides fan speed monitoring for Lenovo Yoga, Legion, and IdeaPad
 * laptops by interfacing with the Embedded Controller (EC) via ACPI.
 *
 * The driver implements a passive discrete-time first-order lag filter
 * with slew-rate limiting (RLLag). This addresses low-resolution
 * tachometer sampling in the EC by smoothing RPM readings based on
 * the time delta (dt) between userspace requests, ensuring physical
 * consistency without background task overhead or race conditions.
 * The filter implements multirate filtering with autoreset in case
 * of large sampling time.
 *
 * Copyright (C) 2021-2026 Sergio Melas <sergiomelas@gmail.com>
 */
#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/math64.h>
/* Driver Configuration Constants */
#define DRVNAME "yogafan"
#define MAX_FANS 8
/* Filter Configuration Constants */
#define TAU_MS          1000    /* Time constant for the first-order lag (ms) */
#define MAX_SLEW_RPM_S  1500     /* Maximum allowed change in RPM per second */
#define MAX_SAMPLING    5000    /* Maximum allowed Ts for reset (ms) */
/* RPM Heuristic and Sanitation Constants */
#define RPM_UNIT_THRESHOLD 500  /* Values below this treated as units of 100 */
#define RPM_UNIT_MULT      100  /* Multiplier for low-res EC readings */
#define RPM_FLOOR_LIMIT    50   /* Snap filtered value to 0 if raw is 0 */

struct yoga_fan_data {
	const char *active_paths[MAX_FANS];
	long filtered_val[MAX_FANS];
	ktime_t last_sample[MAX_FANS]; /* Renamed from last_update for consistency */
	int fan_count;
};
/**
 * apply_rllag_filter - Discrete-time filter update (Passive Multirate)
 * @data: pointer to driver data
 * @idx: fan index
 * @raw_rpm: new raw value from ACPI
 */
static void apply_rllag_filter(struct yoga_fan_data *data, int idx, long raw_rpm)
{
	ktime_t now = ktime_get();
	s64 dt_ms = ktime_to_ms(ktime_sub(now, data->last_sample[idx]));
	long delta, step, limit, alpha;
	s64 temp_num;

	/* Initialize on first run or after long sleep/stall */
	if (data->last_sample[idx] == 0 || dt_ms > MAX_SAMPLING) {
		data->filtered_val[idx] = raw_rpm;
		data->last_sample[idx] = now;
		return;
	}
	if (dt_ms <= 0) return;

	delta = raw_rpm - data->filtered_val[idx];
	if (delta == 0) {
		data->last_sample[idx] = now;
		return;
	}
	/* Alpha with 12-bit precision to prevent alpha=0 on fast polls */
	temp_num = dt_ms << 12;
	alpha = (long)div64_s64(temp_num, (s64)(TAU_MS + dt_ms));
	step = (delta * alpha) >> 12;
	/* FIX THE STALL: Force a move of 1 RPM if alpha*delta rounds to zero */
	if (step == 0 && delta != 0)
		step = (delta > 0) ? 1 : -1;
	/* SLEW RATE LIMITING: Scaled by time delta */
	limit = (MAX_SLEW_RPM_S * (long)dt_ms) / 1000;
	if (limit < 1) limit = 1;
	/* Clamp step to physical slew rate */
	if (step > limit)
		step = limit;
	else if (step < -limit)
		step = -limit;
	data->filtered_val[idx] += step;
	data->last_sample[idx] = now;
}

static int yoga_fan_read(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long *val)
{
	struct yoga_fan_data *data = dev_get_drvdata(dev);
	unsigned long long raw_acpi;
	acpi_status status;
	long rpm;

	if (type != hwmon_fan || attr != hwmon_fan_input)
		return -EOPNOTSUPP;
	status = acpi_evaluate_integer(NULL, (acpi_string)data->active_paths[channel],
					NULL, &raw_acpi);
	if (ACPI_FAILURE(status))
		return -EIO;
	/* * Heuristic: Convert units-of-100 to raw RPM.
	 * Most Yoga/Legion ECs return a single byte (0-255).
	 * We use 500 as a safety threshold to distinguish from raw 16-bit RPM.
	 */
	rpm = (long)raw_acpi;
	if (rpm > 0 && rpm < RPM_UNIT_THRESHOLD)
		rpm *= RPM_UNIT_MULT;
	apply_rllag_filter(data, channel, rpm);
	*val = data->filtered_val[channel];
	return 0;
}

static umode_t yoga_fan_is_visible(const void *data, enum hwmon_sensor_types type,
				   u32 attr, int channel)
{
	const struct yoga_fan_data *fan_data = data;

	if (type == hwmon_fan && channel < fan_data->fan_count)
		return 0444;
	return 0;
}

static const struct hwmon_ops yoga_fan_hwmon_ops = {
	.is_visible = yoga_fan_is_visible,
	.read = yoga_fan_read,
};

static const struct hwmon_channel_info *yoga_fan_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT,
			   HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT),
	NULL
};

static const struct hwmon_chip_info yoga_fan_chip_info = {
	.ops = &yoga_fan_hwmon_ops,
	.info = yoga_fan_info,
};

static int yoga_fan_probe(struct platform_device *pdev)
{
	struct yoga_fan_data *data;
	struct device *hwmon_dev;
	acpi_handle handle;
	int i;
	static const char * const fan_paths[] = {
		"\\_SB.PCI0.LPC0.EC0.FANS",  /* Primary Fan (Yoga 14c) */
		"\\_SB.PCI0.LPC0.EC0.FA2S",  /* Secondary Fan (Legion) */
		"\\_SB.PCI0.LPC0.EC0.FAN0",  /* IdeaPad / Slim */
		"\\_SB.PCI0.LPC.EC.FAN0",    /* Legacy */
		"\\_SB.PCI0.LPC0.EC.FAN0",   /* Alternate */
	};
	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->fan_count = 0;
	for (i = 0; i < ARRAY_SIZE(fan_paths); i++) {
		if (ACPI_SUCCESS(acpi_get_handle(NULL, (char *)fan_paths[i], &handle))) {
			data->active_paths[data->fan_count] = fan_paths[i];
			data->fan_count++;
			if (data->fan_count >= MAX_FANS)
				break;
		}
	}

	if (data->fan_count == 0)
		return -ENODEV;
	platform_set_drvdata(pdev, data);
	hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev, DRVNAME,
							 data, &yoga_fan_chip_info, NULL);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static struct platform_driver yoga_fan_driver = {
	.driver = {
		.name = DRVNAME,
	},
	.probe = yoga_fan_probe,
};

static struct platform_device *yoga_fan_device;

static const struct dmi_system_id yoga_dmi_table[] __initconst = {
	{
		.ident = "Lenovo",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
		},
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, yoga_dmi_table);

static int __init yoga_fan_init(void)
{
	int ret;

	if (!dmi_check_system(yoga_dmi_table))
		return -ENODEV;
	ret = platform_driver_register(&yoga_fan_driver);
	if (ret)
		return ret;
	yoga_fan_device = platform_device_register_simple(DRVNAME, 0, NULL, 0);
	if (IS_ERR(yoga_fan_device)) {
		platform_driver_unregister(&yoga_fan_driver);
		return PTR_ERR(yoga_fan_device);
	}
	return 0;
}

static void __exit yoga_fan_exit(void)
{
	platform_device_unregister(yoga_fan_device);
	platform_driver_unregister(&yoga_fan_driver);
}

module_init(yoga_fan_init);
module_exit(yoga_fan_exit);
MODULE_AUTHOR("Sergio Melas <sergiomelas@gmail.com>");
MODULE_DESCRIPTION("Lenovo Yoga/Legion Fan Monitor Driver");
MODULE_LICENSE("GPL");
