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
	int multiplier;			/* Used if n_max == 0 */
	int fan_count;			/* 1 to 3 */
	int n_max;			/* Discrete steps (0 = Continuous) */
	int r_max;			/* Max physical RPM for estimation */
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
	int fan_count;
	/* Per-device physics constants */
	unsigned int internal_tau_ms;
	unsigned int internal_max_slew_rpm_s;
	unsigned int device_max_rpm;
};

/* --- HARDWARE ABSTRACTION LAYER (HAL) ARCHITECTURE PROFILES --- */

/* --- 1. CONTINUOUS PROFILES (Nmax = 0) --- */

/* 1.1 Single-Fan Continuous */

/* Reference Model: Yoga 14cACN (d=50mm) - Baseline inertia (Reference J) */
static struct yogafan_config yoga_continuous_8bit_cfg = {
	.multiplier = 100, .fan_count = 1, .n_max = 0,
	.r_max = 5500, .tau_ms = 1000, .slew_time_s = 4, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FANS", "\\_SB.PCI0.LPC0.EC0.FAN0" }
};

/* Yoga Slim Series (d=45mm) - Reduced inertia (J ∝ d²) */
static struct yogafan_config yoga_slim_cfg = {
	.multiplier = 100, .fan_count = 1, .n_max = 0,
	.r_max = 5500, .tau_ms = 900, .slew_time_s = 3, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FANS", NULL }
};

/* ThinkPad L-Series / V580 (d=50mm) - Standard inertia */
static struct yogafan_config thinkpad_l_cfg = {
	.multiplier = 100, .fan_count = 1, .n_max = 0,
	.r_max = 5500, .tau_ms = 1000, .slew_time_s = 4, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FAN0", "\\_SB.PCI0.LPC0.EC0.FAN1" }
};

/* 1.2 Dual-Fan Continuous (Gaming & Pro) */

/* Legion 5 / GeekPro (d=60mm) - Gaming high inertia */
static struct yogafan_config legion_5_cfg = {
	.multiplier = 1, .fan_count = 2, .n_max = 0,
	.r_max = 6500, .tau_ms = 1300, .slew_time_s = 5, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FANS", "\\_SB.PCI0.LPC0.EC0.FA2S" }
};

/* Legion 7i / Yoga Pro 9i (d=65mm) - High inertia (Heavy blades) */
static struct yogafan_config legion_high_perf_cfg = {
	.multiplier = 1, .fan_count = 2, .n_max = 0,
	.r_max = 8000, .tau_ms = 1400, .slew_time_s = 6, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FANS", "\\_SB.PCI0.LPC0.EC0.FA2S" }
};

/* LOQ Series (d=55mm) - Medium-high inertia */
static struct yogafan_config loq_cfg = {
	.multiplier = 1, .fan_count = 2, .n_max = 0,
	.r_max = 6500, .tau_ms = 1200, .slew_time_s = 5, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FANS", "\\_SB.PCI0.LPC0.EC0.FA2S" }
};

/* Yoga Pro 7i Aura Edition (83KF) - Dual-fan 8-bit architecture (d=55mm) */
static struct yogafan_config yoga_aura_cfg = {
	.multiplier = 100, .fan_count = 2, .n_max = 0,
	.r_max = 6000, .tau_ms = 1100, .slew_time_s = 4, .stop_threshold = 50,
	.paths = { "\\_SB.PC00.LPCB.EC0.FA1S", "\\_SB.PC00.LPCB.EC0.FA2S" }
};

/* Yoga 13 (d=40mm) - Dual small fans, low inertia */
static struct yogafan_config yoga13_continous_cfg = {
	.multiplier = 100, .fan_count = 2, .n_max = 0,
	.r_max = 5000, .tau_ms = 800, .slew_time_s = 3, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FAN1", "\\_SB.PCI0.LPC0.EC0.FAN2" }
};

