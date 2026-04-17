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

.. tabularcolumns:: |p{5.2cm}|p{1.0cm}|p{1.5cm}|p{5.0cm}|p{1.2cm}|p{1.8cm}|p{10.0cm}|

.. flat-table:: Overview of AFBC formats
    :header-rows:  1
    :stub-columns: 0

    * - Identifier
      - Code
      - Colorspace
      - Bits per component
      - Superblock size
      - Compression parameters
      - DRM format (modifier)
    * - V4L2_PIX_FMT_AFBC_YUV420_16x16_SPLIT
      - 'A168'
      - YUV420
      - 8 bits
      - 16x16
      - Sparse, Split
      - DRM_FORMAT_YUV420_8BIT (AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 | AFBC_FORMAT_MOD_SPARSE | AFBC_FORMAT_MOD_SPLIT)
    * - V4L2_PIX_FMT_AFBC_YUV420_32x8
      - 'A328'
      - YUV420
      - 8 bits
      - 32x8
      - Sparse
      - DRM_FORMAT_YUV420_8BIT (AFBC_FORMAT_MOD_BLOCK_SIZE_32x8 | AFBC_FORMAT_MOD_SPARSE)
    * - V4L2_PIX_FMT_AFBC_YUV420_16x16_10_SPLIT
      - 'A16a'
      - YUV420
      - 10 bits
      - 16x16
      - Sparse, Split
      - DRM_FORMAT_YUV420_10BIT (AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 | AFBC_FORMAT_MOD_SPARSE | AFBC_FORMAT_MOD_SPLIT)
    * - V4L2_PIX_FMT_AFBC_YUV420_32x8_10
      - 'A32a'
      - YUV420
      - 10 bits
      - 32x8
      - Sparse
      - DRM_FORMAT_YUV420_10BIT (AFBC_FORMAT_MOD_BLOCK_SIZE_32x8 | AFBC_FORMAT_MOD_SPARSE)

AFBC Stride computation
=======================

Stride is equal to the aligned width * 3 * bits per component / 2 / 8.

.. _V4L2-PIX-FMT-AFBC-YUV420-16x16-SPLIT:
.. _V4L2-PIX-FMT-AFBC-YUV420-32x8:
.. _V4L2-PIX-FMT-AFBC-YUV420-16x16-10-SPLIT:
.. _V4L2-PIX-FMT-AFBC-YUV420-32x8-10:
