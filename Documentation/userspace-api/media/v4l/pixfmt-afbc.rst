.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later

.. afbc:

*******************************************
ARM Frame Buffer Compression formats (AFBC)
*******************************************

The AFBC format is a lossless compression format which can support
up to four components. It could compress 8 bits to 64 bits per pixel.
The internal superblock size could be:

- 16x16 pixels

- 32x8 pixels

- 64x4 pixels.

The memory layout is composed of a header block followed by payload data.

AFBC Formats
============

.. tabularcolumns:: |p{5.2cm}|p{1.0cm}|p{1.5cm}|p{1.9cm}|p{1.2cm}|p{1.8cm}|

.. flat-table:: Overview of AFBC formats
    :header-rows:  1
    :stub-columns: 0

    * - Identifier
      - Code
      - Colorspace
      - Bits per component
      - Superblock size
      - Compression parameters
    * - V4L2_PIX_FMT_AFBC_YUV420_16x16
      - 'A168'
      - YUV420
      - 8 bits
      - 16x16
      - Sparse, Split
    * - V4L2_PIX_FMT_AFBC_YUV420_32x8
      - 'A328'
      - YUV420
      - 8 bits
      - 32x8
      - Sparse
    * - V4L2_PIX_FMT_AFBC_YUV420_16x16_10
      - 'A16a'
      - YUV420
      - 10 bits
      - 16x16
      - Sparse, Split
    * - V4L2_PIX_FMT_AFBC_YUV420_32x8_10
      - 'A32a'
      - YUV420
      - 10 bits
      - 32x8
      - Sparse

.. _V4L2-PIX-FMT-AFBC-YUV420-16x16:
.. _V4L2-PIX-FMT-AFBC-YUV420-32x8:
.. _V4L2-PIX-FMT-AFBC-YUV420-16x16-10:
.. _V4L2-PIX-FMT-AFBC-YUV420-32x8-10:
