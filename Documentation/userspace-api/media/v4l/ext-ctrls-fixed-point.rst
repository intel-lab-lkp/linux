.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later

.. _fixed-point-controls:

***************************
Fixed Point Control Reference
***************************

These controls are intended to support an asynchronous sample
rate converter.

.. _v4l2-audio-asrc:

``V4L2_CID_ASRC_SOURCE_RATE``
    sets the resampler source rate.

``V4L2_CID_ASRC_DEST_RATE``
    sets the resampler destination rate.

.. c:type:: v4l2_ctrl_fixed_point

.. cssclass:: longtable

.. tabularcolumns:: |p{1.5cm}|p{5.8cm}|p{10.0cm}|

.. flat-table:: struct v4l2_ctrl_fixed_point
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u32
      - ``integer``
      - integer part of fixed point value.
    * - __s32
      - ``fractional``
      - fractional part of fixed point value, which is Q31.
