.. SPDX-License-Identifier: GPL-2.0-only

Open Sensor Fusion
==================

Open Sensor Fusion is a sensor aggregation hub interface. The Linux IIO driver
receives OSF protocol frames from an attached device and registers matching IIO
devices for the sensor classes supported by the driver. The actual sensor
channels are discovered at runtime from mandatory OSF capability reports.

This document is a driver-facing overview for the Linux IIO mapping. The full
wire protocol, firmware behavior, and hardware model details belong in the Open
Sensor Fusion project documentation.

Device Model
------------

An OSF device sends binary frames from the device to the host. Devices using the
``opensensorfusion,osf`` compatible are expected to provide
``CAPABILITY_REPORT`` messages so the host can discover which sensor streams are
available. Device Tree describes the attached OSF sensor aggregation hub; it does
not enumerate the individual sensors discovered at runtime.

The currently supported Linux subset exposes:

* accelerometer samples as ``IIO_ACCEL`` X/Y/Z channels,
* gyroscope samples as ``IIO_ANGL_VEL`` X/Y/Z channels,
* magnetometer samples as ``IIO_MAGN`` X/Y/Z channels, and
* temperature samples as ``IIO_TEMP``.

Protocol Scope
---------------

The driver supports OSF protocol major version 0 for the initial IIO receive
path. The current wire magic is ``OSF0``; that string is a wire-format detail and
is not the Linux driver identity. Device Tree keeps the generic
``opensensorfusion,osf`` compatible rather than naming a product such as OSF
GREEN or a wire magic value.

Protocol versioning is carried by the ``protocol_major`` and ``protocol_minor``
fields at fixed offsets in the OSF frame header. The driver currently
supports ``protocol_major`` 0. ``protocol_minor`` changes within major version
0 are intended to remain backward-compatible within the fixed header layout.
Incompatible wire-format changes require a new ``protocol_major``. A future
device that cannot expose compatible version discovery through that fixed
header layout would need a different Device Tree compatible.

The initial Linux driver handles device-to-host frames for:

* ``SENSOR_SAMPLE`` buffered and direct-mode sample data,
* ``CAPABILITY_REPORT`` based IIO device registration, and
* ``DEVICE_STATUS`` cache updates.

Vendor-private message types are ignored. Command transport, calibration
control ABI, fusion output ABI, and runtime capability removal are outside the
initial Linux IIO receive path.

Timestamps
----------

OSF frames include a device-side ``timestamp_us`` field. Buffered IIO samples use
an IIO timestamp captured on the host when samples are pushed to IIO buffers.
The initial driver does not correlate the device timestamp with the host IIO
clock.

Compatibility Notes
-------------------

The project protocol documentation should define the compatibility rules for
reserved fields, optional flags, and trailing extension data. Until those rules
are finalized, the Linux decoder keeps conservative bounds checks around the
currently supported message layouts.
