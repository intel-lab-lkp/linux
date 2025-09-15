.. SPDX-License-Identifier: GPL-2.0

V4L2 generic ISP parameters and statistics support
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

ISP configuration parameters and statistics are processed and collected by
drivers and exchanged with userspace through data types that usually
reflect the ISP peripheral registers layout.

Each ISP driver defines its own metadata format for parameters and statistics,
and exposing the registers layout in the format definition, part of the Linux
kernel uAPI/uABI interface, makes it really hard, if not impossible, to extend
the format in a backward compatible way to support new hardware blocks or
different revisions of the same peripheral IP.

For these reasons Video4Linux2 defines generic types for ISP configuration
parameters and statistics with a set of associated helpers to support drivers
and users using generic types.

Generic ISP configuration parameters
====================================

Drivers can use the generic and extensible configuration parameters format by
re-using the types defined in the include/uapi/linux/media/v4l2-isp.h uAPI
header file.

The uAPI header defines generic types which the driver is expected to re-use and
provide definitions for the types of supported ISP blocks, their control
flags and the expected maximum size of a buffer of parameters.

When a driver receives a buffer of parameters from userspace it shall validate
it by using the helper functions and types available in include/media/v4l2-isp.h
before accessing the buffer to apply the parameters to the hardware registers.

Generic ISP support driver documentation
========================================
.. kernel-doc:: include/media/v4l2-isp.h
