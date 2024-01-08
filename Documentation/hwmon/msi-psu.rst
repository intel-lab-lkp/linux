.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver msi-psu
=========================

Supported devices:

* MSI MEG Ai1300P

* MSI MEG Ai1000P

Author: Jack Doan

Description
-----------

This driver provides a sysfs interface for MSI PSUs with a HID monitoring
interface.

Measurements for the output voltage and current for each rail are provided,
as well as total output power, temperature, and fan control.

Additional properties are available in debugfs, such as an efficiency
measurement, and switching to/from 12V multi-rail mode

Sysfs entries
-------------

============    ===============================================================
curr1_input     Current on the 12v psu rail
curr2_input     Current on the 5v psu rail
curr3_input     Current on the 3.3v psu rail
fan1_input      RPM of psu fan
in0_input       Voltage of the psu ac input
in1_input       Voltage of the 12v psu rail
in2_input       Voltage of the 5v psu rail
in3_input       Voltage of the 3.3v psu rail
power1_input    Total power usage
pwm1            PWM value for fan1. Writes to this file will switch set
                pwm1_enable to manual control mode.
pwm1_enable     PWM mode for fan1. (1) means "auto", and uses the built-in fan
                curve. (3) means manual control
temp1_input     Temperature of the psu
============    ===============================================================

Usage Notes
-----------

It is an USB HID device, so it is auto-detected, supports hot-swapping and
several devices at once.

Debugfs entries
---------------

====================    ========================================================
multi_rail_enabled      Single or multi rail mode of the PCIe power connectors
efficiency              An efficiency measurement, expressed as per-ten-thousand
                        (ex: 8512 == 85.12%)
product                 Product name of the psu
revision                Revision number of the psu
uptime                  Session uptime of the psu
uptime_total            Total uptime of the psu
vendor                  Vendor name of the psu
====================    ========================================================
