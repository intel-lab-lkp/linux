==============
Audio Clocking
==============

This text describes the audio clocking terms in ASoC and digital audio in
general. Note: Audio clocking can be complex!


Master Clock
------------

Every audio subsystem is driven by a master clock (sometimes referred to as MCLK
or SYSCLK). This audio master clock can be derived from a number of sources
(e.g. crystal, PLL, CPU clock) and is responsible for producing the correct
audio playback and capture sample rates.

Some master clocks (e.g. PLLs and CPU based clocks) are configurable in that
their speed can be altered by software (depending on the system use and to save
power). Other master clocks are fixed at a set frequency (i.e. crystals).

DAI Clocks
----------
The Digital Audio Interface is usually driven by a Bit Clock (often referred to
as BCLK). This clock is used to drive the digital audio data across the link
between the codec and CPU.

The DAI also has a frame clock to signal the start of each audio frame. This
clock is sometimes referred to as LRC (left right clock) or FRAME. This clock
runs at exactly the sample rate (LRC = Rate).

Bit Clock can be generated as follows:-

- BCLK = MCLK / x, or
- BCLK = LRC * x, or
- BCLK = LRC * Channels * Word Size

This relationship depends on the codec or SoC CPU in particular. In general
it is best to configure BCLK to the lowest possible speed (depending on your
rate, number of channels and word size) to save on power.

It is also desirable to use the codec (if possible) to drive (or master) the
audio clocks as it usually gives more accurate sample rates than the CPU.

ASoC provided clock APIs
------------------------

    ::

        int snd_soc_dai_set_sysclk(struct snd_soc_dai *dai, int clk_id,
                                   unsigned int freq, int dir)

    This function is generally called in the machine driver to set the sysclk
    or MCLK. This function in turn calls the codec or platform callbacks to set
    the sysclk/MCLK. If the call end up in codec driver and MCLK is either
    provided by the codec in which case the direction should be SND_SOC_CLOCK_IN
    and if the processor is providing the clock then it should be set to
    SND_SOC_CLOCK_OUT. If the callback ends up in platform/cpu driver then it can
    setup any clocks that is required for platform hardware.

   dai:
       digital audio interface corresponding to the component.

   clk_id:
       dai specific clock id

   frequency:
       new clock frequency in Hz

   dir:
       new clock direction (SND_SOC_CLOCK_IN or SND_SOC_CLOCK_OUT)

    ::

        int snd_soc_dai_set_clkdiv(struct snd_soc_dai *dai,
                                   int div_id, int div)

    This function is used to set the clock divider for the corresponding dai.
    In case of codec dai connected through I2S for data transfer, bit clock
    dividers are set based on this call to either multiple of bitclock
    frequency required to support requested sample rate or equal to
    bitclock frequency.

   dai:
       digital audio interface corresponding to the component.

   div_id:
       DAI specific clock divider ID

   div:
       new clock divisor.


    ::

        int snd_soc_dai_set_pll(struct snd_soc_dai *dai, int pll_id, int source,
                                unsigned int freq_in, unsigned int freq_out)

    This interface function unregisters the "thermal-cpufreq-%x" cooling device.
    This function provides a way for the dai component drivers to configure
    pll based on the input clock. This pll can be used to generate output
    clock such as bit clock for the codec.

   dai:
       digital audio interface corresponding to the component.

   pll_id:
       DAI specific PLL ID

   source:
       DAI specific source for the PLL

   freq_in:
       PLL input clock frequency in Hz

   freq_out:
       requested PLL output clock frequency in Hz


    ::

        int snd_soc_dai_set_bclk_ratio(struct snd_soc_dai *dai, unsigned int ratio)

    This function configures the DAI for a preset BCLK to sample rate ratio.

   dai:
       digital audio interface corresponding to the component.

   ratio:
       ration of BCLK to sample rate
