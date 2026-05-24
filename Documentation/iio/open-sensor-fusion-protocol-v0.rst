.. SPDX-License-Identifier: GPL-2.0-only

Open Sensor Fusion protocol v0
==============================

This document describes the OSF0 UART wire format used by the Linux IIO
driver. It is not a firmware programming interface.

Device model
------------

An Open Sensor Fusion UART device is a sensor aggregation device. It sends
binary frames from the device to the host. The host driver decodes the frames
and maps supported sensors to IIO devices.

The hardware used for smoke testing is an OSF GREEN prototype with an
STM32F405RGT6 MCU, an ICM42688P-class IMU, and an MMC5983MA magnetometer. That
hardware is a test target, not part of the binding ABI.

Transport
---------

The transport is UART at 115200 baud, 8 data bits, no parity, and 1 stop bit.
The Linux transport is serdev. The v0 upstream driver covers device-to-host
frames. Flow control is not used by the tested stream.

Byte order
----------

All multi-byte integer fields are little-endian. Samples use signed 32-bit
little-endian integers when ``sample_format`` is ``S32``.

Frame format
------------

Each frame has a fixed 38-byte header, a payload, and a 4-byte CRC.

.. list-table::
   :header-rows: 1

   * - Offset
     - Size
     - Field
     - Description
   * - 0
     - 4
     - magic
     - ASCII ``OSF0``
   * - 4
     - 1
     - protocol_major
     - Must be ``0``
   * - 5
     - 1
     - protocol_minor
     - Minor version
   * - 6
     - 2
     - header_len
     - Must be ``38``
   * - 8
     - 2
     - message_type
     - Message type
   * - 10
     - 4
     - payload_len
     - Payload length in bytes
   * - 14
     - 8
     - sequence
     - Monotonic device sequence
   * - 22
     - 8
     - timestamp_us
     - Device timestamp in microseconds
   * - 30
     - 4
     - flags
     - Message flags
   * - 34
     - 4
     - reserved
     - Must be zero for v0
   * - 38
     - payload_len
     - payload
     - Message payload
   * - 38 + payload_len
     - 4
     - crc32
     - CRC32 over header and payload

The frame CRC is IEEE CRC32 as implemented by ``crc32_le()`` with initial
value ``0xffffffff`` and final XOR value ``0xffffffff``. The CRC field is not
included in the CRC input.

Message types
-------------

.. list-table::
   :header-rows: 1

   * - Value
     - Name
     - Direction
   * - ``0x0001``
     - ``SENSOR_SAMPLE``
     - device to host
   * - ``0x0002``
     - ``DEVICE_STATUS``
     - device to host
   * - ``0x0003``
     - ``CAPABILITY_REPORT``
     - device to host

Message types ``0x7f00`` through ``0x7fff`` are reserved. Values at or above
``0x8000`` are vendor private and are ignored by the upstream driver.

``SENSOR_SAMPLE`` payload
-------------------------

The base payload size is 16 bytes.

.. list-table::
   :header-rows: 1

   * - Offset
     - Size
     - Field
     - Description
   * - 0
     - 2
     - sensor_type
     - Sensor type ID
   * - 2
     - 2
     - sensor_index
     - Instance index
   * - 4
     - 2
     - channel_count
     - Number of S32 channels
   * - 6
     - 2
     - sample_format
     - Must be ``1`` (``S32``)
   * - 8
     - 4
     - scale_nano
     - Scale factor in nano-units
   * - 12
     - 4
     - reserved
     - Must be zero for v0
   * - 16
     - 4 * channel_count
     - samples
     - Signed 32-bit channel samples

``DEVICE_STATUS`` payload
-------------------------

The payload size is 20 bytes. Fields are ``uptime_s``, ``status_flags``,
``error_flags``, ``dropped_frames``, and a reserved field. Each field is
32 bits.

``CAPABILITY_REPORT`` payload
-----------------------------

The base payload size is 4 bytes. It contains ``capability_count`` and a
reserved field. Each capability entry is 20 bytes:

.. list-table::
   :header-rows: 1

   * - Offset
     - Size
     - Field
     - Description
   * - 0
     - 2
     - sensor_type
     - Sensor type ID
   * - 2
     - 2
     - sensor_index
     - Instance index
   * - 4
     - 2
     - channel_count
     - Number of channels
   * - 6
     - 2
     - sample_format
     - Must be ``1`` (``S32``)
   * - 8
     - 4
     - scale_nano
     - Scale factor in nano-units
   * - 12
     - 4
     - flags
     - Capability flags
   * - 16
     - 4
     - reserved
     - Must be zero for v0

Capability flag bit 0 means enabled by default. Bit 1 means calibrated data can
be provided by the device. Other bits are invalid for v0.

Sensor type IDs
---------------

.. list-table::
   :header-rows: 1

   * - Value
     - Sensor
     - IIO mapping
   * - ``0x0001``
     - accelerometer
     - ``IIO_ACCEL``, X/Y/Z
   * - ``0x0002``
     - gyroscope
     - ``IIO_ANGL_VEL``, X/Y/Z
   * - ``0x0003``
     - magnetometer
     - ``IIO_MAGN``, X/Y/Z
   * - ``0x0004``
     - barometer
     - not mapped in the initial driver
   * - ``0x0005``
     - temperature
     - ``IIO_TEMP``
   * - ``0x0006``
     - humidity
     - not mapped in the initial driver
   * - ``0x0007``
     - ambient light
     - not mapped in the initial driver
   * - ``0x0008``
     - proximity
     - not mapped in the initial driver

Scaling
-------

``scale_nano`` is the per-channel scale value in nano-units. The Linux driver
maps it to ``IIO_CHAN_INFO_SCALE`` as integer plus nano. The exact physical
unit depends on the IIO channel type.

Timestamps
----------

The frame header carries ``timestamp_us``, a device-side timestamp in
microseconds. v0 transports this value for ordering and diagnostics. The driver
does not use it as a production-grade host-correlated timestamp. Buffered IIO
timestamps follow IIO timestamp clock handling.

Non-goals for v0 upstream
-------------------------

The v0 upstream driver does not include USB transport, fusion output,
AHRS/Kalman output, calibration command ABI, custom sysfs control surface,
production timestamp correlation, or runtime capability removal.
