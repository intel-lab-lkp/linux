.. SPDX-License-Identifier: GPL-2.0

======================================
TAS675x Codec Mixer Controls
======================================

This document describes the ALSA mixer controls for the TAS675x
4-channel automotive Class-D amplifier driver.

For device tree bindings, see:
Documentation/devicetree/bindings/sound/ti,tas675x.yaml

DSP Signal Path Mode
====================

DSP Signal Path Mode
--------------------

:Description: Signal processing mode selection.
:Type:        Enumerated
:Default:     Normal
:Options:     Normal, LLP, FFLP

Normal
  Full DSP with all features available.

LLP (Low Latency Path)
  Bypasses DSP processing. DSP protection features (Thermal Foldback,
  PVDD Foldback, Clip Detect) and Real-Time Load Diagnostics unavailable.

FFLP (Full Feature Low Latency Path)
  Reduced latency. Real-Time Load Diagnostics unavailable.

The following controls are unavailable in LLP mode:
``Thermal Foldback Switch``, ``PVDD Foldback Switch``,
``DC Blocker Bypass Switch``, ``Clip Detect Switch``, ``Audio SDOUT Switch``.

The following controls require Normal mode (unavailable in FFLP and LLP):
``CHx RTLDG Switch``, ``RTLDG Clip Mask Switch``, ``ISENSE Calibration Switch``,
``RTLDG Open Load Threshold``, ``RTLDG Short Load Threshold``,
``CHx RTLDG Impedance``, ``RTLDG Fault Latched``.

Volume Controls
===============

Analog Playback Volume
----------------------

:Description: Analog output gain for all channels (CH1/CH2 and CH3/CH4 pairs).
:Type:        Volume (TLV)
:Default:     0 dB
:Range:       -15.5 dB to 0 dB (0.5 dB steps)

Analog Gain Ramp Step
---------------------

:Description: Anti-pop ramp step duration for analog gain transitions.
:Type:        Enumerated
:Default:     15us
:Options:     15us, 60us, 200us, 400us

CHx Digital Playback Volume
---------------------------

:Description: Per-channel digital volume control (x = 1, 2, 3, 4).
:Type:        Volume (TLV)
:Default:     0 dB
:Range:       -103 dB to 0 dB (0.5 dB steps)
:Bounds:      0x30 (min/mute) to 0xFF (max)

Volume Ramp Down Rate
---------------------

:Description: Update frequency during mute transition.
:Type:        Enumerated
:Default:     16 FS
:Options:     4 FS, 16 FS, 32 FS, Instant

Volume Ramp Down Step
---------------------

:Description: dB change per update during mute.
:Type:        Enumerated
:Default:     0.5dB
:Options:     4dB, 2dB, 1dB, 0.5dB

Volume Ramp Up Rate
-------------------

:Description: Update frequency during unmute transition.
:Type:        Enumerated
:Default:     16 FS
:Options:     4 FS, 16 FS, 32 FS, Instant

Volume Ramp Up Step
-------------------

:Description: dB change per update during unmute.
:Type:        Enumerated
:Default:     0.5dB
:Options:     4dB, 2dB, 1dB, 0.5dB

CH1/2 Volume Combine
--------------------

:Description: Links digital volume controls for CH1 and CH2.
:Type:        Enumerated
:Default:     Independent
:Options:     Independent, CH2 follows CH1, CH1 follows CH2

CH3/4 Volume Combine
--------------------

:Description: Links digital volume controls for CH3 and CH4.
:Type:        Enumerated
:Default:     Independent
:Options:     Independent, CH4 follows CH3, CH3 follows CH4

Auto Mute & Silence Detection
==============================

CHx Auto Mute Switch
--------------------

:Description: Enables automatic muting on zero-signal detection (x = 1, 2, 3, 4).
:Type:        Boolean Switch
:Default:     Disabled (0)

Auto Mute Combine Switch
------------------------

