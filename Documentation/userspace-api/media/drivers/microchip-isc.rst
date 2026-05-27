.. SPDX-License-Identifier: GPL-2.0

Microchip ISC/XISC Driver
=========================

The Image Sensor Controller (ISC) on SAMA5D2 and eXtended ISC (XISC) on
SAMA7G5/SAM9X7 provide camera capture with hardware image processing.

Supported Hardware
------------------

==========  ==========  ==============  ================  ===============
SoC         Controller  Max Resolution  Interface         Hue/Saturation
==========  ==========  ==============  ================  ===============
SAMA5D2     ISC         2592x1944       12-bit parallel   No
SAMA7G5     XISC        3264x2464       12-bit + CSI-2    Yes
SAM9X7      XISC        2560x1920       12-bit + CSI-2    Yes
==========  ==========  ==============  ================  ===============

SAM9X7 shares the XISC pipeline with SAMA7G5 but has a smaller internal
line buffer, limiting horizontal resolution to 2560 pixels.

Controls
--------

Standard V4L2 controls:

* ``V4L2_CID_BRIGHTNESS``: -1024..1023, default 0
* ``V4L2_CID_CONTRAST``: -2048..2047, default 256 (1.0x)
* ``V4L2_CID_GAMMA``: 0..2 selects a preset curve. Indices differ
  per SoC: SAMA7G5/SAM9X7 use 0=1/2.4, 1=1/2.2 (default), 2=1/1.8;
  SAMA5D2 uses 0=1/1.8, 1=1/2.0, 2=1/2.2 (default).
* ``V4L2_CID_AUTO_WHITE_BALANCE``: Enable kernel Grey World AWB
* ``V4L2_CID_DO_WHITE_BALANCE``: Trigger one-shot AWB

SAMA7G5/SAM9X7 add:

* ``V4L2_CID_HUE``: -180..180 degrees
* ``V4L2_CID_SATURATION``: 0..127, default 16 (Q4 fixed-point, 16 = 1.0x)

Custom controls (defined in ``atmel-isc-media.h``):

* ``ISC_CID_R_GAIN``, ``ISC_CID_B_GAIN``, ``ISC_CID_GR_GAIN``,
  ``ISC_CID_GB_GAIN``: WB gains, 0..8191, Q2.9 (512 = 1.0x)
* ``ISC_CID_R_OFFSET``, ``ISC_CID_B_OFFSET``, ``ISC_CID_GR_OFFSET``,
  ``ISC_CID_GB_OFFSET``: WB offsets, -4096..4095

Pipeline
--------

Pipeline modules: DPC -> WB -> CFA -> CC -> GAM -> CBHS/CBC -> CSC -> SUB

* DPC: Defective Pixel Correction (XISC only), black level subtraction
  to sensor bit depth, green disparity correction
* WB: White Balance gains/offsets
* CFA: Color Filter Array interpolation (demosaic)
* CC: Color Correction matrix
* GAM: Gamma correction (preset)
* CBHS: Contrast/Brightness/Hue/Saturation (XISC only)
* CBC: Contrast/Brightness (ISC only)
* CSC: Color Space Conversion (RGB to YCbCr)
* SUB: Chroma subsampling (4:2:2, 4:2:0)

Pipeline usage depends on input and output formats:

* Raw Bayer input, RGB output: DPC, WB, CFA, CC, GAM
* Raw Bayer input, YUV output: Full pipeline including CSC, CBHS/CBC, SUB
* Non-RAW input (YUV/RGB sensor): Pipeline bypassed
