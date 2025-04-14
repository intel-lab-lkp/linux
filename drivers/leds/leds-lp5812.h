/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * LP5812 Driver Header
 *
 * Copyright (C) 2025 Texas Instruments
 *
 * Author: Jared Zhou <jared-zhou@ti.com>
 */

#ifndef _LEDS_LP5812_H_
#define _LEDS_LP5812_H_

#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/leds.h>
#include <linux/delay.h>

#define CHIP_EN_REG                     0x00

#define DEV_CONFIG0                     0x01
#define DEV_CONFIG1                     0x02
#define DEV_CONFIG2                     0x03
#define DEV_CONFIG3                     0x04
#define DEV_CONFIG4                     0x05
#define DEV_CONFIG5                     0x06
#define DEV_CONFIG6                     0x07
#define DEV_CONFIG7                     0x08
#define DEV_CONFIG8                     0x09
#define DEV_CONFIG9                     0x0A
#define DEV_CONFIG10                    0x0B
#define DEV_CONFIG11                    0x0c
#define DEV_CONFIG12                    0x0D

#define CMD_UPDATE_REG                  0x10
#define CMD_START_REG                   0x11
#define CMD_STOP_REG                    0x12
#define CMD_PAUSE_REG                   0x13
#define CMD_CONTINUE_REG                0x14

#define LED_ENABLE_1_REG                0x20
#define LED_ENABLE_2_REG                0x21

#define FAULT_CLEAR_REG                 0x22
#define RESET_REG                       0x23

#define MANUAL_DC_LED_0_REG             0x30
#define MANUAL_PWM_LED_0_REG            0x40
#define AUTO_DC_LED_0_REG               0x50

/* value for register */
#define UPDATE_CMD_VAL                  0x55
#define START_CMD_VAL                   0xFF
#define STOP_CMD_VAL                    0xAA
#define PAUSE_CMD_VAL                   0x33
#define CONTINUE_CMD_VAL                0xCC

#define CHIP_ENABLE                     0x01
#define CHIP_DISABLE                    0x00

#define FAULT_CLEAR_ALL                 0x07
#define TSD_CLEAR_VAL                   0x04
#define LSD_CLEAR_VAL                   0x02
#define LOD_CLEAR_VAL                   0x01
#define RESET_REG_VAL                   0x66

#define LED0_AUTO_BASE_ADRR             0x80
#define LED1_AUTO_BASE_ADRR             0x9A
#define LED2_AUTO_BASE_ADRR             0xB4
#define LED3_AUTO_BASE_ADRR             0xCE
#define LED_A0_AUTO_BASE_ADRR           0xE8
#define LED_A1_AUTO_BASE_ADRR           0x102
#define LED_A2_AUTO_BASE_ADRR           0x11C
#define LED_B0_AUTO_BASE_ADRR           0x136
#define LED_B1_AUTO_BASE_ADRR           0x150
#define LED_B2_AUTO_BASE_ADRR           0x16A
#define LED_C0_AUTO_BASE_ADRR           0x184
#define LED_C1_AUTO_BASE_ADRR           0x19E
#define LED_C2_AUTO_BASE_ADRR           0x1B8
#define LED_D0_AUTO_BASE_ADRR           0x1D2
#define LED_D1_AUTO_BASE_ADRR           0x1EC
#define LED_D2_AUTO_BASE_ADRR           0x206

/* Flag Registers */
#define TSD_CONFIG_STAT_REG             0x300
#define LOD_STAT_1_REG                  0x301
#define LOD_STAT_2_REG                  0x302
#define LSD_STAT_1_REG                  0x303
#define LSD_STAT_2_REG                  0x304

#define AUTO_PWM_BASE_ADDR              0x305

#define AEP_STATUS_0_REG                0x315
#define AEP_STATUS_1_REG                0x316
#define AEP_STATUS_2_REG                0x317
#define AEP_STATUS_3_REG                0x318
#define AEP_STATUS_4_REG                0x319
#define AEP_STATUS_5_REG                0x31A
#define AEP_STATUS_6_REG                0x31B
#define AEP_STATUS_7_REG                0x31C

#define LED0                            "led_0"
#define LED1                            "led_1"
#define LED2                            "led_2"
#define LED3                            "led_3"
#define LED_A0                          "led_A0"
#define LED_A1                          "led_A1"
#define LED_A2                          "led_A2"
#define LED_B0                          "led_B0"
#define LED_B1                          "led_B1"
#define LED_B2                          "led_B2"
#define LED_C0                          "led_C0"
#define LED_C1                          "led_C1"
#define LED_C2                          "led_C2"
#define LED_D0                          "led_D0"
#define LED_D1                          "led_D1"
#define LED_D2                          "led_D2"

/* Below define time for (start/stop/slope time) */
#define TIME0                           "no time"
#define TIME1                           "0.09s"
#define TIME2                           "0.18s"
#define TIME3                           "0.36s"
#define TIME4                           "0.54s"
#define TIME5                           "0.80s"
#define TIME6                           "1.07s"
#define TIME7                           "1.52s"
#define TIME8                           "2.06s"
#define TIME9                           "2.50s"
#define TIME10                          "3.04s"
#define TIME11                          "4.02s"
#define TIME12                          "5.01s"
#define TIME13                          "5.99s"
#define TIME14                          "7.06s"
#define TIME15                          "8.05s"
/* End define time for (start/stop/slope time) */

#define AEU1                            "AEU1"
#define AEU2                            "AEU2"
#define AEU3                            "AEU3"

#define MAX_LEDS                        16
#define MAX_TIME                        16
#define MAX_AEU                         3

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

#endif /*_LEDS_LP5812_H_*/
