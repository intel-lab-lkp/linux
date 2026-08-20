.. SPDX-License-Identifier: GPL-2.0
.. Copyright (C) 2026 Nicholas Johnson

===========================
PreSonus Quantum PCI Driver
===========================

The ``snd-quantum`` driver supports PreSonus Quantum PCIe/Thunderbolt audio
interfaces through ALSA. The Quantum 2626 is enabled by default and is the
only model verified by the driver author.

Other related Quantum PCI devices are not currently matched by this driver.

Supported Features
------------------

The driver provides:

* 26-channel PCM playback and capture using the native ``S32_LE`` hardware
  format;
* raw MIDI input and output;
* sample-rate selection from 44100 Hz to 192000 Hz;
* clock-source selection for internal, S/PDIF, word clock, ADAT1, and ADAT2;
* playback and capture XRUN counters.

The hardware has a shared audio DMA engine, so playback and capture run with
the same sample rate, period size, and buffer size while both directions are
active.

Loading
-------

Load the driver with::

    # modprobe snd-quantum

Check that ALSA registered the card with::

    $ cat /proc/asound/cards
    $ aplay -l
    $ arecord -l

ALSA Controls
-------------

The sample rate and clock source are exposed as ALSA mixer controls. The card
identifier normally appears as ``Quantum2626``.

For example::

    $ amixer -c Quantum2626 cset name='Quantum Sample Rate' 48000
    $ amixer -c Quantum2626 cset name='Quantum Clock Source' Internal

XRUN counters can be read with::

    $ amixer -c Quantum2626 cget name='Quantum Playback XRUN Count'
    $ amixer -c Quantum2626 cget name='Quantum Capture XRUN Count'

Audio Testing
-------------

Test playback on the first channel with::

    $ speaker-test -D hw:Quantum2626,0 -F S32_LE -r 48000 -c 26 -s 1 -t sine -l 1

Record all capture channels with::

    $ arecord -D hw:Quantum2626,0 -f S32_LE -r 48000 -c 26 -d 10 /tmp/quantum.wav

The ALSA device exposes the native multichannel hardware stream. Stereo
mixing and speaker routing are expected to be handled by ALSA, PipeWire, JACK,
or the application using the device.

CPU Latency QoS
---------------

Low-latency audio can be sensitive to CPU idle-exit latency. While audio DMA
is active, the driver can hold a per-device CPU latency QoS request. The
request is removed when audio DMA stops.

The default configured latency is 2 microseconds. When automatic low-latency
mode is enabled, the effective request becomes 0 microseconds for the tightest
hardware service intervals.

The controls are exposed below the PCI device, for example::

    /sys/bus/pci/devices/0000:08:00.0/cpu_latency_us
    /sys/bus/pci/devices/0000:08:00.0/cpu_latency_auto_low_latency
    /sys/bus/pci/devices/0000:08:00.0/cpu_latency_effective_us
    /sys/bus/pci/devices/0000:08:00.0/cpu_latency_state

``cpu_latency_us``
    Configured latency request in microseconds. Use ``0`` for the strongest
    constraint, or ``-1`` to disable the request.

``cpu_latency_auto_low_latency``
    Boolean control for automatically tightening the request when the selected
    sample rate and period size require the shortest service interval.

``cpu_latency_effective_us``
    Read-only value showing the latency request that would be applied for the
    current stream configuration. ``-1`` means disabled.

``cpu_latency_state``
    Read-only summary of the configured value, effective value, automatic mode,
    DMA-active state, sample rate, hardware quantum, period size, and buffer
    size.

Example::

    $ cat /sys/bus/pci/devices/0000:08:00.0/cpu_latency_state
    # echo 0 > /sys/bus/pci/devices/0000:08:00.0/cpu_latency_us
    # echo -1 > /sys/bus/pci/devices/0000:08:00.0/cpu_latency_us

Limitations
-----------

Only the Quantum 2626 has been verified by the driver author. Later
Quantum-branded USB interfaces use a different hardware architecture and are
not supported by this driver.
