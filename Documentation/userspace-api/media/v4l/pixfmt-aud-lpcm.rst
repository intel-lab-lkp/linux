.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later

.. _v4l2-audio-fmt-lpcm:

****************************
V4L2_AUDIO_FMT_LPCM ('LPCM')
****************************

Linear Pulse-Code Modulation (LPCM)


Description
===========

LPCM audio is coded using a combination of values such as:

Sample rate: which is the number of times per second that samples are taken,
The typical rates are 8kHz, 11025Hz, 16kHz, 22050Hz, 32kHz, 44100Hz, 48kHz,
88200Hz, 96kHz, 176400Hz, 192kHz...

Sample format: which determines the number of possible digital values that
can be used to represent each sample. The format can be SND_PCM_FORMAT_S8,
SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S24_LE, SND_PCM_FORMAT_S32_LE...

Channels: It is the "location" or "passageway" of a specific signal or
data in a piece of audio. The channel number can be 1,2,3...

Please refer to https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html
for more detail

Each sample contains several channels data,  the channel data format is
defined by sample format.

.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - Sample 0:
      - Channel 0
      - Channel 1
      - Channel 2
      - Channel 3
      - ...
    * - Sample 1:
      - Channel 0
      - Channel 1
      - Channel 2
      - Channel 3
      - ...
    * - Sample 2:
      - Channel 0
      - Channel 1
      - Channel 2
      - Channel 3
      - ...
    * - Sample 3:
      - Channel 0
      - Channel 1
      - Channel 2
      - Channel 3
      - ...
