.. SPDX-License-Identifier: GPL-2.0 OR GFDL-1.1-no-invariants-or-later

.. _media_metadata_layouts:

Metadata Layouts
----------------

The :ref:`metadata layout control <image_source_control_metadata_layout>`
specifies the exact layout of the metadata stream while the a :ref:`generic
metadata mbus code <media-bus-format-generic-meta>` on the subdevice pads
only describe the size of the :term:`Data Unit`.

.. _media-metadata-layout-ccs:

MIPI CCS Embedded Data Layout (``V4L2_METADATA_LAYOUT_CCS``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

`MIPI CCS <https://www.mipi.org/specifications/camera-command-set>`_ defines a
metadata layout for sensor embedded data, identified by
``V4L2_CID_METADATA_LAYOUT`` control value ``V4L2_METADATA_LAYOUT_CCS``, which
is used to store the register configuration used for capturing a given
frame. The layout itself is defined in the CCS specification.

The CCS embedded data format (code ``0xa``) definition includes three levels:

1. Padding within CSI-2 bus :term:`Data Unit` as documented in the MIPI CCS
   specification.

2. The tagged data format as documented in the MIPI CCS specification.

3. Register addresses and register documentation as documented in the MIPI CCS
   specification.

The ``V4L2_METADATA_LAYOUT_CCS`` metadata layout value shall be used only by
devices that fulfill all three levels above.

This metadata layout code is only used for "2-byte simplified tagged data
format" (code ``0xa``) but their use may be extended further in the future, to
cover other CCS embedded data format codes.

Also see :ref:`CCS driver documentation <media-ccs-routes>`.

.. _media-metadata-layout-ov2740:

Omnivision OV2740 Embedded Data Layout (``V4L2_METADATA_LAYOUT_OV2740``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The Omnivision OV2740 camera sensor produces the following embedded data layout,
indicated by ``V4L2_METADATA_LAYOUT_OV2740`` metadata layout. The format
conforms to :ref:`CCS embedded data layout <media-metadata-layout-ccs>` up to
level 1.

.. flat-table:: Omnivision OV2740 Embedded Data Layout. Octets at indices marked
                reserved or unused have been omitted from the table. The values
                are in big endian byte order.
    :header-rows: 1

    * - Offset
      - Size in bits (active bits if not the same as size)
      - Content description
    * - 4
      - 16 (10--0)
      - Analogue gain
    * - 6
      - 16
      - Coarse integration time
    * - 10
      - 8
      - DPC correction threshold bits 9--2
    * - 15
      - 16
      - Output image width
    * - 17
      - 16
      - Output image height
    * - 23
      - 8
      - MIPI header revision number (2)
    * - 31
      - 8
      - Vertical (bit 1) and horizontal flip (bit 0)
    * - 32
      - 8
      - Frame duration A
    * - 33
      - 8
      - Frame duration B
    * - 34
      - 8
      - Context count (2)
    * - 35
      - 8
      - Context select
    * - 54
      - 8
      - Data pedestal bits 9--2
    * - 63
      - 8
      - Frame average bits 9--2
    * - 64
      - 16
      - Digital gain red
    * - 66
      - 16
      - Digital gain greenr
    * - 68
      - 16
      - Digital gain blue
    * - 70
      - 16
      - Digital gain greenb
    * - 89
      - 8
      - Frame counter (starts at 1, wraps to 0 after 255)
