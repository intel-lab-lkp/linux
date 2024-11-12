.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later

.. _v4l2-pix-fmt-cru-sbggr10:
.. _v4l2-pix-fmt-cru-sgbrg10:
.. _v4l2-pix-fmt-cru-sgrbg10:
.. _v4l2-pix-fmt-cru-srggb10:
.. _v4l2-pix-fmt-cru-sbggr12:
.. _v4l2-pix-fmt-cru-sgbrg12:
.. _v4l2-pix-fmt-cru-sgrbg12:
.. _v4l2-pix-fmt-cru-srggb12:
.. _v4l2-pix-fmt-cru-sbggr14:
.. _v4l2-pix-fmt-cru-sgbrg14:
.. _v4l2-pix-fmt-cru-sgrbg14:
.. _v4l2-pix-fmt-cru-srggb14:
.. _v4l2-pix-fmt-cru-sbggr20:
.. _v4l2-pix-fmt-cru-sgbrg20:
.. _v4l2-pix-fmt-cru-sgrbg20:
.. _v4l2-pix-fmt-cru-srggb20:

******************************************************************************************************************************************
V4L2_PIX_FMT_CRU_SBGGRnn ('CnnB'), V4L2_PIX_FMT_CRU_SGBRGnn ('CnnG'), V4L2_PIX_FMT_CRU_SGRBGnn ('Cnng'), V4L2_PIX_FMT_CRU_SRGGBnn ('CnnR')
******************************************************************************************************************************************

===============================================================
Renesas RZ/V2H Camera Receiver Unit 64-bit packed pixel formats
===============================================================

| V4L2_PIX_FMT_CRU_SBGGR10 (C10B)
| V4L2_PIX_FMT_CRU_SGBRG10 (C10G)
| V4L2_PIX_FMT_CRU_SGRBG10 (C10g)
| V4L2_PIX_FMT_CRU_SRGGB10 (C10R)
| V4L2_PIX_FMT_CRU_SBGGR12 (C12B)
| V4L2_PIX_FMT_CRU_SGBRG12 (C12G)
| V4L2_PIX_FMT_CRU_SGRBG12 (C12g)
| V4L2_PIX_FMT_CRU_SRGGB12 (C12R)
| V4L2_PIX_FMT_CRU_SBGGR14 (C14B)
| V4L2_PIX_FMT_CRU_SGBRG14 (C14G)
| V4L2_PIX_FMT_CRU_SGRBG14 (C14g)
| V4L2_PIX_FMT_CRU_SRGGB14 (C14R)
| V4L2_PIX_FMT_CRU_SBGGR20 (C20B)
| V4L2_PIX_FMT_CRU_SGBRG20 (C20G)
| V4L2_PIX_FMT_CRU_SGRBG20 (C20g)
| V4L2_PIX_FMT_CRU_SRGGB20 (C20R)

Description
===========

These pixel formats are some of the Bayer RAW outputs for the Camera Receiver
Unit in the Renesas RZ/V2H SoC. They are raw sRGB / Bayer formats which pack
pixels contiguously into 64-bit units, with the 4 or 8 most significant
bits padded.

**Byte Order**

.. flat-table:: RGB formats
    :header-rows:  2
    :stub-columns: 0
    :widths: 36 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2
    :fill-cells:

    * - :rspan:`1` Pixel Format Code
      - :cspan:`63` Data organization
    * - 63
      - 62
      - 61
      - 60
      - 59
      - 58
      - 57
      - 56
      - 55
      - 54
      - 53
      - 52
      - 51
      - 50
      - 49
      - 48
      - 47
      - 46
      - 45
      - 44
      - 43
      - 42
      - 41
      - 40
      - 39
      - 38
      - 37
      - 36
      - 35
      - 34
      - 33
      - 32
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
    * - V4L2_PIX_FMT_CRU_SBGGR10
      - 0
      - 0
      - 0
      - 0
      - :cspan:`9` P5
      - :cspan:`9` P4
      - :cspan:`9` P3
      - :cspan:`9` P2
      - :cspan:`9` P1
      - :cspan:`9` P0
    * - V4L2_PIX_FMT_CRU_SBGGR12
      - 0
      - 0
      - 0
      - 0
      - :cspan:`11` P4
      - :cspan:`11` P3
      - :cspan:`11` P2
      - :cspan:`11` P1
      - :cspan:`11` P0
    * - V4L2_PIX_FMT_CRU_SBGGR14
      - 0
      - 0
      - 0
      - 0
      - 0
      - 0
      - 0
      - 0
      - :cspan:`13` P3
      - :cspan:`13` P2
      - :cspan:`13` P1
      - :cspan:`13` P0
    * - V4L2_PIX_FMT_CRU_SBGGR20
      - 0
      - 0
      - 0
      - 0
      - :cspan:`19` P2
      - :cspan:`19` P1
      - :cspan:`19` P0
