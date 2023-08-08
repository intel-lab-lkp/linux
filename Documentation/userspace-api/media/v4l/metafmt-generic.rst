.. SPDX-License-Identifier: GPL-2.0 OR GFDL-1.1-no-invariants-or-later

**************************************************************************************************************************************************************************************************************************************************************************************************************************
V4L2_META_FMT_GENERIC_8 ('MET8'), V4L2_META_FMT_GENERIC_CSI2_10 ('MC1A'), V4L2_META_FMT_GENERIC_CSI2_12 ('MC1C'), V4L2_META_FMT_GENERIC_CSI2_14 ('MC1E'), V4L2_META_FMT_GENERIC_CSI2_16 ('MC1G'), V4L2_META_FMT_GENERIC_CSI2_20 ('MC1K'), V4L2_META_FMT_GENERIC_CSI2_24 ('MC1O'), V4L2_META_FMT_GENERIC_CSI2_2_24 ('MC2O')
**************************************************************************************************************************************************************************************************************************************************************************************************************************


Generic line-based metadata formats


Description
===========

These generic line-based metadata formats define the memory layout of the data
without defining the format or meaning of the metadata itself. These formats may
only be used with a Media controller pipeline where the more specific format is
defined in an :ref:`internal source pad <MEDIA-PAD-FL-INTERNAL>` of the source
sub-device. See also :ref:`source routes <v4l2-subdev-source-routes>`.

.. _v4l2-meta-fmt-generic-8:

V4L2_META_FMT_GENERIC_8
-----------------------

The V4L2_META_FMT_GENERIC_8 format is a plain 8-bit metadata format.

This format is also used on CSI-2 on both 8 bits per sample as well as on
16 bits per sample when two bytes of metadata are packed into one sample.

**Byte Order Of V4L2_META_FMT_GENERIC_8.**
Each cell is one byte. "M" denotes a byte of metadata.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8

    * - start + 0:
      - M\ :sub:`00`
      - M\ :sub:`10`
      - M\ :sub:`20`
      - M\ :sub:`30`
    * - start + 4:
      - M\ :sub:`01`
      - M\ :sub:`11`
      - M\ :sub:`21`
      - M\ :sub:`31`

.. _v4l2-meta-fmt-generic-csi2-10:

V4L2_META_FMT_GENERIC_CSI2_10
-----------------------------

V4L2_META_FMT_GENERIC_CSI2_10 contains packed 8-bit generic metadata, 10 bits
for each 8 bits of data. Every four bytes of metadata is followed by a single
byte of padding. The way the data is stored follows the CSI-2 specification.

This format is also used on CSI-2 on 20 bits per sample format that packs two
bytes of metadata into one sample.

This format is little endian.

