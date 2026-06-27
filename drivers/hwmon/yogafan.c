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
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/hwmon-sysfs.h>
#include <linux/wmi.h>

/* Driver Configuration Constants */
#define DRVNAME			"yogafan"
#define MAX_FANS		5

/* Filter Configuration Constants */
#define TAU_MS			1000	/* Time constant for the first-order lag (ms) */
#define MAX_SLEW_RPM_S		1500	/* Maximum allowed change in RPM per second */
#define MAX_SAMPLING		5000	/* Maximum allowed Ts for reset (ms) */
#define MIN_SAMPLING		100	/* Minimum interval between filter updates (ms) */

/* RPM Sanitation Constants */
#define MIN_THRESHOLD_RPM	10	/* Minimum safety floor for per-model stop thresholds */

/* GUID of WMI interface Lenovo */
#define LENOVO_WMI_OTHER_MODE_GUID	"DC2A8805-3A8C-41BA-A6F7-092E0089CD3B"
#define LENOVO_CAPABILITY_DATA_00_GUID	"362A3AFE-3D96-4665-8530-96DAD5BB300E"
#define LENOVO_FAN_TEST_DATA_GUID	"B642801B-3D21-45DE-90AE-6E86F164FB21"

struct yogafan_config {
	int multiplier;
	int fan_count;
	int r_max;		        /* Maximum physical RPM for UI scaling */
	unsigned int tau_ms;		/* To store the smoothing speed */
	unsigned int slew_time_s;	/* To store the acceleration limit */
	unsigned int stop_threshold;	/* To store the RPM floor */
	const char *paths[2];
};

struct yoga_fan_data {
	acpi_handle active_handles[MAX_FANS];
	long filtered_val[MAX_FANS];
	ktime_t last_sample[MAX_FANS];
	int multiplier;
	int fan_count;
	int device_max_rpm;	/* Stores the active maximum RPM ceiling */
	unsigned int internal_tau_ms;
	unsigned int internal_max_slew_rpm_s;
	const struct yogafan_config *config;
};

/* Specific configurations mapped via DMI */
static const struct yogafan_config yoga_8bit_fans_cfg = {
	.multiplier = 100,
	.fan_count = 1,
	.r_max = 5500,
	.tau_ms = 1000,
	.slew_time_s = 4,
	.stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FANS", NULL }
};

static const struct yogafan_config ideapad_8bit_fan0_cfg = {
	.multiplier = 100,
	.fan_count = 1,
	.r_max = 4500,
	.tau_ms = 1000,
	.slew_time_s = 4,
	.stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FAN0", NULL }
};

static const struct yogafan_config legion_16bit_dual_cfg = {
	.multiplier = 1,
	.fan_count = 2,
	.r_max = 6500,
	.tau_ms = 1300,
	.slew_time_s = 5,
	.stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FANS", "\\_SB.PCI0.LPC0.EC0.FA2S" }
};

/*
 * Filter Physics (RLLag) - Deterministic Telemetry
 * ---------------------
 * To address low-resolution tachometer sampling in the Embedded Controller,
 * the driver implements a passive discrete-time first-order lag filter
 * with slew-rate limiting (RLLag).
 *
 * The filter update equation is:
 * RPM_state[t+1] = RPM_state[t] + Clamp(Alpha * (raw_RPM[t] - RPM_state[t]),
 * -limit[t], limit[t])
 * Where:
 * Ts[t]    = Sys_time[t+1] - Sys_time[t]  (Time delta between reads)
 * Alpha    = 1 - exp(-Ts[t] / Tau)        (Low-pass smoothing factor)
 * limit[t] = Slew_Limit * Ts[t]           (Time-normalized slew limit)
 *
 * To avoid expensive floating-point exponential calculations in the kernel,
 * we use a first-order Taylor/Bilinear approximation:
 * Alpha = Ts / (Tau + Ts)
 *
 * Implementing this in the driver state machine:
 * Ts             = current_time - last_sample_time
 * Alpha          = Ts / (Tau + Ts)
 * Physics Principles:
 * step           = Alpha * (raw_RPM - RPM_old)
 * limit          = Slew_Limit * Ts
 * step_clamped   = clamp(step, -limit, limit)
 * RPM_new        = RPM_old + step_clamped
 *
 * Attributes of the RLLag model:
 * - Smoothing: Low-resolution step increments are smoothed into 1-RPM increments.
 * - Slew-Rate Limiting: Capping change to ~1500 RPM/s to match physical inertia.
 * - Polling Independence: Math scales based on Ts, ensuring a consistent physical
 * curve regardless of userspace polling frequency.
 * Fixed-point math (2^12) is used to maintain precision without floating-point
 * overhead, ensuring jitter-free telemetry for thermal management.
 */
