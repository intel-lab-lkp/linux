.. SPDX-License-Identifier: GPL-2.0

=======================================
NXP Neo Image Signal Processor (neoisp)
=======================================

Introduction
============

Neoisp performs a set of image processing tasks on the RAW camera stream, where
the input camera stream and Neoisp processed output image are stored in DDR or
any system memory fast enough to keep up with Neoisp processing.
The overall Neoisp operation is frame based, that is 1 complete image frame is
read and output pixel by pixel, line by line wise.

Revisions
=========

NXP Neoisp hw revisions are listed in UAPI in enum :c:type:`neoisp_version_e`.
Version is stored in the Media device information, hw revision field,
accessible from the ioctl MEDIA_IOC_DEVICE_INFO.

There are 2 versions:

- NEOISP_HW_V1: used at least in i.MX95 A0/A1 SoC, not targeted for production.
- NEOISP_HW_V2: used at least in i.MX95 B0 SoC.

Neoisp hardware
===============

The Neoisp registers and pipeline processing are documented in the NXP Image
Signal Processor Specification document (under NDA).

The NEO pipeline block diagram is shown below:

.. kernel-figure:: nxp-neoisp-diagram.dot
    :alt:   Diagram of neoisp pipeline
    :align: center

It is composed by the following HW blocks:

- a Head Color (HC) selection block,
- two HDR decompression blocks, one for each input,
- three Optical Black correction and White Balance (OBWB) blocks at different
  stages in the pipeline,
- a HDR merge block for HDR image capture from the 2 input lines,
- a RGB-IR to RGGB converter,
- a Bayer Noise Reduction (BNR) block,
- a Vignetting block, aka Lens Shading Correction (LSC),
- a Demosaic block for RAW image conversion to RGB,
- a RGB Color Correction Matrix (CCM) and Color Space Converter aka CSC or
  RGB2YUV,
- a Dynamic Range Compression (DRC) block,
- the Denoising pipeline (composed by multiple blocks for Noise Reduction, Edge
  Enhancement, Gamma Compensation, etc),
- a Packetizer used for UV sub-sampling or RGB packing.

All these blocks are controlled by SW through registers. Some of these
registers are accessible by the uAPI, so that usespace application and Image
Processing Algorithms (IPA) can configure them through the parameters buffers.

Neoisp driver
=============

Neoisp driver is located under drivers/media/platform/nxp/neoisp.
It uses the `V4L2 API` and the `V4L2 subdev API` to register capture and output
video devices in addition to a subdevice for neoisp that connects the video
devices in a single media graph realized using the `Media Controller (MC) API`.

The media topology registered by Neoisp driver is represented below:

.. kernel-figure:: nxp-neoisp.dot
    :alt:   Diagram of neoisp media device topology
    :align: center


The media graph registers the following video device nodes:

- neoisp-input0: output device for RAW frames to be submitted to the ISP for processing.
- neoisp-input1: output device for RAW frames short capture in HDR merge mode.
- neoisp-params: output meta device for parameters provided by user space 3A algorithms.
- neoisp-frame: capture device for RGB/YUV pixels of the processed images.
- neoisp-ir: capture device for the infra-red pixels of the processed images.
- neoisp-stats: capture meta device for generated image statistics for user space 3A algorithms.

neoisp-input0, neoisp-input1
----------------------------

Images to be processed by Neoisp are queued to the neoisp-input0 (and
neoisp-input1 when in HDR mode) output device nodes. Supported image formats
as input to the ISP are:

