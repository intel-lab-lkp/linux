.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later

.. _audiodev:

***************
Audio Interface
***************

The audio interface is implemented on audio device nodes. The audio device
which uses application software for modulation or demodulation. This
interface is intended for controlling and data streaming of such devices

Audio devices are accessed through character device special files named
``/dev/v4l-audio``

Querying Capabilities
=====================

Device nodes supporting the audio capture and output interface set the
``V4L2_CAP_AUDIO_M2M`` flag in the ``device_caps`` field of the
:c:type:`v4l2_capability` structure returned by the :c:func:`VIDIOC_QUERYCAP`
ioctl.

At least one of the read/write or streaming I/O methods must be supported.


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
      - ``rate``
      - The sample rate, set by the application. The range is [5512, 768000].
    * - __u32
      - ``format``
      - The sample format, set by the application. format is defined as
        SNDRV_PCM_FORMAT_S8, SNDRV_PCM_FORMAT_U8, ...,
    * - __u32
      - ``channels``
      - The channel number, set by the application. channel number range is
        [1, 32].
    * - __u32
      - ``buffersize``
      - Maximum buffer size in bytes required for data. The value is set by the
        driver.
