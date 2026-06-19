.. SPDX-License-Identifier: GPL-2.0

Kernel driver stratix10-hwmon
=============================

Supported chips:

 * Altera Stratix 10 SoC FPGA

Authors:
      - Nazim Amirul <muhammad.nazim.amirul.nazle.asmade@altera.com>
      - Tze Yee Ng <tze.yee.ng@altera.com>

Description
-----------

This driver supports hardware monitoring for Altera Stratix 10 SoC FPGA
devices through the Secure Device Manager and Stratix 10 service layer.

The following sensor types are supported:

  * temperature
  * voltage

Usage Notes
-----------

The driver relies on a device tree node to enumerate sensors present on the
specific device. See
Documentation/devicetree/bindings/hwmon/altr,stratix10-hwmon.yaml for details
of the device-tree node.
