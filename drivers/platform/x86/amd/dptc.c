// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD Dynamic Power and Thermal Configuration Interface (DPTCi) driver
 *
 * Implements AGESA ALIB Function 0x0C via the firmware-attributes sysfs ABI.
 * Allows userspace to stage and atomically commit APU power/thermal parameters.
 *
 * Reference: AMD AGESA Publication #44065, Appendix E.5
 *
 * Copyright (C) 2025 Antheas Kapenekakis <lkml@antheas.dev>
 */

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/processor.h>
#include <linux/suspend.h>
#include <linux/sysfs.h>

#include "../firmware_attributes_class.h"

#define DRIVER_NAME		"amd_dptc"

/* AGESA ALIB Function 0x0C - Dynamic Power and Thermal Configuration */
#define ALIB_FUNC_DPTC		0x0C
#define ALIB_PATH		"\\_SB.ALIB"

/* ALIB parameter IDs (AGESA spec Appendix E.5, Table E-52) */
#define ALIB_ID_STAPM_TIME	0x01
#define ALIB_ID_TEMP_TARGET	0x03
#define ALIB_ID_STAPM_LIMIT	0x05
#define ALIB_ID_FAST_LIMIT	0x06
#define ALIB_ID_SLOW_LIMIT	0x07
#define ALIB_ID_SLOW_TIME	0x08
#define ALIB_ID_SKIN_LIMIT	0x2E

MODULE_AUTHOR("Antheas Kapenekakis <lkml@antheas.dev>");
MODULE_DESCRIPTION("AMD DPTCi (ALIB Function 0x0C) firmware attributes driver");
MODULE_LICENSE("GPL");

#ifdef CONFIG_AMD_DPTC_EXTENDED
static char *limit_mode = "unbound";
#else
static char *limit_mode = "device";
#endif
#ifdef CONFIG_AMD_DPTC_EXTENDED
#define DPTC_LIMIT_MODE_DESC "Maximum limit tier to expose: device, expanded, soc, unbound"
#else
#define DPTC_LIMIT_MODE_DESC "Maximum limit tier to expose: device, expanded"
#endif
module_param(limit_mode, charp, 0444);
MODULE_PARM_DESC(limit_mode, DPTC_LIMIT_MODE_DESC);

/* =========================================================================
 * Enums
 * =========================================================================
 */

enum dptc_param_idx {
	DPTC_STAPM_LIMIT,
	DPTC_FAST_LIMIT,
	DPTC_SLOW_LIMIT,
	DPTC_SKIN_LIMIT,
	DPTC_SLOW_TIME,
	DPTC_STAPM_TIME,
	DPTC_TEMP_TARGET,
	DPTC_NUM_PARAMS,
};

enum dptc_limit_mode {
	LIMIT_DEVICE,	/* smin..smax: safe operating range for this device */
	LIMIT_EXPANDED,	/* min..max:   full hardware range for this device */
	LIMIT_SOC,	/* APU hardware limits from ALIB_PARAMS */
	LIMIT_UNBOUND,	/* no firmware-side limits enforced */
};

/* =========================================================================
 * Data structures
 * =========================================================================
 */

/*
 * Per-parameter limits for DMI-matched devices.
 * TDP params (stapm/fast/slow/skin) in milliwatts (watts * 1000).
 * Time params in seconds, temp in degrees Celsius.
 */
struct dptc_param_limits {
	u32 min;	/* expanded floor:   widest safe hardware minimum */
	u32 smin;	/* device floor:     safe operating minimum */
	u32 def;	/* default hint for userspace */
	u32 smax;	/* device ceiling:   safe operating maximum */
	u32 max;	/* expanded ceiling: widest safe hardware maximum */
};

struct dptc_device_limits {
	struct dptc_param_limits p[DPTC_NUM_PARAMS];
};

/* Per-parameter limits from ALIB_PARAMS - the APU's own hardware envelope */
struct dptc_soc_range {
	u32 min;
	u32 max;
};

struct dptc_soc_limits {
	struct dptc_soc_range p[DPTC_NUM_PARAMS];
};

struct dptc_param_desc {
	const char *name;
	const char *display_name;
	u8 param_id;
};

struct dptc_soc_entry {
	const char *cpu_id;		/* substring of x86_model_id */
	const struct dptc_soc_limits *limits;
};

/* =========================================================================
 * Parameter descriptor table
 * =========================================================================
 */

static const struct dptc_param_desc dptc_params[DPTC_NUM_PARAMS] = {
	[DPTC_STAPM_LIMIT] = { "stapm_limit", "Sustained TDP (mW)",
		ALIB_ID_STAPM_LIMIT },
	[DPTC_FAST_LIMIT]  = { "fast_limit",  "Fast PPT limit (mW)",
		ALIB_ID_FAST_LIMIT  },
	[DPTC_SLOW_LIMIT]  = { "slow_limit",  "Slow PPT limit (mW)",
		ALIB_ID_SLOW_LIMIT  },
	[DPTC_SKIN_LIMIT]  = { "skin_limit",  "Skin temperature TDP limit (mW)",
		ALIB_ID_SKIN_LIMIT  },
	[DPTC_SLOW_TIME]   = { "slow_time",   "Slow PPT time constant (s)",
		ALIB_ID_SLOW_TIME   },
	[DPTC_STAPM_TIME]  = { "stapm_time",  "STAPM time constant (s)",
		ALIB_ID_STAPM_TIME  },
	[DPTC_TEMP_TARGET] = { "temp_target", "Thermal control limit (C)",
		ALIB_ID_TEMP_TARGET },
};

