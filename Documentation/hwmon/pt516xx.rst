.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver pt516xx
====================

Supported chips:

  * Astera Labs PT5161L

    Prefix: 'pt5161l'

    Addresses: I2C 0x24

    Datasheet: http://www.asteralabs.com/wp-content/uploads/2021/03/Astera_Labs_PT5161L_Product_Brief.pdf

Authors: Cosmo Chou <cosmo.chou@quantatw.com>

Description
-----------

This driver implements support for temperature monitoring of Astera Labs
PT5161L series PCIe retimer chips.

This driver implementation originates from the CSDK available at
https://github.com/facebook/openbmc/tree/helium/common/recipes-lib/retimer-v2.14
The communication protocol utilized is based on the I2C/SMBus standard.

For more detailed information and specific implementation details, it is
recommended to refer to the CSDK source code available at the provided GitHub
link.

Sysfs entries
----------------

================ ==============================================
temp1_input      Measured temperature (in millidegrees Celsius)
================ ==============================================

Debugfs entries
----------------

================ ====================================
fw_ver           Firmware version of the retimer
health           Health status (8 bits)
                 [0]: Heartbeat Okay (1'b1: OK)
                 [1]: Firmware loaded Okay (1'b1: OK)
                 [7:2]: Reserved
================ ====================================
