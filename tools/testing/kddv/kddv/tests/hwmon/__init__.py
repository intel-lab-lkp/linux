#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Kernel device driver verification
#
# Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
# Author: Wei Yongjun <weiyongjun1@huawei.com>

import logging
from pathlib import Path

logger = logging.getLogger(__name__)

class HwMon:
    def __init__(self, path: Path) -> None:
        hwmon = next(path.glob("hwmon/hwmon*")).name
        self.cdev = Path(f"/sys/class/hwmon/{hwmon}")

    def __enter__(self):
        return self

    def __exit__(self, *_):
        pass

    def read_attr(self, attr):
        rpath = self.cdev / attr
        logger.debug(f"read from file {rpath}")
        return rpath.read_text().rstrip()

    def write_attr(self, attr, value):
        wpath = self.cdev / attr
        logger.debug(f"write '{value}' to file {wpath}")
        wpath.write_text(value)

class HwMonDriver:
    def hwmon_read_attr(self, dev, attr):
        with HwMon(dev.path) as hwmon:
            return int(hwmon.read_attr(attr))

    def hwmon_write_attr(self, dev, attr, value):
        with HwMon(dev.path) as hwmon:
            return hwmon.write_attr(attr, str(value))

    def hwmon_read_attr_str(self, dev, attr):
        with HwMon(dev.path) as hwmon:
            return hwmon.read_attr(attr)

    def hwmon_write_attr_str(self, dev, attr, value):
        with HwMon(dev.path) as hwmon:
            return hwmon.write_attr(attr, value)

    def hwmon_read_temp_label(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_label')

    def hwmon_read_temp_input(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_input')

    def hwmon_write_temp_input(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_input', value)

    def hwmon_read_temp_lcrit(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_lcrit')

    def hwmon_write_temp_lcrit(self, dev, value, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_lcrit', value)

    def hwmon_read_temp_lcrit_hyst(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_lcrit_hyst')

    def hwmon_write_temp_lcrit_hyst(self, dev, value, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_lcrit_hyst', value)

    def hwmon_read_temp_min(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_min')

    def hwmon_write_temp_min(self, dev, value, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_min', value)

    def hwmon_read_temp_min_hyst(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_min_hyst')

    def hwmon_write_temp_min_hyst(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_min_hyst', value)

    def hwmon_read_temp_max(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_max')

    def hwmon_write_temp_max(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_max', value)

    def hwmon_read_temp_max_hyst(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_max_hyst')

    def hwmon_write_temp_max_hyst(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_max_hyst', value)

    def hwmon_read_temp_crit(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_crit')

    def hwmon_write_temp_crit(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_crit', value)

    def hwmon_read_temp_crit_hyst(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_crit_hyst')

    def hwmon_write_temp_crit_hyst(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_crit_hyst', value)

    def hwmon_read_temp_emergency(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_emergency')

    def hwmon_write_temp_emergency(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_emergency', value)

    def hwmon_read_temp_emergency_hyst(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_emergency_hyst')

    def hwmon_write_temp_emergency_hyst(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_emergency_hyst', value)

    def hwmon_read_temp_alarm(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_alarm')

    def hwmon_write_temp_alarm(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_alarm', value)

    def hwmon_read_temp_lcrit_alarm(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_lcrit_alarm')

    def hwmon_write_temp_lcrit_alarm(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_lcrit_alarm', value)

    def hwmon_read_temp_min_alarm(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_min_alarm')

    def hwmon_write_temp_min_alarm(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_min_alarm', value)

    def hwmon_read_temp_max_alarm(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_max_alarm')

    def hwmon_write_temp_max_alarm(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_max_alarm', value)

    def hwmon_read_temp_crit_alarm(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_crit_alarm')

    def hwmon_write_temp_crit_alarm(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_crit_alarm', value)

    def hwmon_read_temp_emergency_alarm(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_emergency_alarm')

    def hwmon_write_temp_emergency_alarm(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_emergency_alarm', value)

    def hwmon_read_temp_fault(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_fault')

    def hwmon_write_temp_fault(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_fault', value)

    def hwmon_read_temp_offset(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_offset')

    def hwmon_write_temp_offset(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_offset', value)

    def hwmon_read_temp_label(self, dev, chan=0):
        return self.hwmon_read_attr_str(dev, f'temp{chan+1}_label')

    def hwmon_write_temp_label(self, dev, value, chan=0):
        return self.hwmon_write_attr_str(dev, f'temp{chan+1}_label', value)

    def hwmon_read_temp_lowest(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_lowest')

    def hwmon_write_temp_lowest(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_lowest', value)

    def hwmon_read_temp_highest(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_highest')

    def hwmon_write_temp_highest(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_highest', value)

    def hwmon_read_temp_reset_history(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_reset_history')

    def hwmon_write_temp_reset_history(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_reset_history', value)

    def hwmon_read_temp_rated_min(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_rated_min')

    def hwmon_write_temp_rated_min(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_rated_min', value)

    def hwmon_read_temp_rated_max(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'temp{chan+1}_rated_max')

    def hwmon_write_temp_rated_max(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'temp{chan+1}_rated_max', value)

    def hwmon_read_fan_enable(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'fan{chan+1}_enable')

    def hwmon_write_fan_enable(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'fan{chan+1}_enable', value)

    def hwmon_read_fan_input(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'fan{chan+1}_input')

    def hwmon_write_fan_input(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'fan{chan+1}_input', value)

    def hwmon_read_fan_fault(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'fan{chan+1}_fault')

    def hwmon_write_fan_fault(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'fan{chan+1}_fault', value)

    def hwmon_read_pwm_input(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'pwm{chan+1}')

    def hwmon_write_pwm_input(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'pwm{chan+1}', value)

    def hwmon_read_pwm_enable(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'pwm{chan+1}_enable')

    def hwmon_write_pwm_enable(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'pwm{chan+1}_enable', value)

    def hwmon_read_pwm_mode(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'pwm{chan+1}_mode')

    def hwmon_write_pwm_mode(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'pwm{chan+1}_mode', value)

    def hwmon_read_pwm_freq(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'pwm{chan+1}_freq')

    def hwmon_write_pwm_freq(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'pwm{chan+1}_freq', value)

    def hwmon_read_pwm_auto_channels_temp(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'pwm{chan+1}_auto_channels_temp')

    def hwmon_write_pwm_auto_channels_temp(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'pwm{chan+1}_auto_channels_temp', value)

    def hwmon_read_in_label(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'in{chan+1}_label')

    def hwmon_read_in_input(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'in{chan+1}_input')

    def hwmon_write_in_input(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'in{chan+1}_input', value)

    def hwmon_read_in_min(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'in{chan+1}_min')

    def hwmon_write_in_min(self, dev, value, chan=0):
        return self.hwmon_read_attr(dev, f'in{chan+1}_min', value)

    def hwmon_read_in_min_alarm(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'in{chan+1}_min_alarm')

    def hwmon_write_in_min_alarm(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'in{chan+1}_min_alarm', value)

    def hwmon_read_in_max(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'in{chan+1}_max')

    def hwmon_write_in_max(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'in{chan+1}_max', value)

    def hwmon_read_in_max_alarm(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'in{chan+1}_max_alarm')

    def hwmon_write_in_max_alarm(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'in{chan+1}_max_alarm', value)

    def hwmon_read_in_crit(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'in{chan+1}_crit')

    def hwmon_write_in_crit(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'in{chan+1}_crit', value)

    def hwmon_read_in_crit_alarm(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'in{chan+1}_crit_alarm')

    def hwmon_write_in_crit_alarm(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'in{chan+1}_crit_alarm', value)

    def hwmon_read_in_lcrit(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'in{chan+1}_lcrit')

    def hwmon_write_in_lcrit(self, dev, value, chan=0):
        return self.hwmon_read_attr(dev, f'in{chan+1}_lcrit', value)

    def hwmon_read_in_lcrit_alarm(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'in{chan+1}_lcrit_alarm')

    def hwmon_write_in_lcrit_alarm(self, dev, value, chan=0):
        return self.hwmon_read_attr(dev, f'in{chan+1}_lcrit_alarm', value)

    def hwmon_read_in_rated_min(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'in{chan+1}_rated_min')

    def hwmon_write_in_rated_min(self, dev, value, chan=0):
        return self.hwmon_read_attr(dev, f'in{chan+1}_rated_min', value)

    def hwmon_read_in_rated_max(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'in{chan+1}_rated_max')

    def hwmon_write_in_rated_max(self, dev, value, chan=0):
        return self.hwmon_read_attr(dev, f'in{chan+1}_rated_max', value)

    def hwmon_read_curr_label(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'curr{chan+1}_label')

    def hwmon_read_curr_input(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'curr{chan+1}_input')

    def hwmon_write_curr_input(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'curr{chan+1}_input', value)

    def hwmon_read_curr_max(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'curr{chan+1}_max')

    def hwmon_write_curr_max(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'curr{chan+1}_max', value)

    def hwmon_read_curr_max_alarm(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'curr{chan+1}_max_alarm')

    def hwmon_write_curr_max_alarm(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'curr{chan+1}_max_alarm', value)

    def hwmon_read_curr_crit(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'curr{chan+1}_crit')

    def hwmon_write_curr_crit(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'curr{chan+1}_crit', value)

    def hwmon_read_curr_crit_alarm(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'curr{chan+1}_crit_alarm')

    def hwmon_write_curr_crit_alarm(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'curr{chan+1}_crit_alarm', value)

    def hwmon_read_curr_lcrit(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'curr{chan+1}_lcrit')

    def hwmon_write_curr_lcrit(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'curr{chan+1}_lcrit', value)

    def hwmon_read_curr_lcrit_alarm(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'curr{chan+1}_lcrit_alarm')

    def hwmon_write_curr_lcrit_alarm(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'curr{chan+1}_lcrit_alarm', value)

    def hwmon_read_curr_rated_max(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'curr{chan+1}_rated_max')

    def hwmon_write_curr_rated_max(self, dev, value, chan=0):
        return self.hwmon_read_attr(dev, f'curr{chan+1}_rated_max', value)

    def hwmon_read_power_input(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'power{chan+1}_input')

    def hwmon_write_power_input(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'power{chan+1}_input', value)

    def hwmon_read_power_alarm(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'power{chan+1}_alarm')

    def hwmon_write_power_alarm(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'power{chan+1}_alarm', value)

    def hwmon_read_power_cap(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'power{chan+1}_cap')

    def hwmon_write_power_cap(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'power{chan+1}_cap', value)

    def hwmon_read_power_cap_alarm(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'power{chan+1}_cap_alarm')

    def hwmon_write_power_cap_alarm(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'power{chan+1}_cap_alarm', value)

    def hwmon_read_power_max(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'power{chan+1}_max')

    def hwmon_write_power_max(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'power{chan+1}_max', value)

    def hwmon_read_power_max_alarm(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'power{chan+1}_max_alarm')

    def hwmon_write_power_max_alarm(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'power{chan+1}_max_alarm', value)

    def hwmon_read_power_crit(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'power{chan+1}_crit')

    def hwmon_write_power_crit(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'power{chan+1}_crit', value)

    def hwmon_read_power_crit_alarm(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'power{chan+1}_crit_alarm')

    def hwmon_write_power_crit_alarm(self, dev, value, chan=0):
        return self.hwmon_write_attr(dev, f'power{chan+1}_crit_alarm', value)

    def hwmon_read_power_rated_max(self, dev, chan=0):
        return self.hwmon_read_attr(dev, f'power{chan+1}_rated_max')

    def hwmon_write_power_rated_max(self, dev, value, chan=0):
        return self.hwmon_read_attr(dev, f'power{chan+1}_rated_max', value)
