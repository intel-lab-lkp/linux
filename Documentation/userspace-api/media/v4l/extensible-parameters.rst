.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later

.. _extensible-parameters:

*************************************
V4L2 extensible ISP parameters format
*************************************

ISP configuration
=================

ISP configuration parameters are computed by userspace and programmed into a
*parameters buffer* which is queued to the ISP driver on a per-frame basis. The
layout of the *parameters buffer* generally reflects the ISP peripheral
registers layout and is, for this reason, platform specific.

The ISP configuration parameters are passed to the ISP driver through a metadata
output video node, using the :c:type:`v4l2_meta_format` interface. Each ISP
driver defines a metadata format that implements the configuration parameters
layout.

Metadata output formats that describe ISP configuration parameters are most of
the time realized by implementing C structures that reflect the registers layout
and gets populated by userspace before queueing the buffer to the ISP. Each
C structure usually corresponds to one ISP *processing block*, with each block
implementing one of the ISP supported features.

The uAPI/ABI problem
--------------------

By upstreaming data types that describe the configuration parameters layout,
driver developers make them part of the Linux kernel ABI. As it sometimes
happens for most peripherals in Linux, ISP drivers development is often an
iterative process, where sometimes not all the hardware features are supported
in the first version that lands in the kernel, and some parts of the interface
have to later be modified for bug-fixes or improvements.

If any later bug-fix/improvement requires changes to the metadata output format,
this is considered an ABI-breakage that is strictly forbidden by the Linux
kernel policies. For this reason, each new iteration of an ISP driver support
would require defining a new metadata output format, implying that drivers have
to be made ready to handle several different configuration formats.

Support for generic ISP parameters buffer has been designed with the goal of
being:

- Extensible: new features can be added later on without breaking the existing
  interface
- Versioned: different versions of the format can be defined without
  breaking the existing interface

The extensible parameters format
================================

Extensible configuration parameters formats are realized by a defining a single
C structure that contains a few control parameters and a binary buffer where
userspace programs a variable number of *ISP configuration blocks* data.

The generic :c:type:`v4l2_params_buffer` defines a base type that each driver
can use by properly sizing the data buffer array by providing a definition of
maximum supported parameters buffer size.

Each *ISP configuration block* is identified by an header and contains the
parameters for that specific block.

The generic :c:type:`v4l2_params_block_header` defines a base type that each
driver can re-use as it is or extend appropriately.

Userspace applications are responsible for correctly populating the parameters
block header fields (type, flags and size) and the block-specific parameters.

When userspace wants to configure and enable an ISP block it shall fully
populate the block configuration and set the V4L2_PARAMS_FL_BLOCK_ENABLE
bit in the flags field.

When userspace simply wants to disable an ISP block the
V4L2_PARAMS_FL_BLOCK_DISABLE bit should be set in flags field. The driver
ignores the rest of the block configuration structure in this case.

If a new configuration of an ISP block has to be applied, userspace shall fully
populate the ISP block configuration and omit setting the
V4L2_PARAMS_FL_BLOCK_ENABLE and V4L2_PARAMS_FL_BLOCK_DISABLE bits in
the flags field.

Setting both the V4L2_PARAMS_FL_BLOCK_ENABLE and V4L2_PARAMS_FL_BLOCK_DISABLE
bits in the flags field is not allowed and not accepted.

Any further development that happens after the ISP driver has been merged in
Linux and which requires supporting new ISP features can be implemented by
adding new blocks definition without invalidating the existing ones. Similarly,
any change to the existing ISP configuration blocks can be handled by versioning
them, again without invalidating the existing ones.

V4L2 extensible parameters uAPI data types
==========================================

.. kernel-doc:: include/uapi/linux/media/v4l2-isp.h