/* Standard Dual-Fan (d=50/55mm) - Baseline inertia (Reference J) */
static struct yogafan_config yoga_dual_8bit_cfg = {
	.multiplier = 100, .fan_count = 2, .n_max = 0,
	.r_max = 6000, .tau_ms = 1100, .slew_time_s = 4, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FANS", "\\_SB.PCI0.LPC0.EC0.FA2S" }
};

/* 1.3 Triple-Fan Continuous */

/* Legion 9i (d=70mm primary) - Massive inertia, triple assembly */
static struct yogafan_config legion_triple_16bit_cfg = {
	.multiplier = 1, .fan_count = 3, .n_max = 0,
	.r_max = 8000, .tau_ms = 1500, .slew_time_s = 6, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FANS"
		 , "\\_SB.PCI0.LPC0.EC0.FA2S"
		 , "\\_SB.PCI0.LPC0.EC0.FA3S" }
};

//* --- 2. DISCRETE ESTIMATION PROFILES (Nmax > 0) --- */

/* 2.1 Single-Fan Discrete */

/* Legacy Performance (d=55mm) - Higher inertia (J ∝ d²) */
static struct yogafan_config ideapad_y580_discrete_cfg = {
	.multiplier = 0, .fan_count = 1, .n_max = 35, .r_max = 4800,
	.tau_ms = 1100, .slew_time_s = 4, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FAN0", "\\_SB.PCI0.LPC0.EC0.FANS" }
};

/* Standard Legacy (d=50mm) - Baseline inertia (Reference J) */
static struct yogafan_config yoga_710_discrete_cfg = {
	.multiplier = 0, .fan_count = 1, .n_max = 59, .r_max = 4500,
	.tau_ms = 1000, .slew_time_s = 4, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FAN0", NULL }
};

static struct yogafan_config yoga_510_discrete_cfg = {
	.multiplier = 0, .fan_count = 1, .n_max = 41, .r_max = 4500,
	.tau_ms = 1000, .slew_time_s = 4, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FAN0", NULL }
};

/* Slim Discrete Models (d=45mm) - Reduced inertia */
static struct yogafan_config ideapad_500s_discrete_cfg = {
	.multiplier = 0, .fan_count = 1, .n_max = 44, .r_max = 5500,
	.tau_ms = 900, .slew_time_s = 3, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FAN0", NULL }
};

/* Standard Discrete (d=50mm) */
static struct yogafan_config yoga3_14_discrete_cfg = {
	.multiplier = 0, .fan_count = 1, .n_max = 80, .r_max = 5000,
	.tau_ms = 1000, .slew_time_s = 4, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FAN0", "\\_SB.PCI0.LPC0.EC0.FANS" }
};

/* Ultra-portable (d=35mm) - Minimal inertia, fast response */
static struct yogafan_config yoga_11s_discrete_cfg = {
	.multiplier = 0, .fan_count = 1, .n_max = 80, .r_max = 4500,
	.tau_ms = 600, .slew_time_s = 2, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FAN0", "\\_SB.PCI0.LPC0.EC0.FANS" }
};

/* Small Discrete (d=45mm) */
static struct yogafan_config yoga2_13_discrete_cfg = {
	.multiplier = 0, .fan_count = 1, .n_max = 8, .r_max = 4200,
	.tau_ms = 800, .slew_time_s = 3, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FAN0", NULL }
};

/* Legacy U-Series / High-Res Discrete (d=40mm) - Small blade mass */
static struct yogafan_config legacy_u_discrete_cfg = {
	.multiplier = 0, .fan_count = 1, .n_max = 768, .r_max = 5000,
	.tau_ms = 800, .slew_time_s = 3, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FAN0", NULL }
};

