.. SPDX-License-Identifier: GPL-2.0

=========================================
Reloop Jockey 3 DJ Controller Driver
=========================================

The ``snd-reloop-jockey3`` driver supports the Reloop Jockey 3 family of USB DJ
controllers. These devices do not implement USB Audio Class; they use a
proprietary protocol from Ploytec GmbH, which this driver implements from
reverse engineering.

Supported devices
=================

=================================  ===========  =========
Device                             VID:PID      Status
=================================  ===========  =========
Reloop Jockey 3 Remix              200c:1037    Tested
Reloop Jockey 3 Master Edition     200c:1019    Tested
=================================  ===========  =========

The Master Edition is confirmed to be protocol-compatible. The driver author
does not own one, but user feedback on the Master Edition has been received
confirming the device is fully functional with this driver.

Audio
=====

The device provides 4 playback and 6 capture channels, all S24_3LE, at 44.1,
48, 88.2 or 96 kHz. The sample rate is a device-wide property: if one stream is
already running, a second stream opening is constrained to the rate in use.

Channel layout
--------------

Playback:

=======  ==========================
Channel  Signal
=======  ==========================
1-2      Master Out L/R
3-4      Headphone (cue) L/R
=======  ==========================

Capture:

=======  ==========================
Channel  Signal
=======  ==========================
1-2      Input 1 L/R
3-4      Input 2 L/R
5-6      Microphone
=======  ==========================

The line inputs are only routed to the ADC when the input selector on the unit
is in the ``SW`` (software) position.

The microphone is **mono**. Its balanced input stage feeds the same analog
signal to both converters, so channels 5 and 6 carry identical content -- though
not bit-identical, since each has its own ADC and picks up independent
converter noise. Applications may treat the pair as a stereo stream with mono
content, or simply use one of the two channels.

The channel map advertised to userspace marks only the playback Master pair as
``FL``/``FR``, so that audio servers can identify the primary output. The
remaining pairs are reported as ``UNKNOWN``: they are discrete inputs and
outputs rather than a speaker arrangement, and giving them surround positions
would invite applications to route, for example, the headphone cue output to
rear speakers.

Microphone level
----------------

The microphone level appears to be quieter than when using the vendor
software. The ADC is a PCM1803A with fixed gain, and the only gain control in
the signal path is analog, so any additional gain must be applied digitally
somewhere in the vendor stack; whether that happens in the driver or in the
application has not been established. This driver applies no digital gain of
its own, so apply it in userspace if required.

MIDI
====

The control surface is exposed as a standard ALSA rawmidi device, with input
and output ports.

MIDI output is multiplexed into the audio playback stream: every outgoing
packet reserves one byte for MIDI. As a result the playback URBs run
continuously whether or not any PCM stream is open, and MIDI output throughput
is bounded by the packet rate. The driver rate-limits MIDI output to roughly
2500 bytes/sec, as the device otherwise overruns its internal buffers,
truncates messages, and/or the control surface becomes unresponsive.

The device does not accept MIDI Running Status, so the driver expands the
outgoing stream to give every message an explicit status byte.

Stream liveness and recovery
============================

Changing the sample rate requires stopping the URBs, reprogramming the device
over the control endpoint, and restarting them. During testing and
validation, cases have been observed where one of the USB streams failed to
start or stalled afterwards -- the control transfers reported success, but
the endpoint delivered no data, which surfaces to applications as ``EIO`` on
the affected direction. The driver guards against this:

* After every rate change, and whenever a stream is prepared, each direction
  is checked for liveness by watching for URB completion activity.

* A stalled direction is first recovered with a lightweight URB stop/restart.
  If that does not bring it back, recovery escalates to a full USB device
  reset, subject to a bounded retry budget: a chip-wide limit on how many
  resets may be attempted within a rolling time window, so a persistently
  misbehaving device is reported instead of being reset in a tight loop.

* Playback also carries MIDI output, so a playback stall is always recovered
  immediately through this path.

* A capture stall is recovered immediately the same way only if a capture
  stream is currently open. Otherwise it is left alone at that moment, to
  avoid an audible reset glitch on working playback audio for the sake of a
  direction nobody is using; the same liveness check and recovery catch it
  the next time a capture stream is opened instead.

A stall that triggers immediate recovery is logged with ``dev_warn()``; a
stall on an idle, unused capture endpoint whose recovery is deferred is
logged at ``dev_dbg()`` instead, since it is an expected, tolerated state
rather than something acted on right away. Recovery outcomes -- a successful
URB restart, an escalated reset, an exhausted retry budget, or a stream still
dead after a reset -- are always logged with ``dev_warn()`` or ``dev_err()``,
so real-world frequency and severity can be tracked via ``dmesg``.

URB liveness watchdog
=====================

