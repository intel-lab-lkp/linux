.. SPDX-License-Identifier: GPL-2.0-only

Raspberry Pi RP1 fan tachometer
=======================================

Supported chips:

 * Raspberry Pi RP1

   Prefix: 'rp1_fan'

   Addresses scanned: -

   Datasheet: not public

Author: Andrea della Porta <andrea.porta@suse.com>

Description
-----------

The Raspberry Pi RP1 is a multi-function I/O controller (southbridge) used
on boards such as the Raspberry Pi 5. Among its peripherals, the RP1 includes
a PWM controller responsible for fan control.

In addition to standard PWM output, the RP1 provides fan speed monitoring
(tachometer). The tachometer pulse measurement is performed internally by
firmware running on the RP1 Cortex-M microcontroller core. The computed fan
speed (RPM) is periodically updated by the firmware directly into dedicated
MMIO registers located within the RP1 PWM block.

This driver interfaces with the RP1 PWM controller to read these firmware-updated
registers and exposes the fan speed as standard hwmon sysfs attributes.

Sysfs entries
-------------

The following attributes are supported.

======================= ========================================================
fan1_input              fan tachometer speed in RPM (RO)
======================= ========================================================
