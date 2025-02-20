/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * LP5812 Common Driver Header
 *
 * Copyright (C) 2025 Texas Instruments
 *
 * Author: Jared Zhou <jared-zhou@ti.com>
 */

#ifndef _LEDS_LP5812_COMMON_H_
#define _LEDS_LP5812_COMMON_H_

#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/leds.h>
#include <linux/delay.h>

#include "leds-lp5812-regs.h"

#define LED0        "led_0"
#define LED1        "led_1"
#define LED2        "led_2"
#define LED3        "led_3"
#define LED_A0      "led_A0"
#define LED_A1      "led_A1"
#define LED_A2      "led_A2"
#define LED_B0      "led_B0"
#define LED_B1      "led_B1"
#define LED_B2      "led_B2"
#define LED_C0      "led_C0"
#define LED_C1      "led_C1"
#define LED_C2      "led_C2"
#define LED_D0      "led_D0"
#define LED_D1      "led_D1"
#define LED_D2      "led_D2"

/* Below define time for (start/stop/slope time) */
#define TIME0       "no time"
#define TIME1       "0.09s"
#define TIME2       "0.18s"
#define TIME3       "0.36s"
#define TIME4       "0.54s"
#define TIME5       "0.80s"
#define TIME6       "1.07s"
#define TIME7       "1.52s"
#define TIME8       "2.06s"
#define TIME9       "2.50s"
#define TIME10      "3.04s"
#define TIME11      "4.02s"
#define TIME12      "5.01s"
#define TIME13      "5.99s"
#define TIME14      "7.06s"
#define TIME15      "8.05s"
/* End define time for (start/stop/slope time) */

#define AEU1        "AEU1"
#define AEU2        "AEU2"
#define AEU3        "AEU3"

#define MAX_LEDS    16
#define MAX_TIME    16
#define MAX_AEU     3

#define LP5812_DEV_ATTR_RW(name)   \
	DEVICE_ATTR_RW(name)
#define LP5812_DEV_ATTR_RO(name)          \
	DEVICE_ATTR_RO(name)
#define LP5812_DEV_ATTR_WO(name)         \
	DEVICE_ATTR_WO(name)

#define LP5812_KOBJ_ATTR(_name, _mode, _show, _store) \
	struct kobj_attribute kobj_attr_##_name = __ATTR(_name, _mode, _show, _store)
#define LP5812_KOBJ_ATTR_RW(name, show, store) \
	LP5812_KOBJ_ATTR(name, 0644, show, store)
#define LP5812_KOBJ_ATTR_RO(name, show) \
	LP5812_KOBJ_ATTR(name, 0444, show, NULL)
#define LP5812_KOBJ_ATTR_WO(name, store) \
	LP5812_KOBJ_ATTR(name, 0200, NULL, store)

enum pwm_slope_time_num {
	PWM1 = 1,
	PWM2,
	PWM3,
	PWM4,
	PWM5,
	SLOPE_T1,
	SLOPE_T2,
	SLOPE_T3,
	SLOPE_T4
};

enum dimming_type {
	ANALOG,
	PWM
};

enum pwm_dimming_scale {
	LINEAR = 0,
	EXPONENTIAL
};

enum control_mode {
	MANUAL = 0,
	AUTONOMOUS
};

enum device_command {
	NONE,
	UPDATE,
	START,
	STOP,
	PAUSE,
	CONTINUE
};

enum animation_addr {
	AUTO_PAUSE = 0,
	AUTO_PLAYBACK,
	AEU1_PWM_1,
	AEU1_PWM_2,
	AEU1_PWM_3,
	AEU1_PWM_4,
	AEU1_PWM_5,
	AEU1_T12,
	AEU1_T34,
	AEU1_PLAYBACK,
	AEU2_PWM_1,
	AEU2_PWM_2,
	AEU2_PWM_3,
	AEU2_PWM_4,
	AEU2_PWM_5,
	AEU2_T12,
	AEU2_T34,
	AEU2_PLAYBACK,
	AEU3_PWM_1,
	AEU3_PWM_2,
	AEU3_PWM_3,
	AEU3_PWM_4,
	AEU3_PWM_5,
	AEU3_T12,
	AEU3_T34,
	AEU3_PLAYBACK
};

enum drive_mode {
	DIRECT_MODE = 0,
	TCM_1_SCAN,
	TCM_2_SCAN,
	TCM_3_SCAN,
	TCM_4_SCAN,
	MIX_1_SCAN,
	MIX_2_SCAN,
	MIX_3_SCAN
};

enum aeu_select {
	ONLY_AEU1,
	AEU1_AEU2,
	AEU1_AEU2_AEU3
};

union time {
	struct {
		u8 first:4;
		u8 second:4;
	} __packed s_time;
	u8 time_val;
}; /* type for start/stop pause time and slope time */

union led_playback {
	struct {
		u8 led_playback_time:4;
		u8 aeu_selection:2;
		u8 reserved:2;
	} __packed s_led_playback;
	u8 led_playback_val;
};

union scan_order {
	struct {
		u8 scan_order_0:2;
		u8 scan_order_1:2;
		u8 scan_order_2:2;
		u8 scan_order_3:2;
	} __packed s_scan_order;
	u8 scan_order_val;
};

union drive_mode_info {
	struct {
		u8 mix_sel_led_0:1;
		u8 mix_sel_led_1:1;
		u8 mix_sel_led_2:1;
		u8 mix_sel_led_3:1;
		u8 led_mode:3;
		u8 pwm_fre:1;
	} __packed s_drive_mode;
	u8 drive_mode_val;
};

struct drive_mode_led_map {
	const char *drive_mode;
	const char **led_arr;
};

