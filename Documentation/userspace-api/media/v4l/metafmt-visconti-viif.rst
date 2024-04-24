.. SPDX-License-Identifier: GPL-2.0

.. _v4l2-meta-fmt-visconti-viif-params:

.. _v4l2-meta-fmt-visconti-viif-stats:

***************************************************************************************
V4L2_META_FMT_VISCONTI_VIIF_PARAMS ('vifp'), V4L2_META_FMT_VISCONTI_VIIF_STATS ('vifs')
***************************************************************************************

Configuration parameters
========================

The configuration parameters are passed to the
:ref:`viif_params <viif_params>` metadata output video node, using
the :c:type:`v4l2_meta_format` interface. The buffer contains
a single instance of the C structure :c:type:`visconti_viif_isp_config` defined in
``visconti_viif.h``. So the structure can be obtained from the buffer by:

.. code-block:: c

	struct visconti_viif_isp_config *params = (struct visconti_viif_isp_config*) buffer;

VIIF statistics
===============

The VIIF device collects different statistics over an input Bayer frame.
Those statistics are obtained from the :ref:`viif_stats <viif_stats>`
metadata capture video node,
using the :c:type:`v4l2_meta_format` interface. The buffer contains a single
instance of the C structure :c:type:`visconti_viif_isp_stat` defined in
``visconti_viif.h``. So the structure can be obtained from the buffer by:

.. code-block:: c

	struct visconti_viif_isp_stat *stats = (struct visconti_viif_isp_stat*) buffer;

The statistics collected are Exposure, AWB (auto white balance) and errors.
See :c:type:`visconti_viif_isp_stat` for details of the statistics.

The statistics and configuration parameters described here are usually
consumed and produced by dedicated user space libraries that comprise the
tuning tools using software control loop.

visconti viif uAPI data types
=============================

.. kernel-doc:: include/uapi/linux/visconti_viif.h
