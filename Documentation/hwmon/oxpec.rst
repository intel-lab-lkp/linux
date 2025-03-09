.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver oxpec
=========================

Authors:
    - Derek John Clark <derekjohn.clark@gmail.com>
    - Joaquín Ignacio Aramendía <samsagax@gmail.com>
    - Antheas Kapenekakis <lkml@antheas.dev>

Description:
------------

Handheld devices from OneXPlayer and AOKZOE provide fan readings and fan
control through their embedded controllers, which can be accessed via this
module. If the device has the platform `tt_toggle` attribute (see
Documentation/ABI/testing/sysfs-platform-oxp), controlling these attributes
without having it engaged is undefined behavior.

In addition, for legacy reasons, this driver provides hwmon functionality
to Ayaneo devices, and the OrangePi Neo (AOKZOE is a sister company of
OneXPlayer and uses the same EC).

Supported devices
-----------------

Currently the driver supports the following handhelds:
 - AOKZOE A1
 - AOKZOE A1 PRO
 - OneXPlayer 2/2 Pro
 - OneXPlayer AMD
 - OneXPlayer mini AMD
 - OneXPlayer mini AMD PRO
 - OneXPlayer OneXFly variants
 - OneXPlayer X1 variants

In addition, until a driver is upstreamed for the following, the driver
also supports controlling them:
 - AYANEO 2
 - AYANEO 2S
 - AYANEO AIR
 - AYANEO AIR 1S
 - AYANEO AIR Plus (Mendocino)
 - AYANEO AIR Pro
 - AYANEO Flip DS
 - AYANEO Flip KB
 - AYANEO Geek
 - AYANEO Geek 1S
 - AYANEO KUN
 - OrangePi NEO-01

Sysfs entries
-------------

The following attributes are supported:

fan1_input
  Read Only. Reads current fan RPM.

pwm1_enable
  Read Write. Enable manual fan control. Write "1" to set to manual, write "0"
  to let the EC control de fan speed. Read this attribute to see current status.

pwm1
  Read Write. Read this attribute to see current duty cycle in the range [0-255].
  When pwm1_enable is set to "1" (manual) write any value in the range [0-255]
  to set fan speed.