:Description: Coordinated muting behaviour across all channels.
:Type:        Boolean Switch
:Default:     Disabled (0)
:Behavior:    Disabled: channels mute independently when their signal is zero.
              Enabled: all channels mute together only when all detect zero
              signal; unmute when any channel has non-zero signal.

CHx Auto Mute Time
------------------

:Description: Duration of zero signal before muting triggers (x = 1, 2, 3, 4).
:Type:        Enumerated
:Default:     11.5ms
:Options:     11.5ms, 53ms, 106.5ms, 266.5ms, 535ms, 1065ms, 2665ms, 5330ms
:Note:        Values are at 96 kHz. At 48 kHz, times are doubled.

Clock & EMI Management
======================

Spread Spectrum Mode
--------------------

:Description: Frequency dithering mode to reduce peak EMI.
:Type:        Enumerated
:Default:     Disabled
:Options:     Disabled, Triangle, Random, Triangle and Random

SS Triangle Range
-----------------

:Description: Frequency deviation range for Triangle spread spectrum.
:Type:        Enumerated
:Default:     6.5%
:Options:     6.5%, 13.5%, 5%, 10%
:Note:        Applies only when Spread Spectrum Mode includes Triangle.

SS Random Range
---------------

:Description: Frequency deviation range for Random spread spectrum.
:Type:        Enumerated
:Default:     0.83%
:Options:     0.83%, 2.50%, 5.83%, 12.50%, 25.83%
:Note:        Applies only when Spread Spectrum Mode includes Random.

SS Random Dwell Range
---------------------

:Description: Dwell time range for Random spread spectrum (FSS = spread
              spectrum modulation frequency).
:Type:        Enumerated
:Default:     1/FSS to 2/FSS
:Options:     1/FSS to 2/FSS, 1/FSS to 4/FSS, 1/FSS to 8/FSS, 1/FSS to 15/FSS
:Note:        Applies only when Spread Spectrum Mode includes Random.

SS Triangle Dwell Min
---------------------

:Description: Minimum dwell time for Triangle spread spectrum.
:Type:        Integer
:Default:     0
:Range:       0 to 15 (raw register value)

SS Triangle Dwell Max
---------------------

:Description: Maximum dwell time for Triangle spread spectrum.
:Type:        Integer
:Default:     0
:Range:       0 to 15 (raw register value)

Hardware Protection
===================

OTSD Auto Recovery Switch
--------------------------

:Description: Enables automatic recovery from over-temperature shutdown.
:Type:        Boolean Switch
:Default:     Disabled (0)
:Note:        When disabled, manual fault clearing is required after OTSD events.

Overcurrent Limit Level
-----------------------

:Description: Current-limit trip point sensitivity.
:Type:        Enumerated
:Default:     Level 4 (least sensitive)
:Options:     Level 4 (least sensitive), Level 3, Level 2, Level 1 (most sensitive)

CHx OTW Threshold
-----------------

:Description: Over-temperature warning threshold per channel (x = 1, 2, 3, 4).
:Type:        Enumerated
:Default:     >95C
:Options:     Disabled, >95C, >110C, >125C, >135C, >145C, >155C, >165C

Temperature and Voltage Monitoring
===================================

PVDD Sense
----------

:Description: Supply voltage sense register.
:Type:        Integer (read-only)
:Range:       0 to 255
:Conversion:  value × 0.19 V

Global Temperature
------------------

:Description: Global die temperature sense register.
:Type:        Integer (read-only)
:Range:       0 to 255
:Conversion:  (value × 0.5 °C) − 50 °C

CHx Temperature Range
---------------------

:Description: Per-channel coarse temperature range indicator (x = 1, 2, 3, 4).
:Type:        Integer (read-only)
:Range:       0 to 3
:Mapping:     0 = <80 °C, 1 = 80–100 °C, 2 = 100–120 °C, 3 = >120 °C

Load Diagnostics
================

The TAS675x provides three load diagnostic modes:

