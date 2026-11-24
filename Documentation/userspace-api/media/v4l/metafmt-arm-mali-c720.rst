.. SPDX-License-Identifier: GPL-2.0

.. _v4l2-meta-fmt-mali-c720-stats:

*******************************************************************************
V4L2_META_FMT_MALI_C720_STATS ('C72S')
*******************************************************************************

For details on queueing statistics buffers and synchronizing them with
image buffers using Media Requests, see :ref:`mali-c720-frame-scheduling`.

3A Statistics
=============

The ISP collects statistics at different stages of frame processing. These
may describe Bayer or processed image data, depending on the statistics
block and its configuration. Userspace can obtain these statistics from the
:ref:`mali-c720 3a stats <mali-c720-3a-stats>` metadata capture video node, using
the :c:type:`v4l2_meta_format` interface.

Userspace may consume these statistics in 3A algorithms, including
auto-exposure, auto-white balance and autofocus, to compute adjustments to
sensor and ISP settings.

Statistics for the Mali-C720 use the :ref:`generic V4L2 ISP format <v4l2-isp>`.
The buffer contains a :c:type:`v4l2_isp_buffer` header followed by statistics
blocks in its ``data`` member. Each block embeds
:c:type:`v4l2_isp_block_header` as its first member, with the ``type`` member
set to a value from :c:type:`mali_c720_stats_type`. The available statistics
depend on the hardware configuration. These can be queried via the statistics
capability control
:ref:`V4L2_CID_MALI_C720_STATS_CAPABILITIES <mali-c720-controls>`.

For details of the statistics, see the statistics blocks defined in
``mali-c720-config.h``.

Arm Mali-C720 uAPI data types
=============================

.. kernel-doc:: include/uapi/linux/media/arm/mali-c720-config.h
