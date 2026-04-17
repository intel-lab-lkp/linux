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
#define MAX_FANS		3

/* Filter Configuration Constants */
#define MAX_SAMPLING		5000	/* Maximum allowed Ts for reset (ms) */
#define MIN_SAMPLING		100	/* Minimum interval between filter updates (ms) */

/* RPM Sanitation Constants */
#define MIN_THRESHOLD_RPM	10	/* Minimum safety floor for per-model stop thresholds */

/* GUID of WMI interface Lenovo */
#define LENOVO_WMI_OTHER_MODE_GUID      "DC2A8805-3A8C-41BA-A6F7-092E0089CD3B"
#define LENOVO_CAPABILITY_DATA_00_GUID  "024D9939-9528-40F7-B4EF-792E0089CD3B"
#define LENOVO_WMI_FAN_GUID             "05244583-1621-468E-9366-0744D661F033"

struct yogafan_config {
	u32 multiplier;			/* Used if n_max == 0 */
	u32 fan_count;			/* 1 to 3 */
	u32 n_max;			/* Discrete steps (0 = Continuous) */
	u32 r_max;			/* Max physical RPM for estimation */
	unsigned int tau_ms;		/* To store the smoothing speed    */
	unsigned int slew_time_s;	/* To store the acceleration limit */
	unsigned int stop_threshold;	/* To store the RPM floor */
	const char *paths[MAX_FANS];	/* Paths */
};

struct yoga_fan_data {
	acpi_handle active_handles[MAX_FANS];
	long filtered_val[MAX_FANS];
	ktime_t last_sample[MAX_FANS];
	const struct yogafan_config *config;
	u32 fan_count;
	/* Per-device physics constants */
	unsigned int internal_tau_ms;
	unsigned int internal_max_slew_rpm_s;
	unsigned int device_max_rpm;
};

/* --- HARDWARE ABSTRACTION LAYER (HAL) ARCHITECTURE PROFILES --- */

/* --- 1. CONTINUOUS PROFILES (Nmax = 0) --- */

/* 1.1 Single-Fan Continuous */

/* Reference Model: Yoga 14cACN (d=50mm) - Baseline inertia (Reference J) */
static const struct yogafan_config yoga_continuous_8bit_cfg = {
	.multiplier = 100, .fan_count = 1, .n_max = 0,
	.r_max = 5500, .tau_ms = 1000, .slew_time_s = 4, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FANS", "\\_SB.PCI0.LPC0.EC0.FAN0" }
};

/* 1.2 Dual-Fan Continuous (Gaming & Pro) */

/* Legion 5 / GeekPro (d=60mm) - Gaming high inertia */
static const struct yogafan_config legion_5_cfg = {
	.multiplier = 1, .fan_count = 2, .n_max = 0,
	.r_max = 6500, .tau_ms = 1300, .slew_time_s = 5, .stop_threshold = 50,
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
 * Physics Principles (IEC 61511 / IEC 61508):
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
	s64 dt_ms = ktime_to_ms(ktime_sub(now, data->last_sample[idx]));
	long delta, step, limit, alpha;
	s64 temp_num;

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
	if (data->last_sample[idx] == 0 || dt_ms > MAX_SAMPLING) {
		data->filtered_val[idx] = raw_rpm;
		data->last_sample[idx] = now;
		return;
	}

	delta = raw_rpm - data->filtered_val[idx];
	if (delta == 0) {
		data->last_sample[idx] = now;
		return;
	}

	/* 4. Physics Engine: Discretized RLLAG filter (Fixed-Point 2^12) */
	/* Ref: [TAG: MODEL_CONST, ALPHA_DERIVATION, ANTI_STALL_LOGIC] */
	temp_num = dt_ms << 12;
	alpha = (long)div64_s64(temp_num, (s64)(data->internal_tau_ms + dt_ms));
	step = (delta * alpha) >> 12;

	/* Ensure minimal movement for small deltas */
	if (step == 0 && delta != 0)
		step = (delta > 0) ? 1 : -1;

	/* 5. Dynamic Slew Limiting: Applied per-model inertia ramp */
	/* Ref: [TAG: SLEW_RATE_MAX, SLOPE_CALC, MIN_SLEW_LIMIT] */
	limit = ((long)data->internal_max_slew_rpm_s * (long)dt_ms) / 1000;
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
	const struct yogafan_config *cfg = data->config;
	unsigned long long raw_acpi;
	long rpm_raw;
	acpi_status status;
	s64 dt_ms;

	if (type != hwmon_fan)
		return -EOPNOTSUPP;

	/* 1. Handle static MAX attribute immediately without filtering */
	if (attr == hwmon_fan_max) {
		*val = (long)data->device_max_rpm;
		return 0;
	}

	if (attr != hwmon_fan_input)
		return -EOPNOTSUPP;

	/* 2. Polling Protection */
	dt_ms = ktime_to_ms(ktime_sub(ktime_get_boottime(), data->last_sample[channel]));

	if (data->last_sample[channel] != 0 && dt_ms < MIN_SAMPLING) {
		*val = data->filtered_val[channel];
		return 0;
	}

	/* 3. Hardware Reading with AND fallback logic */
	status = acpi_evaluate_integer(data->active_handles[channel], NULL, NULL, &raw_acpi);

	/* If the first attempt fails AND there is a second handle for that channel, */
	/* try the second one */
	if (ACPI_FAILURE(status) && cfg->paths[channel + 1])
		status = acpi_evaluate_integer(data->active_handles[channel + 1],
					       NULL, NULL, &raw_acpi);

	/* If it still fails after the fallback, return I/O error */
	if (ACPI_FAILURE(status))
		return -EIO;

	/* 4. RPM Calculation  */
	if (cfg->n_max > 0) {
		/* Formula: (raw_acpi * device_max_rpm) / n_max */
		/* mul_u64_u32_div handles the 64-bit precision internally */
		rpm_raw = (long)mul_u64_u32_div(raw_acpi, data->device_max_rpm, cfg->n_max);
	} else {
		rpm_raw = (long)raw_acpi * cfg->multiplier;
	}

	/* 5. Apply filter on speed readings */
	apply_rllag_filter(data, channel, rpm_raw);

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

static const struct dmi_system_id yogafan_quirks[] = {
/* --- 1. YOGA SERIES --- */
{
		.ident = "Lenovo Yoga 14cACN (82N7)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "82N7")
		},
		.driver_data = (void *)&yoga_continuous_8bit_cfg,
	},

/* --- 3. LEGION SERIES --- */
	{
		.ident = "Lenovo Legion 5 (82JW)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "82JW")
		},
		.driver_data = (void *)&legion_5_cfg,
	},

