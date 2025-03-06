.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later

.. _v4l2-meta-fmt-uvc-custom:

*******************************
V4L2_META_FMT_UVC_CUSTOM ('UVCC')
*******************************

UVC Custom Payload Metadata.


Description
===========

V4L2_META_FMT_UVC_CUSTOM buffers follow the metadata buffer layout of
V4L2_META_FMT_UVC with the only difference that it includes all the UVC
metadata, not just the first 2-12 bytes.

The most common metadata format is the one proposed by Microsoft(R)'s UVC
extension [1_], but other vendors might have different formats.

Applications might use information from the Hardware Database (hwdb)[2_] to
process the camera's metadata accordingly.

.. _1:

[1] https://docs.microsoft.com/en-us/windows-hardware/drivers/stream/uvc-extensions-1-5

.. _2:
[2] https://www.freedesktop.org/software/systemd/man/latest/hwdb.html
