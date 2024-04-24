.. SPDX-License-Identifier: GPL-2.0

======================================================
Visconti Video Input Interface Driver (visconti-viif)
======================================================

Introduction
============

This file documents the driver for the Video Input Interface (VIIF) that is
part of Toshiba Visconti SoCs.
The driver is located under drivers/media/platform/toshiba/visconti and uses
the Media-Controller API.

The Visconti VIIF Hardware
==========================

The Visconti VIIF hardware is a proprietary video capture device.
Following function modules are integrated:

* MIPI CSI2 receiver (CSI2RX)
* L1 Image Signal Processor (L1ISP)

  * Correction, enhancement, adjustment on RAW pictures.

* L2 Image Signal Processor (L2ISP)

  * Lens distortion correction
  * Scaling
  * Cropping

* Video DMAC

  * format picture (RGB, YUV, Grayscale, ...)
  * write picture into DRAM

Visconti5 SoC has two VIIF hardware instances.

Topology
========

.. _visconti_viif_topology_graph:

.. kernel-figure:: visconti-viif.dot
	:alt: Diagram of the default media pipeline topology
	:align: center

The driver has 5 video devices:

- viif_capture_post0: capture device for image.
    - corresponds to L2ISP and Video DMAC HW.
- viif_capture_post1: capture device for image.
    - corresponds to L2ISP and Video DMAC HW.
- viif_capture_sub: capture device for RAW image.
    - corresponds to Video DMAC HW.
- viif_params: a metadata output device that receives ISP parameters.
    - corresponds to L1ISP and L2ISP HW.
- viif_stats: a metadata capture device that sends statistics.
    - corresponds to L1ISP and L2ISP HW.

The driver has 2 subdevices:

- visconti-viif:csi2rx: CSI2 receiver operation.
    - corresponds to CSI2RX HW.
- visconti-viif:isp: Image Signal Processor operation.
    - corresponds to L1ISP and L2ISP HW.

viif_capture_post0, viif_capture_post1 - Processed Image Capture Video Node
---------------------------------------------------------------------------

These video nodes are used for capturing images processed at ISPs.
They capture only following formats:

- RGB3
- AR24
- YM16
- YM24
- Y16

Bayer format is not supported. Use viif_capture_sub instead.

POST0 and POST1 can output images from the same input image
using different cropping and scaling settings.

viif_capture_sub - Raw Image Capture Video Node
-----------------------------------------------

This video node is used for capturing bayer image from the sensor.
The output picture has exactly the same resolution and format as the sensor input.
The following depth of bayer format is supported:

- 8bit
- 10bit
- 12bit
- 14bit

visconti-viif:csi2rx - CSI2 Receiver Subdevice Node
---------------------------------------------------

This subdevice node corresponds to a MIPI CSI2 receiver.
It resides between an image sensor subdevice and the ISP subdevice.
It controls CSI2 link configuration and training process.

visconti-viif:isp - ISP Subdevice Node
--------------------------------------

This subdevice node corresponds to L1/L2 ISPs.
It receives pictures from an sensor (via CSI2RX),
applies multiple operations on pictures, then passes resulting images to capture nodes.

L1 ISP provides following operations:

- Input: accepts 8, 10, 12, 14bit bayer format
- Preprocessing: generate intermediate data (24bit RAW)
    - HDRE: HDR expansion (only for PWL image);
      see :c:type:`viif_l1_hdre_config`, :c:type:`viif_l1_input_mode_config`
    - SLIC: Image Extraction (x3 12bit planes for preprocessing);
      see :c:type:`viif_l1_img_extraction_config`
    - ABPC/DPC: Defect pixel correction :c:type:`viif_l1_dpc_config`
    - PWHB: Preset white balance; see :c:type:`viif_l1_preset_white_balance_config`
    - RCNR: RAW color noise reduction; see :c:type:`viif_l1_raw_color_noise_reduction_config`
    - HDRS: HDR synthesis; see :c:type:`viif_l1_hdrs_config`
- Processing on RAW image
    - BLVC: black level correction and normalization;
      see :c:type:`viif_l1_black_level_correction_config`
    - LSSC: Lens shading correction; see :c:type:`viif_l1_lsc_config`
    - MPRO: digital amplifier; see :c:type:`viif_l1_main_process_config`
    - MPRO: bayer demosaicing; see :c:type:`viif_l1_main_process_config`
    - MPRO: color matrix correction; see :c:type:`viif_l1_main_process_config`
    - HDRC: HDR compression;
      see :c:type:`viif_l1_hdrc_config`, :c:type:`viif_l1_hdrc_ltm_config`,
      :c:type:`viif_l1_rgb_to_y_coef_config`
- Processing on RGB/YUV image
    - VPRO: gamma correction; see :c:type:`viif_l1_gamma_config`
    - VPRO: RGB2YUV;
      see :c:type:`viif_l1_rgb_to_y_coef_config`,
      :c:type:`viif_l1_img_quality_adjustment_config`
    - VPRO: image quality adjustment; see :c:type:`viif_l1_img_quality_adjustment_config`
