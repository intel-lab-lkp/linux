.. SPDX-License-Identifier: GPL-2.0

Kernel driver tids
===================

Supported Chips:

  * WSEN TIDS

    Prefix: 'tids'

    Addresses scanned: None

    Datasheet:

      English: https://www.we-online.com/components/products/manual/Manual-um-wsen-tids-2521020222501%20(rev1.2).pdf

Author: Thomas Marangoni <Thomas.Marangoni@becom-group.com>


Description
-----------

This driver implements support for the WSEN TIDS chip, a temperature
sensor. Temperature is measured in degree celsius. In sysfs interface,
all values are scaled by 1000, i.e. the value for 31.5 degrees celsius is 31500.

Usage Notes
-----------

The device communicates with the I2C protocol. Sensors can have the I2C
address 0x38 or 0x3F. See Documentation/i2c/instantiating-devices.rst for methods
to instantiate the device.

Sysfs entries
-------------

=============== ============================================
temp1_input     Measured temperature in millidegrees Celsius
update_interval The interval for polling the sensor, in
                milliseconds. Writable. Supported values are
                5, 10, 20 or 40.
temp1_max       The temperature in millidegrees Celsius, that
                is triggering the temp1_max_alarm. Writable.
                The lowest supported value is -39680 and the
                highest supported value is 122880. Values are
                saved in steps of 640.
temp1_min       The temperature in millidegrees Celsius, that
                is triggering the temp1_min_alarm. Writable.
                The lowest supported value is -39680 and the
                highest supported value is 122880. Values are
                saved in steps of 640.
temp1_max_alarm The alarm will be triggered when the level
                reaches the value specified in
                temp1_max. It will reset automatically
                once it has been read.
temp1_min_alarm The alarm will be triggered when the level
                reaches the value specified in
                temp1_min. It will reset automatically
                once it has been read.
=============== ============================================