/* =========================================================================
 * Device limit classes TDP values multiplied by 1000 (milliwatts).
 * =========================================================================
 */

/* 18W class: AYANEO AIR Plus (Ryzen 5 5560U) */
static const struct dptc_device_limits limits_18w = { .p = {
	[DPTC_STAPM_LIMIT] = {     0,  5000, 15000, 18000, 22000 },
	[DPTC_FAST_LIMIT]  = {     0,  5000, 15000, 20000, 25000 },
	[DPTC_SLOW_LIMIT]  = {     0,  5000, 15000, 18000, 22000 },
	[DPTC_SKIN_LIMIT]  = {     0,  5000, 15000, 18000, 22000 },
	[DPTC_SLOW_TIME]   = {     5,     5,    10,    10,    10 },
	[DPTC_STAPM_TIME]  = {   100,   100,   100,   200,   200 },
	[DPTC_TEMP_TARGET] = {    60,    70,    85,    90,   100 },
}};

/* 25W class: Ryzen 5000 handhelds (AYANEO NEXT, KUN) */
static const struct dptc_device_limits limits_25w = { .p = {
	[DPTC_STAPM_LIMIT] = {     0,  4000, 15000, 25000, 32000 },
	[DPTC_FAST_LIMIT]  = {     0,  4000, 25000, 30000, 37000 },
	[DPTC_SLOW_LIMIT]  = {     0,  4000, 20000, 27000, 35000 },
	[DPTC_SKIN_LIMIT]  = {     0,  4000, 15000, 25000, 32000 },
	[DPTC_SLOW_TIME]   = {     5,     5,    10,    10,    10 },
	[DPTC_STAPM_TIME]  = {   100,   100,   100,   200,   200 },
	[DPTC_TEMP_TARGET] = {    60,    70,    85,    90,   100 },
}};

/* 28W class: GPD Win series, AYANEO 2, OrangePi NEO-01 */
static const struct dptc_device_limits limits_28w = { .p = {
	[DPTC_STAPM_LIMIT] = {     0,  4000, 15000, 28000, 32000 },
	[DPTC_FAST_LIMIT]  = {     0,  4000, 25000, 32000, 37000 },
	[DPTC_SLOW_LIMIT]  = {     0,  4000, 20000, 30000, 35000 },
	[DPTC_SKIN_LIMIT]  = {     0,  4000, 15000, 28000, 32000 },
	[DPTC_SLOW_TIME]   = {     5,     5,    10,    10,    10 },
	[DPTC_STAPM_TIME]  = {   100,   100,   100,   200,   200 },
	[DPTC_TEMP_TARGET] = {    60,    70,    85,    90,   100 },
}};

/* 30W class: OneXPlayer, AYANEO AIR/FLIP/GEEK/SLIDE/3, AOKZOE */
static const struct dptc_device_limits limits_30w = { .p = {
	[DPTC_STAPM_LIMIT] = {     0,  4000, 15000, 30000, 40000 },
	[DPTC_FAST_LIMIT]  = {     0,  4000, 25000, 41000, 50000 },
	[DPTC_SLOW_LIMIT]  = {     0,  4000, 20000, 32000, 43000 },
	[DPTC_SKIN_LIMIT]  = {     0,  4000, 15000, 30000, 40000 },
	[DPTC_SLOW_TIME]   = {     5,     5,    10,    10,    10 },
	[DPTC_STAPM_TIME]  = {   100,   100,   100,   200,   200 },
	[DPTC_TEMP_TARGET] = {    60,    70,    85,    90,   100 },
}};

/* Win 5 class: GPD Win 5 */
static const struct dptc_device_limits limits_win5 = { .p = {
	[DPTC_STAPM_LIMIT] = {      0,  4000, 25000,  85000, 100000 },
	[DPTC_FAST_LIMIT]  = {      0,  4000, 40000,  85000, 100000 },
	[DPTC_SLOW_LIMIT]  = {      0,  4000, 27000,  85000, 100000 },
	[DPTC_SKIN_LIMIT]  = {      0,  4000, 25000,  85000, 100000 },
	[DPTC_SLOW_TIME]   = {      5,     5,    10,     10,     10 },
	[DPTC_STAPM_TIME]  = {    100,   100,   100,    200,    200 },
	[DPTC_TEMP_TARGET] = {     60,    70,    95,     95,    100 },
}};

/* =========================================================================
 * SoC limit classes
 * =========================================================================
 */

/* Standard SoC: Ryzen 5000 through 8000, Z1, AI 9 HX 370 */
static const struct dptc_soc_limits soc_standard = { .p = {
	[DPTC_STAPM_LIMIT] = {  0,  54000 },
	[DPTC_FAST_LIMIT]  = {  0,  54000 },
	[DPTC_SLOW_LIMIT]  = {  0,  54000 },
	[DPTC_SKIN_LIMIT]  = {  0,  54000 },
	[DPTC_SLOW_TIME]   = {  0,     30 },
	[DPTC_STAPM_TIME]  = {  0,    300 },
	[DPTC_TEMP_TARGET] = {  0,    105 },
}};

/* AI MAX SoC: Ryzen AI MAX series */
static const struct dptc_soc_limits soc_aimax = { .p = {
	[DPTC_STAPM_LIMIT] = {  0, 120000 },
	[DPTC_FAST_LIMIT]  = {  0, 120000 },
	[DPTC_SLOW_LIMIT]  = {  0, 120000 },
	[DPTC_SKIN_LIMIT]  = {  0, 120000 },
	[DPTC_SLOW_TIME]   = {  0,     30 },
	[DPTC_STAPM_TIME]  = {  0,    300 },
	[DPTC_TEMP_TARGET] = {  0,    105 },
}};

