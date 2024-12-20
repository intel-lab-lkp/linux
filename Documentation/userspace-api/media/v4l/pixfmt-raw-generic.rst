.. SPDX-License-Identifier: GPL-2.0 OR GFDL-1.1-no-invariants-or-later

************************************************************************************************************************************
V4L2_PIX_FMT_RAW_8 ('RAW8'), V4L2_PIX_FMT_RAW_CSI2_10 ('RACA'), V4L2_PIX_FMT_RAW_CSI2_12 ('RACC'), V4L2_PIX_FMT_RAW_CSI2_14 ('RACE')
************************************************************************************************************************************


Generic line-based raw image data formats


Description
===========

These generic raw image data formats define the memory layout of the data without
defining the order of the colour components or even the colour components
themselves.

.. _v4l2-pix-fmt-raw-8:

V4L2_PIX_FMT_RAW_8
------------------

The V4L2_PIX_FMT_RAW_8 format is a plain 8-bit raw pixel data format. This
format is used on CSI-2 for 8 bits per :term:`Data Unit`.

**Byte Order Of V4L2_PIX_FMT_RAW_8.**
Each cell is one byte. "P" denotes a pixel.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|

.. flat-table:: Sample 4x2 Image Frame
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8

    * - start + 0:
      - P\ :sub:`00`
      - P\ :sub:`10`
      - P\ :sub:`20`
      - P\ :sub:`30`
    * - start + 4:
      - P\ :sub:`01`
      - P\ :sub:`11`
      - P\ :sub:`21`
      - P\ :sub:`31`

.. _v4l2-pix-fmt-raw-csi2-10:

V4L2_PIX_FMT_RAW_CSI2_10
------------------------

V4L2_PIX_FMT_RAW_CSI2_10 contains 10-bit packed image data, with four bytes
containing the top 8 bits of the pixels followed by lowest 2 bits of the pixels
packed into one byte. This format is typically used by CSI-2 receivers with
a source that transmits MEDIA_BUS_FMT_RAW_10 and the CSI-2 receiver writes the
received data to memory as-is.

The packing of the data follows the MIPI CSI-2 specification.

**Byte Order Of V4L2_PIX_FMT_RAW_CSI2_10.**
Each cell is one byte. "P" denotes a pixel.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{2cm}|

.. flat-table:: Sample 4x2 Image Frame
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8 8

    * - start + 0:
      - P\ :sub:`00 bits 9--2`
      - P\ :sub:`10 bits 9--2`
      - P\ :sub:`20 bits 9--2`
      - P\ :sub:`30 bits 9--2`
      - P\ :sub:`00 bits 1--0` (bits 1--0),
        P\ :sub:`10 bits 1--0` (bits 3--2),
        P\ :sub:`20 bits 1--0` (bits 5--4),
        P\ :sub:`30 bits 1--0` (bits 7--6)
    * - start + 5:
      - P\ :sub:`01 bits 9--2`
      - P\ :sub:`11 bits 9--2`
      - P\ :sub:`21 bits 9--2`
      - P\ :sub:`31 bits 9--2`
      - P\ :sub:`01 bits 1--0` (bits 1--0),
        P\ :sub:`11 bits 1--0` (bits 3--2),
        P\ :sub:`21 bits 1--0` (bits 5--4),
        P\ :sub:`31 bits 1--0` (bits 7--6)

.. _v4l2-pix-fmt-raw-csi2-12:

V4L2_PIX_FMT_RAW_CSI2_12
------------------------

V4L2_PIX_FMT_RAW_CSI2_12 contains 12-bit packed image data, with two bytes
containing the top 8 bits of the pixels followed by lowest 4 bits of the pixels
packed into 1 byte. This format is typically used by CSI-2 receivers with
a source that transmits MEDIA_BUS_FMT_RAW_12 and the CSI-2 receiver writes the
received data to memory as-is.

The packing of the data follows the MIPI CSI-2 specification.

**Byte Order Of V4L2_PIX_FMT_RAW_CSI2_12.**
Each cell is one byte. "P" denotes a pixel.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{1.2cm}|p{2cm}|p{1.2cm}|p{1.2cm}|p{2cm}|

.. flat-table:: Sample 4x2 Image Frame
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8 8 8

    * - start + 0:
      - P\ :sub:`00 bits 11--4`
      - P\ :sub:`10 bits 11--4`
      - P\ :sub:`00 bits 3--0` (bits 3--0),
        P\ :sub:`10 bits 3--0` (bits 7--4)
      - P\ :sub:`20 bits 11--4`
      - P\ :sub:`30 bits 11--4`
      - P\ :sub:`20 bits 3--0` (bits 3--0),
        P\ :sub:`30 bits 3--0` (bits 7--4)
    * - start + 6:
      - P\ :sub:`01 bits 11--4`
      - P\ :sub:`11 bits 11--4`
      - P\ :sub:`01 bits 3--0` (bits 3--0),
        P\ :sub:`11 bits 3--0` (bits 7--4)
      - P\ :sub:`21 bits 11--4`
      - P\ :sub:`31 bits 11--4`
      - P\ :sub:`21 bits 3--0` (bits 3--0),
        P\ :sub:`31 bits 3--0` (bits 7--4)

.. _v4l2-pix-fmt-raw-csi2-14:

V4L2_PIX_FMT_RAW_CSI2_14
------------------------

V4L2_PIX_FMT_RAW_CSI2_14 contains 14-bit packed image data, with four bytes
containing the top 8 bits of the pixels followed by lowest 6 bits of the pixels
packed into three bytes. This format is typically used by CSI-2 receivers with a
source that transmits MEDIA_BUS_FMT_RAW_14 and the CSI-2 receiver writes the
received data to memory as-is.

The packing of the data follows the MIPI CSI-2 specification.

**Byte Order Of V4L2_PIX_FMT_RAW_CSI2_14.**
Each cell is one byte. "P" denotes a pixel.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{2cm}|p{2cm}|p{2cm}|

.. flat-table:: Sample 4x2 Image Frame
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8 8 8 8

    * - start + 0:
      - P\ :sub:`00 bits 13--6`
      - P\ :sub:`10 bits 13--6`
      - P\ :sub:`20 bits 13--6`
      - P\ :sub:`30 bits 13--6`
      - P\ :sub:`10 bits 1--0` (bits 7--6),
        P\ :sub:`00 bits 5--0` (bits 5--0)
      - P\ :sub:`20 bits 3--0` (bits 7--4),
        P\ :sub:`10 bits 5--2` (bits 3--0)
      - P\ :sub:`30 bits 5--0` (bits 7--2),
        P\ :sub:`20 bits 5--4` (bits 1--0)
    * - start + 7:
      - P\ :sub:`01 bits 13--6`
      - P\ :sub:`11 bits 13--6`
      - P\ :sub:`21 bits 13--6`
      - P\ :sub:`31 bits 13--6`
      - P\ :sub:`11 bits 1--0` (bits 7--6),
        P\ :sub:`01 bits 5--0` (bits 5--0)
      - P\ :sub:`21 bits 3--0` (bits 7--4),
        P\ :sub:`11 bits 5--2` (bits 3--0)
      - P\ :sub:`31 bits 5--0` (bits 7--2),
        P\ :sub:`21 bits 5--4` (bits 1--0)
