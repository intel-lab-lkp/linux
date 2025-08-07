.. SPDX-License-Identifier: GPL-2.0 OR GFDL-1.1-no-invariants-or-later

****************************************************************************************************************************************************************************************************************************************************************************************************************************************************************
V4L2_PIX_FMT_RAW_8 ('RAW8'), V4L2_PIX_FMT_RAW_CSI2_10 ('RACA'), V4L2_PIX_FMT_RAW_CSI2_12 ('RACC'), V4L2_PIX_FMT_RAW_CSI2_14 ('RACE'), V4L2_PIX_FMT_RAW_10 ('RAWA'), V4L2_PIX_FMT_RAW_12 ('RAWC'), V4L2_PIX_FMT_RAW_14 ('RAWE'), V4L2_PIX_FMT_RAW_16 ('RAWG'), V4L2_PIX_FMT_RAW_CSI2_20 ('RACK'), V4L2_PIX_FMT_RAW_24 ('RAWO'), V4L2_PIX_FMT_RAW_CSI2_28 ('RACS')
****************************************************************************************************************************************************************************************************************************************************************************************************************************************************************


Generic line-based raw image data formats


Description
===========

These generic raw image data formats define the memory layout of the data without
defining the order of the colour components or even the colour components
themselves. These formats may only be used with a Media Controller pipeline where
the more specific format is reported by the :ref:`V4L2_CID_CFA_PATTERN
<image-source-control-cfa-pattern>` control on the source sub-device.

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

.. _v4l2-pix-fmt-raw-10:

V4L2_PIX_FMT_RAW_10
-------------------

V4L2_PIX_FMT_RAW_10 contains 10-bit image data, with each two consecutive
bytes forming a pixel value. This format is typically used by CSI-2 receivers
with a source that transmits MEDIA_BUS_FMT_RAW_10 and the CSI-2 receiver writes
the received data to memory unpacked.

**Byte Order Of V4L2_PIX_FMT_RAW_10.**
Each cell is one byte. "P" denotes a pixel.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|

.. flat-table:: Sample 4x2 Image Frame
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8 8 8 8 8

    * - start + 0:
      - P\ :sub:`00 bits 7--0`
      - P\ :sub:`00 bits 9--8` (bits 1--0)
      - P\ :sub:`10 bits 7--0`
      - P\ :sub:`10 bits 9--8` (bits 1--0)
      - P\ :sub:`20 bits 7--0`
      - P\ :sub:`20 bits 9--8` (bits 1--0)
      - P\ :sub:`30 bits 7--0`
      - P\ :sub:`30 bits 9--8` (bits 1--0)
    * - start + 8:
      - P\ :sub:`01 bits 7--0`
      - P\ :sub:`01 bits 9--8` (bits 1--0)
      - P\ :sub:`11 bits 7--0`
      - P\ :sub:`11 bits 9--8` (bits 1--0)
      - P\ :sub:`21 bits 7--0`
      - P\ :sub:`21 bits 9--8` (bits 1--0)
      - P\ :sub:`31 bits 7--0`
      - P\ :sub:`31 bits 9--8` (bits 1--0)

.. _v4l2-pix-fmt-raw-12:

V4L2_PIX_FMT_RAW_12
-------------------

V4L2_PIX_FMT_RAW_12 contains 12-bit image data, with each two consecutive
bytes forming a pixel value. This format is typically used by CSI-2 receivers
with a source that transmits MEDIA_BUS_FMT_RAW_12 and the CSI-2 receiver writes
the received data to memory unpacked.

**Byte Order Of V4L2_PIX_FMT_RAW_12.**
Each cell is one byte. "P" denotes a pixel.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|