/* =========================================================================
 * SoC CPU table
 * Substring matches against boot_cpu_data.x86_model_id.
 * Order matters: more specific strings before broader ones.
 * =========================================================================
 */

static const struct dptc_soc_entry dptc_soc_table[] = {
	/* AI MAX - must precede "AMD Ryzen AI"; 395 before 385 to avoid short match */
	{ "AMD RYZEN AI MAX+ 395",	&soc_aimax    },
	{ "AMD RYZEN AI MAX+ 385",	&soc_aimax    },
	{ "AMD RYZEN AI MAX 380",	&soc_aimax    },
	/* Ryzen AI */
	{ "AMD Ryzen AI 9 HX 370",	&soc_standard },
	{ "AMD Ryzen AI HX 360",	&soc_standard },
	/* Z1 - Extreme before plain Z1 */
	{ "AMD Ryzen Z1 Extreme",	&soc_standard },
	{ "AMD Ryzen Z1",		&soc_standard },
	/* Ryzen 8000 */
	{ "AMD Ryzen 7 8840U",		&soc_standard },
	/* Ryzen 7040 */
	{ "AMD Ryzen 7 7840U",		&soc_standard },
	/* Ryzen 6000 */
	{ "AMD Ryzen 7 6800U",		&soc_standard },
	{ "AMD Ryzen 7 6600U",		&soc_standard },
	/* Ryzen 5000 */
	{ "AMD Ryzen 7 5800U",		&soc_standard },
	{ "AMD Ryzen 7 5700U",		&soc_standard },
	{ "AMD Ryzen 5 5560U",		&soc_standard },
	{ }
};

/* =========================================================================
 * DMI device table
 * Excluded: ASUS ROG, Lenovo Legion devices.
 * =========================================================================
 */

static const struct dmi_system_id dptc_dmi_table[] = {
	/* --- GPD (DMI_SYS_VENDOR + DMI_PRODUCT_NAME) --- */
	{
		.ident = "GPD Win Mini",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1617-01"),
		},
		.driver_data = (void *)&limits_28w,
	},
	{
		.ident = "GPD Win Mini 2024",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1617-02"),
		},
		.driver_data = (void *)&limits_28w,
	},
	{
		.ident = "GPD Win Mini 2024",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1617-02-L"),
		},
		.driver_data = (void *)&limits_28w,
	},
	{
		.ident = "GPD Win 4",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1618-04"),
		},
		.driver_data = (void *)&limits_28w,
	},
	{
		.ident = "GPD Win 5",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1618-05"),
		},
		.driver_data = (void *)&limits_win5,
	},
	{
		.ident = "GPD Win Max 2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1619-04"),
		},
		.driver_data = (void *)&limits_28w,
	},
	{
		.ident = "GPD Win Max 2 2024",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1619-05"),
		},
		.driver_data = (void *)&limits_28w,
	},
	{
		.ident = "GPD Duo",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1622-01"),
		},
		.driver_data = (void *)&limits_28w,
	},
	{
		.ident = "GPD Duo",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1622-01-L"),
		},
		.driver_data = (void *)&limits_28w,
	},
	{
		.ident = "GPD Pocket 4",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1628-04"),
		},
		.driver_data = (void *)&limits_28w,
	},
	{
		.ident = "GPD Pocket 4",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1628-04-L"),
		},
		.driver_data = (void *)&limits_28w,
	},
	/* --- OrangePi (DMI_BOARD_VENDOR + DMI_BOARD_NAME) --- */
	{
		.ident = "OrangePi NEO-01",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "OrangePi"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "NEO-01"),
		},
		.driver_data = (void *)&limits_28w,
	},
	/* --- AOKZOE (DMI_BOARD_VENDOR + DMI_BOARD_NAME) --- */
	{
		.ident = "AOKZOE A1 AR07",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AOKZOE"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "AOKZOE A1 AR07"),
		},
		.driver_data = (void *)&limits_30w,
	},
	{
		.ident = "AOKZOE A1 Pro",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AOKZOE"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "AOKZOE A1 Pro"),
		},
		.driver_data = (void *)&limits_30w,
	},
	{
		.ident = "AOKZOE A1X",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AOKZOE"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "AOKZOE A1X"),
		},
		.driver_data = (void *)&limits_30w,
	},
	{
		.ident = "AOKZOE A2 Pro",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AOKZOE"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "AOKZOE A2 Pro"),
		},
		.driver_data = (void *)&limits_30w,
	},
	/* --- OneXPlayer (DMI_BOARD_VENDOR "ONE-NETBOOK" + DMI_BOARD_NAME) ---
	 * AMD-based devices only; Intel variants share board names but we
	 * rely on the SoC table to reject non-AMD CPUs.
	 */
	{
		.ident = "ONEXPLAYER F1Pro",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "ONE-NETBOOK"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "ONEXPLAYER F1Pro"),
		},
		.driver_data = (void *)&limits_30w,
	},
	{
		.ident = "ONEXPLAYER F1 EVA-02",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "ONE-NETBOOK"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "ONEXPLAYER F1 EVA-02"),
		},
		.driver_data = (void *)&limits_30w,
	},
	{
		.ident = "ONEXPLAYER 2",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "ONE-NETBOOK"),
			DMI_MATCH(DMI_BOARD_NAME, "ONEXPLAYER 2"),
		},
		.driver_data = (void *)&limits_30w,
	},
	{
		.ident = "ONEXPLAYER X1 A",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "ONE-NETBOOK"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "ONEXPLAYER X1 A"),
		},
		.driver_data = (void *)&limits_30w,
	},
	{
		.ident = "ONEXPLAYER X1z",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "ONE-NETBOOK"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "ONEXPLAYER X1z"),
		},
		.driver_data = (void *)&limits_30w,
	},
	{
		.ident = "ONEXPLAYER X1Pro",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "ONE-NETBOOK"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "ONEXPLAYER X1Pro"),
		},
		.driver_data = (void *)&limits_30w,
	},
	{
		.ident = "ONEXPLAYER G1 A",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "ONE-NETBOOK"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "ONEXPLAYER G1 A"),
		},
		.driver_data = (void *)&limits_30w,
	},
	/* --- AYANEO (DMI_BOARD_VENDOR + DMI_BOARD_NAME) --- */
	/* 18W */
	{
		.ident = "AYANEO AIR Plus",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "AIR Plus"),
		},
		.driver_data = (void *)&limits_18w,
	},
	/* 25W - Ryzen 5000 */
	{
		.ident = "AYANEO NEXT Advance",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "NEXT Advance"),
		},
		.driver_data = (void *)&limits_25w,
	},
	{
		.ident = "AYANEO NEXT Lite",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "NEXT Lite"),
		},
		.driver_data = (void *)&limits_25w,
	},
	{
		.ident = "AYANEO NEXT Pro",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "NEXT Pro"),
		},
		.driver_data = (void *)&limits_25w,
	},
	{
		.ident = "AYANEO NEXT",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "NEXT"),
		},
		.driver_data = (void *)&limits_25w,
	},
	{
		.ident = "AYANEO KUN",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "KUN"),
		},
		.driver_data = (void *)&limits_25w,
	},
	{
		.ident = "AYANEO KUN",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "AYANEO KUN"),
		},
		.driver_data = (void *)&limits_25w,
	},
	/* 28W - Ryzen 6000 */
	{
		.ident = "AYANEO 2",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_MATCH(DMI_BOARD_NAME, "AYANEO 2"),
		},
		.driver_data = (void *)&limits_28w,
	},
	{
		.ident = "SuiPlay0X1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Mysten Labs, Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "SuiPlay0X1"),
		},
		.driver_data = (void *)&limits_28w,
	},
	/* 30W - Ryzen 7040 / Z1 */
	{
		/* Must come before the shorter "AIR" match */
		.ident = "AYANEO AIR 1S",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_MATCH(DMI_BOARD_NAME, "AIR 1S"),
		},
		.driver_data = (void *)&limits_30w,
	},
	{
		.ident = "AYANEO AIR Pro",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "AIR Pro"),
		},
		.driver_data = (void *)&limits_30w,
	},
	{
		.ident = "AYANEO AIR",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "AIR"),
		},
		.driver_data = (void *)&limits_30w,
	},
	{
		/* DMI_MATCH catches all FLIP variants (DS, KB, 1S DS, 1S KB) */
		.ident = "AYANEO FLIP",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_MATCH(DMI_BOARD_NAME, "FLIP"),
		},
		.driver_data = (void *)&limits_30w,
	},
	{
		/* DMI_MATCH catches GEEK and GEEK 1S */
		.ident = "AYANEO GEEK",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_MATCH(DMI_BOARD_NAME, "GEEK"),
		},
		.driver_data = (void *)&limits_30w,
	},
	{
		.ident = "AYANEO SLIDE",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "SLIDE"),
		},
		.driver_data = (void *)&limits_30w,
	},
	{
		.ident = "AYANEO 3",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "AYANEO 3"),
		},
		.driver_data = (void *)&limits_30w,
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, dptc_dmi_table);

