.. SPDX-License-Identifier: GPL-2.0

Kernel driver nct736x
=====================

Supported chip:

  * Nuvoton NCT7362Y NCT7363Y

    Prefix: nct736x

    Addresses: I2C 0x20, 0x21, 0x22, 0x23

Author: Ban Feng <kcfeng0@nuvoton.com>


Description
-----------

The NCT736X is a Fan controller which provides up to 16 independent
FAN input monitors, and up to 16 independent PWM output with SMBus interface.
Besides, NCT7363Y has a built-in watchdog timer which is used for
conditionally generating a system reset output (INT#).


Sysfs entries
-------------

Currently, the driver supports the following features:

======================= =======================================================
fanX_input		provide current fan rotation value in RPM

pwmX			get or set PWM fan control value.
======================= =======================================================
