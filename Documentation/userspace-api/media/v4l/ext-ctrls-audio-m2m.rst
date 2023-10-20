.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later

.. _audiom2m-controls:

***************************
Audio M2M Control Reference
***************************

The Audio M2M class includes controls for audio memory-to-memory
use cases. The controls can be used for audio codecs, audio
preprocessing, audio postprocessing.

Audio M2M Control IDs
-----------------------

.. _audiom2m-control-id:

``V4L2_CID_M2M_AUDIO_CLASS (class)``
    The Audio M2M class descriptor. Calling
    :ref:`VIDIOC_QUERYCTRL` for this control will
    return a description of this control class.

.. _v4l2-audio-asrc:

``V4L2_CID_M2M_AUDIO_SOURCE_RATE``
    Sets the audio source rate, unit is (Hz)

``V4L2_CID_M2M_AUDIO_DEST_RATE``
    Sets the audio destination rate, unit is (Hz)

``V4L2_CID_M2M_AUDIO_SOURCE_RATE_OFFSET``
    Sets the offset for audio source rate, unit is (Hz).
    Offset expresses the drift of clock if there is. It is
    equal to real rate minus ideal rate.

``V4L2_CID_M2M_AUDIO_DEST_RATE_OFFSET``
    Sets the offset for audio destination rate, unit is (Hz)
    Offset expresses the drift of clock if there is. It is
    equal to real rate minus ideal rate.
