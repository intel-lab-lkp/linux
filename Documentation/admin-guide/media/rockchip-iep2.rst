.. SPDX-License-Identifier: GPL-2.0

Rockchip IEP2 deinterlacer driver
=================================

The rockchip-iep2 driver supports the IEP2 deinterlacer of the RK3568 and
RK3588 as a V4L2 mem2mem device. OUTPUT takes interlaced frames, CAPTURE
returns progressive frames in the same format. For
``V4L2_FIELD_INTERLACED`` the field order follows the height: bottom field
first below 576 lines, top field first otherwise.

``V4L2_CID_DEINTERLACING_MODE (menu)``
    The rate cannot change while a queue is streaming (``EBUSY``).

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 4

    * - ``"Bob, Frame Rate" (0)``
      - One CAPTURE buffer per OUTPUT buffer. Default.
    * - ``"Bob, Field Rate" (1)``
      - Two CAPTURE buffers per OUTPUT buffer.
    * - ``"Motion Adaptive, Frame Rate" (2)``
      - Uses the previous OUTPUT buffer as reference, so at least two
        OUTPUT buffers are needed. One CAPTURE buffer per OUTPUT buffer.
    * - ``"Motion Adaptive, Field Rate" (3)``
      - As above, two CAPTURE buffers per OUTPUT buffer.

The hardware reads past the end of the source image, so the OUTPUT
``sizeimage`` is 4 KiB larger than the image and OUTPUT buffers must be at
least that size.