.. flat-table:: Sample 4x2 Image Frame
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8 8 8 8 8

    * - start + 0:
      - P\ :sub:`00 bits 7--0`
      - P\ :sub:`00 bits 11--8` (bits 3--0)
      - P\ :sub:`10 bits 7--0`
      - P\ :sub:`10 bits 11--8` (bits 3--0)
      - P\ :sub:`20 bits 7--0`
      - P\ :sub:`20 bits 11--8` (bits 3--0)
      - P\ :sub:`30 bits 7--0`
      - P\ :sub:`30 bits 11--8` (bits 3--0)
    * - start + 8:
      - P\ :sub:`01 bits 7--0`
      - P\ :sub:`01 bits 11--8` (bits 3--0)
      - P\ :sub:`11 bits 7--0`
      - P\ :sub:`11 bits 11--8` (bits 3--0)
      - P\ :sub:`21 bits 7--0`
      - P\ :sub:`21 bits 11--8` (bits 3--0)
      - P\ :sub:`31 bits 7--0`
      - P\ :sub:`31 bits 11--8` (bits 3--0)

.. _v4l2-pix-fmt-raw-14:

V4L2_PIX_FMT_RAW_14
-------------------

V4L2_PIX_FMT_RAW_14 contains 14-bit image data, with each two consecutive
bytes forming a pixel value. This format is typically used by CSI-2 receivers
with a source that transmits MEDIA_BUS_FMT_RAW_14 and the CSI-2 receiver writes
the received data to memory unpacked.

**Byte Order Of V4L2_PIX_FMT_RAW_14.**
Each cell is one byte. "P" denotes a pixel.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|

.. flat-table:: Sample 4x2 Image Frame
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8 8 8 8 8

    * - start + 0:
      - P\ :sub:`00 bits 7--0`
      - P\ :sub:`00 bits 13--8` (bits 5--0)
      - P\ :sub:`10 bits 7--0`
      - P\ :sub:`10 bits 13--8` (bits 5--0)
      - P\ :sub:`20 bits 7--0`
      - P\ :sub:`20 bits 13--8` (bits 5--0)
      - P\ :sub:`30 bits 7--0`
      - P\ :sub:`30 bits 13--8` (bits 5--0)
    * - start + 8:
      - P\ :sub:`01 bits 7--0`
      - P\ :sub:`01 bits 13--8` (bits 5--0)
      - P\ :sub:`11 bits 7--0`
      - P\ :sub:`11 bits 13--8` (bits 5--0)
      - P\ :sub:`21 bits 7--0`
      - P\ :sub:`21 bits 13--8` (bits 5--0)
      - P\ :sub:`31 bits 7--0`
      - P\ :sub:`31 bits 13--8` (bits 5--0)

.. _v4l2-pix-fmt-raw-16:

V4L2_PIX_FMT_RAW_16
-------------------

V4L2_PIX_FMT_RAW_16 contains 16-bit image data, with each two consecutive
bytes forming a pixel value. This format is typically used by CSI-2 receivers
with a source that transmits MEDIA_BUS_FMT_RAW_16 and the CSI-2 receiver writes
the received data to memory as-is.

The packing of the data follows the MIPI CSI-2 specification.

**Byte Order Of V4L2_PIX_FMT_RAW_16.**
Each cell is one byte. "P" denotes a pixel.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|

.. flat-table:: Sample 4x2 Image Frame
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8 8 8 8 8

    * - start + 0:
      - P\ :sub:`00 bits 7--0`
      - P\ :sub:`00 bits 15--8`
      - P\ :sub:`10 bits 7--0`
      - P\ :sub:`10 bits 15--8`
      - P\ :sub:`20 bits 7--0`
      - P\ :sub:`20 bits 15--8`
      - P\ :sub:`30 bits 7--0`
      - P\ :sub:`30 bits 15--8`
    * - start + 8:
      - P\ :sub:`01 bits 7--0`
      - P\ :sub:`01 bits 15--8`
      - P\ :sub:`11 bits 7--0`
      - P\ :sub:`11 bits 15--8`
      - P\ :sub:`21 bits 7--0`
      - P\ :sub:`21 bits 15--8`
      - P\ :sub:`31 bits 7--0`
      - P\ :sub:`31 bits 15--8`

