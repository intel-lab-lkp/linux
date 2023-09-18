.. SPDX-License-Identifier: GPL-2.0

=======================
MARIAN Seraph M2 Driver
=======================

Sep 18, 2023

Ivan Orlov <ivan.orlov0322@gmail.com>

STATE OF DEVELOPMENT
====================

This driver is based on the driver written by Florian Faber in 2012, which seemed to work fine.
However, the initial code contained multiple issues, which had to be solved before sending the
driver upstream.

The vendor lost the full documentation, so what we have here was recovered from drafts and found
after experiments with the card.

What seems to be working fine:
- Playback and capture for all supported rates
- Integrated loopback (with some exceptions, see below)

MEMORY MODEL
============

The hardware requires one huge contiguous DMA space to be allocated. After allocation, the bus address of
this buffer should be written to the hardware register.

We can split this space into two parts: the first one contains samples for capture, another one contains
play samples:

CAPTURE_CH_0, CAPTURE_CH_1, ..., CAPTURE_CH_127 | PLAY_CH_0, PLAY_CH_1, ..., PLAY_CH_127

The card supports the non-interleaved access mode only, so samples for each channel lay together:

C0, C0, ..., C0, C1, C1, ..., C1, ..., C127 | C0, C0, ..., C0, C1, C1, ..., C1, ..., C127

The count of samples per each channel buffer needs to be set explicitly, so the address of the first
byte of the playback data depends on this value. The playback buffer starts where the capture buffer ends.
It makes the arbitrary period count/buffer size feature impossible to implement, and the driver supports only
2 periods per buffer.

Controls
========

Input 1 Sync
    0 - No signal, 1 - valid MADI signal found, 2 - Synced with MADI signal
Input 2 Sync
    0 - No signal, 1 - valid MADI signal found, 2 - Synced with MADI signal

Input 1 Channel Mode
    0 - 56 channels, 1 - 64 channels
Input 2 Channel Mode
    0 - 56 channels, 1 - 64 channels

Input 1 Frame Mode
    0 - 48 kHz, 1 - 96 kHz
Input 2 Frame Mode
    0 - 48 kHz, 1 - 96 kHz

Input 1 Frequency
    Measured frequency on Input 1
Input 2 Frequency
    Measured frequency on Input 2

Output 1 Channel Mode
    0 - 56 channels, 1 - 64 channels
Output 2 Channel Mode
    0 - 56 channels, 1 - 64 channels

Output 1 Frame Mode
    0 - 48 kHz, 1 - 96 kHz
Output 2 Frame Mode
    0 - 48 kHz, 1 - 96 kHz

Clock Source
    Internal/Sync Bus, Input Port 1, Input Port 2

Speed mode
    VCO clock range (slow/fast)

DCO Frequency (Hz)

DCO Frequency (ms)

DCO Detune
    Range: -200...200

Loopback
    Enable/Disable integrated loopback


Loopback
========

The card contains integrated loopback. When it is enabled, it sets the hardware’s DAW-in memory pointer
to the hardware’s DAW-out memory. So, what you play is what you record.

You can enable the integrated loopback using the corresponding control.

The loopback seems to work well on lower rates (like 28000). However, when the rate goes higher, I observe
multiple mistakes in recorded byte ordering.