/* =========================================================================
 * Per-parameter sysfs state
 * =========================================================================
 */

struct dptc_param_sysfs {
	struct kobj_attribute current_value;
	struct kobj_attribute default_value;
	struct kobj_attribute min_value;
	struct kobj_attribute max_value;
	struct kobj_attribute scalar_increment;
	struct kobj_attribute display_name;
	struct kobj_attribute type;
	struct attribute *attrs[8]; /* 7 attrs + NULL */
	struct attribute_group group;
	int idx;
};

struct dptc_mode_sysfs {
	struct kobj_attribute current_value;
	struct kobj_attribute possible_values;
	struct kobj_attribute default_value;
	struct kobj_attribute display_name;
	struct kobj_attribute type;
	struct attribute *attrs[6]; /* 5 attrs + NULL */
	struct attribute_group group;
};

/* =========================================================================
 * Driver private state
 * =========================================================================
 */

struct dptc_priv {
	struct device *fw_attr_dev;
	struct kset *fw_attr_kset;

	const struct dptc_device_limits *dev_limits;	/* NULL if no DMI match */
	const struct dptc_soc_limits *soc_limits;	/* NULL if SoC unrecognized */

	enum dptc_limit_mode active_mode;
	enum dptc_limit_mode max_mode;

	enum dptc_commit_mode { COMMIT_AUTO, COMMIT_MANUAL } commit_mode;

	u32 staged[DPTC_NUM_PARAMS];
	bool has_staged[DPTC_NUM_PARAMS];

	/* Protects staged values and has_staged flags */
	struct mutex lock;

	struct dptc_param_sysfs params[DPTC_NUM_PARAMS];
	struct dptc_mode_sysfs mode_attr;
	struct kobj_attribute save_settings_attr;
};

static struct dptc_priv *dptc;

/* =========================================================================
 * Limit accessors
 * =========================================================================
 */

