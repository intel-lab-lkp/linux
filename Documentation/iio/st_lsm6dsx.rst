.. SPDX-License-Identifier: GPL-2.0-only

=================
ST LSM6DSX driver
=================

Device driver for STMicroelectronics LSM6DSx, LSM9DS1, ASM330, and ISM330 series
of 6-axis inertial measurement units. The core module is called ``st_lsm6dsx``,
and the transport-specific modules are called ``st_lsm6dsx_i2c``,
``st_lsm6dsx_spi``, and ``st_lsm6dsx_i3c``.

IIO devices
===========

This driver instantiates multiple IIO devices:

* accelerometer (IIO_ACCEL channel)
* gyroscope (IIO_ANGL_VEL channel)
* (optionally) magnetometer (IIO_MAGN channel), if the device has a secondary
  I2C interface connected to a slave sensor device (sensor hub functionality)
* (optionally) sensor fusion (IIO_CUSTOM channel), which combines acceleration
  and angular velocity data

Sensor Fusion
-------------

Some chips supported by this driver implement an internal algorithm that takes
input data from the accelerometer and gyroscope, and calculates the device
orientation in 3D space, which is then made available in the hardware FIFO.
Orientation is expressed in the form of a 4-dimensional quaternion vector, whose
components are typically represented in an array as ``[x, y, z, w]``.
The sensor device outputs the ``[x, y, z]`` components of the quaternion,
expressed as half-precision (16-bit) floating-point numbers.

The ``z`` component is not output by the device, but its value can be derived
from the rest of the data, due to the following constraints:

* the quaternion vector is normalized, i.e. :math:`x^2 + y^2 + z^2 + w^2 = 1`
* the rotation angle :math:`\theta` remains within
  :math:`[-180^\circ, 180^\circ]`, i.e. the ``w`` component is non-negative:
  :math:`w = \cos(\theta/2) \geq 0`

These constraints allow the ``w`` value to be calculated from the other
components: :math:`w = \sqrt{1 - (x^2 + y^2 + z^2)}`.
