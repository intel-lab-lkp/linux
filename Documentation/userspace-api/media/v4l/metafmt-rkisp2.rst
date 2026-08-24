.. SPDX-License-Identifier: GPL-2.0

.. _v4l2-meta-fmt-rkisp2-params:

************************************
V4L2_META_FMT_RKISP2_PARAMS ('RK2P')
************************************

Configuration Parameters
========================

The configuration of the RkISP1 ISP is performed by userspace by providing
parameters for the ISP to the driver using the :c:type:`v4l2_meta_format`
interface.

The configuration parameters are passed to the :ref:`rkisp2_params
<rkisp2_params>` metadata output video node, using the
:c:type:`v4l2_meta_format` interface. Rather than a single struct containing
sub-structs for each configurable area of the ISP, parameters for the Mali-C55
use the v4l2-isp parameters system, through which groups of parameters are
defined as distinct structs or "blocks" which may be added to the data member
of :c:type:`v4l2_isp_params_buffer`. Userspace is responsible for populating
the data member with the blocks that need to be configured by the driver. Each
block-specific struct embeds :c:type:`v4l2_isp_params_block_header` as its
first member and userspace must populate the type member with a value from
:c:type:`rkisp2_params_block_type`.

.. code-block:: c

	struct v4l2_isp_params_buffer *params =
		(struct v4l2_isp_params_buffer *)buffer;

	params->version = V4L2_ISP_PARAMS_VERSION_V1;
	params->data_size = 0;

	void *data = (void *)params->data;

	struct rkisp2_params_awb_gains *gains =
		(struct rkisp2_params_awb_gains *)data;

	gains->header.type = RKISP2_PARAMS_BLOCK_AWB_GAINS;
	gains->header.flags |= V4L2_ISP_PARAMS_FL_BLOCK_ENABLE;
	gains->header.size = sizeof(struct rkisp2_params_awb_gains);

        gains->gains[0].gb = 256;
        gains->gains[0].b = 256;
        gains->gains[0].r = 256;
        gains->gains[0].gr = 256;

	data += sizeof(struct rkisp2_params_awb_gains);
	params->data_size += sizeof(struct rkisp2_params_awb_gains);

	struct rkisp2_params_bls *bls = (struct rkisp2_params_bls *)data;

	bls->header.type = RKISP2_PARAMS_BLOCK_BLS;
	bls->header.flags |= V4L2_ISP_PARAMS_FL_BLOCK_ENABLE;
	bls->header.size = sizeof(struct rkisp2_params_bls);

        bls->enable_auto = 0;
        bls->bls_fixed_val.a = 256;
        bls->bls_fixed_val.b = 256;
        bls->bls_fixed_val.c = 256;
        bls->bls_fixed_val.d = 256;

	params->data_size += sizeof(struct rkisp2_params_bls);

rkisp2 uAPI data types
======================

.. kernel-doc:: include/uapi/linux/rkisp2-config.h