/* ThinkPad Discrete (d=50mm) */
static struct yogafan_config thinkpad_discrete_cfg = {
	.multiplier = 0, .fan_count = 1, .n_max = 7,
	.r_max = 5500, .tau_ms = 1000, .slew_time_s = 4, .stop_threshold = 50,
	.paths = { "\\_SB.PCI0.LPC0.EC0.FAN0", "\\_SB.PCI0.LPC0.EC0.FANS" }
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
	if (raw_rpm < (long)(data->config->stop_threshold < MIN_THRESHOLD_RPM
		? MIN_THRESHOLD_RPM : data->config->stop_threshold)) {
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

	/* 4. Cybersecurity Gating: Ignore polling spam (<100ms) to protect EC */
	/* Ref: [TAG: SPAM_FILTER, MIN_INTERVAL] */
	if (dt_ms < MIN_SAMPLING)
		return;

	delta = raw_rpm - data->filtered_val[idx];
	if (delta == 0) {
		data->last_sample[idx] = now;
		return;
	}

	/* 5. Physics Engine: Discretized RLLAG filter (Fixed-Point 2^12) */
	/* Ref: [TAG: MODEL_CONST, ALPHA_DERIVATION, ANTI_STALL_LOGIC] */
	temp_num = dt_ms << 12;
	alpha = (long)div64_s64(temp_num, (s64)(data->internal_tau_ms + dt_ms));
	step = (delta * alpha) >> 12;

	/* Ensure minimal movement for small deltas */
	if (step == 0 && delta != 0)
		step = (delta > 0) ? 1 : -1;

	/* 6. Dynamic Slew Limiting: Applied per-model inertia ramp */
	/* Ref: [TAG: SLEW_RATE_MAX, SLOPE_CALC, MIN_SLEW_LIMIT] */
	limit = ((long)data->internal_max_slew_rpm_s * (long)dt_ms) / 1000;
	if (limit < 1)
		limit = 1;

	if (step > limit)
		step = limit;
	else if (step < -limit)
		step = -limit;

	/* Update internal state */
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

	if (type != hwmon_fan)
		return -EOPNOTSUPP;

	/* 1. Handle static MAX attribute immediately without filtering */
	if (attr == hwmon_fan_max) {
		*val = (long)data->device_max_rpm;
		return 0;
	}

	if (attr != hwmon_fan_input)
		return -EOPNOTSUPP;

	/* 2. Get hardware data only for INPUT requests */
	status = acpi_evaluate_integer(data->active_handles[channel], NULL, NULL, &raw_acpi);
	if (ACPI_FAILURE(status))
		return -EIO;

	/* 3. Calculate raw RPM based on architecture */
	if (cfg->n_max > 0)
		/* Use s64 promotion to prevent overflow during multiplication before division */
		rpm_raw = (long)div64_s64((s64)data->device_max_rpm * raw_acpi, cfg->n_max);
	else
		rpm_raw = (long)raw_acpi * cfg->multiplier;

	/* 4. Apply filter only for real speed readings */
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
		.ident = "Lenovo Yoga Pro 9i (83DN)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "83DN") },
		.driver_data = &legion_high_perf_cfg, /* 16" Chassis - High Inertia */
	},
	{
		.ident = "Lenovo Yoga Pro 9 (83CV) - Aura Edition",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "83CV") },
		.driver_data = &yoga_slim_cfg,
	},
	{
		.ident = "Lenovo Yoga Pro 9i (83E2 - Alt)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "83E2") },
		.driver_data = &yoga_dual_8bit_cfg,
	},
	{
		.ident = "Lenovo Yoga Pro 7i Aura (83KF)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "83KF") },
		.driver_data = &yoga_aura_cfg, /* Aura Edition - Modern PC00 Path */
	},
	{
		.ident = "Lenovo Yoga Pro (Legacy ID)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "Yoga Pro") },
		.driver_data = &legion_high_perf_cfg,
	},
	{
		.ident = "Lenovo Yoga Slim 7 (82A2)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "82A2") },
		.driver_data = &yoga_slim_cfg,
	},
	{
		.ident = "Lenovo Yoga Slim 7 (82A3)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "82A3") },
		.driver_data = &yoga_slim_cfg,
	},
	{
		.ident = "Lenovo Yoga Slim 7 Pro / ProX",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "Yoga Slim 7 Pro") },
		.driver_data = &yoga_dual_8bit_cfg,
	},
	{
		.ident = "Lenovo Yoga Slim 7 Carbon",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "Yoga Slim 7 Carbon") },
		.driver_data = &yoga_slim_cfg,
	},
	{
		.ident = "Lenovo Yoga 14cACN (82N7)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "82N7") },
		.driver_data = &yoga_continuous_8bit_cfg,
	},
	{
		.ident = "Lenovo Yoga 14s",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "Yoga 14s") },
		.driver_data = &yoga_continuous_8bit_cfg,
	},
	{
		.ident = "Lenovo Yoga 710 (80V2)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "80V2") },
		.driver_data = &yoga_710_discrete_cfg,
	},
	{
		.ident = "Lenovo Yoga 720 (81C3)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "81C3") },
		.driver_data = &yoga_710_discrete_cfg,
	},
	{
		.ident = "Lenovo Yoga 710/720 (String Match)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "Yoga 710") },
		.driver_data = &yoga_710_discrete_cfg,
	},
	{
		.ident = "Lenovo Yoga 510 (80S7)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "80S7") },
		.driver_data = &yoga_510_discrete_cfg,
	},
	{
		.ident = "Lenovo Yoga 510 (String Match)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "Yoga 510") },
		.driver_data = &yoga_510_discrete_cfg,
	},
	{
		.ident = "Lenovo Yoga 3 14 (80JH)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "80JH") },
		.driver_data = &yoga3_14_discrete_cfg,
	},
	{
		.ident = "Lenovo Yoga 2 13 (20344)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "20344") },
		.driver_data = &yoga2_13_discrete_cfg,
	},
	{
		.ident = "Lenovo Yoga 13 (20191)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "20191") },
		.driver_data = &yoga13_continous_cfg,
	},
	{
		.ident = "Lenovo Yoga 11s (Legacy)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "Yoga 11s") },
		.driver_data = &yoga_11s_discrete_cfg,
	},
	{
		.ident = "Lenovo Yoga Aura Edition",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "Aura Edition") },
		.driver_data = &yoga_continuous_8bit_cfg,
	},