The checks above run at specific moments: after a rate change, and when a stream
is prepared. A device that stops completing URBs at any other time is invisible
to them, and to every other error path in the driver, because all of those hang
off a URB completion. When completions stop, nothing runs and nothing is logged.
A playback stream in that state does not even report an underrun, since the
hardware pointer never advances far enough to overtake the application.

A periodic work item therefore checks both directions for liveness for as long
as the device is bound. It runs over the device's whole lifetime rather than
only while a PCM stream is open, because the URBs do too: MIDI output is carried
in every playback packet, so there is no idle state in which a total absence of
completions is legitimate.

The watchdog acts as well as reports. A new stall onset enters the same
recovery ladder described above -- a lightweight URB stop/start, escalating to
a full device reset if that does not take. This is the only place recovery can
begin without a PCM ioctl re-entering the driver first, which matters because a
long-running, uninterrupted stream never re-enters otherwise. Escalation to a
reset is drawn from a chip-wide bounded budget, so a device that keeps stalling
is not reset in a tight loop, and only one recovery ladder runs at a time.

Logging is edge-triggered: one line when a direction stops completing URBs, one
when it starts again, with nothing repeated in between. The message carries the
measured age of the stall rather than a fixed threshold, since the threshold
alone would only bound it to the width of one poll interval.

An idle, unused capture endpoint stalling is the one case the driver
deliberately tolerates without treating it as a fault: recovery for it is
deferred to the next capture open, as described above, so it can persist
indefinitely with nothing wrong. Every other persistent stall means recovery
did not succeed, and the ``dev_err()`` logged for an exhausted retry budget
or a stream still dead after a reset should already explain why.

Module parameters
=================

The driver takes the standard ALSA ``index``, ``id`` and ``enable`` parameters.

Kconfig
=======

``CONFIG_SND_USB_JOCKEY3``
    Build the driver.

``CONFIG_SND_USB_JOCKEY3_REFERENCE_CODEC``
    Use the portable reference implementation of the sample codec instead of
    the architecture-optimized one. The optimized codec is the default and is
    what should normally be used; the reference implementation is much slower,
    and exists as a readable definition of the wire format and as a fallback.
    This option depends on ``CONFIG_EXPERT``.

``CONFIG_SND_USB_JOCKEY3_CODEC_KUNIT_TEST``
    Build the KUnit tests for the sample codec. Because the codec functions
    are internal to the driver, the tests are linked into the driver module
    rather than built as a separate one. See `Testing`_ below.

Testing
=======

The sample codec has KUnit coverage. It is worth running after any change to
``ploytec_codec.c``, and on any architecture the optimized codec has not been
exercised on before::

    tools/testing/kunit/kunit.py run --kunitconfig=sound/usb/jockey3 \
        --arch=x86_64

Running under UML needs three extra options::

    tools/testing/kunit/kunit.py run --kunitconfig=sound/usb/jockey3 \
        --kconfig_add CONFIG_VIRTIO=y \
        --kconfig_add CONFIG_VIRTIO_UML=y \
        --kconfig_add CONFIG_UML_PCI_OVER_VIRTIO=y

UML disables IOMEM by default, which puts ``CONFIG_USB`` - and with it the
whole of ``sound/usb`` - out of reach. ``UML_PCI_OVER_VIRTIO`` selects
``UML_PCI``, which brings in the IOMEM emulation that makes it selectable
again. These are passed on the command line rather than placed in
``.kunitconfig`` because they only exist under ``arch/um``, and kunit.py
treats a requested option it cannot satisfy as an error - so putting them in
the shared fragment would break configuration on every other architecture.

Other architectures run under QEMU, which is the point of the exercise: the
codec assumes a little-endian sample format and reaches for 32- and 64-bit
words through the unaligned accessors, so word size, alignment strictness and
byte order all matter::

    tools/testing/kunit/kunit.py run --kunitconfig=sound/usb/jockey3 \
        --arch=arm --cross_compile=arm-linux-gnueabihf-

s390 is worth a run as the only readily available big-endian target, and needs
``--kconfig_add CONFIG_PCI=y``: on s390 ``HAS_IOMEM`` is ``def_bool PCI``, so
without it there is no sound subsystem to build against.

The suite has been run on um, i386, arm, arm64, riscv and s390.

Only one of the three codec variants is compiled into any given build, so the
portable reference deserves a run of its own::

    tools/testing/kunit/kunit.py run --kunitconfig=sound/usb/jockey3 \
        --kconfig_add CONFIG_EXPERT=y \
        --kconfig_add CONFIG_SND_USB_JOCKEY3_REFERENCE_CODEC=y

The tests check the compiled-in variant against a declarative description of
the wire format rather than against a second copy of the same loops. They also
exploit the fact that the codec is a bit permutation, and therefore linear over
GF(2): one case enumerates the mapping's action on every input bit, another
establishes linearity, and a linear map is fully determined by its action on
the basis vectors.

Further information
===================

Protocol notes, USB captures and ongoing reverse-engineering work are kept at
https://github.com/fvdpol/alsa-jockey3.