- Output: 16bit YUV
- Feedback loop with software
    - AWHB: auto white balance; see :c:type:`viif_l1_awb_config`,
      :c:type:`viif_isp_capture_status`
    - AEXP: auto exposure (average luminance calculation);
      see :c:type:`viif_l1_avg_lum_generation_config`,
      :c:type:`viif_l1_rgb_to_y_coef_config`, :c:type:`viif_isp_capture_status`
    - AG: analog gain calculation;
      see :c:type:`viif_l1_ag_mode_config`, :c:type:`viif_l1_ag_config`

L2 ISP provides following operations:

- Input: accepts 16bit YUV
- Operations:
    - Lens undistortion; see :c:type:`viif_l2_undist`
    - Scaling; see :c:type:`viif_l2_roi`
    - Cropping; see :c:type:`viif_l2_roi`
    - Gamma correction; see :c:type:`viif_l2_gamma_config`
    - YUV2RGB
- Output: RGB, YUV422, YUV444

ISP configurations/parameters are passed from userland via viif_params node.
The status of ISP operations are passed to userland via viif_stats node.

.. _viif_params:

viif_params - ISP Parameters Video Node
---------------------------------------

The viif_params video node receives a set of ISP parameters from userspace
to be applied to the hardware during a video stream.

The buffer format is defined by struct :c:type:`visconti_viif_isp_config`, and userspace should set
:ref:`V4L2_META_FMT_VISCONTI_VIIF_PARAMS <v4l2-meta-fmt-visconti-viif-params>` as the data format.

.. _viif_stats:

viif_stats - Statistics Video Node
----------------------------------

The viif_stats video node provides current status of ISP.

Following information is included:

* statistics of auto white balance
* average luminance information which can be used by auto exposure software impl.

The buffer format is defined by struct :c:type:`visconti_viif_isp_stat`, and userspace should set
:ref:`V4L2_META_FMT_VISCONTI_VIIF_STATS <v4l2-meta-fmt-visconti-viif-stats>` as the data format.

Capturing Video Frames Example
==============================

In the following example,
imx219 camera is connected to pad 0 of 'visconti-viif:isp'.

The following commands can be used to capture video from the main path 0
with dimension 1024x1024 and RGB format.

.. code-block:: bash

	# set the links
	media-ctl -d platform:visconti-viif-0 -r
	media-ctl -d platform:visconti-viif-0 -l '"imx219 1-0010":0 -> "visconti-viif:csi2rx":0 [1]'
	media-ctl -d platform:visconti-viif-0 -l '"visconti-viif:csi2rx":1 -> "visconti-viif:isp":0 [1]'
	media-ctl -d platform:visconti-viif-0 -l '"visconti-viif:isp":1 -> "viif_capture_post0":0 [1]'
	media-ctl -d platform:visconti-viif-0 -l '"visconti-viif:isp":2 -> "viif_capture_post1":0 [1]'
	media-ctl -d platform:visconti-viif-0 -l '"visconti-viif:isp":3 -> "viif_capture_sub":0 [1]'
	media-ctl -d platform:visconti-viif-0 -l '"viif_params":0 -> "visconti-viif:isp":4 [1]'
	media-ctl -d platform:visconti-viif-0 -l '"visconti-viif:isp":5 -> "viif_stats":0 [1]'

	# set format for imx219
	media-ctl -d platform:visconti-viif-0 --set-v4l2 '"imx219 1-0010":0 [fmt:SRGGB10_1X10/1920x1080]'

	# set format for csi2rx
	media-ctl -d platform:visconti-viif-0 --set-v4l2 '"visconti-viif:csi2rx":1 [fmt:SRGGB10_1X10/1920x1080]'

	# set format for isp pads (
	media-ctl -d platform:visconti-viif-0 --set-v4l2 '"visconti-viif:isp":1 [fmt:YUV8_1X24/1024 crop:(640,0)/1024x1024]'

	# set format for main path0
	v4l2-ctl -z platform:visconti-viif-0 -d viif_capture_post0 -v "width=1024,height=1024"
	v4l2-ctl -z platform:visconti-viif-0 -d viif_capture_post0 -v "pixelformat=RGB3"

	# start streaming
	v4l2-ctl -z platform:visconti-viif-0 -d viif_capture_post0 --stream-mmap --stream-count 10


Use of coherent memory
======================

Visconti5 SoC has two independent DDR SDRAM controllers.
Each controller is mapped to 36bit address space.

Accelerator bus masters have two paths to access memory;
one is directly connected to SDRAM controller,
the another is connected via a cache coherency bus
which keeps coherency among CPUs.

From accelerators and CPUs, the address map is following:

* 0x0_8000_0000 DDR0 direct access
* 0x4_8000_0000 DDR0 coherency bus
* 0x8_8000_0000 DDR1 direct access
* 0xC_8000_0000 DDR1 coherency bus

The base address can be specified with "memory" and "reserved-memory" elements
in a device tree description.
It's not recommended to mix direct address and coherent address.

The Visconti5 VIIF driver always use only direct address to configure Video DMACs of the hardware.
This design is to avoid great performance loss at coherency bus caused by massive memory access.
You should not put the dma_coherent attribute to viif element in device tree.
Cache operations are done automatically by videobuf2 driver.