/* --- 5. IDEAPAD SERIES --- */
	{
		.ident = "Lenovo IdeaPad 5 (81YM)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "81YM")
		},
		.driver_data = (void *)&yoga_continuous_8bit_cfg,
	},
	{ }
};

MODULE_DEVICE_TABLE(dmi, yogafan_quirks);

/* Static configuration for the hwmon core */
static const struct hwmon_channel_info *const yoga_fan_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_MAX),
	NULL
};

static const struct hwmon_chip_info yoga_fan_chip_info = {
	.ops = &yoga_fan_hwmon_ops,
	.info = yoga_fan_info,
};

static int yoga_fan_probe(struct platform_device *pdev)
{
	const struct dmi_system_id *dmi_id;
	const struct yogafan_config *cfg;
	struct yoga_fan_data *data;
	acpi_status status;
	int i;

	/* Check for WMI interfaces that handle fan/thermal management. */
	/*  If present, we yield to the WMI driver to prevent double-reporting. */
	if (wmi_has_guid(LENOVO_WMI_OTHER_MODE_GUID) &&
	    wmi_has_guid(LENOVO_CAPABILITY_DATA_00_GUID) &&
	    wmi_has_guid(LENOVO_WMI_FAN_GUID)) {
		dev_info(&pdev->dev, "Lenovo WMI management interface detected; yielding to WMI driver\n");
		return -ENODEV;
	}

	dmi_id = dmi_first_match(yogafan_quirks);
	if (!dmi_id)
		return -ENODEV;

	cfg = dmi_id->driver_data;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	/* * 1. Hardware Calibration & Inertia Scaling (Note 3):
	 * Dynamic parameters (TAU and SLEW) are calibrated relative to fan diameter
	 * based on the moment of inertia relationship (J ∝ d²).
	 */
	data->config = cfg;
	data->device_max_rpm = cfg->r_max ?: 5000;
	data->internal_tau_ms = cfg->tau_ms ?: 1000; /* Robustness: Prevent zero-division */

	/* * Log physical parameters for safety traceability (IEC 61508):
	 * Provides a deterministic baseline for the RLLag filter verification.
	 */
	data->internal_max_slew_rpm_s = data->device_max_rpm / (cfg->slew_time_s ?: 1);
	dev_info(&pdev->dev, "Identified hardware: %s\n", dmi_id->ident);
	dev_info(&pdev->dev, "HAL Profile: [Tau: %ums, Slew: %u RPM/s, Max: %u RPM]\n",
		 data->internal_tau_ms, data->internal_max_slew_rpm_s, data->device_max_rpm);

	/* * 2. Deterministic Multi-Path Discovery:
	 * We iterate through the available paths to find physical handles.
	 * This loop tests variations until data->fan_count matches the
	 * cfg->fan_count expected for this model profile.
	 */
	for (i = 0; i < MAX_FANS && data->fan_count < cfg->fan_count; i++) {
		acpi_handle handle;

		/* Integrity check: End of defined paths in the quirk table */
		if (!cfg->paths[i])
			break;

		status = acpi_get_handle(NULL, cfg->paths[i], &handle);
		if (ACPI_SUCCESS(status)) {
			data->active_handles[data->fan_count] = handle;
			data->fan_count++;
		} else {
			/* Log variation failure for troubleshooting */
			dev_dbg(&pdev->dev, "Fan path variation %s not found\n", cfg->paths[i]);
		}
	}

	/* Integrity Check: Fail probe if no fans were successfully registered */
	if (data->fan_count == 0) {
		dev_err(&pdev->dev, "Hardware identification failed: No fans found\n");
		return -ENODEV;
	}

	/* * 3. Finalize registration using the static template */
	return PTR_ERR_OR_ZERO(devm_hwmon_device_register_with_info(&pdev->dev,
				DRVNAME, data, &yoga_fan_chip_info, NULL));
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