static void apply_rllag_filter(struct yoga_fan_data *data, int idx, long raw_rpm)
{
	ktime_t now = ktime_get_boottime();
	s64 raw_dt_ms;
	long delta, step, limit, alpha;
	s64 temp_num;
	u32 dt_ms;

	/* 1. PHYSICAL CLAMP: Use per-device device_max_rpm */
	if (raw_rpm > (long)data->device_max_rpm)
		raw_rpm = (long)data->device_max_rpm;

	/* 2. Threshold logic: Deterministic safe-state */
	if (raw_rpm < (long)max_t(u32, MIN_THRESHOLD_RPM, data->config->stop_threshold)) {
		data->filtered_val[idx] = 0;
		data->last_sample[idx] = now;
		return;
	}

	/* 3. Auto-Reset Logic: Snap to hardware value after long gaps (>5s) */
	/*   Ref: [TAG: INIT_STATE, STALE_DATA_THRESHOLD] */
	raw_dt_ms = ktime_to_ms(ktime_sub(now, data->last_sample[idx]));

	if (data->last_sample[idx] == 0 || raw_dt_ms < MIN_SAMPLING || raw_dt_ms > MAX_SAMPLING) {
		data->filtered_val[idx] = raw_rpm;
		data->last_sample[idx] = now;
		return;
	}

	dt_ms = (u32)raw_dt_ms;

	delta = raw_rpm - data->filtered_val[idx];
	if (delta == 0) {
		data->last_sample[idx] = now;
		return;
	}

	/* 4. Physics Engine: Discretized RLLAG filter (Fixed-Point 2^12) */
	/* Ref: [TAG: MODEL_CONST, ALPHA_DERIVATION, ANTI_STALL_LOGIC] */
	temp_num = (s64)dt_ms << 12;
	alpha = div64_u64(temp_num, data->internal_tau_ms + dt_ms);
	step = (delta * alpha) >> 12;

	/* Ensure minimal movement for small deltas */
	if (step == 0 && delta != 0)
		step = (delta > 0) ? 1 : -1;

	/* 5. Dynamic Slew Limiting: Applied per-model inertia ramp */
	/* Ref: [TAG: SLEW_RATE_MAX, SLOPE_CALC, MIN_SLEW_LIMIT] */
	limit = (data->internal_max_slew_rpm_s * dt_ms) / 1000;

	if (limit < 1)
		limit = 1;

	if (step > limit)
		step = limit;
	else if (step < -limit)
		step = -limit;

	/* 6. Update internal state */
	data->filtered_val[idx] += step;
	data->last_sample[idx] = now;
}

static int yoga_fan_read(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long *val)
{
	struct yoga_fan_data *data = dev_get_drvdata(dev);
	unsigned long long raw_acpi;
	acpi_status status;

	if (type != hwmon_fan)
		return -EOPNOTSUPP;

	/* Intercept MAX attribute queries to feed the UI scale framework */
	if (attr == hwmon_fan_max) {
		*val = (long)data->device_max_rpm;
		return 0;
	}

	if (attr != hwmon_fan_input)
		return -EOPNOTSUPP;

	status = acpi_evaluate_integer(data->active_handles[channel], NULL, NULL, &raw_acpi);
	if (ACPI_FAILURE(status))
		return -EIO;

	apply_rllag_filter(data, channel, (long)raw_acpi * data->multiplier);
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

/* Static configuration for the hwmon core */
static const struct hwmon_channel_info *const yoga_fan_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_MAX),
	NULL
};

static const struct hwmon_chip_info yoga_fan_chip_info = {
	.ops = &yoga_fan_hwmon_ops,
	.info = yoga_fan_info,
};

static const struct dmi_system_id yogafan_quirks[] = {
	{
		.ident = "Lenovo Yoga",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_FAMILY, "Yoga"),
		},
		.driver_data = (void *)&yoga_8bit_fans_cfg,
	},
	{
		.ident = "Lenovo Legion",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_FAMILY, "Legion"),
		},
		.driver_data = (void *)&legion_16bit_dual_cfg,
	},
	{
		.ident = "Lenovo IdeaPad",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_FAMILY, "IdeaPad"),
		},
		.driver_data = (void *)&ideapad_8bit_fan0_cfg,
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, yogafan_quirks);

static int yoga_fan_probe(struct platform_device *pdev)
{
	const struct dmi_system_id *dmi_id;
	const struct yogafan_config *cfg;
	struct yoga_fan_data *data;
	struct device *hwmon_dev;
	int i;

	/* Check for WMI interfaces that handle fan/thermal management. */
	/*  If present, we yield to the WMI driver to prevent double-reporting. */
#if IS_REACHABLE(CONFIG_ACPI_WMI)
	if (wmi_has_guid(LENOVO_WMI_OTHER_MODE_GUID) &&
	    wmi_has_guid(LENOVO_CAPABILITY_DATA_00_GUID) &&
	    wmi_has_guid(LENOVO_FAN_TEST_DATA_GUID)) {
		dev_info(&pdev->dev, "Lenovo WMI management interface detected; yielding to WMI driver\n");
		return -ENODEV;
	}
#endif

	dmi_id = dmi_first_match(yogafan_quirks);
	if (!dmi_id)
		return -ENODEV;

	cfg = dmi_id->driver_data;
	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->config = cfg;
	data->multiplier = cfg->multiplier;
	data->device_max_rpm = cfg->r_max ?: 5000; /* Fallback safety baseline */
	data->internal_tau_ms = cfg->tau_ms ?: 1000; /* Robustness: Prevent zero-division */
	data->internal_max_slew_rpm_s = data->device_max_rpm / (cfg->slew_time_s ?: 1);

	for (i = 0; i < cfg->fan_count; i++) {
		acpi_status status;

		status = acpi_get_handle(NULL, (char *)cfg->paths[i],
					 &data->active_handles[data->fan_count]);
		if (ACPI_SUCCESS(status))
			data->fan_count++;
	}

	if (data->fan_count == 0)
		return -ENODEV;

	hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev, DRVNAME,
							 data, &yoga_fan_chip_info, NULL);

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static struct platform_driver yoga_fan_driver = {
	.driver = { .name = DRVNAME },
	.probe = yoga_fan_probe,
};

static struct platform_device *yoga_fan_device;

static int __init yoga_fan_init(void)
{
	int ret;

	if (!dmi_check_system(yogafan_quirks))
		return -ENODEV;

	ret = platform_driver_register(&yoga_fan_driver);
	if (ret)
		return ret;

	yoga_fan_device = platform_device_register_simple(DRVNAME, -1, NULL, 0);
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
