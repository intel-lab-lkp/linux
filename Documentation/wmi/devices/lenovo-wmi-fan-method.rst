.. SPDX-License-Identifier: GPL-2.0-or-later

==================================
Lenovo Fan Method WMI Driver
==================================

WMI GUID ``92549549-4BDE-4F06-AC04-CE8BF898DBAA``

The Lenovo Fan Method interface provides a ten-point firmware fan table on
Legion Go 8APU1, Legion Go 8ASP2, Legion Go 8AHP2, Legion Go S 8ARP1, and
Legion Go S 8APU1 products.

The driver adds ``pwm1_auto_point1_*`` through
``pwm1_auto_point10_*`` to the HWMON device that the Lenovo Other Mode driver
owns. The temperature attributes are fixed and read-only. They contain points
from 10 through 100 degrees Celsius in 10-degree steps.

Each ``pwm1_auto_point*_pwm`` attribute is read-write and passes a firmware
control value from 0 through 255 without scaling. On the tested Legion Go
8APU1, the hardware responds from 0 through 115, corresponding to Lenovo's
0 through 115 percent range. Lenovo software shows 0 through 100 percent to
the user, and 115 percent reaches the RPM observed in Full Speed mode. The
firmware accepts values through 255, but values above 115 caused no observed
RPM increase.

A control-value read validates both ten-entry tables before returning data. A
point write reads the current table, changes one control value, and submits the
complete request. It preserves the other nine control values and all returned
temperatures.

The Fan Method and Other Mode drivers use separate modules and WMI devices.
The component framework associates devices that belong to the same WMI
provider. Fan Method curve attributes are absent when either interface is not
available.
