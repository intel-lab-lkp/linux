.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later

.. _v4l2-isp:

************************
Generic V4L2 ISP formats
************************

ISP configuration and statistics: theory of operations
======================================================

ISP configuration parameters are computed by userspace and programmed into a
*parameters buffer* which is queued to the ISP driver on a per-frame basis.

ISP statistics are collected at a specific time point and drivers use them to
populate a *statistics buffer* which is then returned to userspace.

The parameters and statistics buffers are organized in a driver-specific
way, and their data layout differs between one driver and another.

ISP drivers generally exchange parameters and statistics with userspace through
a metadata output and capture node respectively, implementing the
:c:type:`v4l2_meta_format` interface. Each ISP driver defines one metadata
capture format and one metadata output format to be used on those video nodes,
and the buffer content layout and organization is fixed by the format definition.

The uAPI/ABI problem
--------------------

By upstreaming the metadata formats that describe the parameters and statistics
buffers layout, driver developers make them part of the Linux kernel ABI. As for
most peripherals, ISP driver development in Linux is often an iterative process,
in which not all of the hardware features are supported in the first version.

The support for new features and/or bug fixes may land in the kernel at a later
stage and require changes to the metadata formats definition. This is
considered an ABI breakage that is strictly forbidden by the Linux kernel
policies. For this reason, any change in the ISP parameters and statistics
buffer layout would require defining a new metadata format.

For these reasons Video4Linux2 has introduced support for generic ISP parameters
and statistics data types, designed with the goal of being:

- Extensible: new features can be added later on without breaking the existing
  interface
- Versioned: different versions of the format can be defined without
  breaking the existing interface

ISP configuration
=================

Before the introduction of generic formats
------------------------------------------

Metadata output formats that describe ISP configuration parameters were
typically realized by defining C structures that reflect the ISP registers
layout and get populated by userspace before queueing the buffer to the ISP.
Each C structure usually corresponds to one ISP *processing block*, with each
block implementing one of the ISP supported features.

The number of supported ISP blocks, the layout of their configuration data are
fixed by the format definition, incurring in the above described uAPI/uABI
problem.

Generic ISP parameters
----------------------

The generic ISP configuration parameters format is realized by a defining a
single C structure that contains a header, followed by a binary buffer where
userspace programs a variable number of ISP configuration data block, one for
each supported ISP feature.

The :c:type:`v4l2_isp_params_buffer` structure defines the parameters buffer
header which is followed by a binary buffer of ISP configuration parameters.
Userspace shall correctly populate the buffer header with the versioning
information and with the size (in bytes) of the binary data buffer where it will
store the ISP blocks configuration.

Each *ISP configuration block* is preceded by an header implemented by the
:c:type:`v4l2_isp_params_block_header` structure, followed by the configuration
parameters for that specific block, defined by the ISP driver specific data
types.

Userspace applications are responsible for correctly populating each block's
header fields (type, flags and size) and the block-specific parameters.

ISP Block enabling, disabling and configuration
-----------------------------------------------

When userspace wants to configure and enable an ISP block it shall fully
populate the block configuration and set the V4L2_ISP_PARAMS_FL_BLOCK_ENABLE
bit in the block header's `flags` field.

When userspace simply wants to disable an ISP block the
V4L2_ISP_PARAMS_FL_BLOCK_DISABLE bit should be set in block header's `flags`
field. Drivers accept a configuration parameters block with no additional
data after the header in this case.

If the configuration of an already active ISP block has to be updated,
userspace shall fully populate the ISP block parameters and omit setting the
V4L2_ISP_PARAMS_FL_BLOCK_ENABLE and V4L2_ISP_PARAMS_FL_BLOCK_DISABLE bits in the
header's `flags` field.

Setting both the V4L2_ISP_PARAMS_FL_BLOCK_ENABLE and
V4L2_ISP_PARAMS_FL_BLOCK_DISABLE bits in the flags field is not allowed and not
accepted.

Any further extension to the parameters layout that happens after the ISP driver
has been merged in Linux can be implemented by adding new blocks definition
without invalidating the existing ones.

ISP statistics
==============

Support for generic statistics format is not yet implemented in Video4Linux2.

V4L2 ISP uAPI data types
========================

.. kernel-doc:: include/uapi/linux/media/v4l2-isp.h