.. _v4l2-pix-fmt-raw-csi2-20:

V4L2_PIX_FMT_RAW_CSI2_20
------------------------

V4L2_PIX_FMT_RAW_CSI2_20 contains 20-bit packed image data, with four bytes
containing the top 8 bits of two pixels followed by lowest 4 bits of the pixels
packed into one byte. This format is typically used by CSI-2 receivers with a
source that transmits MEDIA_BUS_FMT_RAW_20 and the CSI-2 receiver writes the
received data to memory as-is.

The packing of the data follows the MIPI CSI-2 specification.

**Byte Order Of V4L2_PIX_FMT_RAW_CSI2_20.**
Each cell is one byte. "P" denotes a pixel.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{2cm}

.. flat-table:: Sample 4x2 Image Frame
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8 8 8 8 8 8 8

    * - start + 0:
      - P\ :sub:`00 bits 19--12`
      - P\ :sub:`00 bits 11--4`
      - P\ :sub:`10 bits 19--12`
      - P\ :sub:`10 bits 11--4`
      - P\ :sub:`10 bits 3--0` (bits 7--4),
        P\ :sub:`00 bits 3--0` (bits 3--0)
      - P\ :sub:`20 bits 19--12`
      - P\ :sub:`20 bits 11--4`
      - P\ :sub:`30 bits 19--12`
      - P\ :sub:`30 bits 11--4`
      - P\ :sub:`30 bits 3--0` (bits 7--4),
        P\ :sub:`20 bits 3--0` (bits 3--0)
    * - start + 10:
      - P\ :sub:`01 bits 19--12`
      - P\ :sub:`01 bits 11--4`
      - P\ :sub:`11 bits 19--12`
      - P\ :sub:`11 bits 11--4`
      - P\ :sub:`11 bits 3--0` (bits 7--4),
        P\ :sub:`01 bits 3--0` (bits 3--0)
      - P\ :sub:`21 bits 19--12`
      - P\ :sub:`21 bits 11--4`
      - P\ :sub:`31 bits 19--12`
      - P\ :sub:`31 bits 11--4`
      - P\ :sub:`31 bits 3--0` (bits 7--4),
        P\ :sub:`21 bits 3--0` (bits 3--0)

.. _v4l2-pix-fmt-raw-24:

V4L2_PIX_FMT_RAW_24
-------------------

V4L2_PIX_FMT_RAW_24 contains 24-bit packed image data, with each three bytes
containing the value of one pixel. This format is typically used by CSI-2
receivers with a source that transmits MEDIA_BUS_FMT_RAW_24 and the CSI-2
receiver writes the received data to memory as-is.

The packing of the data follows the MIPI CSI-2 specification.

**Byte Order Of V4L2_PIX_FMT_RAW_24.**
Each cell is one byte. "P" denotes a pixel.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{.8cm}|p{.8cm}|p{1.2cm}|p{.8cm}|p{.8cm}|p{1.2cm}|p{.8cm}|p{.8cm}|p{1.2cm}|p{.8cm}|p{.8cm}|

.. flat-table:: Sample 4x2 Image Frame
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8 8 8 8 8 8 8 8 8

    * - start + 0:
      - P\ :sub:`00 bits 23--16`
      - P\ :sub:`00 bits 15--8`
      - P\ :sub:`00 bits 7--0`
      - P\ :sub:`10 bits 23--16`
      - P\ :sub:`10 bits 15--8`
      - P\ :sub:`10 bits 7--0`
      - P\ :sub:`20 bits 23--16`
      - P\ :sub:`20 bits 15--8`
      - P\ :sub:`20 bits 7--0`
      - P\ :sub:`30 bits 23--16`
      - P\ :sub:`30 bits 15--8`
      - P\ :sub:`30 bits 7--0`
    * - start + 12:
      - P\ :sub:`01 bits 23--16`
      - P\ :sub:`01 bits 15--8`
      - P\ :sub:`01 bits 7--0`
      - P\ :sub:`11 bits 23--16`
      - P\ :sub:`11 bits 15--8`
      - P\ :sub:`11 bits 7--0`
      - P\ :sub:`21 bits 23--16`
      - P\ :sub:`21 bits 15--8`
      - P\ :sub:`21 bits 7--0`
      - P\ :sub:`31 bits 23--16`
      - P\ :sub:`31 bits 15--8`
      - P\ :sub:`31 bits 7--0`

