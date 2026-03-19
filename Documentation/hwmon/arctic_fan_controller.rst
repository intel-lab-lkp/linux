.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver arctic_fan_controller
===================================

Supported devices:

* ARCTIC Fan Controller (USB HID, VID 0x3904, PID 0xF001)

Author: Aureo Serrano de Souza <aureo.serrano@arctic.de>

Description
-----------

This driver provides hwmon support for the ARCTIC Fan Controller, a USB Custom HID
device with 10 fan channels. The device sends IN reports about once per second
containing current PWM (bytes 1–10) and RPM (bytes 11–30). PWM is set via OUT reports
(bytes 1–10, 0–100% per channel). Fan control is manual-only: the device does not
change PWM autonomously, only when it receives an OUT report from the host.

Usage notes
-----------

Since it is a USB device, hotplug is supported. The device is autodetected.

Sysfs entries
-------------

================ ===============================================================
fan[1-10]_input   Fan speed in RPM (read-only, from device IN reports).
pwm[1-10]         PWM duty cycle. Sysfs uses 0–255 (0%–100%); the device uses
                  0–100% internally. Read: current duty from IN report (scaled
                  to 0–255). Write: set duty via OUT report (value 0–255).
================ ===============================================================
