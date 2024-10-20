Codec-to-Codec Connections in ALSA
====================================

An ALSA-based audio system typically involves playback and capture
functionalities, where users may require audio file playback through
speakers or recording from microphones. However, certain systems
necessitate audio data routing directly between components, such as FM
radio to speakers, without CPU involvement. For such scenarios, ALSA
provides a mechanism known as codec-to-codec connections, leveraging
the Dynamic Audio Power Management (DAPM) framework to facilitate
direct data transfers between codecs.

Introduction
------------

In most audio systems, audio data flows from the CPU to the codec. In
specific configurations, such as those involving Bluetooth codecs,
audio can be transmitted directly between codecs without CPU
intervention. ALSA supports both architectures, and for systems that
do not involve the CPU, it utilizes codec-to-codec digital audio
interface (DAI) connections. This document discusses the procedure
for establishing codec-to-codec DAI links to enable such
functionalities.

Audio Data Flow Paths
----------------------

In a typical configuration, audio flow can be visualized as follows:

.. code-block:: text

    ---------          ---------
   |         |  dai   |         |
       CPU    ------->    codec
   |         |        |         |
    ---------          ---------

In more intricate setups, the system may not involve the CPU but
instead utilizes multiple codecs as shown below. For instance,
Codec-2 acts as a cellular modem, while Codec-3 connects to a
speaker. Audio data can be received by Codec-2 and transmitted to
Codec-3 without CPU intervention, demonstrating the ideal conditions
for establishing a codec-to-codec DAI connection.

.. code-block:: text

                        ---------
                       |         |
                         codec-1 <---cellular modem
                       |         |
                       ---------
                            |
                          dai-1
                            ↓
    ----------          ---------
   |          |cpu_dai |         |
    dummy CPU  ------->  codec-2
   |          |        |         |
    ----------          ---------
                            |
                          dai-3
                            ↓
                        ---------
                       |         |
                         codec-3 ---->speaker
                       |         |
                       ---------

Creating Codec-to-Codec Connections in ALSA
----------------------------------------------

To create a codec-to-codec DAI in ALSA, a ``snd_soc_dai_link`` must be
added to the machine driver before registering the sound card.
During this registration, the core checks for the presence of
``c2c_params`` within the ``snd_soc_dai_link``, determining whether
to classify the DAI link as codec-to-codec.

While establishing the PCM node, the ALSA core inspects this
parameter. Instead of generating a user-space PCM node, it creates
an internal PCM node utilized by kernel drivers. Consequently,
running ``cat /proc/asound/pcm`` will yield no visible PCM nodes.

After this setup, the ALSA core invokes the DAPM core to connect a
single ``cpu_dai`` with both ``codec_dais``. Boot-up logs will
display messages similar to:

.. code-block:: bash

   ASoC: registered pcm #0 codec2codec(Playback Codec)
   multicodec <-> cpu_dai mapping ok
   connected DAI link Dummy-CPU:cpu_dai -> codec-1:dai_1
   connected DAI link Dummy-CPU:cpu_dai -> codec-2:dai_2

To trigger this DAI link, a control interface is established by the
DAPM core during internal DAI creation. This interface links to
the ``snd_soc_dai_link_event`` function, which is invoked when a
path connects in the DAPM core. A mixer must be created to trigger
the connection, prompting the DAPM core to evaluate path
connections and call the ``snd_soc_dai_link_event`` callback with
relevant events.

It is important to note that not all operations defined in
``snd_soc_dai_ops`` are invoked as codec-to-codec connections offer
limited control over DAI configuration. For greater control, a
hostless configuration is recommended. The operations typically
executed in codec-to-codec setups include startup, ``hw_params``,
``hw_free``, digital mute, and shutdown from the
``snd_soc_dai_ops`` structure.

Code Changes for Codec-to-Codec
----------------------------------

The DAI link configuration in the machine file should resemble the
following code snippet:

.. code-block:: c

   /*
    * This PCM stream only supports 24-bit, 2 channels, and
    * 48kHz sampling rate.
    */
   static const struct snd_soc_pcm_stream dsp_codec_params = {
       .formats = SNDRV_PCM_FMTBIT_S24_LE,
       .rate_min = 48000,
       .rate_max = 48000,
       .channels_min = 2,
       .channels_max = 2,
   };

   static struct snd_soc_dai_link dai_links[] = {
   {
       .name = "CPU-DSP",
       .stream_name = "CPU-DSP",
       .cpu_dai_name = "samsung-i2s.0",
       .codec_name = "codec-2",
       .codec_dai_name = "codec-2-dai_name",
       .platform_name = "samsung-i2s.0",
       .dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF
                  | SND_SOC_DAIFMT_CBM_CFM,
       .ignore_suspend = 1,
       .c2c_params = &dsp_codec_params,
       .num_c2c_params = 1,
   },
   {
       .name = "DSP-CODEC",
       .stream_name = "DSP-CODEC",
       .cpu_dai_name = "wm0010-sdi2",
       .codec_name = "codec-3",
       .codec_dai_name = "codec-3-dai_name",
       .dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF
                  | SND_SOC_DAIFMT_CBM_CFM,
       .ignore_suspend = 1,
       .c2c_params = &dsp_codec_params,
       .num_c2c_params = 1,
   },
   };

This snippet draws inspiration from the configuration found in
``sound/soc/samsung/speyside.c``. The inclusion of the
``c2c_params`` indicates to the DAPM core that the DAI link is a
codec-to-codec connection.

In the DAPM core, a route is established between the CPU DAI
playback widget and the codec DAI capture widget for playback, with
the reverse applying to the capture path. To trigger these routes,
DAPM requires valid endpoints, which can be either sink or source
widgets corresponding to the playback and capture paths,
respectively.

To activate this DAI link widget, a lightweight codec driver for
the speaker amplifier can be implemented, following a similar
strategy to that in ``wm8727.c``. This driver should set the
necessary constraints for the device, even with minimal control
requirements.

It's crucial to append “Playback” and “Capture” suffixes to the
respective CPU and codec DAI names for playback and capture, as
the DAPM core links and powers these DAIs based on their naming
conventions.

In a codec-to-codec scenario involving multiple codecs (above
bootup logs are for multicodec scenario), it is not feasible to
control individual codecs using dummy kcontrols or DAPM widgets.
This limitation arises because the CPU DAI is statically
connected to all codecs. Consequently, when a path is enabled,
the DAPM core does not verify all the widgets that may be linked
to the mixer widget. It’s important to note that the mixer widget
serves as the trigger for these paths.

Simple-audio-card configuration
----------------------------------
A dai_link in a "simple-audio-card" will automatically be
detected as codec-to-codec when all DAIs on the link belong to
codec components. The dai_link will be initialized with the
subset of stream parameters (channels, format, sample rate)
supported by all DAIs on the link. Since there is no way to
provide these parameters in the device tree, this is mostly useful
for communication with simple fixed-function codecs, such as a
Bluetooth controller or cellular modem.
