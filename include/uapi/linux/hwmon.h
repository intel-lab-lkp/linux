/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Hardware monitoring sensor types, attributes and configuration bits.
 *
 * Copyright (C) 2005 Mark M. Hoffman <mhoffman@lightlink.com>
 * Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
 */

#ifndef _UAPI_LINUX_HWMON_H
#define _UAPI_LINUX_HWMON_H

#include <linux/const.h>

/*
 * Existing identifiers are ABI: never change their values. Append new attributes
 * at the end of their enum and new sensor types before hwmon_max.
 */

enum hwmon_sensor_types {
	hwmon_chip = 0,
	hwmon_temp = 1,
	hwmon_in = 2,
	hwmon_curr = 3,
	hwmon_power = 4,
	hwmon_energy = 5,
	hwmon_energy64 = 6,
	hwmon_humidity = 7,
	hwmon_fan = 8,
	hwmon_pwm = 9,
	hwmon_intrusion = 10,
	hwmon_max = 11,
};

enum hwmon_chip_attributes {
	hwmon_chip_temp_reset_history = 0,
	hwmon_chip_in_reset_history = 1,
	hwmon_chip_curr_reset_history = 2,
	hwmon_chip_power_reset_history = 3,
	hwmon_chip_register_tz = 4,
	hwmon_chip_update_interval = 5,
	hwmon_chip_update_interval_us = 6,
	hwmon_chip_alarms = 7,
	hwmon_chip_samples = 8,
	hwmon_chip_curr_samples = 9,
	hwmon_chip_in_samples = 10,
	hwmon_chip_power_samples = 11,
	hwmon_chip_temp_samples = 12,
	hwmon_chip_beep_enable = 13,
	hwmon_chip_pec = 14,
};

#define HWMON_C_TEMP_RESET_HISTORY _BITUL(hwmon_chip_temp_reset_history)
#define HWMON_C_IN_RESET_HISTORY _BITUL(hwmon_chip_in_reset_history)
#define HWMON_C_CURR_RESET_HISTORY _BITUL(hwmon_chip_curr_reset_history)
#define HWMON_C_POWER_RESET_HISTORY _BITUL(hwmon_chip_power_reset_history)
#define HWMON_C_REGISTER_TZ _BITUL(hwmon_chip_register_tz)
#define HWMON_C_UPDATE_INTERVAL _BITUL(hwmon_chip_update_interval)
#define HWMON_C_UPDATE_INTERVAL_US _BITUL(hwmon_chip_update_interval_us)
#define HWMON_C_ALARMS _BITUL(hwmon_chip_alarms)
#define HWMON_C_SAMPLES _BITUL(hwmon_chip_samples)
#define HWMON_C_CURR_SAMPLES _BITUL(hwmon_chip_curr_samples)
#define HWMON_C_IN_SAMPLES _BITUL(hwmon_chip_in_samples)
#define HWMON_C_POWER_SAMPLES _BITUL(hwmon_chip_power_samples)
#define HWMON_C_TEMP_SAMPLES _BITUL(hwmon_chip_temp_samples)
#define HWMON_C_BEEP_ENABLE _BITUL(hwmon_chip_beep_enable)
#define HWMON_C_PEC _BITUL(hwmon_chip_pec)

enum hwmon_temp_attributes {
	hwmon_temp_enable = 0,
	hwmon_temp_input = 1,
	hwmon_temp_type = 2,
	hwmon_temp_lcrit = 3,
	hwmon_temp_lcrit_hyst = 4,
	hwmon_temp_min = 5,
	hwmon_temp_min_hyst = 6,
	hwmon_temp_max = 7,
	hwmon_temp_max_hyst = 8,
	hwmon_temp_crit = 9,
	hwmon_temp_crit_hyst = 10,
	hwmon_temp_emergency = 11,
	hwmon_temp_emergency_hyst = 12,
	hwmon_temp_alarm = 13,
	hwmon_temp_lcrit_alarm = 14,
	hwmon_temp_min_alarm = 15,
	hwmon_temp_max_alarm = 16,
	hwmon_temp_crit_alarm = 17,
	hwmon_temp_emergency_alarm = 18,
	hwmon_temp_fault = 19,
	hwmon_temp_offset = 20,
	hwmon_temp_label = 21,
	hwmon_temp_lowest = 22,
	hwmon_temp_highest = 23,
	hwmon_temp_reset_history = 24,
	hwmon_temp_rated_min = 25,
	hwmon_temp_rated_max = 26,
	hwmon_temp_beep = 27,
};

