.. SPDX-License-Identifier: GPL-2.0

.. _v4l2-meta-fmt-mali-c55-params:
.. _v4l2-meta-fmt-mali-c55-3a-stats:

********************************************************************************
V4L2_META_FMT_MALI_C55_3A_STATS ('C55S'), V4L2_META_FMT_MALI_C55_PARAMS ('C55P')
********************************************************************************

3A Statistics
=============
The ISP device collects different statistics over an input bayer frame. Those
statistics can be obtained by userspace from the
:ref:`mali-c55 3a stats <mali-c55-3a-stats>` metadata capture video node, using
the :c:type:`v4l2_meta_format` interface. The buffer contains a single instance
of the C structure :c:type:`mali_c55_stats_buffer` defined in
``mali-c55-config.h``, so the structure can be obtained from the buffer by:

.. code-block:: C

	struct mali_c55_stats_buffer *stats =
		(struct mali_c55_stats_buffer *)buf;

For details of the statistics see :c:type:`mali_c55_stats_buffer`.

Configuration Parameters
========================

The configuration parameters are passed to the
:ref:`mali-c55 3a params <mali-c55-3a-params>` metadata output video node, using
the :c:type:`v4l2_meta_format` interface. Rather than a single struct containing
sub-structs for each configurable area of the ISP, parameters for the Mali-C55
are defined as distinct structs or "blocks" which may be added to the data
member of struct mali_c55_params_buffer. Userspace is responsible for populating
the data member with the blocks that need to be configured by the driver, but
need not populate it with **all** the blocks, or indeed with any at all if there
are no configuration changes to make. Populated blocks **must** be consecutive
in the buffer. To assist both userspace and the driver in identifying the
blocks each block-specific struct should embed
struct mali_c55_params_block_header as its first member and userspace must
populate the type member with a value from enum mali_c55_param_block_type. Once
the blocks have been populated into the data buffer, the combined size of all
populated blocks should be set in the total_size member of
struct mali_c55_params_buffer. For example:

.. code-block:: c

	struct mali_c55_params_buffer *params =
		(struct mali_c55_params_buffer *)buffer;

	params->version = MALI_C55_PARAM_BUFFER_V0;

	void *data = (void *)params->data;

	struct mali_c55_params_awb_gains *gains =
		(struct mali_c55_params_awb_gains *)data;

	gains->header.type = MALI_C55_PARAM_BLOCK_AWB_GAINS;
	gains->header.enabled = true;
	gains->header.size = sizeof(struct mali_c55_params_awb_gains);

	gains->gain00 = 256;
	gains->gain00 = 256;
	gains->gain00 = 256;
	gains->gain00 = 256;

	data += sizeof(struct mali_c55_params_awb_gains)

	struct mali_c55_params_sensor_off_preshading *blc =
		(struct mali_c55_params_sensor_off_preshading *)data;

	blc->header.type = MALI_C55_PARAM_BLOCK_SENSOR_OFFS;
	blc->header.enabled = true;
	blc->header.size = sizeof(struct mali_c55_params_sensor_off_preshading);

	blc->chan00 = 51200;
	blc->chan01 = 51200;
	blc->chan10 = 51200;
	blc->chan11 = 51200;

	params->total_size = sizeof(struct mali_c55_params_awb_gains) +
			     sizeof(struct mali_c55_params_sensor_off_preshading);

Arm Mali-C55 uAPI data types
============================

.. kernel-doc:: include/uapi/linux/media/arm/mali-c55-config.h
