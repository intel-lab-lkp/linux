.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver POWERZ
====================

Supported chips:

  * ChargerLAB POWER-Z KM003C

    Prefix: 'powerz'

    Addresses scanned: -

Author:

  - Thomas Weißschuh <linux@weissschuh.net>

Description
-----------

This driver implements support for the ChargerLAB POWER-Z USB-C power testing
family.

The device communicates with the custom protocol over USB.

The channel labels exposed via hwmon match the labels used by the on-device
display and the official POWER-Z PC software.