#define HWMON_T_ENABLE _BITUL(hwmon_temp_enable)
#define HWMON_T_INPUT _BITUL(hwmon_temp_input)
#define HWMON_T_TYPE _BITUL(hwmon_temp_type)
#define HWMON_T_LCRIT _BITUL(hwmon_temp_lcrit)
#define HWMON_T_LCRIT_HYST _BITUL(hwmon_temp_lcrit_hyst)
#define HWMON_T_MIN _BITUL(hwmon_temp_min)
#define HWMON_T_MIN_HYST _BITUL(hwmon_temp_min_hyst)
#define HWMON_T_MAX _BITUL(hwmon_temp_max)
#define HWMON_T_MAX_HYST _BITUL(hwmon_temp_max_hyst)
#define HWMON_T_CRIT _BITUL(hwmon_temp_crit)
#define HWMON_T_CRIT_HYST _BITUL(hwmon_temp_crit_hyst)
#define HWMON_T_EMERGENCY _BITUL(hwmon_temp_emergency)
#define HWMON_T_EMERGENCY_HYST _BITUL(hwmon_temp_emergency_hyst)
#define HWMON_T_ALARM _BITUL(hwmon_temp_alarm)
#define HWMON_T_MIN_ALARM _BITUL(hwmon_temp_min_alarm)
#define HWMON_T_MAX_ALARM _BITUL(hwmon_temp_max_alarm)
#define HWMON_T_CRIT_ALARM _BITUL(hwmon_temp_crit_alarm)
#define HWMON_T_LCRIT_ALARM _BITUL(hwmon_temp_lcrit_alarm)
#define HWMON_T_EMERGENCY_ALARM _BITUL(hwmon_temp_emergency_alarm)
#define HWMON_T_FAULT _BITUL(hwmon_temp_fault)
#define HWMON_T_OFFSET _BITUL(hwmon_temp_offset)
#define HWMON_T_LABEL _BITUL(hwmon_temp_label)
#define HWMON_T_LOWEST _BITUL(hwmon_temp_lowest)
#define HWMON_T_HIGHEST _BITUL(hwmon_temp_highest)
#define HWMON_T_RESET_HISTORY _BITUL(hwmon_temp_reset_history)
#define HWMON_T_RATED_MIN _BITUL(hwmon_temp_rated_min)
#define HWMON_T_RATED_MAX _BITUL(hwmon_temp_rated_max)
#define HWMON_T_BEEP _BITUL(hwmon_temp_beep)

enum hwmon_in_attributes {
	hwmon_in_enable = 0,
	hwmon_in_input = 1,
	hwmon_in_min = 2,
	hwmon_in_max = 3,
	hwmon_in_lcrit = 4,
	hwmon_in_crit = 5,
	hwmon_in_average = 6,
	hwmon_in_lowest = 7,
	hwmon_in_highest = 8,
	hwmon_in_reset_history = 9,
	hwmon_in_label = 10,
	hwmon_in_alarm = 11,
	hwmon_in_min_alarm = 12,
	hwmon_in_max_alarm = 13,
	hwmon_in_lcrit_alarm = 14,
	hwmon_in_crit_alarm = 15,
	hwmon_in_rated_min = 16,
	hwmon_in_rated_max = 17,
	hwmon_in_beep = 18,
	hwmon_in_fault = 19,
};

