.. SPDX-License-Identifier: GPL-2.0-only

Kernel driver qcom-hamoa-ec
==========================

Supported systems:

  * Lenovo Yoga Slim 7x (14Q8X9)

    Prefix: 'qcom_ec'

    I2C address: 0x76, instantiated from the device tree. No scanning.

Description
-----------

The Yoga Slim 7x embedded controller provides a fan speed channel and a
thermistor through a legacy interface. It does not provide the thermal
capability response used by the Qualcomm reference-board interface.

On this system the driver only reads sensors. Cooling remains under firmware
control, including when the driver is unloaded. The driver does not change
fan curves, fan profiles, PWM settings, SCI events or EC standby state.
The exposed RPM channel does not establish the number of physical fans.

The hwmon interface is selected by the ``lenovo,yoga-slim7x-ec`` compatible.
It is not exposed on Qualcomm reference boards by this driver.

Sysfs attributes
----------------

All attributes below are read-only.

=============== ======================================================
fan1_input      Fan speed in RPM. Zero is valid when the fan is stopped.
temp1_input     EC thermistor temperature in millidegrees Celsius.
=============== ======================================================

An unavailable thermistor reading returns ``ENODATA``. A malformed fan
response or an incomplete bus transfer returns an error, not a zero speed.
