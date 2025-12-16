.. SPDX-License-Identifier: GPL-2.0
Kernel driver altr-hwmon
=========================

Supported chips:

 * Intel N5X
 * Stratix10
 * Agilex
 * Agilex5

Contributor: Kris Chaplin <kris.chaplin@intel.com>
             Khairul Anuar Romli <khairul.anuar.romli@altera.com>
             Muhammad Amirul Asyraf Mohamad Jamian <muhammad.amirul.asyraf.mohamad.jamian@altera.com>

Description
-----------

This driver supports hardware monitoring for 64-Bit SoC FPGA and eASIC devices
based around the Secure Device Manager and Stratix 10 Service layer.

The following sensor types are supported

  * temperature
  * voltage


Usage Notes
-----------

The driver relies on a device tree node to enumerate support present on the
specific device. See Documentation/devicetree/bindings/hwmon/altr,socfpga-hwmon.yaml for details of the device-tree node.