#define HWMON_I_ENABLE _BITUL(hwmon_in_enable)
#define HWMON_I_INPUT _BITUL(hwmon_in_input)
#define HWMON_I_MIN _BITUL(hwmon_in_min)
#define HWMON_I_MAX _BITUL(hwmon_in_max)
#define HWMON_I_LCRIT _BITUL(hwmon_in_lcrit)
#define HWMON_I_CRIT _BITUL(hwmon_in_crit)
#define HWMON_I_AVERAGE _BITUL(hwmon_in_average)
#define HWMON_I_LOWEST _BITUL(hwmon_in_lowest)
#define HWMON_I_HIGHEST _BITUL(hwmon_in_highest)
#define HWMON_I_RESET_HISTORY _BITUL(hwmon_in_reset_history)
#define HWMON_I_LABEL _BITUL(hwmon_in_label)
#define HWMON_I_ALARM _BITUL(hwmon_in_alarm)
#define HWMON_I_MIN_ALARM _BITUL(hwmon_in_min_alarm)
#define HWMON_I_MAX_ALARM _BITUL(hwmon_in_max_alarm)
#define HWMON_I_LCRIT_ALARM _BITUL(hwmon_in_lcrit_alarm)
#define HWMON_I_CRIT_ALARM _BITUL(hwmon_in_crit_alarm)
#define HWMON_I_RATED_MIN _BITUL(hwmon_in_rated_min)
#define HWMON_I_RATED_MAX _BITUL(hwmon_in_rated_max)
#define HWMON_I_BEEP _BITUL(hwmon_in_beep)
#define HWMON_I_FAULT _BITUL(hwmon_in_fault)

enum hwmon_curr_attributes {
	hwmon_curr_enable = 0,
	hwmon_curr_input = 1,
	hwmon_curr_min = 2,
	hwmon_curr_max = 3,
	hwmon_curr_lcrit = 4,
	hwmon_curr_crit = 5,
	hwmon_curr_average = 6,
	hwmon_curr_lowest = 7,
	hwmon_curr_highest = 8,
	hwmon_curr_reset_history = 9,
	hwmon_curr_label = 10,
	hwmon_curr_alarm = 11,
	hwmon_curr_min_alarm = 12,
	hwmon_curr_max_alarm = 13,
	hwmon_curr_lcrit_alarm = 14,
	hwmon_curr_crit_alarm = 15,
	hwmon_curr_rated_min = 16,
	hwmon_curr_rated_max = 17,
	hwmon_curr_beep = 18,
};

#define HWMON_C_ENABLE _BITUL(hwmon_curr_enable)
#define HWMON_C_INPUT _BITUL(hwmon_curr_input)
#define HWMON_C_MIN _BITUL(hwmon_curr_min)
#define HWMON_C_MAX _BITUL(hwmon_curr_max)
#define HWMON_C_LCRIT _BITUL(hwmon_curr_lcrit)
#define HWMON_C_CRIT _BITUL(hwmon_curr_crit)
#define HWMON_C_AVERAGE _BITUL(hwmon_curr_average)
#define HWMON_C_LOWEST _BITUL(hwmon_curr_lowest)
#define HWMON_C_HIGHEST _BITUL(hwmon_curr_highest)
#define HWMON_C_RESET_HISTORY _BITUL(hwmon_curr_reset_history)
#define HWMON_C_LABEL _BITUL(hwmon_curr_label)
#define HWMON_C_ALARM _BITUL(hwmon_curr_alarm)
#define HWMON_C_MIN_ALARM _BITUL(hwmon_curr_min_alarm)
#define HWMON_C_MAX_ALARM _BITUL(hwmon_curr_max_alarm)
#define HWMON_C_LCRIT_ALARM _BITUL(hwmon_curr_lcrit_alarm)
#define HWMON_C_CRIT_ALARM _BITUL(hwmon_curr_crit_alarm)
#define HWMON_C_RATED_MIN _BITUL(hwmon_curr_rated_min)
#define HWMON_C_RATED_MAX _BITUL(hwmon_curr_rated_max)
#define HWMON_C_BEEP _BITUL(hwmon_curr_beep)

