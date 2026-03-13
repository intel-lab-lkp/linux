.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver arctic_fan_controller
=====================================

Supported devices:

* ARCTIC Fan Controller (USB HID, VID 0x3904, PID 0xF001)

Author: Aureo Serrano de Souza <aureo.serrano@arctic.de>

Description
-----------

This driver provides hwmon support for the ARCTIC Fan Controller, a USB
Custom HID device with 10 fan channels. The device sends IN reports about
once per second containing current RPM values (bytes 11-30, 10 x uint16 LE).
Fan speed control is manual-only: the device does not change PWM
autonomously; it only applies a new duty cycle when it receives an OUT
report from the host.

After the device applies an OUT report, it sends back a 2-byte ACK IN
report (Report ID 0x02, byte 1 = 0x00 on success) confirming the command
was applied.

Usage notes
-----------

Since it is a USB device, hotplug is supported. The device is autodetected.

Sysfs entries
-------------

================ ==============================================================
fan[1-10]_input  Fan speed in RPM (read-only). Updated from IN reports at ~1 Hz.
pwm[1-10]        PWM duty cycle (0-255). Write: sends an OUT report setting the
                 duty cycle (scaled from 0-255 to 0-100% for the device). Read:
                 returns the last value written; the device is manual-only so
                 the host cache is authoritative.
================ ==============================================================
