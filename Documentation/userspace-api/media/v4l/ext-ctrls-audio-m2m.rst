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

``V4L2_CID_M2M_AUDIO_SOURCE_RATE (integer menu)``
    Sets the audio source sample rate, unit is Hz

``V4L2_CID_M2M_AUDIO_DEST_RATE (integer menu)``
    Sets the audio destination sample rate, unit is Hz

``V4L2_CID_M2M_AUDIO_SOURCE_RATE_OFFSET (fixed point)``
    Sets the offset from the audio source sample rate, unit is Hz.
    The offset compensates for any clock drift. The actual source audio
    sample rate is the ideal source audio sample rate from
    ``V4L2_CID_M2M_AUDIO_SOURCE_RATE`` plus this fixed point offset.

``V4L2_CID_M2M_AUDIO_DEST_RATE_OFFSET (fixed point)``
    Sets the offset from the audio destination sample rate, unit is Hz.
    The offset compensates for any clock drift. The actual destination audio
    sample rate is the ideal source audio sample rate from
    ``V4L2_CID_M2M_AUDIO_DEST_RATE`` plus this fixed point offset.
