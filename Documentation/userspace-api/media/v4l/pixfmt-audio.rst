.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later

.. _pixfmt-audio:

*************
Audio Formats
*************

These formats are used for :ref:`audiomem2mem` interface only.

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
    * .. _V4L2-AUDIO-FMT-U8:

      - ``V4L2_AUDIO_FMT_U8``
      - 'U8'
      - Corresponds to SNDRV_PCM_FORMAT_U8 in ALSA
    * .. _V4L2-AUDIO-FMT-S16-LE:

      - ``V4L2_AUDIO_FMT_S16_LE``
      - 'S16_LE'
      - Corresponds to SNDRV_PCM_FORMAT_S16_LE in ALSA
    * .. _V4L2-AUDIO-FMT-S16-BE:

      - ``V4L2_AUDIO_FMT_S16_BE``
      - 'S16_BE'
      - Corresponds to SNDRV_PCM_FORMAT_S16_BE in ALSA
    * .. _V4L2-AUDIO-FMT-U16-LE:

      - ``V4L2_AUDIO_FMT_U16_LE``
      - 'U16_LE'
      - Corresponds to SNDRV_PCM_FORMAT_U16_LE in ALSA
    * .. _V4L2-AUDIO-FMT-U16-BE:

      - ``V4L2_AUDIO_FMT_U16_BE``
      - 'U16_BE'
      - Corresponds to SNDRV_PCM_FORMAT_U16_BE in ALSA
    * .. _V4L2-AUDIO-FMT-S24-LE:

      - ``V4L2_AUDIO_FMT_S24_LE``
      - 'S24_LE'
      - Corresponds to SNDRV_PCM_FORMAT_S24_LE in ALSA
    * .. _V4L2-AUDIO-FMT-S24-BE:

      - ``V4L2_AUDIO_FMT_S24_BE``
      - 'S24_BE'
      - Corresponds to SNDRV_PCM_FORMAT_S24_BE in ALSA
    * .. _V4L2-AUDIO-FMT-U24-LE:

      - ``V4L2_AUDIO_FMT_U24_LE``
      - 'U24_LE'
      - Corresponds to SNDRV_PCM_FORMAT_U24_LE in ALSA
    * .. _V4L2-AUDIO-FMT-U24-BE:

      - ``V4L2_AUDIO_FMT_U24_BE``
      - 'U24_BE'
      - Corresponds to SNDRV_PCM_FORMAT_U24_BE in ALSA
    * .. _V4L2-AUDIO-FMT-S32-LE:

      - ``V4L2_AUDIO_FMT_S32_LE``
      - 'S32_LE'
      - Corresponds to SNDRV_PCM_FORMAT_S32_LE in ALSA
    * .. _V4L2-AUDIO-FMT-S32-BE:

      - ``V4L2_AUDIO_FMT_S32_BE``
      - 'S32_BE'
      - Corresponds to SNDRV_PCM_FORMAT_S32_BE in ALSA
    * .. _V4L2-AUDIO-FMT-U32-LE:

      - ``V4L2_AUDIO_FMT_U32_LE``
      - 'U32_LE'
      - Corresponds to SNDRV_PCM_FORMAT_U32_LE in ALSA
    * .. _V4L2-AUDIO-FMT-U32-BE:

      - ``V4L2_AUDIO_FMT_U32_BE``
      - 'U32_BE'
      - Corresponds to SNDRV_PCM_FORMAT_U32_BE in ALSA
    * .. _V4L2-AUDIO-FMT-FLOAT-LE:

      - ``V4L2_AUDIO_FMT_FLOAT_LE``
      - 'FLOAT_LE'
      - Corresponds to SNDRV_PCM_FORMAT_FLOAT_LE in ALSA
    * .. _V4L2-AUDIO-FMT-FLOAT-BE:

      - ``V4L2_AUDIO_FMT_FLOAT_BE``
      - 'FLOAT_BE'
      - Corresponds to SNDRV_PCM_FORMAT_FLOAT_BE in ALSA
    * .. _V4L2-AUDIO-FMT-FLOAT64-LE:

      - ``V4L2_AUDIO_FMT_FLOAT64_LE``
      - 'FLOAT64_LE'
      - Corresponds to SNDRV_PCM_FORMAT_FLOAT64_LE in ALSA
    * .. _V4L2-AUDIO-FMT-FLOAT64-BE:

      - ``V4L2_AUDIO_FMT_FLOAT64_BE``
      - 'FLOAT64_BE'
      - Corresponds to SNDRV_PCM_FORMAT_FLOAT64_BE in ALSA
    * .. _V4L2-AUDIO-FMT-IEC958-SUBFRAME-LE:

      - ``V4L2_AUDIO_FMT_IEC958_SUBFRAME_LE``
      - 'IEC958_SUBFRAME_LE'
      - Corresponds to SNDRV_PCM_FORMAT_IEC958_SUBFRAME_LE in ALSA
    * .. _V4L2-AUDIO-FMT-IEC958-SUBFRAME-BE:

      - ``V4L2_AUDIO_FMT_IEC958_SUBFRAME_BE``
      - 'IEC958_SUBFRAME_BE'
      - Corresponds to SNDRV_PCM_FORMAT_IEC958_SUBFRAME_BE in ALSA
    * .. _V4L2-AUDIO-FMT-S20-LE:

      - ``V4L2_AUDIO_FMT_S20_LE``
      - 'S20_LE'
      - Corresponds to SNDRV_PCM_FORMAT_S20_LE in ALSA
    * .. _V4L2-AUDIO-FMT-S20-BE:

      - ``V4L2_AUDIO_FMT_S20_BE``
      - 'S20_BE'
      - Corresponds to SNDRV_PCM_FORMAT_S20_BE in ALSA
    * .. _V4L2-AUDIO-FMT-U20-LE:

      - ``V4L2_AUDIO_FMT_U20_LE``
      - 'U20_LE'
      - Corresponds to SNDRV_PCM_FORMAT_U20_LE in ALSA
    * .. _V4L2-AUDIO-FMT-U20-BE:

      - ``V4L2_AUDIO_FMT_U20_BE``
      - 'U20_BE'
      - Corresponds to SNDRV_PCM_FORMAT_U20_BE in ALSA
    * .. _V4L2-AUDIO-FMT-S24-3LE:

      - ``V4L2_AUDIO_FMT_S24_3LE``
      - 'S24_3LE'
      - Corresponds to SNDRV_PCM_FORMAT_S24_3LE in ALSA
    * .. _V4L2-AUDIO-FMT-S24-3BE:

      - ``V4L2_AUDIO_FMT_S24_3BE``
      - 'S24_3BE'
      - Corresponds to SNDRV_PCM_FORMAT_S24_3BE in ALSA
    * .. _V4L2-AUDIO-FMT-U24-3LE:

      - ``V4L2_AUDIO_FMT_U24_3LE``
      - 'U24_3LE'
      - Corresponds to SNDRV_PCM_FORMAT_U24_3LE in ALSA
    * .. _V4L2-AUDIO-FMT-U24-3BE:

      - ``V4L2_AUDIO_FMT_U24_3BE``
      - 'U24_3BE'
      - Corresponds to SNDRV_PCM_FORMAT_U24_3BE in ALSA
    * .. _V4L2-AUDIO-FMT-S20-3LE:

      - ``V4L2_AUDIO_FMT_S20_3LE``
      - 'S20_3LE'
      - Corresponds to SNDRV_PCM_FORMAT_S24_3LE in ALSA
    * .. _V4L2-AUDIO-FMT-S20-3BE:

      - ``V4L2_AUDIO_FMT_S20_3BE``
      - 'S20_3BE'
      - Corresponds to SNDRV_PCM_FORMAT_S20_3BE in ALSA
    * .. _V4L2-AUDIO-FMT-U20-3LE:

      - ``V4L2_AUDIO_FMT_U20_3LE``
      - 'U20_3LE'
      - Corresponds to SNDRV_PCM_FORMAT_U20_3LE in ALSA
    * .. _V4L2-AUDIO-FMT-U20-3BE:

      - ``V4L2_AUDIO_FMT_U20_3BE``
      - 'U20_3BE'
      - Corresponds to SNDRV_PCM_FORMAT_U20_3BE in ALSA
    * .. _V4L2-AUDIO-FMT-S18-3LE:

      - ``V4L2_AUDIO_FMT_S18_3LE``
      - 'S18_3LE'
      - Corresponds to SNDRV_PCM_FORMAT_S18_3LE in ALSA
    * .. _V4L2-AUDIO-FMT-S18-3BE:

      - ``V4L2_AUDIO_FMT_S18_3BE``
      - 'S18_3BE'
      - Corresponds to SNDRV_PCM_FORMAT_S18_3BE in ALSA
    * .. _V4L2-AUDIO-FMT-U18-3LE:

      - ``V4L2_AUDIO_FMT_U18_3LE``
      - 'U18_3LE'
      - Corresponds to SNDRV_PCM_FORMAT_U18_3LE in ALSA
    * .. _V4L2-AUDIO-FMT-U18-3BE:

      - ``V4L2_AUDIO_FMT_U18_3BE``
      - 'U18_3BE'
      - Corresponds to SNDRV_PCM_FORMAT_U18_3BE in ALSA
