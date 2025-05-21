.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later
.. c:namespace:: V4L

.. _image-source-controls:

******************************
Image Source Control Reference
******************************

The Image Source control class is intended for low-level control of
image source devices such as image sensors. The devices feature an
analogue to digital converter and a bus transmitter to transmit the
image data out of the device.


.. _image-source-control-id:

Image Source Control IDs
========================

``V4L2_CID_IMAGE_SOURCE_CLASS (class)``
    The IMAGE_SOURCE class descriptor.

``V4L2_CID_VBLANK (integer)``
    Vertical blanking. The idle period after every frame during which no
    image data is produced. The unit of vertical blanking is a line.
    Every line has length of the image width plus horizontal blanking at
    the pixel rate defined by ``V4L2_CID_PIXEL_RATE`` control in the
    same sub-device.

``V4L2_CID_HBLANK (integer)``
    Horizontal blanking. The idle period after every line of image data
    during which no image data is produced. The unit of horizontal
    blanking is pixels.

``V4L2_CID_ANALOGUE_GAIN (integer)``
    Analogue gain is gain affecting all colour components in the pixel
    matrix. The gain operation is performed in the analogue domain
    before A/D conversion.

``V4L2_CID_TEST_PATTERN_RED (integer)``
    Test pattern red colour component.

``V4L2_CID_TEST_PATTERN_GREENR (integer)``
    Test pattern green (next to red) colour component.

``V4L2_CID_TEST_PATTERN_BLUE (integer)``
    Test pattern blue colour component.

``V4L2_CID_TEST_PATTERN_GREENB (integer)``
    Test pattern green (next to blue) colour component.

``V4L2_CID_UNIT_CELL_SIZE (struct)``
    This control returns the unit cell size in nanometers. The struct
    :c:type:`v4l2_area` provides the width and the height in separate
    fields to take into consideration asymmetric pixels.
    This control does not take into consideration any possible hardware
    binning.
    The unit cell consists of the whole area of the pixel, sensitive and
    non-sensitive.
    This control is required for automatic calibration of sensors/cameras.

.. c:type:: v4l2_area

.. flat-table:: struct v4l2_area
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u32
      - ``width``
      - Width of the area.
    * - __u32
      - ``height``
      - Height of the area.

``V4L2_CID_NOTIFY_GAINS (integer array)``
    The sensor is notified what gains will be applied to the different
    colour channels by subsequent processing (such as by an ISP). The
    sensor is merely informed of these values in case it performs
    processing that requires them, but it does not apply them itself to
    the output pixels.

    Currently it is defined only for Bayer sensors, and is an array
    control taking 4 gain values, being the gains for each of the
    Bayer channels. The gains are always in the order B, Gb, Gr and R,
    irrespective of the exact Bayer order of the sensor itself.

    The use of an array allows this control to be extended to sensors
    with, for example, non-Bayer CFAs (colour filter arrays).

    The units for the gain values are linear, with the default value
    representing a gain of exactly 1.0. For example, if this default value
    is reported as being (say) 128, then a value of 192 would represent
    a gain of exactly 1.5.

.. _image-source-control-cfa-pattern:

``V4L2_CID_CFA_PATTERN (integer)``
    This control determines the color components and native pixel order in the
    sensor's CFA (Color Filter Array) when used in conjunction with
    :ref:`generic raw mbus codes MEDIA_BUS_FMT_RAW_x (where 'x' is the bit
    depth) <v4l2-mbus-pixelcode-generic-raw>` pixelcodes. The native pixel
    order is constant for a given device, it is not affected by cropping or
    flipping.

    This control may only be used on V4L2 sub-devices.

    This is a read-only control.

    Available patterns:

.. flat-table:: V4L2 Color Filter Array Patterns
    :header-rows:  1
    :stub-columns: 0
    :widths:       1 2 1

    * - Macro name
      - Synopsis
      - Value
    * - V4L2_CFA_PATTERN_GRBG
      - Bayer pattern (2x2 macroblocks). Each macroblock has a green filter in
        the top-left and bottom-right pixels, a red filter in the top-right
        pixel, and a blue filter in the bottom-left pixel.
      - 0x00000000
    * - V4L2_CFA_PATTERN_RGGB
      - Bayer pattern (2x2 macroblocks). Each macroblock has a green filter in
        the top-right and bottom-left pixels, a red filter in the top-left
        pixel, and a blue filter in the bottom-right pixel.
      - 0x00000001
    * - V4L2_CFA_PATTERN_BGGR
      - Bayer pattern (2x2 macroblocks). Each macroblock has a green filter in
        the top-right and bottom-left pixels, a blue filter in the top-left
        pixel, and a red filter in the bottom-right pixel.
      - 0x00000002
    * - V4L2_CFA_PATTERN_GBRG
      - Bayer pattern (2x2 macroblocks). Each macroblock has a green filter in
        the top-left and bottom-right pixels, a blue filter in the top-right
        pixel, and a red filter in the bottom-left pixel.
      - 0x00000003
    * - V4L2_CFA_PATTERN_MONO
      - Monochromatic sensor, no CFA pattern.
      - 0x00010000

.. _image-source-control-cfa-pattern-flip:

``V4L2_CID_CFA_PATTERN_FLIP (bitmask)``
    This control determines whether the horizontal or vertical flipping controls
    (V4L2_CID_HFLIP and V4L2_CID_VFLIP) have an effect on the pixel order of the
    output color pattern. Macros ``V4L2_CFA_PATTERN_FLIP_HORIZONTAL`` and
    ``V4L2_CFA_PATTERN_FLIP_VERTICAL`` define bitmasks for both bits. If either
    horizontal or vertical bit is set, the readout pattern order is that of the
    reversed readout. ``V4L2_CFA_PATTERN_FLIP_BOTH`` for setting both
    ``V4L2_CFA_PATTERN_FLIP_HORIZONTAL`` and ``V4L2_CFA_PATTERN_FLIP_VERTICAL``
    is provided as well.

    This is a read-only control.

``V4L2_CID_METADATA_LAYOUT (integer)``
    The metadata layout control defines the metadata layout on the device's data
    interface for metadata streams. The control is used in conjunction with
    :ref:`generic metadata formats <media-bus-format-generic-meta>` formats to
    specify the layout of the data.