.. _v4l2-pix-fmt-raw-csi2-28:

V4L2_PIX_FMT_RAW_CSI2_28
------------------------

V4L2_PIX_FMT_RAW_CSI2_28 contains 28-bit packed image data, with four bytes
containing the top 8 bits of two pixels followed by lowest 4 bits of the pixels
packed into one byte. This format is typically used by CSI-2 receivers with a
source that transmits MEDIA_BUS_FMT_RAW_28 and the CSI-2 receiver writes the
received data to memory as-is.

The packing of the data follows the MIPI CSI-2 specification.

**Byte Order Of V4L2_PIX_FMT_RAW_CSI2_28.**
Each cell is one byte. "P" denotes a pixel.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}

.. flat-table:: Sample 4x2 Image Frame
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8 8 8 8 8 8 8 8 8 8 8

    * - start + 0:
      - P\ :sub:`00 bits 27--20`
      - P\ :sub:`00 bits 13--6`
      - P\ :sub:`10 bits 27--20`
      - P\ :sub:`10 bits 13--6`
      - P\ :sub:`00 bits 19--14` (bits 7--2),
	P\ :sub:`00 bits 1--0` (bits 1--0)
      - P\ :sub:`00 bits 5--2` (bits 7--4),
	P\ :sub:`10 bits 17--14` (bits 3--0)
      - P\ :sub:`10 bits 19--18` (bits 7--6),
	P\ :sub:`10 bits 5--0` (bits 5--0)
      - P\ :sub:`20 bits 27--20`
      - P\ :sub:`20 bits 13--6`
      - P\ :sub:`30 bits 27--20`
      - P\ :sub:`30 bits 13--6`
      - P\ :sub:`20 bits 19--14` (bits 7--2),
	P\ :sub:`20 bits 1--0` (bits 1--0)
      - P\ :sub:`20 bits 5--2` (bits 7--4),
	P\ :sub:`30 bits 17--14` (bits 3--0)
      - P\ :sub:`30 bits 19--18` (bits 7--6),
	P\ :sub:`30 bits 5--0` (bits 5--0)
    * - start + 14:
      - P\ :sub:`01 bits 27--20`
      - P\ :sub:`01 bits 13--6`
      - P\ :sub:`11 bits 27--20`
      - P\ :sub:`11 bits 13--6`
      - P\ :sub:`01 bits 19--14` (bits 7--2),
	P\ :sub:`01 bits 1--0` (bits 1--0)
      - P\ :sub:`01 bits 5--2` (bits 7--4),
	P\ :sub:`11 bits 17--14` (bits 3--0)
      - P\ :sub:`11 bits 19--18` (bits 7--6),
	P\ :sub:`11 bits 5--0` (bits 5--0)
      - P\ :sub:`21 bits 27--20`
      - P\ :sub:`21 bits 13--6`
      - P\ :sub:`31 bits 27--20`
      - P\ :sub:`31 bits 13--6`
      - P\ :sub:`21 bits 19--14` (bits 7--2),
	P\ :sub:`21 bits 1--0` (bits 1--0)
      - P\ :sub:`21 bits 5--2` (bits 7--4),
	P\ :sub:`31 bits 17--14` (bits 3--0)
      - P\ :sub:`31 bits 19--18` (bits 7--6),
	P\ :sub:`31 bits 5--0` (bits 5--0)
