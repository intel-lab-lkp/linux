/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: function.h
 *
 * Abstract: define time and power state struct
 *
 * Version: 1.00
 *
 * Environment:	OS Independent
 */

#include "basic.h"
#ifndef _FUNCTION_H
#define _FUNCTION_H
typedef struct {
	u32 auto_dmt_tick;
	u32 auto_cmd12_tick;
	u32 auto_poweroff_tick;
	u32 auto_led_off_tick;

	u32 last_tick;

	/*
	 * Below 3 variable shows the value for auto dmt time; million second
	 */
	u32 auto_dmt_time;
	u32 auto_cmd12_time;
	u32 auto_poweroff_time;
	u32 auto_led_off_time;

	bool auto_dmt_enable;
	bool auto_cmd12_enable;
	bool auto_poweroff_enable;
	bool auto_led_off_enable;

	bool enable_hibernate;

	bool stop;
	bool enable;
	bool cancel;

	bool s3reusme_cardchg_issuefix_en;
	u8 s3reusme_timer_expect_cnt;
	u8 s3reusme_timer_actual_cnt;
	bool s3s4_start_timer;
} time_struct;

typedef struct {
	bool s3s4_entered;
	bool rtd3_entered;
	bool s5_entered;
	bool warm_boot_entered;
	bool rtd3_en;
	bool d3_silc_en;
	bool d3_silc_submode2_en;

	u32 reg_0x304;
	/* PartA */
	u32 reg_0xdc;
	u32 reg_0x3e0;
	u32 reg_0x3e8;
	u32 reg_0x3ec;
	u32 reg_0xf4;
	u32 reg_0x74;
	u32 reg_0xf0;
	u32 reg_0x90;
	u32 reg_0x3e4;
	/* PartB */
	u32 reg_0x64;
	u32 reg_0xec;
	u32 reg_0xd4;
	u32 reg_0x328;
	u32 reg_0x300;
	u32 reg_0x68;
	u32 reg_0x350;
	u32 reg_0x35c;
	u32 reg_0x334;
	u32 reg_0xd8;
	u32 reg_0x3f0;
	u32 reg_0x88;
	u32 reg_0x33c;
	u32 reg_0xe0;
	u32 reg_0xfc;
} pm_state_t;

typedef enum {
	THERMAL_COOL = 0x00,
	THERMAL_HOT = 0x01,
	THERMAL_NORMAL = 0x02
} e_thermal_val;

#endif
