.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later
.. c:namespace:: V4L

.. _V4L2-PIX-FMT-SRGGB24:
.. _v4l2-pix-fmt-sbggr24:
.. _v4l2-pix-fmt-sgbrg24:
.. _v4l2-pix-fmt-sgrbg24:

***************************************************************************************************************************
V4L2_PIX_FMT_SBGGR24 ('BG24'), V4L2_PIX_FMT_SGBRG24 ('GB24'), V4L2_PIX_FMT_SGRBG24 ('GR24'), V4L2_PIX_FMT_SRGGB24 ('RG24'),
***************************************************************************************************************************


====================
24-bit Bayer formats
====================


Description
===========

These four pixel formats are raw sRGB / Bayer formats with 24 bits per
sample. Each sample is stored in a 32-bit word. Each n-pixel row contains
n/2 green samples and n/2 blue or red samples, with alternating red and blue
rows. Bytes are stored in memory in little endian order. They are
conventionally described as GRGR... BGBG..., RGRG... GBGB..., etc. Below is
an example of one sample.

.. cssclass: longtable

.. flat-table:: 24-bit Bayer Formats
    :header-rows:  2
    :stub-columns: 0

    * -
      - :cspan:`31` Sample organization
    * - Bit
      - 31
      - 30
      - 29
      - 28
      - 27
      - 26
      - 25
      - 24
      - 23
      - 22
      - 21
      - 20
      - 19
      - 18
      - 17
      - 16
      - 15
      - 14
      - 13
      - 12
      - 11
      - 10
      - 9
      - 8
      - 7
      - 6
      - 5
      - 4
      - 3
      - 2
      - 1
      - 0
    * -
      - *
      - *
      - *
      - *
      - *
      - *
      - *
      - *
      - P\ :sub:`23`
      - P\ :sub:`22`
      - P\ :sub:`21`
      - P\ :sub:`20`
      - P\ :sub:`19`
      - P\ :sub:`18`
      - P\ :sub:`17`
      - P\ :sub:`16`
      - P\ :sub:`15`
      - P\ :sub:`14`
      - P\ :sub:`13`
      - P\ :sub:`12`
      - P\ :sub:`11`
      - P\ :sub:`10`
      - P\ :sub:`9`
      - P\ :sub:`8`
      - P\ :sub:`7`
      - P\ :sub:`6`
      - P\ :sub:`5`
      - P\ :sub:`4`
      - P\ :sub:`3`
      - P\ :sub:`2`
      - P\ :sub:`1`
      - P\ :sub:`0`
