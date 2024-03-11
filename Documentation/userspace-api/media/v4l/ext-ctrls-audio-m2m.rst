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
    This control specifies the audio source sample rate, unit is Hz

``V4L2_CID_M2M_AUDIO_DEST_RATE (integer menu)``
    This control specifies the audio destination sample rate, unit is Hz

``V4L2_CID_M2M_AUDIO_SOURCE_RATE_OFFSET (fixed point)``
    This control specifies the offset from the audio source sample rate,
    unit is Hz.

    The offset compensates for any clock drift. The actual source audio
    sample rate is the ideal source audio sample rate from
    ``V4L2_CID_M2M_AUDIO_SOURCE_RATE`` plus this fixed point offset.

    The audio source clock may have some drift. Reducing or increasing the
    audio sample rate dynamically to ensure that Sample Rate Converter is
    working on the real sample rate, this feature is for the Asynchronous
    Sample Rate Converter module.
    So, userspace would be expected to be monitoring such drift
    and increasing/decreasing the sample frequency as needed by this control.

``V4L2_CID_M2M_AUDIO_DEST_RATE_OFFSET (fixed point)``
    This control specifies the offset from the audio destination sample rate,
    unit is Hz.

    The offset compensates for any clock drift. The actual destination audio
    sample rate is the ideal source audio sample rate from
    ``V4L2_CID_M2M_AUDIO_DEST_RATE`` plus this fixed point offset.

    The audio destination clock may have some drift. Reducing or increasing
    the audio sample rate dynamically to ensure that sample rate converter
    is working on the real sample rate, this feature is for the Asynchronous
    Sample Rate Converter module.
    So, userspace would be expected to be monitoring such drift
    and increasing/decreasing the sample frequency as needed by this control.
