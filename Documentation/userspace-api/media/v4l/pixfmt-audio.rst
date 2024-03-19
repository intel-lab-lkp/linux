.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later

.. _pixfmt-audio:

*************
Audio Formats
*************

These formats are used for :ref:`audiomem2mem` interface only.

All FourCCs starting with 'AU' are reserved for mappings
of the snd_pcm_format_t type.

The v4l2_audfmt_to_fourcc() is defined to convert the snd_pcm_format_t
type to a FourCC. The first character is 'A', the second character
is 'U', and the remaining two characters is the snd_pcm_format_t
value in ASCII. Example: SNDRV_PCM_FORMAT_S16_LE (with value 2)
maps to 'AU02' and SNDRV_PCM_FORMAT_S24_3LE (with value 32) maps
to 'AU32'."

The v4l2_fourcc_to_audfmt() is defined to convert these FourCCs to
snd_pcm_format_t type.

.. tabularcolumns:: |p{5.8cm}|p{1.2cm}|p{10.3cm}|

.. cssclass:: longtable

.. flat-table:: Audio Format
    :header-rows:  1
    :stub-columns: 0
    :widths:       3 1 4

    * - Identifier
      - Code
      - Details
    * .. _V4L2-AUDIO-FMT-S8:

      - ``V4L2_AUDIO_FMT_S8``
      - 'S8'
      - Corresponds to SNDRV_PCM_FORMAT_S8 in ALSA
    * .. _V4L2-AUDIO-FMT-S16-LE:

      - ``V4L2_AUDIO_FMT_S16_LE``
      - 'S16_LE'
      - Corresponds to SNDRV_PCM_FORMAT_S16_LE in ALSA
    * .. _V4L2-AUDIO-FMT-U16-LE:

      - ``V4L2_AUDIO_FMT_U16_LE``
      - 'U16_LE'
      - Corresponds to SNDRV_PCM_FORMAT_U16_LE in ALSA
    * .. _V4L2-AUDIO-FMT-S24-LE:

      - ``V4L2_AUDIO_FMT_S24_LE``
      - 'S24_LE'
      - Corresponds to SNDRV_PCM_FORMAT_S24_LE in ALSA
    * .. _V4L2-AUDIO-FMT-U24-LE:

      - ``V4L2_AUDIO_FMT_U24_LE``
      - 'U24_LE'
      - Corresponds to SNDRV_PCM_FORMAT_U24_LE in ALSA
    * .. _V4L2-AUDIO-FMT-S32-LE:

      - ``V4L2_AUDIO_FMT_S32_LE``
      - 'S32_LE'
      - Corresponds to SNDRV_PCM_FORMAT_S32_LE in ALSA
    * .. _V4L2-AUDIO-FMT-U32-LE:

      - ``V4L2_AUDIO_FMT_U32_LE``
      - 'U32_LE'
      - Corresponds to SNDRV_PCM_FORMAT_U32_LE in ALSA
    * .. _V4L2-AUDIO-FMT-FLOAT-LE:

      - ``V4L2_AUDIO_FMT_FLOAT_LE``
      - 'FLOAT_LE'
      - Corresponds to SNDRV_PCM_FORMAT_FLOAT_LE in ALSA
    * .. _V4L2-AUDIO-FMT-IEC958-SUBFRAME-LE:

      - ``V4L2_AUDIO_FMT_IEC958_SUBFRAME_LE``
      - 'IEC958_SUBFRAME_LE'
      - Corresponds to SNDRV_PCM_FORMAT_IEC958_SUBFRAME_LE in ALSA
    * .. _V4L2-AUDIO-FMT-S24-3LE:

      - ``V4L2_AUDIO_FMT_S24_3LE``
      - 'S24_3LE'
      - Corresponds to SNDRV_PCM_FORMAT_S24_3LE in ALSA
    * .. _V4L2-AUDIO-FMT-U24-3LE:

      - ``V4L2_AUDIO_FMT_U24_3LE``
      - 'U24_3LE'
      - Corresponds to SNDRV_PCM_FORMAT_U24_3LE in ALSA
    * .. _V4L2-AUDIO-FMT-S20-3LE:

      - ``V4L2_AUDIO_FMT_S20_3LE``
      - 'S20_3LE'
      - Corresponds to SNDRV_PCM_FORMAT_S24_3LE in ALSA
    * .. _V4L2-AUDIO-FMT-U20-3LE:

      - ``V4L2_AUDIO_FMT_U20_3LE``
      - 'U20_3LE'
      - Corresponds to SNDRV_PCM_FORMAT_U20_3LE in ALSA
