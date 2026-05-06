.. SPDX-License-Identifier: GPL-2.0

Kernel driver prom21-hwmon
==========================

Supported chips:

  * AMD PROM21 xHCI

    Prefix: 'prom21_hwmon'

    PCI ID: 1022:43fd

Author:

  - Jihong Min <hurryman2212@gmail.com>

Description
-----------

This driver exposes the temperature sensor in AMD PROM21 xHCI controllers.

The driver binds to an auxiliary device created by the xHCI PCI driver for
supported controllers. The sensor value is accessed through a vendor-specific
index/data register pair in the controller's PCI MMIO BAR.

Since the xHCI controllers are integrated in PROM21, this temperature can also
be used as a monitor for a temperature close to the AMD chipset temperature.

Register access
---------------

The temperature value is read through a vendor-specific index/data register
pair in the xHCI PCI MMIO BAR. The driver uses the following byte offsets from
the MMIO BAR base:

======================= =====================================================
0x3000			Vendor index register
0x3008			Vendor data register
======================= =====================================================

The driver saves the current vendor index register value, writes the
temperature selector ``0x0001e520`` to the vendor index register, reads the
vendor data register, and restores the previous vendor index value before
returning. The raw temperature value is the low 8 bits of the vendor data
register value.

No public AMD reference is available for the raw value. The temperature
conversion formula is derived from observed PROM21 xHCI temperature readings:

  temp[C] = raw * 0.9066 - 78.624

Module parameters
-----------------

allow_pm_switch: bool
  Allow temperature reads to wake the xHCI PCI device. This is enabled by
  default. If disabled, the driver does not wake the xHCI PCI device from a
  temperature read. It reads the temperature only when the device is not
  suspended. A read from a suspended device returns ``-EAGAIN``.

Sysfs entries
-------------

======================= =====================================================
temp1_input		Temperature in millidegrees Celsius
temp1_label		"xHCI"
======================= =====================================================

The hwmon device name is ``prom21_hwmon``. The sysfs path depends on the hwmon
device number assigned by the kernel. Userspace can locate the device by
matching the ``name`` attribute:

.. code-block:: sh

   for hwmon in /sys/class/hwmon/hwmon*; do
           [ "$(cat "$hwmon/name")" = "prom21_hwmon" ] || continue
           cat "$hwmon/temp1_label"
           cat "$hwmon/temp1_input"
   done

``temp1_input`` reports millidegrees Celsius, so a value of ``50113`` means
50.113 degrees Celsius. If the raw register value is invalid, ``temp1_input``
returns ``-ENODATA``.
