.. SPDX-License-Identifier: GPL-2.0

.. _v4l2-meta-fmt-mali-c720-params:
.. _v4l2-meta-fmt-mali-c720-stats:

*******************************************************************************
V4L2_META_FMT_MALI_C720_STATS ('C72S'), V4L2_META_FMT_MALI_C720_PARAMS ('C72P')
*******************************************************************************

For details on queueing statistics and parameter buffers and synchronizing
them with image buffers using Media Requests, see
:ref:`mali-c720-frame-scheduling`.

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

Configuration Parameters
========================

The configuration parameters are passed to the :ref:`mali-c720 3a params
<mali-c720-3a-params>` metadata output video node, using the
:c:type:`v4l2_meta_format` interface. Parameters for the Mali-C720 use the
:ref:`generic V4L2 ISP format <v4l2-isp>`, through which groups of parameters
are defined as distinct structures or blocks in the ``data`` member of
:c:type:`v4l2_isp_buffer`. Userspace is responsible for populating the data
member with the blocks that need to be configured by the driver. It is also
responsible for clearing the parameter buffer before writing new parameters.
Each block-specific structure embeds :c:type:`v4l2_isp_block_header` as its
first member and userspace must populate the ``type`` member with a value from
:c:type:`mali_c720_param_block_type`, provided that type is supported based
on querying the parameters capability control
:ref:`V4L2_CID_MALI_C720_PARAM_CAPABILITIES <mali-c720-controls>`.

Each block-specific structure embeds an ``updated_mask``. The corresponding
``mali_c720_params_*_mask`` enum defines bit positions: set ``1ULL << value``
for each field or group of fields to update. Other fields retain their previous
configuration.

.. code-block:: c

    struct v4l2_isp_buffer *params =
            (struct v4l2_isp_buffer *)buffer;

    memset(buffer, 0, buffer_size);

    params->version = V4L2_ISP_VERSION_V1;
    params->data_size = 0;

    void *data = (void *)params->data;

    struct mali_c720_params_awb_gains *gains =
            (struct mali_c720_params_awb_gains *)data;

    gains->header.type = MALI_C720_PARAM_BLOCK_AWB_GAINS_HV;
    gains->header.flags |= V4L2_ISP_PARAMS_FL_BLOCK_ENABLE;
    gains->header.size = sizeof(struct mali_c720_params_awb_gains);

    gains->gain00 = 256;
    gains->gain01 = 256;
    gains->gain10 = 256;
    gains->gain11 = 256;

    gains->updated_mask =
        (1ULL << MALI_C720_PARAMS_AWB_GAINS_MASK_GAIN00) |
        (1ULL << MALI_C720_PARAMS_AWB_GAINS_MASK_GAIN01) |
        (1ULL << MALI_C720_PARAMS_AWB_GAINS_MASK_GAIN10) |
        (1ULL << MALI_C720_PARAMS_AWB_GAINS_MASK_GAIN11);

    data += sizeof(struct mali_c720_params_awb_gains);
    params->data_size += sizeof(struct mali_c720_params_awb_gains);

    struct mali_c720_params_digital_gain *gain =
            (struct mali_c720_params_digital_gain *)data;

    gain->header.type = MALI_C720_PARAM_BLOCK_DIGITAL_GAIN_INP;
    gain->header.flags |= V4L2_ISP_PARAMS_FL_BLOCK_ENABLE;
    gain->header.size = sizeof(struct mali_c720_params_digital_gain);

    gain->gain = 512; // 2x gain
    gain->offset = 51200; // Black level offset
    gain->updated_mask =
        (1ULL << MALI_C720_PARAMS_DIGITAL_GAIN_MASK_GAIN) |
        (1ULL << MALI_C720_PARAMS_DIGITAL_GAIN_MASK_OFFSET);

    params->data_size += sizeof(struct mali_c720_params_digital_gain);

Arm Mali-C720 uAPI data types
=============================

.. kernel-doc:: include/uapi/linux/media/arm/mali-c720-config.h