static u32 dptc_get_min(int idx)
{
	switch (dptc->active_mode) {
	case LIMIT_DEVICE:   return dptc->dev_limits->p[idx].smin;
	case LIMIT_EXPANDED: return dptc->dev_limits->p[idx].min;
	case LIMIT_SOC:      return dptc->soc_limits->p[idx].min;
	case LIMIT_UNBOUND:  return 0;
	}
	return 0;
}

static u32 dptc_get_max(int idx)
{
	switch (dptc->active_mode) {
	case LIMIT_DEVICE:   return dptc->dev_limits->p[idx].smax;
	case LIMIT_EXPANDED: return dptc->dev_limits->p[idx].max;
	case LIMIT_SOC:      return dptc->soc_limits->p[idx].max;
	case LIMIT_UNBOUND:  return U32_MAX;
	}
	return 0;
}

/* Default hint comes from the device DMI table; 0 if no DMI match */
static u32 dptc_get_default(int idx)
{
	if (dptc->dev_limits)
		return dptc->dev_limits->p[idx].def;
	return 0;
}

/* =========================================================================
 * ALIB call
 * =========================================================================
 */

static int dptc_alib_commit(void)
{
	union acpi_object in_params[2];
	struct acpi_object_list input;
	u32 vals[DPTC_NUM_PARAMS];
	u8 ids[DPTC_NUM_PARAMS];
	acpi_status status;
	int i, off;
	int count = 0;
	u32 buf_size;
	u8 *buf;

	for (i = 0; i < DPTC_NUM_PARAMS; i++) {
		if (!dptc->has_staged[i])
			continue;
		ids[count]  = dptc_params[i].param_id;
		vals[count] = dptc->staged[i];
		count++;
	}

	if (count == 0)
		return -EINVAL;

	/* Buffer layout: WORD total_size + count * (BYTE id + DWORD value) */
	buf_size = 2 + count * 5;
	buf = kzalloc(buf_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = buf_size & 0xff;
	buf[1] = (buf_size >> 8) & 0xff;

	for (i = 0; i < count; i++) {
		off  = 2 + i * 5;
		buf[off]     = ids[i];
		buf[off + 1] = vals[i] & 0xff;
		buf[off + 2] = (vals[i] >>  8) & 0xff;
		buf[off + 3] = (vals[i] >> 16) & 0xff;
		buf[off + 4] = (vals[i] >> 24) & 0xff;
	}

	in_params[0].type          = ACPI_TYPE_INTEGER;
	in_params[0].integer.value = ALIB_FUNC_DPTC;
	in_params[1].type          = ACPI_TYPE_BUFFER;
	in_params[1].buffer.length  = buf_size;
	in_params[1].buffer.pointer = buf;

	input.count   = 2;
	input.pointer = in_params;

	status = acpi_evaluate_object(NULL, ALIB_PATH, &input, NULL);
	kfree(buf);

	if (ACPI_FAILURE(status)) {
		pr_err(DRIVER_NAME ": ALIB call failed: %s\n",
		       acpi_format_exception(status));
		return -EIO;
	}

	pr_debug(DRIVER_NAME ": committed %d parameter(s)\n", count);
	return 0;
}

/* =========================================================================
 * Sysfs callbacks - per-parameter attributes
 * =========================================================================
 */

static ssize_t dptc_current_value_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	struct dptc_param_sysfs *ps =
		container_of(attr, struct dptc_param_sysfs, current_value);

	if (!dptc->has_staged[ps->idx])
		return sysfs_emit(buf, "\n");
	return sysfs_emit(buf, "%u\n", dptc->staged[ps->idx]);
}

static ssize_t dptc_current_value_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	struct dptc_param_sysfs *ps =
		container_of(attr, struct dptc_param_sysfs, current_value);
	u32 val, min, max;
	int ret;

	/* Empty write clears the staged value */
	if (count == 0 || (count == 1 && buf[0] == '\n')) {
		mutex_lock(&dptc->lock);
		dptc->has_staged[ps->idx] = false;
		mutex_unlock(&dptc->lock);
		return count;
	}

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return ret;

	mutex_lock(&dptc->lock);
	min = dptc_get_min(ps->idx);
	max = dptc_get_max(ps->idx);
	if (val < min || (max != U32_MAX && val > max)) {
		mutex_unlock(&dptc->lock);
		return -EINVAL;
	}
	dptc->staged[ps->idx]    = val;
	dptc->has_staged[ps->idx] = true;
	if (dptc->commit_mode == COMMIT_AUTO)
		ret = dptc_alib_commit();
	mutex_unlock(&dptc->lock);

	return ret ? ret : count;
}

static ssize_t dptc_default_value_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	struct dptc_param_sysfs *ps =
		container_of(attr, struct dptc_param_sysfs, default_value);
	u32 def = dptc_get_default(ps->idx);

	if (!dptc->dev_limits)
		return sysfs_emit(buf, "\n");
	return sysfs_emit(buf, "%u\n", def);
}

static ssize_t dptc_min_value_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	struct dptc_param_sysfs *ps =
		container_of(attr, struct dptc_param_sysfs, min_value);
	return sysfs_emit(buf, "%u\n", dptc_get_min(ps->idx));
}

static ssize_t dptc_max_value_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	struct dptc_param_sysfs *ps =
		container_of(attr, struct dptc_param_sysfs, max_value);
	u32 max = dptc_get_max(ps->idx);

	if (max == U32_MAX)
		return sysfs_emit(buf, "\n");
	return sysfs_emit(buf, "%u\n", max);
}

static ssize_t dptc_scalar_increment_show(struct kobject *kobj,
					  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "1\n");
}

