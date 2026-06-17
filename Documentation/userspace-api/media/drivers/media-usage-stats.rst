.. SPDX-License-Identifier: GPL-2.0

.. _media-usage-stats:

==========================
Media client usage stats
==========================

Stateless V4L2 codec drivers can optionally expose per-file-descriptor usage
statistics via ``/proc/<pid>/fdinfo/<fd>``. This is analogous to the DRM fdinfo
mechanism documented in :ref:`drm-client-usage-stats`, but uses the ``media-``
key prefix for V4L2 media devices.

This interface is specific to stateless (request API based) codec devices,
including both decoders and encoders. With stateless codecs, the kernel driver
explicitly submits each frame to the hardware and receives a completion
interrupt, providing a clean per-job boundary that can be attributed to the
submitting file descriptor.

Stateful codec devices cannot support this interface because their firmware
manages job scheduling internally. The kernel driver submits bitstream data
but has no visibility into per-frame hardware execution timing.

Implementation
==============

The V4L2 core provides the plumbing: drivers implement the ``show_fdinfo``
callback in ``struct v4l2_file_operations``, and the core wires it into the
kernel ``struct file_operations`` so that ``/proc/<pid>/fdinfo/<fd>`` output
includes the driver-provided keys.

File format specification
=========================

- File shall contain one key value pair per one line of text.
- Colon character (``:``) must be used to delimit keys and values.
- All standardised keys shall be prefixed with ``media-``.
- Driver-specific keys shall be prefixed with ``driver_name-``.

Mandatory keys
--------------

- media-driver: <valstr>

  String shall contain the name of the media driver.

- media-type: <valstr>

  String shall identify the type of media engine exposed through this file
  descriptor. Standard values are ``decoder`` and ``encoder``.

Utilization keys
----------------

- media-engine-usage: <uint> ns

  Time in nanoseconds that the hardware engine spent busy processing work
  belonging to this file descriptor. The engine being measured is identified
  by the ``media-type`` key.

  Values are not required to be constantly monotonic if it makes the driver
  implementation easier, but are required to catch up with the previously
  reported larger value within a reasonable period.

Frequency keys
--------------

- media-maxfreq: <uint> Hz

  Maximum operating frequency of the main engine clock.

- media-curfreq: <uint> Hz

  Current operating frequency of the main engine clock.

Example output
==============

::

  media-driver:           hantro-vpu
  media-type:             decoder
  media-engine-usage:     123456789 ns
  media-maxfreq:          600000000 Hz
  media-curfreq:          600000000 Hz