**Byte Order Of V4L2_META_FMT_GENERIC_CSI2_10.**
Each cell is one byte. "M" denotes a byte of metadata and "p" a byte of padding.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{.8cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8 8

    * - start + 0:
      - M\ :sub:`00`
      - M\ :sub:`10`
      - M\ :sub:`20`
      - M\ :sub:`30`
      - p
    * - start + 5:
      - M\ :sub:`01`
      - M\ :sub:`11`
      - M\ :sub:`21`
      - M\ :sub:`31`
      - p

.. _v4l2-meta-fmt-generic-csi2-12:

V4L2_META_FMT_GENERIC_CSI2_12
-----------------------------

V4L2_META_FMT_GENERIC_CSI2_12 contains packed 8-bit generic metadata, 12 bits
for each 8 bits of data. Every four bytes of metadata is followed by two bytes
of padding. The way the data is stored follows the CSI-2 specification.

This format is little endian.

**Byte Order Of V4L2_META_FMT_GENERIC_CSI2_12.**
Each cell is one byte. "M" denotes a byte of metadata and "p" a byte of padding.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{.8cm}|p{.8cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8 8 8

    * - start + 0:
      - M\ :sub:`00`
      - M\ :sub:`10`
      - M\ :sub:`20`
      - M\ :sub:`30`
      - p
      - p
    * - start + 6:
      - M\ :sub:`01`
      - M\ :sub:`11`
      - M\ :sub:`21`
      - M\ :sub:`31`
      - p
      - p

.. _v4l2-meta-fmt-generic-csi2-14:

V4L2_META_FMT_GENERIC_CSI2_14
-----------------------------

V4L2_META_FMT_GENERIC_CSI2_14 contains packed 8-bit generic metadata, 14 bits
for each 8 bits of data. Every four bytes of metadata is followed by three
bytes of padding. The way the data is stored follows the CSI-2 specification.

This format is little endian.

**Byte Order Of V4L2_META_FMT_GENERIC_CSI2_14.**
Each cell is one byte. "M" denotes a byte of metadata and "p" a byte of padding.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{1.2cm}|p{.8cm}|p{.8cm}|p{.8cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8 8 8 8

    * - start + 0:
      - M\ :sub:`00`
      - M\ :sub:`10`
      - M\ :sub:`20`
      - M\ :sub:`30`
      - p
      - p
      - p
    * - start + 7:
      - M\ :sub:`01`
      - M\ :sub:`11`
      - M\ :sub:`21`
      - M\ :sub:`31`
      - p
      - p
      - p

.. _v4l2-meta-fmt-generic-csi2-16:

V4L2_META_FMT_GENERIC_CSI2_16
-----------------------------

V4L2_META_FMT_GENERIC_CSI2_16 contains packed 8-bit generic metadata, 16 bits
for each 8 bits of data. Every byte of metadata is followed by one byte of
padding. The way the data is stored follows the CSI-2 specification.

This format is little endian.

**Byte Order Of V4L2_META_FMT_GENERIC_CSI2_16.**
Each cell is one byte. "M" denotes a byte of metadata and "p" a byte of padding.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{.8cm}|p{1.2cm}|p{.8cm}|p{1.2cm}|p{.8cm}|p{1.2cm}|p{.8cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8 8 8 8 8

    * - start + 0:
      - M\ :sub:`00`
      - p
      - M\ :sub:`10`
      - p
      - M\ :sub:`20`
      - p
      - M\ :sub:`30`
      - p
    * - start + 8:
      - M\ :sub:`01`
      - p
      - M\ :sub:`11`
      - p
      - M\ :sub:`21`
      - p
      - M\ :sub:`31`
      - p

.. _v4l2-meta-fmt-generic-csi2-20:

V4L2_META_FMT_GENERIC_CSI2_20
-----------------------------

V4L2_META_FMT_GENERIC_CSI2_20 contains packed 8-bit generic metadata, 20 bits
for each 8 bits of data. Every byte of metadata is followed by alternating one
and two bytes of padding. The way the data is stored follows the CSI-2
specification.

This format is little endian.

**Byte Order Of V4L2_META_FMT_GENERIC_CSI2_20.**
Each cell is one byte. "M" denotes a byte of metadata and "p" a byte of padding.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{.8cm}|p{1.2cm}|p{.8cm}|p{.8cm}|p{1.2cm}|p{.8cm}|p{1.2cm}|p{.8cm}|p{.8cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8 8 8 8 8 8 8

    * - start + 0:
      - M\ :sub:`00`
      - p
      - M\ :sub:`10`
      - p
      - p
      - M\ :sub:`20`
      - p
      - M\ :sub:`30`
      - p
      - p
    * - start + 10:
      - M\ :sub:`01`
      - p
      - M\ :sub:`11`
      - p
      - p
      - M\ :sub:`21`
      - p
      - M\ :sub:`31`
      - p
      - p

.. _v4l2-meta-fmt-generic-csi2-24:

V4L2_META_FMT_GENERIC_CSI2_24
-----------------------------

V4L2_META_FMT_GENERIC_CSI2_24 contains packed 8-bit generic metadata, 24 bits
for each 8 bits of data. Every byte of metadata is followed by two bytes of
padding. The way the data is stored follows the CSI-2 specification.

This format is little endian.

**Byte Order Of V4L2_META_FMT_GENERIC_CSI2_24.**
Each cell is one byte. "M" denotes a byte of metadata and "p" a byte of padding.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{.8cm}|p{.8cm}|p{1.2cm}|p{.8cm}|p{.8cm}|p{1.2cm}|p{.8cm}|p{.8cm}|p{1.2cm}|p{.8cm}|p{.8cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8 8 8 8 8 8 8 8 8

    * - start + 0:
      - M\ :sub:`00`
      - p
      - p
      - M\ :sub:`10`
      - p
      - p
      - M\ :sub:`20`
      - p
      - p
      - M\ :sub:`30`
      - p
      - p
    * - start + 12:
      - M\ :sub:`01`
      - p
      - p
      - M\ :sub:`11`
      - p
      - p
      - M\ :sub:`21`
      - p
      - p
      - M\ :sub:`31`
      - p
      - p

.. _v4l2-meta-fmt-generic-csi2-2-24:

V4L2_META_FMT_GENERIC_CSI2_2_24
-------------------------------

V4L2_META_FMT_GENERIC_CSI2_2_24 contains packed 8-bit generic metadata, 24 bits
for each two times 8 bits of data. Every two bytes of metadata are followed by
one byte of padding. The way the data is stored follows the CSI-2
specification.

This format is little endian.

**Byte Order Of V4L2_META_FMT_GENERIC_CSI2_2_24.**
Each cell is one byte. "M" denotes a byte of metadata and "p" a byte of padding.

.. tabularcolumns:: |p{2.4cm}|p{1.2cm}|p{1.2cm}|p{.8cm}|p{1.2cm}|p{1.2cm}|p{.8cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 8 8 8 8 8 8

    * - start + 0:
      - M\ :sub:`00`
      - M\ :sub:`10`
      - p
      - M\ :sub:`20`
      - M\ :sub:`30`
      - p
    * - start + 6:
      - M\ :sub:`01`
      - M\ :sub:`11`
      - p
      - M\ :sub:`21`
      - M\ :sub:`31`
      - p