static ssize_t dptc_display_name_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	struct dptc_param_sysfs *ps =
		container_of(attr, struct dptc_param_sysfs, display_name);
	return sysfs_emit(buf, "%s\n", dptc_params[ps->idx].display_name);
}

static ssize_t dptc_type_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "integer\n");
}

static ssize_t dptc_save_settings_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	if (dptc->commit_mode == COMMIT_AUTO)
		return sysfs_emit(buf, "single\n");
	return sysfs_emit(buf, "bulk\n");
}

static ssize_t dptc_save_settings_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	int ret = 0;

	if (sysfs_streq(buf, "save")) {
		mutex_lock(&dptc->lock);
		ret = dptc_alib_commit();
		mutex_unlock(&dptc->lock);
	} else if (sysfs_streq(buf, "single")) {
		mutex_lock(&dptc->lock);
		dptc->commit_mode = COMMIT_AUTO;
		mutex_unlock(&dptc->lock);
	} else if (sysfs_streq(buf, "bulk")) {
		mutex_lock(&dptc->lock);
		dptc->commit_mode = COMMIT_MANUAL;
		mutex_unlock(&dptc->lock);
	} else {
		return -EINVAL;
	}

	return ret ? ret : count;
}

static const char * const mode_names[] = {
	[LIMIT_DEVICE]   = "device",
	[LIMIT_EXPANDED] = "expanded",
	[LIMIT_SOC]      = "soc",
	[LIMIT_UNBOUND]  = "unbound",
};

static bool dptc_mode_available(enum dptc_limit_mode mode)
{
	if (mode == LIMIT_DEVICE || mode == LIMIT_EXPANDED)
		return dptc->dev_limits;
	if (mode == LIMIT_SOC)
		return dptc->soc_limits;
	return true; /* LIMIT_UNBOUND always available */
}

static ssize_t dptc_mode_current_value_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buf)
{
	return sysfs_emit(buf, "%s\n", mode_names[dptc->active_mode]);
}

static ssize_t dptc_mode_current_value_store(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     const char *buf, size_t count)
{
	enum dptc_limit_mode new_mode;
	int m;

	for (m = 0; m <= LIMIT_UNBOUND; m++) {
		if (sysfs_streq(buf, mode_names[m])) {
			new_mode = m;
			goto found;
		}
	}
	return -EINVAL;

found:
	if (new_mode > dptc->max_mode)
		return -EPERM;
	if (!dptc_mode_available(new_mode))
		return -ENODEV;

	mutex_lock(&dptc->lock);
	dptc->active_mode = new_mode;
	/* Clear staged values: limits changed, old values may be out of range */
	memset(dptc->has_staged, 0, sizeof(dptc->has_staged));
	mutex_unlock(&dptc->lock);

	return count;
}

static ssize_t dptc_mode_possible_values_show(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      char *buf)
{
	char tmp[64];
	char *p = tmp;
	bool first = true;
	int m;

	for (m = 0; m <= (int)dptc->max_mode; m++) {
		if (!dptc_mode_available(m))
			continue;
		if (!first)
			*p++ = ';';
		first = false;
		p += snprintf(p, tmp + sizeof(tmp) - p, "%s", mode_names[m]);
	}
	*p = '\0';

	return sysfs_emit(buf, "%s\n", tmp);
}

static ssize_t dptc_mode_default_value_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buf)
{
	return sysfs_emit(buf, "device\n");
}

static ssize_t dptc_mode_display_name_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buf)
{
	return sysfs_emit(buf, "TDP Limit Mode\n");
}

static ssize_t dptc_mode_type_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "enumeration\n");
}

/* =========================================================================
 * Sysfs setup
 * =========================================================================
 */

static void dptc_setup_param_sysfs(struct dptc_param_sysfs *ps, int idx)
{
	ps->idx = idx;

	sysfs_attr_init(&ps->current_value.attr);
	ps->current_value.attr.name  = "current_value";
	ps->current_value.attr.mode  = 0644;
	ps->current_value.show       = dptc_current_value_show;
	ps->current_value.store      = dptc_current_value_store;

	sysfs_attr_init(&ps->default_value.attr);
	ps->default_value.attr.name  = "default_value";
	ps->default_value.attr.mode  = 0444;
	ps->default_value.show       = dptc_default_value_show;

	sysfs_attr_init(&ps->min_value.attr);
	ps->min_value.attr.name      = "min_value";
	ps->min_value.attr.mode      = 0444;
	ps->min_value.show           = dptc_min_value_show;

	sysfs_attr_init(&ps->max_value.attr);
	ps->max_value.attr.name      = "max_value";
	ps->max_value.attr.mode      = 0444;
	ps->max_value.show           = dptc_max_value_show;

	sysfs_attr_init(&ps->scalar_increment.attr);
	ps->scalar_increment.attr.name = "scalar_increment";
	ps->scalar_increment.attr.mode = 0444;
	ps->scalar_increment.show      = dptc_scalar_increment_show;

	sysfs_attr_init(&ps->display_name.attr);
	ps->display_name.attr.name   = "display_name";
	ps->display_name.attr.mode   = 0444;
	ps->display_name.show        = dptc_display_name_show;

	sysfs_attr_init(&ps->type.attr);
	ps->type.attr.name           = "type";
	ps->type.attr.mode           = 0444;
	ps->type.show                = dptc_type_show;

	ps->attrs[0] = &ps->current_value.attr;
	ps->attrs[1] = &ps->default_value.attr;
	ps->attrs[2] = &ps->min_value.attr;
	ps->attrs[3] = &ps->max_value.attr;
	ps->attrs[4] = &ps->scalar_increment.attr;
	ps->attrs[5] = &ps->display_name.attr;
	ps->attrs[6] = &ps->type.attr;
	ps->attrs[7] = NULL;

	ps->group.name  = dptc_params[idx].name;
	ps->group.attrs = ps->attrs;
}