enum hwmon_power_attributes {
	hwmon_power_enable = 0,
	hwmon_power_average = 1,
	hwmon_power_average_interval = 2,
	hwmon_power_average_interval_max = 3,
	hwmon_power_average_interval_min = 4,
	hwmon_power_average_highest = 5,
	hwmon_power_average_lowest = 6,
	hwmon_power_average_max = 7,
	hwmon_power_average_min = 8,
	hwmon_power_input = 9,
	hwmon_power_input_highest = 10,
	hwmon_power_input_lowest = 11,
	hwmon_power_reset_history = 12,
	hwmon_power_accuracy = 13,
	hwmon_power_cap = 14,
	hwmon_power_cap_hyst = 15,
	hwmon_power_cap_max = 16,
	hwmon_power_cap_min = 17,
	hwmon_power_min = 18,
	hwmon_power_max = 19,
	hwmon_power_crit = 20,
	hwmon_power_lcrit = 21,
	hwmon_power_label = 22,
	hwmon_power_alarm = 23,
	hwmon_power_cap_alarm = 24,
	hwmon_power_min_alarm = 25,
	hwmon_power_max_alarm = 26,
	hwmon_power_lcrit_alarm = 27,
	hwmon_power_crit_alarm = 28,
	hwmon_power_rated_min = 29,
	hwmon_power_rated_max = 30,
};

#define HWMON_P_ENABLE _BITUL(hwmon_power_enable)
#define HWMON_P_AVERAGE _BITUL(hwmon_power_average)
#define HWMON_P_AVERAGE_INTERVAL _BITUL(hwmon_power_average_interval)
#define HWMON_P_AVERAGE_INTERVAL_MAX _BITUL(hwmon_power_average_interval_max)
#define HWMON_P_AVERAGE_INTERVAL_MIN _BITUL(hwmon_power_average_interval_min)
#define HWMON_P_AVERAGE_HIGHEST _BITUL(hwmon_power_average_highest)
#define HWMON_P_AVERAGE_LOWEST _BITUL(hwmon_power_average_lowest)
#define HWMON_P_AVERAGE_MAX _BITUL(hwmon_power_average_max)
#define HWMON_P_AVERAGE_MIN _BITUL(hwmon_power_average_min)
#define HWMON_P_INPUT _BITUL(hwmon_power_input)
#define HWMON_P_INPUT_HIGHEST _BITUL(hwmon_power_input_highest)
#define HWMON_P_INPUT_LOWEST _BITUL(hwmon_power_input_lowest)
#define HWMON_P_RESET_HISTORY _BITUL(hwmon_power_reset_history)
#define HWMON_P_ACCURACY _BITUL(hwmon_power_accuracy)
#define HWMON_P_CAP _BITUL(hwmon_power_cap)
#define HWMON_P_CAP_HYST _BITUL(hwmon_power_cap_hyst)
#define HWMON_P_CAP_MAX _BITUL(hwmon_power_cap_max)
#define HWMON_P_CAP_MIN _BITUL(hwmon_power_cap_min)
#define HWMON_P_MIN _BITUL(hwmon_power_min)
#define HWMON_P_MAX _BITUL(hwmon_power_max)
#define HWMON_P_LCRIT _BITUL(hwmon_power_lcrit)
#define HWMON_P_CRIT _BITUL(hwmon_power_crit)
#define HWMON_P_LABEL _BITUL(hwmon_power_label)
#define HWMON_P_ALARM _BITUL(hwmon_power_alarm)
#define HWMON_P_CAP_ALARM _BITUL(hwmon_power_cap_alarm)
#define HWMON_P_MIN_ALARM _BITUL(hwmon_power_min_alarm)
#define HWMON_P_MAX_ALARM _BITUL(hwmon_power_max_alarm)
#define HWMON_P_LCRIT_ALARM _BITUL(hwmon_power_lcrit_alarm)
#define HWMON_P_CRIT_ALARM _BITUL(hwmon_power_crit_alarm)
#define HWMON_P_RATED_MIN _BITUL(hwmon_power_rated_min)
#define HWMON_P_RATED_MAX _BITUL(hwmon_power_rated_max)

enum hwmon_energy_attributes {
	hwmon_energy_enable = 0,
	hwmon_energy_input = 1,
	hwmon_energy_label = 2,
};

#define HWMON_E_ENABLE _BITUL(hwmon_energy_enable)
#define HWMON_E_INPUT _BITUL(hwmon_energy_input)
#define HWMON_E_LABEL _BITUL(hwmon_energy_label)

