.. SPDX-License-Identifier: GPL-2.0

*****************************************
V4L2_META_FMT_NEO_ISP_EXT_PARAMS ('nnep')
*****************************************

The Neoisp image signal processor is configured by userspace through a buffer
of parameters to :ref:`neoisp-params <neoisp_params>` output device node using
the :c:type:`v4l2_meta_format` interface.

The parameters buffer uses the generic `extensible parameters` configuration
format.

.. _v4l2-meta-fmt-neo-isp-ext-params:

Extensible parameters configuration format
==========================================

When using the `extensible parameters` configuration format, parameters are
passed to the :ref:`neoisp-params <neoisp_params>` metadata output video node
using the `V4L2_META_FMT_NEO_ISP_EXT_PARAMS` meta format.

The buffer contains a single instance of the C structure
:c:type:`v4l2_isp_params_buffer` defined in ``v4l2-isp.h``. The
:c:type:`v4l2_isp_params_buffer` structure is designed to allow userspace to
populate the data buffer with only the configuration data for the Neoisp blocks
it intends to configure. The extensible parameters format design allows
developers to define new block types to support new configuration parameters,
and defines a versioning scheme so that it can be extended and versioned
without breaking compatibility with existing applications.

****************************************
V4L2_META_FMT_NEO_ISP_EXT_STATS ('nnes')
****************************************

The Neoisp image signal processor generates statistics data while processing an
input image. These statistics are captured in a buffer and provided to
userspace through the :ref:`neoisp-stats <neoisp_stats>` capture video node
using the :c:type:`v4l2_meta_format` interface. The statistics data are
processed by userspace application to produce the next Neoisp parameters.

The statistics buffer uses the generic `extensible statistics` format.

.. _v4l2-meta-fmt-neo-isp-ext-stats:

Extensible statistics format
============================

When using the `extensible statistics` format, the statistics buffer is passed
from the :ref:`neoisp-stats <neoisp_stats>` metadata capture video node using
the `V4L2_META_FMT_NEO_ISP_EXT_STATS` meta format.

The buffer contains a single instance of the C structure
:c:type:`v4l2_isp_stats_buffer` defined in ``v4l2-isp.h``. The
:c:type:`v4l2_isp_stats_buffer` structure is designed to allow future Neoisp
driver versions to populate the statistics buffer with future blocks
statistics, and defines a versioning scheme so that it can be extended and
versioned without breaking compatibility with existing applications.

**********************
Neoisp uAPI data types
**********************

This chapter describes the data types exposed to userspace by Neoisp driver.

Some structure members are in a fixed-point format, in this case the related
description will be ended by a fixed-point definition between parenthesis.

.. kernel-doc:: include/uapi/linux/media/nxp/nxp_neoisp.h

