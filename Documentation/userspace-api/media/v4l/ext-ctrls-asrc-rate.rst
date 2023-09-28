.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later

.. _asrc-rate-controls:

***************************
ASRC RATE Control Reference
***************************

These controls is intended to support asynchronous sample
rate converter.

.. _v4l2-audio-asrc:

``V4L2_CID_ASRC_SOURCE_RATE``
    sets the rasampler source rate.

``V4L2_CID_ASRC_DEST_RATE``
    sets the rasampler destination rate.

.. c:type:: v4l2_ctrl_asrc_rate

.. cssclass:: longtable

.. tabularcolumns:: |p{1.5cm}|p{5.8cm}|p{10.0cm}|

.. flat-table:: struct v4l2_ctrl_asrc_rate
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u32
      - ``rate_integer``
      - integer part of sample rate.
    * - __s32
      - ``rate_fractional``
      - fractional part of sample rate, which is Q31.
