.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later
.. c:namespace:: V4L

.. _VIDIOC_DELETE_BUFS:

************************
ioctl VIDIOC_DELETE_BUFS
************************

Name
====

VIDIOC_DELETE_BUFS - Deletes buffers from a queue

Synopsis
========

.. c:macro:: VIDIOC_DELETE_BUFs

``int ioctl(int fd, VIDIOC_DELETE_BUFs, struct v4l2_delete_buffers *argp)``

Arguments
=========

``fd``
    File descriptor returned by :c:func:`open()`.

``argp``
    Pointer to struct :c:type:`v4l2_delete_buffers`.

Description
===========

Applications can optionally call the :ref:`VIDIOC_DELETE_BUFS` ioctl to
delete buffers from a queue.
:ref:`VIDIOC_CREATE_BUFS` ioctl support is mandatory to enable :ref:`VIDIOC_DELETE_BUFS`.

.. c:type:: v4l2_delete_buffers

.. tabularcolumns:: |p{4.4cm}|p{4.4cm}|p{8.5cm}|

.. flat-table:: struct v4l2_delete_buffers
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u32
      - ``index``
      - The starting buffer index to delete.
    * - __u32
      - ``count``
      - The number of buffers to be deleted with indices 'index' until 'index + count - 1'.
        All buffers in this range must be valid and in DEQUEUED state.
        If count is set to 0 :ref:`VIDIOC_DELETE_BUFS` will do nothing and return 0.
    * - __u32
      - ``type``
      - Type of the stream or buffers, this is the same as the struct
	:c:type:`v4l2_format` ``type`` field. See
	:c:type:`v4l2_buf_type` for valid values.
    * - __u32
      - ``reserved``\ [13]
      - A place holder for future extensions. Drivers and applications
	must set the array to zero.

Return Value
============

On success 0 is returned, on error -1 and the ``errno`` variable is set
appropriately. The generic error codes are described at the
:ref:`Generic Error Codes <gen-errors>` chapter.

EBUSY
    File I/O is in progress.
    Any buffer in range ``index`` to ``index + count - 1`` is not in
    DEQUEUED state.

EINVAL
    Any buffer in range ``index`` to ``index + count - 1`` doesn't exist in the queue.
