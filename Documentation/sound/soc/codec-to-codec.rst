Codec-to-Codec Connections in ALSA
==================================

An ALSA-based audio system typically involves playback and capture
functionality, where users may require audio file playback through
speakers or recording from microphones. However, certain systems
necessitate audio data routing directly between components, such as FM
radio to speakers, without CPU involvement. For such scenarios, ASoC(
ALSA system on chip) provides a mechanism known as codec-to-codec (C2C)
connections, leveraging the Dynamic Audio Power Management (DAPM)
framework to facilitate direct data transfers between codecs.

Introduction
------------

In most audio systems, audio data flows from the CPU to the codec. In
specific configurations, such as those involving Bluetooth codecs,
audio can be transmitted directly between codecs without CPU
intervention. ASoC supports both architectures, and for systems that
do not involve the CPU, it utilizes C2C digital audio
interface (DAI) connections. This document discusses the procedure
for establishing C2C DAI links to enable such functionality.

Audio Data Flow Paths
---------------------

In a typical configuration, audio flow can be visualized as follows:

.. code-block:: text

    ---------          ---------
   |         |  dai   |         |
   |    CPU   -------->  codec  |
   |         |        |         |
    ---------          ---------

In more intricate setups, the system may not involve the CPU but
instead utilizes multiple codecs as shown below. For instance,
Codec-2 acts as a cellular modem, while Codec-3 connects to a
speaker. Audio data can be received by Codec-2 and transmitted to
Codec-3 without CPU intervention, demonstrating the ideal conditions
for establishing a C2C DAI connection.

.. code-block:: text

                       ---------
                      |         |
                      | codec-1  <---cellular modem
                      |         |
                      ---------
                           |
                         dai-1
                           ↓
    ---------          ---------
   |         |cpu_dai |         |
   |   CPU    ------->  codec-2 |
   |         |        |         |
    ---------          ---------
                           |
                         dai-3
                           ↓
                       ---------
                      |         |
                      | codec-3  --->speaker
                      |         |
                       ---------

In the diagram above, two kinds of use cases can be supported:

  1. Host music playback: CPU -> codec-2 -> codec-3 -> Speaker

     When an application on the host plays audio, ASoC informs the DAPM
     (Dynamic Audio Power Management) core that the main CPU DAI
     (Digital Audio Interface) is now an active source. DAPM then parses
     through the audio graph until it finds the speaker sink.

     The act of playing audio triggers the following power-up sequence:

     - The ``CPU -> codec-2`` DAI is activated.
     - DAPM powers up the C2C  DAI link between codec-2 and codec-3, as
        it is part of the active audio path.

  2. Cellular call: codec-1 -> codec-2 -> codec-3 -> Speaker

     In this case, the host is not involved at all. The modem acts as the
     audio source, and DAPM powers up everything between it and the sink
     (i.e., the speaker). This power-up sequence involves:

     - The C2C DAI link between codec-1 and codec-2.
     - The C2C DAI link between codec-2 and codec-3.

     DAPM ensures that all necessary components in the audio path from the
     modem to the speaker are powered up, enabling direct audio playback
     from the modem without host intervention.


Creating Codec-to-Codec Connections in ALSA
-------------------------------------------

To create a C2C DAI in ALSA, a ``snd_soc_dai_link`` must be
added to the machine driver before registering the sound card.
During this registration, the core checks for the presence of
``c2c_params`` within the ``snd_soc_dai_link``, determining whether
to classify the DAI link as C2C.

While establishing the PCM node, the ASoC core inspects this
parameter. Instead of generating a user-space PCM node, it creates
an internal PCM node utilized by kernel drivers. Consequently,
running ``cat /proc/asound/pcm`` will yield no visible PCM nodes.

Boot-up logs will display message similar to:

.. code-block:: text

   ASoC: registered pcm #0 codec2codec(Playback Codec)

To trigger this DAI link, a control interface is established by the
DAPM core during internal DAI creation. This interface links to
the ``snd_soc_dai_link_event`` function, which is invoked when a
path connects in the DAPM core. A mixer must be created to trigger
the connection, prompting the DAPM core to evaluate path
connections and call the ``snd_soc_dai_link_event`` callback with
SND_SOC_DAPM_*_PMU and SND_SOC_DAPM_*_PMD events.

It is important to note that not all operations defined in
``snd_soc_dai_ops`` are invoked as C2C connections offer
limited control over DAI configuration. The operations typically
executed in C2C setups include startup, ``hw_params``, ``hw_free``,
digital mute, and shutdown from the ``snd_soc_dai_ops`` struct.

Code Changes for Codec-to-Codec
-------------------------------

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

To better understand the configuration inspired by the setup found in
``sound/soc/samsung/speyside.c``, here are several key points:

1. The presence of ``c2c_params`` informs the DAPM core that the DAI link
   represents a C2C connection.

2. ``c2c_params`` can be an array, and ``num_c2c_params`` defines the size
   of this array.

3. If ``num_c2c_params`` is 1:

   - The C2C DAI is configured with the provided ``snd_soc_pcm_stream``
     parameters.

4. If ``num_c2c_params`` is greater than 1:

   - A kcontrol is created, allowing the user to select the index of the
     ``c2c_params`` array to be used.

This flexible approach enables dynamic configuration of C2C
connections based on runtime requirements.

In the DAPM core, a route is established between the CPU DAI
playback widget and the codec DAI capture widget for playback, with
the reverse applying to the capture path. To trigger these routes,
DAPM requires valid endpoints, which can be either sink or source
widgets corresponding to the playback and capture paths, respectively.

To activate this DAI link widget, codec driver is required.
If it doesn't exist, thin codec driver can be implemented,
following a similar strategy to that in ``wm8727.c``. This driver
should set the necessary constraints for the device, even with
minimal control requirements.


Simple-audio-card configuration
-------------------------------

A dai_link in a "simple-audio-card" will automatically be
detected as C2C when all DAIs on the link belong to
codec components. The dai_link will be initialized with the
subset of stream parameters (channels, format, sample rate)
supported by all DAIs on the link. Since there is no way to
provide these parameters in the device tree, this is mostly useful
for communication with simple fixed-function codecs, such as a
Bluetooth controller or cellular modem.
