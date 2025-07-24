.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later
.. c:namespace:: V4L

.. _vidioc_subdev_bind_context:

********************************
ioctl VIDIOC_SUBDEV_BIND_CONTEXT
********************************

Name
====

VIDIOC_SUBDEV_BIND_CONTEXT - Bind a subdevice file handle to a media device
context

Synopsis
========

.. c:macro:: VIDIOC_SUBDEV_BIND_CONTEXT

``int ioctl(int fd, VIDIOC_SUBDEV_BIND_CONTEXT, struct v4l2_subdev_bind_context *argp)``

Arguments
=========

``fd``
    File descriptor returned by :c:func:`open()`.

``argp``
    Pointer to struct :c:type:`v4l2_subdev_bind_context`.

Description
===========

Applications call the ``VIDIOC_SUBDEV_BIND_CONTEXT`` ioctl to bind a subdevice
file handle to a media device  context. Binding a subdevice file handle to a
media device context creates an isolated execution context which allows to
multiplex the usage of a video device. This means, in practice, that the
subdevice configuration (format, sizes etc) applied on a file handle bound to a
media device context won't be visible on file handles bound to a different media
device context (or not bound at all).

By opening a media device applications create a media device context to which
video devices and subdevices file handles can be bound to. The file descriptor
returned by a call to :c:func:`open()` on the media device identifies uniquely
the media device context. Application populates the ``context_fd`` field of
:c:type:`v4l2_subdev_bind_context` with the file descriptor of an open media
device to identify the media context to which they want to bind a subdevice
to.

Applications can open a subdevice node multiple times, and call
``VIDIOC_BIND_CONTEXT`` on each file handle returned by a successful call to
:c:func:`open()` to isolate the operations performed on that file handle from
any operation performed on other file handles bound to different contexts. This
means, in example, that the subdevice format and sizes are isolated from the
ones associated with a file descriptor, obtained by opening the same subdevice
but bound to a different media device context (or not bound at all).

The bounding operation realizes a permanent association valid until the
subdevice context is released by closing the file handle.

A subdevice file handle can be bound to the same media device context once
only. Trying to bind the same file handle to the same media device context a
second time, without releasing the already established context by closing the
bound file descriptor first, will result in an error.

Bounding is an opt-in feature that applications are free to ignore. Any
operation directed to a non bound file handle will continue to work as it used
to, and the video device configuration (formats, sizes etc) will be visible
across all the other non-bound file handles.

Return Value
============

On success 0 is returned, on error -1 and the ``errno`` variable is set
appropriately. The generic error codes are described at the
:ref:`Generic Error Codes <gen-errors>` chapter.

EINVAL
    The media device context file handle ``context_fd`` is not valid or the
    subdevice file handle is already bound to a context.
