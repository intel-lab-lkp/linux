.. SPDX-License-Identifier: GPL-2.0-only

.. _mali-c720-controls:

Arm Mali-C720 ISP driver
========================

The Arm Mali-C720 ISP driver implements the following driver-specific control:

``V4L2_CID_MALI_C720_STATS_CAPABILITIES (__u16 array)``
    Lists the statistics block types supported by the ISP. Each element is a
    value from :c:type:`mali_c720_stats_type`.

The Mali-C720 ISP can be configured to include or exclude statistics blocks.
This control allows the driver to communicate to userspace which statistics
block types are supported by the driver and present in the hardware.

Format enumeration
------------------

The available image formats depend on the output pipeline. For example, the
``mali-c720 fr0-rgb`` capture device supports RGB formats, while the
``mali-c720 fr0-yuv`` capture device supports YUV formats. Future hardware
versions may also expose different supported formats. Applications must select
an image format compatible with the media bus format configured on the connected
subdevice's source pad. Applications should enumerate the formats supported by
each video device using :ref:`VIDIOC_ENUM_FMT <VIDIOC_ENUM_FMT>`.

Buffer alignment
----------------

Image capture buffers require DMA base addresses aligned to 32 bytes.
Statistics capture buffers require DMA base addresses aligned to 16 bytes.

Buffers allocated by the driver using MMAP satisfy these requirements
automatically. Imported DMA-BUFs must provide suitable alignment.
The driver rejects buffers whose DMA base addresses do not meet these
requirements.

Image strides must also be aligned to 32 bytes. Format negotiation adjusts
``bytesperline`` accordingly, and applications must use the returned value.
