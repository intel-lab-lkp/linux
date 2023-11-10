.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later

.. _audiomem2mem:

********************************
Audio Memory-To-Memory Interface
********************************

An audio memory-to-memory device can compress, decompress, transform, or
otherwise convert audio data from one format into another format, in memory.
Such memory-to-memory devices set the ``V4L2_CAP_AUDIO_M2M`` capability.
Examples of memory-to-memory devices are audio codecs, audio preprocessing,
audio postprocessing.

A memory-to-memory audio node supports both output (sending audio frames from
memory to the hardware) and capture (receiving the processed audio frames
from the hardware into memory) stream I/O. An application will have to
setup the stream I/O for both sides and finally call
:ref:`VIDIOC_STREAMON <VIDIOC_STREAMON>` for both capture and output to
start the hardware.

Memory-to-memory devices function as a shared resource: you can
open the audio node multiple times, each application setting up their
own properties that are local to the file handle, and each can use
it independently from the others. The driver will arbitrate access to
the hardware and reprogram it whenever another file handler gets access.

Audio memory-to-memory devices are accessed through character device
special files named ``/dev/v4l-audio``

Querying Capabilities
=====================

Device nodes supporting the audio memory-to-memory interface set the
``V4L2_CAP_AUDIO_M2M`` flag in the ``device_caps`` field of the
:c:type:`v4l2_capability` structure returned by the :c:func:`VIDIOC_QUERYCAP`
ioctl.

Data Format Negotiation
=======================

The audio device uses the :ref:`format` ioctls to select the capture format.
The audio buffer content format is bound to that selected format. In addition
to the basic :ref:`format` ioctls, the :c:func:`VIDIOC_ENUM_FMT` ioctl must be
supported as well.

To use the :ref:`format` ioctls applications set the ``type`` field of the
:c:type:`v4l2_format` structure to ``V4L2_BUF_TYPE_AUDIO_CAPTURE`` or to
``V4L2_BUF_TYPE_AUDIO_OUTPUT``. Both drivers and applications must set the
remainder of the :c:type:`v4l2_format` structure to 0.

.. c:type:: v4l2_audio_format

.. tabularcolumns:: |p{1.4cm}|p{2.4cm}|p{13.5cm}|

.. flat-table:: struct v4l2_audio_format
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u32
      - ``pixelformat``
      - The sample format, set by the application. see :ref:`pixfmt-audio`
    * - __u32
      - ``channels``
      - The channel number, set by the application. channel number range is
        [1, 32].
    * - __u32
      - ``buffersize``
      - Maximum buffer size in bytes required for data. The value is set by the
        driver.