enum hwmon_humidity_attributes {
	hwmon_humidity_enable = 0,
	hwmon_humidity_input = 1,
	hwmon_humidity_label = 2,
	hwmon_humidity_min = 3,
	hwmon_humidity_min_hyst = 4,
	hwmon_humidity_max = 5,
	hwmon_humidity_max_hyst = 6,
	hwmon_humidity_alarm = 7,
	hwmon_humidity_fault = 8,
	hwmon_humidity_rated_min = 9,
	hwmon_humidity_rated_max = 10,
	hwmon_humidity_min_alarm = 11,
	hwmon_humidity_max_alarm = 12,
};

#define HWMON_H_ENABLE _BITUL(hwmon_humidity_enable)
#define HWMON_H_INPUT _BITUL(hwmon_humidity_input)
#define HWMON_H_LABEL _BITUL(hwmon_humidity_label)
#define HWMON_H_MIN _BITUL(hwmon_humidity_min)
#define HWMON_H_MIN_HYST _BITUL(hwmon_humidity_min_hyst)
#define HWMON_H_MAX _BITUL(hwmon_humidity_max)
#define HWMON_H_MAX_HYST _BITUL(hwmon_humidity_max_hyst)
#define HWMON_H_ALARM _BITUL(hwmon_humidity_alarm)
#define HWMON_H_FAULT _BITUL(hwmon_humidity_fault)
#define HWMON_H_RATED_MIN _BITUL(hwmon_humidity_rated_min)
#define HWMON_H_RATED_MAX _BITUL(hwmon_humidity_rated_max)
#define HWMON_H_MIN_ALARM _BITUL(hwmon_humidity_min_alarm)
#define HWMON_H_MAX_ALARM _BITUL(hwmon_humidity_max_alarm)

enum hwmon_fan_attributes {
	hwmon_fan_enable = 0,
	hwmon_fan_input = 1,
	hwmon_fan_label = 2,
	hwmon_fan_min = 3,
	hwmon_fan_max = 4,
	hwmon_fan_div = 5,
	hwmon_fan_pulses = 6,
	hwmon_fan_target = 7,
	hwmon_fan_alarm = 8,
	hwmon_fan_min_alarm = 9,
	hwmon_fan_max_alarm = 10,
	hwmon_fan_fault = 11,
	hwmon_fan_beep = 12,
};

#define HWMON_F_ENABLE _BITUL(hwmon_fan_enable)
#define HWMON_F_INPUT _BITUL(hwmon_fan_input)
#define HWMON_F_LABEL _BITUL(hwmon_fan_label)
#define HWMON_F_MIN _BITUL(hwmon_fan_min)
#define HWMON_F_MAX _BITUL(hwmon_fan_max)
#define HWMON_F_DIV _BITUL(hwmon_fan_div)
#define HWMON_F_PULSES _BITUL(hwmon_fan_pulses)
#define HWMON_F_TARGET _BITUL(hwmon_fan_target)
#define HWMON_F_ALARM _BITUL(hwmon_fan_alarm)
#define HWMON_F_MIN_ALARM _BITUL(hwmon_fan_min_alarm)
#define HWMON_F_MAX_ALARM _BITUL(hwmon_fan_max_alarm)
#define HWMON_F_FAULT _BITUL(hwmon_fan_fault)
#define HWMON_F_BEEP _BITUL(hwmon_fan_beep)

enum hwmon_pwm_attributes {
	hwmon_pwm_input = 0,
	hwmon_pwm_enable = 1,
	hwmon_pwm_mode = 2,
	hwmon_pwm_freq = 3,
	hwmon_pwm_auto_channels_temp = 4,
};

#define HWMON_PWM_INPUT _BITUL(hwmon_pwm_input)
#define HWMON_PWM_ENABLE _BITUL(hwmon_pwm_enable)
#define HWMON_PWM_MODE _BITUL(hwmon_pwm_mode)
#define HWMON_PWM_FREQ _BITUL(hwmon_pwm_freq)
#define HWMON_PWM_AUTO_CHANNELS_TEMP _BITUL(hwmon_pwm_auto_channels_temp)

enum hwmon_intrusion_attributes {
	hwmon_intrusion_alarm = 0,
	hwmon_intrusion_beep = 1,
};

#define HWMON_INTRUSION_ALARM _BITUL(hwmon_intrusion_alarm)
#define HWMON_INTRUSION_BEEP _BITUL(hwmon_intrusion_beep)

#endif /* _UAPI_LINUX_HWMON_H */