struct lp5812_specific_regs {
	u16 enable_reg;
	u16 reset_reg;
	u16 update_cmd_reg;
	u16 start_cmd_reg;
	u16 stop_cmd_reg;
	u16 pause_cmd_reg;
	u16 continue_cmd_reg;
	u16 fault_clear_reg;
	u16 tsd_config_status_reg;
};

struct anim_engine_unit {
	struct kobject                        kobj;
	struct lp5812_led                     *led;
	struct attribute_group                attr_group;
	const char                            *aeu_name;
	int                                   aeu_number; /* start from 1 */

	/* To know led using this AEU or not*/
	int                                   enabled;
};

struct lp5812_led {
	struct kobject                        kobj;
	struct lp5812_chip                    *priv;
	struct attribute_group                attr_group;
	int                                   enable;
	enum control_mode                     mode;
	enum dimming_type                     dimming_type;
	u8                                    lod_lsd;
	u8                                    auto_pwm;
	u8                                    aep_status;
	u16                                   anim_base_addr;
	int                                   led_number; /* start from 0 */
	int                                   is_sysfs_created;
	const char                            *led_name;

	union led_playback                    led_playback;
	union time                            start_stop_pause_time;

	int                                   total_aeu;
	struct anim_engine_unit               aeu[MAX_AEU];
};

struct lp5812_chip {
	struct i2c_client                     *i2c_cl;
	struct mutex                          lock; /* Protects access to device registers */
	struct device                         *dev;
	struct attribute_group                attr_group;
	const struct lp5812_specific_regs     *regs;
	const struct drive_mode_led_map       *chip_leds_map;
	enum device_command                   command;
	int                                   total_leds;
	union scan_order                      u_scan_order;
	union drive_mode_info                 u_drive_mode;

	struct lp5812_led                     leds[MAX_LEDS]; /* MAX 16 LEDs */
};

int lp5812_write(struct lp5812_chip *chip, u16 reg, u8 val);
int lp5812_read(struct lp5812_chip *chip, u16 reg, u8 *val);
int lp5812_update_bit(struct lp5812_chip *chip, u16 reg, u8 mask, u8 val);

int lp5812_read_tsd_config_status(struct lp5812_chip *chip, u8 *reg_val);
int lp5812_read_lod_status(struct lp5812_chip *chip, int led_number, u8 *val);
int lp5812_read_lsd_status(struct lp5812_chip *chip, int led_number, u8 *val);
int lp5812_read_auto_pwm_value(struct lp5812_chip *chip, int led_number, u8 *val);
int lp5812_read_aep_status(struct lp5812_chip *chip, int led_number, u8 *val);
int lp5812_update_regs_config(struct lp5812_chip *chip);
int lp5812_enable_disable(struct lp5812_chip *chip, int enable);
int lp5812_reset(struct lp5812_chip *chip);
int lp5812_fault_clear(struct lp5812_chip *chip, u8 value);
int lp5812_device_command(struct lp5812_chip *chip, enum device_command command);
void lp5812_dump_regs(struct lp5812_chip *chip, u16 from_reg, u16 to_reg);
int lp5812_set_led_mode(struct lp5812_chip *chip, int led_number,
		enum control_mode mode);
int lp5812_get_led_mode(struct lp5812_chip *chip,
		int led_number, enum control_mode *mode);
int lp5812_set_pwm_dimming_scale(struct lp5812_chip *chip, int led_number,
		enum pwm_dimming_scale scale);
int lp5812_get_pwm_dimming_scale(struct lp5812_chip *chip,
		int led_number, enum pwm_dimming_scale *scale);
int lp5812_manual_dc_pwm_control(struct lp5812_chip *chip,
		int led_number, u8 val, enum dimming_type dimming_type);
int lp5812_manual_dc_pwm_read(struct lp5812_chip *chip,
		int led_number, u8 *val, enum dimming_type dimming_type);
int lp5812_autonomous_dc_pwm_control(struct lp5812_chip *chip,
		int led_number, u8 val, enum dimming_type dimming_type);
int lp5812_autonomous_dc_pwm_read(struct lp5812_chip *chip,
		int led_number, u8 *val, enum dimming_type dimming_type);
int lp5812_disable_all_leds(struct lp5812_chip *chip);
int lp5812_set_drive_mode_scan_order(struct lp5812_chip *chip);
int lp5812_get_drive_mode_scan_order(struct lp5812_chip *chip);
int lp5812_get_phase_align(struct lp5812_chip *chip, int led_number,
		int *phase_align_val);
int lp5812_set_phase_align(struct lp5812_chip *chip, int led_number,
		int phase_align_val);
int lp5812_initialize(struct lp5812_chip *chip);

int led_set_autonomous_animation_config(struct lp5812_led *led);
int led_get_autonomous_animation_config(struct lp5812_led *led);

int led_aeu_pwm_set_val(struct anim_engine_unit *aeu, u8 val,
		enum pwm_slope_time_num pwm_num);
int led_aeu_pwm_get_val(struct anim_engine_unit *aeu, u8 *val,
		enum pwm_slope_time_num pwm_num);
int led_aeu_slope_time_set_val(struct anim_engine_unit *aeu, u8 val,
		enum pwm_slope_time_num slope_time_num);
int led_aeu_slope_time_get_val(struct anim_engine_unit *aeu, u8 *val,
		enum pwm_slope_time_num slope_time_num);
int led_aeu_playback_time_set_val(struct anim_engine_unit *aeu, u8 val);
int led_aeu_playback_time_get_val(struct anim_engine_unit *aeu, u8 *val);

#endif /*_LEDS_LP5812_COMMON_H_*/
