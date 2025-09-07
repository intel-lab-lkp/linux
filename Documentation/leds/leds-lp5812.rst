========================
Kernel driver for lp5812
========================

* TI/National Semiconductor LP5812 LED Driver
* Datasheet: https://www.ti.com/product/LP5812#tech-docs

Authors: Jared Zhou <jared-zhou@ti.com>

Description
===========

The LP5812 is a 4x3 matrix LED driver with support for both manual and
autonomous animation control. This driver provides sysfs interfaces to
control and configure the LP5812 device and its LED channels.

Sysfs Interface
===============

LP5812 device exposes a chip-level sysfs group:
  /sys/bus/i2c/devices/<i2c-dev-addr>/lp5812_chip_setup/

The following attributes are available at chip level:
  - dev_config: Configure drive mode and scan order (RW)
  - sw_reset: Reset the hardware (WO)
  - fault_clear: Clear any device faults (WO)
  - tsd_config_status: Read thermal shutdown config status (RO)

Each LED channel is exposed as:
  /sys/class/leds/led_<id>/

Each LED exposes the following attributes:
  - activate: Activate or deactivate the LED (WO)
  - led_current: DC current value (0–255) (WO)
  - max_current: maximum DC current bit setting (RO)
  - lod_lsd: lod and lsd fault detected status (RO)

Example Usage
=============

To control led_A in manual mode::
    echo "tcmscan:4:0:1:2:3" > /sys/bus/i2c/devices/.../lp5812_chip_setup/dev_config
    echo 1 1 1 > /sys/class/leds/LED_A/activate
    echo 100 100 100 > /sys/class/leds/LED_A/led_current
    echo 50 50 50 > /sys/class/leds/LED_A/multi_intensity
    echo 255 > /sys/class/leds/LED_A/brightness