DC Load Diagnostics (DC LDG)
  Measures DC resistance to detect S2G (short-to-ground), S2P
  (short-to-power), OL (open load), and SL (shorted load) faults.

AC Load Diagnostics (AC LDG)
  Measures complex AC impedance at a configurable frequency. Detects
  capacitive loads and tweeter configurations.

Real-Time Load Diagnostics (RTLDG)
  Monitors impedance continuously during playback using a pilot tone.
  Normal DSP mode only, at 48 kHz or 96 kHz.

Fast Boot Mode
--------------

By default the device runs DC load diagnostics at initialisation before
accepting audio. Setting ``ti,fast-boot`` in the device tree bypasses this
initial diagnostic run for faster startup. Automatic diagnostics after
fault recovery remain enabled::

  codec: tas675x@70 {
      compatible = "ti,tas67524";
      reg = <0x70>;
      ti,fast-boot;
  };

DC Load Diagnostics
-------------------

The ``CHx DC LDG Report`` 4-bit fault field uses the following encoding:

  ======  ===========  ===================================================
  Bit     Fault        Description
  ======  ===========  ===================================================
  [3]     S2G          Short-to-Ground
  [2]     S2P          Short-to-Power
  [1]     OL           Open Load
  [0]     SL           Shorted Load
  ======  ===========  ===================================================

DC LDG Trigger
~~~~~~~~~~~~~~

:Description: Triggers manual DC load diagnostics on all channels.
:Type:        Boolean (write-only)
:Note:        Returns -EBUSY if any DAI stream (playback or capture) is active.
              The driver manages all channel state transitions. Blocks until
              diagnostics complete or time out (300 ms).

DC LDG Auto Diagnostics Switch
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

:Description: Enables automatic DC diagnostics after fault recovery.
:Type:        Boolean Switch
:Default:     Enabled (1)
:Note:        When enabled, affected channels re-run diagnostics after fault
              recovery and retry approximately every 750 ms until resolved.

CHx LO LDG Switch
~~~~~~~~~~~~~~~~~

:Description: Enables line output load detection per channel (x = 1, 2, 3, 4).
:Type:        Boolean Switch
:Default:     Disabled (0)
:Note:        When enabled and DC diagnostics report OL, the device tests for
              a high-impedance line output load.

DC LDG SLOL Ramp Time
~~~~~~~~~~~~~~~~~~~~~

:Description: Voltage ramp time for shorted-load and open-load detection.
:Type:        Enumerated
:Default:     15 ms
:Options:     15 ms, 30 ms, 10 ms, 20 ms

DC LDG SLOL Settling Time
~~~~~~~~~~~~~~~~~~~~~~~~~~

:Description: Settling time for shorted-load and open-load detection.
:Type:        Enumerated
:Default:     10 ms
:Options:     10 ms, 5 ms, 20 ms, 15 ms

DC LDG S2PG Ramp Time
~~~~~~~~~~~~~~~~~~~~~

:Description: Voltage ramp time for short-to-power and short-to-ground detection.
:Type:        Enumerated
:Default:     5 ms
:Options:     5 ms, 2.5 ms, 10 ms, 15 ms

DC LDG S2PG Settling Time
~~~~~~~~~~~~~~~~~~~~~~~~~~

:Description: Settling time for short-to-power and short-to-ground detection.
:Type:        Enumerated
:Default:     10 ms
:Options:     10 ms, 5 ms, 20 ms, 30 ms

CHx DC LDG SL Threshold
~~~~~~~~~~~~~~~~~~~~~~~~

:Description: Shorted-load detection threshold per channel (x = 1, 2, 3, 4).
:Type:        Enumerated
:Default:     1 Ohm
:Options:     0.5 Ohm, 1 Ohm, 1.5 Ohm, 2 Ohm, 2.5 Ohm,
              3 Ohm, 3.5 Ohm, 4 Ohm, 4.5 Ohm, 5 Ohm

DC LDG Result
~~~~~~~~~~~~~

