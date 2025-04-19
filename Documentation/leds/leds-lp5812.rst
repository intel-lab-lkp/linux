========================
Kernel driver for lp5812
========================

* TI/National Semiconductor LP5812 LED Driver
* Datasheet: https://www.ti.com/product/LP5812#tech-docs

Authors: Jared Zhou <jared-zhou@ti.com>

Description
===========

The LP5812 is a 4x3 maxtrix LED driver with support for both manual and
autonomous animation control. It provides features such as:

- PWM dimming and DC current control
- Slope time configuration
- Autonomous Engine Unit (AEU) for LED animation playback
- Flexible scan and drive mode configuration

This driver provides sysfs interfaces to control and configure the LP5812
device and its LED channels.

Sysfs Interface
===============

LP5812 device exposes a chip-level sysfs group:
  /sys/bus/i2c/devices/<i2c-dev-addr>/lp5812_chip_setup/

The following attributes are available at chip level:
  - device_enable: Enable/disable the device (RW)
  - dev_config: Configure drive mode and scan order (RW)
  - device_command: Issue device-wide commands (WO)
  - sw_reset: Reset the hardware (WO)
  - fault_clear: Clear any device faults (WO)
  - tsd_config_status: Read thermal shutdown config status (RO)

Each LED channel is exposed as:
  /sys/bus/i2c/devices/<i2c-dev-addr>/led_<id>/

Each LED exposes the following attributes:
  - activate: Activate or deactivate the LED (RW)
  - mode: manual or autonomous mode (RW)
  - manual_dc: DC current value (0–255) (RW)
  - manual_pwm: PWM duty cycle (0–255) (RW)
  - autonomous_dc: DC current in autonomous mode (RW)
  - pwm_dimming_scale: linear or exponential (RW)
  - pwm_phase_align: PWM alignment mode (RW)
  - autonomous_animation: Config autonomous animation mode with aeu number, start pause time, stop pause time, playback time (RW)
  - aep_status: autonomous engine pattern status (RO)
  - auto_pwm_val: pwm value in autonomous mode when pause the animation (RO)
  - lod_lsd: lod and lsd fault detected status (RO)

Example Usage
=============

To control led_A0 in manual mode::
    echo 1 > /sys/bus/i2c/drivers/lp5812/xxxx/lp5812_chip_setup/device_enable
    echo 1 > /sys/bus/i2c/drivers/lp5812/xxxx/led_A0/activate
    echo manual > /sys/bus/i2c/drivers/lp5812/xxxx/led_A0/mode
    echo 100 > /sys/bus/i2c/drivers/lp5812/xxxx/led_A0/manual_dc
    echo 100 > /sys/bus/i2c/drivers/lp5812/xxxx/led_A0/manual_pwm

To control led_A0 in autonomous mode::
    echo 1 > /sys/bus/i2c/drivers/lp5812/xxxx/lp5812_chip_setup/device_enable
    echo 1 > /sys/bus/i2c/drivers/lp5812/xxxx/led_A0/activate
    echo autonomous > /sys/bus/i2c/drivers/lp5812/xxxx/led_A0/mode
    echo 1:10:10:15 > /sys/bus/i2c/drivers/lp5812/xxxx/led_A0/autonomous_animation
    echo 100 > /sys/bus/i2c/drivers/lp5812/xxxx/led_A0/AEU1/pwm1
    echo 100 > /sys/bus/i2c/drivers/lp5812/xxxx/led_A0/AEU1/pwm2
    echo 100 > /sys/bus/i2c/drivers/lp5812/xxxx/led_A0/AEU1/pwm3
    echo 100 > /sys/bus/i2c/drivers/lp5812/xxxx/led_A0/AEU1/pwm4
    echo 100 > /sys/bus/i2c/drivers/lp5812/xxxx/led_A0/AEU1/pwm5
    echo 5 > /sys/bus/i2c/drivers/lp5812/xxxx/led_A0/AEU1/slope_time_t1
    echo 5 > /sys/bus/i2c/drivers/lp5812/xxxx/led_A0/AEU1/slope_time_t2
    echo 5 > /sys/bus/i2c/drivers/lp5812/xxxx/led_A0/AEU1/slope_time_t3
    echo 5 > /sys/bus/i2c/drivers/lp5812/xxxx/led_A0/AEU1/slope_time_t4
    echo 1 > /sys/bus/i2c/drivers/lp5812/xxxx/led_A0/AEU1/playback_time
    echo start > /sys/bus/i2c/drivers/lp5812/xxxx/lp5812_chip_setup/device_command