- Raw bayer formats:

  - 8 bits raw (V4L2_PIX_FMT_SRGGB8, V4L2_PIX_FMT_SBGGR8, V4L2_PIX_FMT_SGBRG8,
    V4L2_PIX_FMT_SGRBG8)
  - 10 bits raw (V4L2_PIX_FMT_SRGGB10, V4L2_PIX_FMT_SBGGR10,
    V4L2_PIX_FMT_SGBRG10, V4L2_PIX_FMT_SGRBG10)
  - 12 bits raw (V4L2_PIX_FMT_SRGGB12, V4L2_PIX_FMT_SBGGR12,
    V4L2_PIX_FMT_SGBRG12, V4L2_PIX_FMT_SGRBG12)
  - 14 bits raw (V4L2_PIX_FMT_SRGGB14, V4L2_PIX_FMT_SBGGR14,
    V4L2_PIX_FMT_SGBRG14, V4L2_PIX_FMT_SGRBG14)
  - 16 bits raw (V4L2_PIX_FMT_SRGGB16, V4L2_PIX_FMT_SBGGR16,
    V4L2_PIX_FMT_SGBRG16, V4L2_PIX_FMT_SGRBG16)

- Monochrome formats:

  - 8 bits Monochrome (V4L2_PIX_FMT_GREY)
  - 10 bits Monochrome (V4L2_PIX_FMT_Y10)
  - 12 bits Monochrome (V4L2_PIX_FMT_Y12)
  - 14 bits Monochrome (V4L2_PIX_FMT_Y14)
  - 16 bits Monochrome (V4L2_PIX_FMT_Y16)

.. note::
   RGBIr camera sensors are supported as well, and can be used through user
   space activation of the IR block.

.. note::
   neoisp-input1 link is mutable and should be enabled in case a short capture
   image buffer is provided to the ISP for HDR merge.

.. _neoisp_params:

neoisp-params
-------------

The neoisp-params output meta device receives configuration data to be written
to Neoisp registers and internal memory for desired input image processing.
This v4l2 device accepts the `legacy parameters` format, or the `extensible
parameters` format.

When using the `legacy parameters` format, the parameters buffer is defined by
structure :c:type:`neoisp_meta_params_s`, and userspace should set
:ref:`V4L2_META_FMT_NEO_ISP_PARAMS <v4l2-meta-fmt-neo-isp-params>` as dataformat.

When using the `extensible parameters` format, the parameters buffer is defined
by structure :c:type:`neoisp_ext_params_s`, and userspace should set
:ref:`V4L2_META_FMT_NEO_ISP_EXT_PARAMS <v4l2-meta-fmt-neo-isp-ext-params>` as
dataformat.

When the related media link is disabled, the image decoding will be done based
on the default parameters of the ISP.

neoisp-frame
------------

The capture device writes to memory the RGB or YUV pixels of the image processed
by Neoisp when the media link is enabled. If the related media link is disabled,
the processed image will be written to dummy buffer and not delivered to the
neoisp-frame video device node.

neoisp-ir
---------

The capture device writes to memory the RGBIr pixels of the image processed by
Neoisp when the media link is enabled. If the related media link is disabled,
the processed image will not be delivered to the neoisp-ir video device node.

.. _neoisp_stats:

neoisp-stats
------------

The neoisp-stats capture meta device provides statistics data generated by
Neoisp hardware while processing the input image.
This v4l2 device accepts the `legacy statistics` format, or the `extensible
statistics` format.

When using the `legacy statistics` format, the statistics buffer is defined by
structure :c:type:`neoisp_meta_stats_s`, and userspace should set
:ref:`V4L2_META_FMT_NEO_ISP_STATS <v4l2-meta-fmt-neo-isp-stats>` as dataformat.

When using the `extensible statistics` format, the statistics buffer is defined
by structure :c:type:`neoisp_ext_stats_s`, and userspace should set
:ref:`V4L2_META_FMT_NEO_ISP_EXT_STATS <v4l2-meta-fmt-neo-isp-ext-stats>` as
dataformat.

When the related media link is disabled, the decoding statistics will not be
delivered to the neoisp-stats meta device node.

Control
=======

To support additional neoisp hardware revisions, the read-only bitmask control
`V4L2_CID_NEOISP_SUPPORTED_PARAMS_BLOCKS` can be used to query the list of
supported blocks. Each bit represents the availability of the corresponding
entry from the :c:type:`neoisp_param_block_type_e` enum. In current driver
version, default and max values represent the blocks supported by the i.MX95
SoC.