static void dptc_setup_mode_sysfs(struct dptc_mode_sysfs *ms)
{
	sysfs_attr_init(&ms->current_value.attr);
	ms->current_value.attr.name  = "current_value";
	ms->current_value.attr.mode  = 0644;
	ms->current_value.show       = dptc_mode_current_value_show;
	ms->current_value.store      = dptc_mode_current_value_store;

	sysfs_attr_init(&ms->possible_values.attr);
	ms->possible_values.attr.name = "possible_values";
	ms->possible_values.attr.mode = 0444;
	ms->possible_values.show      = dptc_mode_possible_values_show;

	sysfs_attr_init(&ms->default_value.attr);
	ms->default_value.attr.name  = "default_value";
	ms->default_value.attr.mode  = 0444;
	ms->default_value.show       = dptc_mode_default_value_show;

	sysfs_attr_init(&ms->display_name.attr);
	ms->display_name.attr.name   = "display_name";
	ms->display_name.attr.mode   = 0444;
	ms->display_name.show        = dptc_mode_display_name_show;

	sysfs_attr_init(&ms->type.attr);
	ms->type.attr.name           = "type";
	ms->type.attr.mode           = 0444;
	ms->type.show                = dptc_mode_type_show;

	ms->attrs[0] = &ms->current_value.attr;
	ms->attrs[1] = &ms->possible_values.attr;
	ms->attrs[2] = &ms->default_value.attr;
	ms->attrs[3] = &ms->display_name.attr;
	ms->attrs[4] = &ms->type.attr;
	ms->attrs[5] = NULL;

	ms->group.name  = "limit_mode";
	ms->group.attrs = ms->attrs;
}

/* =========================================================================
 * PM notifier - re-apply staged values after resume
 * =========================================================================
 */

static int dptc_pm_notify(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	if ((action == PM_POST_SUSPEND || action == PM_POST_HIBERNATION) &&
	    dptc->commit_mode == COMMIT_AUTO) {
		mutex_lock(&dptc->lock);
		dptc_alib_commit();
		mutex_unlock(&dptc->lock);
	}
	return NOTIFY_OK;
}

static struct notifier_block dptc_pm_nb = {
	.notifier_call = dptc_pm_notify,
};

/* =========================================================================
 * Module init / exit
 * =========================================================================
 */

