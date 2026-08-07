.. SPDX-License-Identifier: GPL-2.0-only

====================================
Kernel driver minisforum-um780xtx
====================================

Supported systems:

  * Minisforum UM780 XTX

    * DMI product name: ``Venus series``
    * Mainboard: ``F7BSD``, revision ``1.1``
    * BIOS version: ``1.06``

Author: Sebastián Peyrott <speyrott@gmail.com>

Description
-----------

This driver exposes hardware monitoring and fan-control data cached by the
IT5571E embedded controller. It uses the ACPI EC transport and only binds to
the exact system and firmware identity listed above.

The two temperature channels are the values used by the EC's fan-control
loops. Their physical sensor placement is not known. ``temp1`` is an external
thermistor input used for the system fan, while ``temp2`` is the filtered AMD
SB-TSI temperature used for the CPU fan.

The fan tachometers are read through OEM EC commands. Since each byte is
returned by a separate command, the driver uses a high-low-high sequence and
retries if the high byte changes.

Sysfs entries
-------------

============================  ==============================================
``temp1_input``               System-fan control temperature
``temp2_input``               CPU-fan control temperature
``fan1_input``                CPU fan speed in RPM
``fan2_input``                System fan speed in RPM
``pwm1_enable``               CPU profile: 2 is OEM B1, 3 is OEM B2
``pwm2_enable``               Always 2 (automatic firmware control)
``pwm2_auto_channels_temp``   Always 1 (``temp1`` controls this output)
``pwm2_auto_point1_temp``     Off-to-low system-fan transition temperature
``pwm2_auto_point1_pwm``      Fixed low target, 40 on the 0--255 scale
``pwm2_auto_point2_temp``     Low-to-high system-fan transition temperature
``pwm2_auto_point2_pwm``      Fixed high target, 102 on the 0--255 scale
============================  ==============================================

CPU fan profiles
----------------

Values 2 and 3 of ``pwm1_enable`` select the two complete automatic profiles
implemented by the firmware. Writing either value reloads the whole CPU fan
curve, including firmware state which is not visible through the ACPI EC
window. Other values are rejected.

System fan thresholds
---------------------

The two writable system-fan temperatures accept whole degrees Celsius only.
The driver requires strict ordering between both visible thresholds and the
firmware's internal third threshold. The third threshold does not select a new
PWM target and is therefore not exposed as another auto point.

Resume state preservation
-------------------------

Fan settings are held in EC RAM. On the supported firmware, initialization
after s2idle resume reloads the factory system-fan curve. The driver caches the
coherent profile and thresholds present when it probes, updates the cache after
successful hwmon writes, and performs one delayed comparison after resume. It
restores only values which differ from that cache; it does not poll during
normal operation.

The ``resume_restore_delay_ms`` module parameter selects the delay from the
kernel resume callback. Its default is 1000 ms. Values from 1000 through 60000
ms are accepted, and 0 disables the post-resume check. A runtime change applies
to the next resume, not to work which is already pending.