:Description: Overall DC diagnostic result register.
:Type:        Integer (read-only)
:Range:       0x00 to 0xFF
:Bit Encoding:

  ========  =====================================================
  Bits      Description
  ========  =====================================================
  [7:4]     Line output detection result, one bit per channel
  [3:0]     DC diagnostic pass/fail per channel (1=pass, 0=fail)
  ========  =====================================================

CHx DC LDG Report
~~~~~~~~~~~~~~~~~

:Description: DC diagnostic fault status per channel (x = 1, 2, 3, 4).
:Type:        Integer (read-only)
:Range:       0x0 to 0xF
:Note:        See fault bit encoding table at the start of this section.

CHx LO LDG Report
~~~~~~~~~~~~~~~~~

:Description: Line output load detection result per channel (x = 1, 2, 3, 4).
:Type:        Boolean (read-only)
:Values:      0 = not detected, 1 = line output load detected

CHx DC Resistance
~~~~~~~~~~~~~~~~~

:Description: Measured DC load resistance per channel (x = 1, 2, 3, 4).
:Type:        Float (read-only, displayed in ohms)
:Resolution:  0.1 ohm per code (10-bit value)
:Range:       0.0 to 102.3 ohms

AC Load Diagnostics
-------------------

AC LDG Trigger
~~~~~~~~~~~~~~

:Description: Triggers AC impedance measurement on all channels.
:Type:        Boolean (write-only)
:Note:        Returns -EBUSY if any DAI stream (playback or capture) is active.
              The driver transitions all channels to SLEEP state before starting
              the measurement. Blocks until diagnostics complete or time out.

AC DIAG GAIN
~~~~~~~~~~~~

:Description: Measurement resolution for AC diagnostics.
:Type:        Boolean Switch
:Default:     1 (Gain 8)
:Values:      0 = 0.8 ohm/code (Gain 1), 1 = 0.1 ohm/code (Gain 8)
:Note:        Gain 8 recommended for load impedances below 8 ohms.

AC LDG Test Frequency
~~~~~~~~~~~~~~~~~~~~~

:Description: Test signal frequency for AC impedance measurement.
:Type:        Integer
:Default:     200 (0xC8 = 18.75 kHz)
:Range:       0x01 to 0xFF (0x00 reserved)
:Formula:     Frequency = 93.75 Hz × register value

CHx AC LDG Real / CHx AC LDG Imag
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

:Description: Real and imaginary AC impedance components per channel
              (x = 1, 2, 3, 4).
:Type:        Integer (read-only)
:Range:       0x00 to 0xFF (8-bit signed)
:Note:        Scale set by AC DIAG GAIN.

Speaker Protection & Detection
-------------------------------

Tweeter Detection Switch
~~~~~~~~~~~~~~~~~~~~~~~~

:Description: Enables tweeter detection using the AC impedance magnitude comparator.
:Type:        Boolean Switch
:Default:     Enabled (1)
:Note:        Inverted logic — control value 0 = enabled, 1 = disabled.

Tweeter Detect Threshold
~~~~~~~~~~~~~~~~~~~~~~~~~

:Description: Magnitude threshold for tweeter detection.
:Type:        Integer
:Default:     0
:Range:       0x00 to 0xFF
:Resolution:  0.8 ohm/code (AC DIAG GAIN=0) or 0.1 ohm/code (AC DIAG GAIN=1)

CHx Tweeter Detect Report
~~~~~~~~~~~~~~~~~~~~~~~~~~

:Description: Tweeter detection result per channel (x = 1, 2, 3, 4).
:Type:        Boolean (read-only)
:Values:      0 = no tweeter detected, 1 = tweeter detected

DSP Protection Features
=======================

These controls are unavailable in LLP mode.

Thermal Foldback Switch
-----------------------

:Description: Enables dynamic gain reduction based on die temperature.
:Type:        Boolean Switch
:Default:     Disabled (0)

PVDD Foldback Switch
--------------------

:Description: Enables automatic gain limiting when supply voltage drops
              (Automatic Gain Limiter).