static int __init dptc_init(void)
{
	const struct dptc_soc_entry *soc_entry = NULL;
	const struct dmi_system_id *dmi_entry;
	enum dptc_limit_mode max_mode;
	int i, ret;

	/* Check ALIB ACPI method presence */
	if (!acpi_has_method(NULL, ALIB_PATH)) {
		pr_debug(DRIVER_NAME ": ALIB ACPI method not present\n");
		return -ENODEV;
	}

	/* Parse limit_mode module parameter */
	if (!strcmp(limit_mode, "device")) {
		max_mode = LIMIT_DEVICE;
	} else if (!strcmp(limit_mode, "expanded")) {
		max_mode = LIMIT_EXPANDED;
	} else if (!strcmp(limit_mode, "soc")) {
		max_mode = LIMIT_SOC;
	} else if (!strcmp(limit_mode, "unbound")) {
		max_mode = LIMIT_UNBOUND;
	} else {
		pr_err(DRIVER_NAME ": unknown limit_mode '%s'\n", limit_mode);
		return -EINVAL;
	}
#ifndef CONFIG_AMD_DPTC_EXTENDED
	if (max_mode > LIMIT_EXPANDED) {
		pr_err(DRIVER_NAME ": limit_mode '%s' requires CONFIG_AMD_DPTC_EXTENDED\n",
		       limit_mode);
		return -EINVAL;
	}
#endif

	/* SoC match - required for device/expanded/soc, optional for unbound */
	for (i = 0; dptc_soc_table[i].cpu_id; i++) {
		if (strstr(boot_cpu_data.x86_model_id, dptc_soc_table[i].cpu_id)) {
			soc_entry = &dptc_soc_table[i];
			break;
		}
	}
	if (!soc_entry && max_mode < LIMIT_UNBOUND) {
		pr_debug(DRIVER_NAME ": unrecognized SoC '%s'\n",
			 boot_cpu_data.x86_model_id);
		return -ENODEV;
	}

	/* Optional device DMI match */
	dmi_entry = dmi_first_match(dptc_dmi_table);

	/*
	 * device and expanded modes require a DMI match; refuse to load if
	 * the user requested one of those tiers but the device is unknown.
	 */
	if (max_mode <= LIMIT_EXPANDED && !dmi_entry) {
		pr_debug(DRIVER_NAME
			": limit_mode='%s' requires a device DMI match\n",
			limit_mode);
		return -ENODEV;
	}

	dptc = kzalloc(sizeof(*dptc), GFP_KERNEL);
	if (!dptc)
		return -ENOMEM;

	mutex_init(&dptc->lock);
	dptc->soc_limits = soc_entry ? soc_entry->limits : NULL;
	dptc->max_mode   = max_mode;
	if (dmi_entry)
		dptc->dev_limits = dmi_entry->driver_data;

	if (dptc->dev_limits)
		dptc->active_mode = LIMIT_DEVICE;
	else if (dptc->soc_limits)
		dptc->active_mode = LIMIT_SOC;
	else
		dptc->active_mode = LIMIT_UNBOUND;

	/* ---- Probe logging ---- */
	if (soc_entry)
		pr_info(DRIVER_NAME ": SoC: %s\n", soc_entry->cpu_id);
	else
		pr_info(DRIVER_NAME ": SoC unrecognized ('%s'), running unbound\n",
			boot_cpu_data.x86_model_id);

	if (dmi_entry) {
		pr_info(DRIVER_NAME ": Device: %s\n", dmi_entry->ident);
		pr_info(DRIVER_NAME ": Device limits (device mode):\n");
		for (i = 0; i < DPTC_NUM_PARAMS; i++) {
			pr_info(DRIVER_NAME ":   %-16s  smin=%-8u smax=%-8u def=%u\n",
				dptc_params[i].name,
				dptc->dev_limits->p[i].smin,
				dptc->dev_limits->p[i].smax,
				dptc->dev_limits->p[i].def);
		}
		if (max_mode >= LIMIT_EXPANDED) {
			pr_info(DRIVER_NAME ": Device limits (expanded mode):\n");
			for (i = 0; i < DPTC_NUM_PARAMS; i++) {
				pr_info(DRIVER_NAME ":   %-16s  min=%-8u  max=%u\n",
					dptc_params[i].name,
					dptc->dev_limits->p[i].min,
					dptc->dev_limits->p[i].max);
			}
		}
	} else {
		pr_info(DRIVER_NAME ": No device DMI match\n");
	}

	if (max_mode >= LIMIT_SOC && dptc->soc_limits) {
		pr_info(DRIVER_NAME ": SoC limits:\n");
		for (i = 0; i < DPTC_NUM_PARAMS; i++) {
			pr_info(DRIVER_NAME ":   %-16s  max=%u\n",
				dptc_params[i].name,
				dptc->soc_limits->p[i].max);
		}
	}

	pr_info(DRIVER_NAME ": active_mode=%s  max_mode=%s\n",
		mode_names[dptc->active_mode], mode_names[dptc->max_mode]);

	/* ---- Firmware-attributes class device ---- */
	dptc->fw_attr_dev = device_create(&firmware_attributes_class,
					  NULL, MKDEV(0, 0), NULL,
					  DRIVER_NAME);
	if (IS_ERR(dptc->fw_attr_dev)) {
		ret = PTR_ERR(dptc->fw_attr_dev);
		goto err_free;
	}

	dptc->fw_attr_kset = kset_create_and_add("attributes", NULL,
						 &dptc->fw_attr_dev->kobj);
	if (!dptc->fw_attr_kset) {
		ret = -ENOMEM;
		goto err_dev;
	}

	for (i = 0; i < DPTC_NUM_PARAMS; i++) {
		dptc_setup_param_sysfs(&dptc->params[i], i);
		ret = sysfs_create_group(&dptc->fw_attr_kset->kobj,
					 &dptc->params[i].group);
		if (ret) {
			while (--i >= 0)
				sysfs_remove_group(&dptc->fw_attr_kset->kobj,
						   &dptc->params[i].group);
			goto err_kset;
		}
	}

	dptc_setup_mode_sysfs(&dptc->mode_attr);
	ret = sysfs_create_group(&dptc->fw_attr_kset->kobj,
				 &dptc->mode_attr.group);
	if (ret) {
		for (i = 0; i < DPTC_NUM_PARAMS; i++)
			sysfs_remove_group(&dptc->fw_attr_kset->kobj,
					   &dptc->params[i].group);
		goto err_kset;
	}

	sysfs_attr_init(&dptc->save_settings_attr.attr);
	dptc->save_settings_attr.attr.name  = "save_settings";
	dptc->save_settings_attr.attr.mode  = 0644;
	dptc->save_settings_attr.show       = dptc_save_settings_show;
	dptc->save_settings_attr.store      = dptc_save_settings_store;
	ret = sysfs_create_file(&dptc->fw_attr_kset->kobj,
				&dptc->save_settings_attr.attr);
	if (ret) {
		sysfs_remove_group(&dptc->fw_attr_kset->kobj, &dptc->mode_attr.group);
		for (i = 0; i < DPTC_NUM_PARAMS; i++)
			sysfs_remove_group(&dptc->fw_attr_kset->kobj,
					   &dptc->params[i].group);
		goto err_kset;
	}

	register_pm_notifier(&dptc_pm_nb);
	pr_info(DRIVER_NAME ": loaded\n");
	return 0;

err_kset:
	kset_unregister(dptc->fw_attr_kset);
err_dev:
	device_destroy(&firmware_attributes_class, MKDEV(0, 0));
err_free:
	mutex_destroy(&dptc->lock);
	kfree(dptc);
	dptc = NULL;
	return ret;
}

static void __exit dptc_exit(void)
{
	int i;

	unregister_pm_notifier(&dptc_pm_nb);
	sysfs_remove_file(&dptc->fw_attr_kset->kobj, &dptc->save_settings_attr.attr);
	sysfs_remove_group(&dptc->fw_attr_kset->kobj, &dptc->mode_attr.group);
	for (i = 0; i < DPTC_NUM_PARAMS; i++)
		sysfs_remove_group(&dptc->fw_attr_kset->kobj,
				   &dptc->params[i].group);
	kset_unregister(dptc->fw_attr_kset);
	device_destroy(&firmware_attributes_class, MKDEV(0, 0));
	mutex_destroy(&dptc->lock);
	kfree(dptc);
	dptc = NULL;
}

module_init(dptc_init);
module_exit(dptc_exit);
