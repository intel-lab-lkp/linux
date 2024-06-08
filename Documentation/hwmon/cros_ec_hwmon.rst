.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver cros_ec_hwmon
===========================

Supported chips:

  * ChromeOS embedded controllers.

    Prefix: 'cros_ec'

    Addresses scanned: -

Author:

  - Thomas Weißschuh <linux@weissschuh.net>

Description
-----------

This driver implements support for hardware monitoring commands exposed by the
ChromeOS embedded controller used in Chromebooks and other devices.

The channel labels exposed via hwmon are retrieved from the EC itself.

Supported features:

  - Current fan speed
  - Target fan speed (for fan 1 only)
  - PWM-based fan control
  - Current temperature