/* --- 2. XIAOXIN SERIES (PRC) --- */
	{
		.ident = "Lenovo Xiaoxin Pro (83JC)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "83JC") },
		.driver_data = &yoga3_14_discrete_cfg,
	},
	{
		.ident = "Lenovo Xiaoxin Pro (83DX)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "83DX") },
		.driver_data = &yoga3_14_discrete_cfg,
	},
	{
		.ident = "Lenovo Xiaoxin Pro (83FD)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "83FD") },
		.driver_data = &yoga_continuous_8bit_cfg,
	},
	{
		.ident = "Lenovo Xiaoxin Pro (83DE)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "83DE") },
		.driver_data = &yoga_continuous_8bit_cfg,
	},

/* --- 3. LEGION SERIES --- */
	{
		.ident = "Lenovo Legion 9i / Extreme",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "Legion 9") },
		.driver_data = &legion_triple_16bit_cfg,
	},
	{
		.ident = "Lenovo Legion High Perf (P-Series)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "Legion P") },
		.driver_data = &legion_high_perf_cfg,
	},
	{
		.ident = "Lenovo Legion 7i (82WQ)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "82WQ") },
		.driver_data = &legion_high_perf_cfg,
	},
	{
		.ident = "Lenovo Legion 5 (82JW)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "82JW") },
		.driver_data = &legion_5_cfg,
	},
	{
		.ident = "Lenovo Legion 5 (82JU)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "82JU") },
		.driver_data = &legion_5_cfg,
	},
	{
		.ident = "Lenovo GeekPro G5000/6000",
		.matches = { DMI_MATCH(DMI_PRODUCT_FAMILY, "GeekPro") },
		.driver_data = &legion_5_cfg,
	},