:Type:        Boolean Switch
:Default:     Disabled (0)

DC Blocker Bypass Switch
------------------------

:Description: Bypasses the DC-blocking high-pass filter.
:Type:        Boolean Switch
:Default:     Not bypassed (0)

Clip Detect Switch
------------------

:Description: Enables DSP-based clip detection (Pseudo-Analog Clip Detect).
:Type:        Boolean Switch
:Default:     Disabled (0)

Audio SDOUT Switch
------------------

:Description: Routes post-processed audio to the SDOUT pin instead of
              Vpredict data.
:Type:        Boolean Switch
:Default:     Disabled (0)
:Note:        Requires I2S or TDM format. Not supported in Left-justified
              or DSP mode formats.

Real-Time Load Diagnostics
===========================

These controls require Normal DSP mode at 48 kHz or 96 kHz. They are
unavailable at 192 kHz and in FFLP and LLP modes.

The ``RTLDG Fault Latched`` register uses the following encoding:

  ========  ==========================================
  Bits      Description
  ========  ==========================================
  [7:4]     Shorted Load faults, CH1–CH4 respectively
  [3:0]     Open Load faults, CH1–CH4 respectively
  ========  ==========================================

CHx RTLDG Switch
----------------

:Description: Enables real-time impedance monitoring during playback
              (x = 1, 2, 3, 4).
:Type:        Boolean Switch
:Default:     Disabled (0)

RTLDG Clip Mask Switch
----------------------

:Description: Suppresses impedance updates during clipping events.
:Type:        Boolean Switch
:Default:     Enabled (1)

ISENSE Calibration Switch
--------------------------

:Description: Enables current sense calibration for accurate impedance
              measurements.
:Type:        Boolean Switch
:Default:     Disabled (0)

RTLDG Open Load Threshold
--------------------------

:Description: DSP coefficient for open load fault detection threshold.
:Type:        DSP coefficient (extended control)
:Location:    DSP book 0x8C, page 0x22

RTLDG Short Load Threshold
---------------------------

:Description: DSP coefficient for shorted load fault detection threshold.
:Type:        DSP coefficient (extended control)
:Location:    DSP book 0x8C, page 0x22

CHx RTLDG Impedance
-------------------

:Description: Real-time load impedance per channel (x = 1, 2, 3, 4).
:Type:        Float (read-only, displayed in ohms)
:Note:        Valid only during PLAY state with RTLDG enabled at 48 or
              96 kHz. Holds stale data in SLEEP, MUTE, or Hi-Z states.

RTLDG Fault Latched
-------------------

:Description: Latched fault register for OL and SL conditions detected
              during playback.
:Type:        Integer (read-only, read-to-clear)
:Range:       0x00 to 0xFF
:Note:        See bit encoding table at the start of this section.
              Reading the register clears all latched bits.

Known Limitations
=================

Clock Fault Behaviour
---------------------

On Stream Stop
~~~~~~~~~~~~~~

Every time a playback stream stops the FAULT pin briefly asserts.
The CPU DAI (McASP) stops SCLK during ``trigger(STOP)`` — an atomic
context where codec I2C writes are not permitted — before the codec can
transition to sleep. The device detects the clock halt and latches
``CLK_FAULT_LATCHED``, which asserts the FAULT pin. The driver clears
the latch in the ``mute_stream`` callback that follows, so the FAULT pin
flicker lasts only a few milliseconds. Audio output is not affected and
no kernel log message is produced.

On Rapid Rate Switching
~~~~~~~~~~~~~~~~~~~~~~~

When streams are started in rapid succession, an intermittent
``Clock Fault Latched: 0x01`` message may appear in the kernel log.
A 0.5 second settling gap between sessions eliminates this.

References
==========

- TAS675x Technical Reference Manual: SLOU589A
- Device Tree Bindings: Documentation/devicetree/bindings/sound/ti,tas675x.yaml
- ALSA Control Name Conventions: Documentation/sound/designs/control-names.rst
