.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver lattepanda-sigma-ec
=================================

Supported systems:

  * LattePanda Sigma (Intel 13th Gen i5-1340P)

    DMI vendor: LattePanda

    DMI product: LattePanda Sigma

    Datasheet: Not available (EC registers discovered empirically)

Author: Mariano Abad <weimaraner@gmail.com>

Description
-----------

This driver provides hardware monitoring for the LattePanda Sigma
single-board computer. The board's Embedded Controller manages a CPU
cooling fan but does not expose sensor data through standard ACPI
interfaces.

The BIOS declares the ACPI Embedded Controller (``PNP0C09``) with
``_STA`` returning 0 (not present), preventing the kernel's ACPI EC
subsystem from initializing. However, the EC hardware is fully
functional on the standard ACPI EC I/O ports (``0x62`` data, ``0x66``
command/status). This driver uses direct port I/O with EC read command
``0x80`` to access sensor registers.

The EC register map was discovered empirically by dumping all 256
registers, identifying those that change in real-time, then validating
by physically stopping the fan and observing the RPM drop to zero.

The driver uses DMI matching and will only load on LattePanda Sigma
hardware.

Sysfs attributes
----------------

======================= ===============================================
``fan1_input``          Fan speed in RPM (EC registers 0x2E:0x2F,
                        16-bit big-endian)
``fan1_label``          "CPU Fan"
``temp1_input``         Board/ambient temperature in millidegrees
                        Celsius (EC register 0x60)
``temp1_label``         "Board Temp"
``temp2_input``         CPU proximity temperature in millidegrees
                        Celsius (EC register 0x70)
``temp2_label``         "CPU Temp"
======================= ===============================================

Known limitations
-----------------

* The EC register map was reverse-engineered on a LattePanda Sigma with
  BIOS version 5.27. Different BIOS versions may use different register
  offsets.
* Fan speed control is not supported. The fan is always under EC
  automatic control.
* The I/O ports ``0x62``/``0x66`` are shared with the ACPI EC subsystem
  and are not exclusively reserved by this driver.