/* --- 4. LOQ SERIES --- */
	{
		.ident = "Lenovo LOQ (82XV)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "82XV") },
		.driver_data = &loq_cfg,
	},
	{
		.ident = "Lenovo LOQ (83DV)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "83DV") },
		.driver_data = &loq_cfg,
	},

/* --- 5. IDEAPAD SERIES --- */
	{
		.ident = "Lenovo IdeaPad 5 (81YM)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "81YM") },
		.driver_data = &yoga_continuous_8bit_cfg,
	},
	{
		.ident = "Lenovo IdeaPad 5 (82FG)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "82FG") },
		.driver_data = &yoga_continuous_8bit_cfg,
	},
	{
		.ident = "Lenovo IdeaPad Y580",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "IdeaPad Y580") },
		.driver_data = &ideapad_y580_discrete_cfg,
	},
	{
		.ident = "Lenovo IdeaPad Y580 (Legacy Match)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "Lenovo IdeaPad Y580") },
		.driver_data = &ideapad_y580_discrete_cfg,
	},
	{
		.ident = "Lenovo Ideapad 500S-13 (80SR)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "80SR") },
		.driver_data = &ideapad_500s_discrete_cfg,
	},
	{
		.ident = "Lenovo Ideapad 500S-13 (80SX)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "80SX") },
		.driver_data = &ideapad_500s_discrete_cfg,
	},
	{
		.ident = "Lenovo Ideapad 500S (String Match)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "Ideapad 500S") },
		.driver_data = &ideapad_500s_discrete_cfg,
	},
	{
		.ident = "Lenovo Ideapad 510S (80TK)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "80TK") },
		.driver_data = &yoga_510_discrete_cfg,
	},
	{
		.ident = "Lenovo Ideapad 510s (String Match)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "Ideapad 510s") },
		.driver_data = &yoga_510_discrete_cfg,
	},
	{
		.ident = "Lenovo Ideapad 710S (80S9)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "80S9") },
		.driver_data = &yoga13_continous_cfg,
	},
	{
		.ident = "Lenovo Ideapad 710S (String Match)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "Ideapad 710S") },
		.driver_data = &yoga13_continous_cfg,
	},
	{
		.ident = "Lenovo IdeaPad Pro 5 (Modern)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "IdeaPad Pro 5") },
		.driver_data = &yoga_dual_8bit_cfg,
	},

/* --- 6. FLEX SERIES --- */
	{
		.ident = "Lenovo Flex 5 (81X1)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "81X1") },
		.driver_data = &yoga_continuous_8bit_cfg,
	},

/* --- 7. THINKPAD SERIES --- */
	{
		.ident = "ThinkPad 13 (20GJ/20GK)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "ThinkPad 13") },
		.driver_data = &thinkpad_discrete_cfg,
	},
	{
		.ident = "ThinkPad Helix (3698)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "3698") },
		.driver_data = &thinkpad_discrete_cfg,
	},
	{
		.ident = "ThinkPad Classic (Generic T/X/Edge)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "ThinkPad") },
		.driver_data = &thinkpad_discrete_cfg,
	},
	{
		.ident = "ThinkPad L-Series (Generic Match)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "ThinkPad L") },
		.driver_data = &thinkpad_l_cfg,
	},
	{
		.ident = "ThinkPad x121e (3051)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "3051") },
		.driver_data = &yoga_11s_discrete_cfg,
	},

/* --- 8. THINKBOOK SERIES --- */
	{
		.ident = "Lenovo ThinkBook 14 G7+ (83GD)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "83GD") },
		.driver_data = &yoga_continuous_8bit_cfg, /* Forza profilo singolo se WMI è off */
	},
	{
		.ident = "Lenovo ThinkBook 14/16 Plus/p",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "ThinkBook 1") },
		.driver_data = &yoga_dual_8bit_cfg,
	},

