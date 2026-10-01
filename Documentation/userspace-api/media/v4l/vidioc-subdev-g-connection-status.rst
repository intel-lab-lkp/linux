.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later
.. c:namespace:: V4L

.. _VIDIOC_SUBDEV_G_CONNECTION_STATUS:

****************************************
ioctl VIDIOC_SUBDEV_G_CONNECTION_STATUS
****************************************

Name
====

VIDIOC_SUBDEV_G_CONNECTION_STATUS - Query whether the sub-device hardware is
physically connected.

Synopsis
========

.. c:macro:: VIDIOC_SUBDEV_G_CONNECTION_STATUS

``int ioctl(int fd, VIDIOC_SUBDEV_G_CONNECTION_STATUS, struct v4l2_subdev_connected_status *argp)``

Arguments
=========

``fd``
    File descriptor returned by :c:func:`open()`.

``argp``
    Pointer to struct :c:type:`v4l2_subdev_connected_status`.

Description
===========

The ``VIDIOC_SUBDEV_G_CONNECTION_STATUS`` ioctl allows userspace to query
whether the hardware behind a V4L2 sub-device is physically connected.

The ioctl takes a pointer to a struct :c:type:`v4l2_subdev_connected_status`
which is filled by the driver. The driver probes the hardware and reports the
connection status.

.. tabularcolumns:: |p{1.5cm}|p{2.9cm}|p{12.9cm}|

.. c:type:: v4l2_subdev_connected_status

.. flat-table:: struct v4l2_subdev_connected_status
    :header-rows:  0
    :stub-columns: 0
    :widths:       3 4 20

    * - __u32
      - ``status``
      - Connection status of the sub-device, see
	:ref:`subdev-connection-status`.
    * - __u32
      - ``reserved``\ [7]
      - Reserved for future extensions. Set to 0 by the V4L2 core.

.. tabularcolumns:: |p{6.8cm}|p{2.4cm}|p{8.1cm}|

.. _subdev-connection-status:

.. flat-table:: Connection Status Values
    :header-rows:  1
    :stub-columns: 0
    :widths:       3 1 4

    * - Status
      - Value
      - Description
    * - ``V4L2_SUBDEV_STATUS_CONNECTED``
      - 1
      - The sub-device hardware is connected and reachable.
    * - ``V4L2_SUBDEV_STATUS_DISCONNECTED``
      - 2
      - The sub-device hardware is not connected or not reachable.
    * - ``V4L2_SUBDEV_STATUS_UNKNOWN``
      - 3
      - The connection status could not be determined.

Return Value
============

On success 0 is returned, on error -1 and the ``errno`` variable is set
appropriately. The generic error codes are described at the
:ref:`Generic Error Codes <gen-errors>` chapter.

ENOIOCTLCMD
   The kernel does not support this ioctl.