/* --- 9. V-SERIES --- */
	{
		.ident = "Lenovo V330-15IKB (81AX)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "81AX") },
		.driver_data = &thinkpad_l_cfg,
	},
	{
		.ident = "Lenovo V330 (String Match)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "V330-15IKB") },
		.driver_data = &thinkpad_l_cfg,
	},
	{
		.ident = "Lenovo V580 (String Match)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "V580") },
		.driver_data = &thinkpad_l_cfg,
	},
	{
		.ident = "Lenovo Edge E520 / V580 (20147)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "20147") },
		.driver_data = &thinkpad_l_cfg,
	},

/* --- 10. U-SERIES (LEGACY) --- */
	{
		.ident = "Lenovo U330p/U430p",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "Lenovo u330p") },
		.driver_data = &legacy_u_discrete_cfg,
	},
	{
		.ident = "Lenovo U31-70 (80KU)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "80KU") },
		.driver_data = &ideapad_500s_discrete_cfg,
	},
	{
		.ident = "Lenovo U31-70 (String Match)",
		.matches = { DMI_MATCH(DMI_PRODUCT_NAME, "U31-70") },
		.driver_data = &ideapad_500s_discrete_cfg,
	},
	{ }
};

MODULE_DEVICE_TABLE(dmi, yogafan_quirks);

static int yoga_fan_probe(struct platform_device *pdev)
{
	const struct dmi_system_id *dmi_id;
	const struct yogafan_config *cfg;
	struct yoga_fan_data *data;
	struct hwmon_chip_info *chip_info;
	struct hwmon_channel_info *info;
	u32 *fan_config;
	acpi_status status;
	int i;
	const struct hwmon_channel_info **chip_info_array;

	/* Check for WMI interfaces that handle fan/thermal management. */
	/*  If present, we yield to the WMI driver to prevent double-reporting. */
	if (wmi_has_guid(LENOVO_WMI_OTHER_MODE_GUID) ||
	    wmi_has_guid(LENOVO_CAPABILITY_DATA_00_GUID) ||
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

	/* Calculate Slew Rate based on time-to-max-RPM physics */
	data->internal_max_slew_rpm_s = data->device_max_rpm / (cfg->slew_time_s ?: 1);

	/* * Log physical parameters for safety traceability (IEC 61508):
	 * Provides a deterministic baseline for the RLLag filter verification.
	 */
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

	/* * 3. HWMON Configuration:
	 * Dynamically build the HWMON channel configuration based on the
	 * number of fans actually discovered. We allocate one extra slot
	 * to serve as a null terminator for the HWMON core.
	 */
	fan_config = devm_kcalloc(&pdev->dev, data->fan_count + 1, sizeof(u32), GFP_KERNEL);
	if (!fan_config)
		return -ENOMEM;

	for (i = 0; i < data->fan_count; i++)
		fan_config[i] = HWMON_F_INPUT | HWMON_F_MAX;

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->type = hwmon_fan;
	info->config = fan_config;

	/* 4. Wrap it in chip_info for registration */
	chip_info = devm_kzalloc(&pdev->dev, sizeof(*chip_info), GFP_KERNEL);
	if (!chip_info)
		return -ENOMEM;

	chip_info->ops = &yoga_fan_hwmon_ops;

	chip_info_array = devm_kcalloc(&pdev->dev, 2, sizeof(*chip_info_array), GFP_KERNEL);
	if (!chip_info_array)
		return -ENOMEM;

	chip_info_array[0] = info;
	chip_info_array[1] = NULL; /* Null terminated */

	chip_info->info = chip_info_array;

	/* 5. Finalize registration with the accurate hardware description */
	return PTR_ERR_OR_ZERO(devm_hwmon_device_register_with_info(&pdev->dev,
				DRVNAME, data, chip_info, NULL));
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
