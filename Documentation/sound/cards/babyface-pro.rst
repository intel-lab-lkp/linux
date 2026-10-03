.. SPDX-License-Identifier: GPL-2.0

=================================================
RME Babyface Pro / Pro FS (snd-usb-babyface-pro)
=================================================

This document describes the design of the ``snd-usb-babyface-pro``
driver - what problem the driver solves, why it is a standalone
driver instead of a snd-usb-audio quirk, and the four design
decisions (protocol shape, stream model, mixer-state persistence,
front-panel emulation) that shape most of the code.

Two USB personalities, one device
==================================

The RME Babyface Pro and Babyface Pro FS present two different USB
configurations depending on a physical/firmware switch: a
class-compliant one, already handled by ``snd-usb-audio``, and a
proprietary one (USB ID ``2a39:3fc0``) that this driver covers.  The
two hardware models share the same USB ID, ``bcdDevice`` and
``iProduct`` string shape; nothing in the descriptors tells them
apart, and the driver runs unmodified on both.

In proprietary mode, interface 5 carries the PCM stream on two
INTERRUPT endpoints (``0x01`` OUT, ``0x82`` IN) instead of the
isochronous endpoints the USB Audio Class specifies.  Isochronous
transfers are rejected there with ``-EINVAL``.  ``snd-usb-audio`` has
no interrupt-PCM transport, so this mode cannot be a quirk on top of
it; the driver is standalone, modelled on ``snd-usb-caiaq`` (another
interrupt-streaming RME/NI-style device).

Why interrupt endpoints and not isochronous is a hardware/firmware
choice on RME's side, not something this driver can change - the
class-compliant mode already exists on the same device for users who
want a fully standard, quirk-free path with a subset of the
functionality (no mixer, no front panel).  This driver is for users
who want the full mixer, routing matrix, and hardware DSP EQ that
only the proprietary mode exposes.

The vendor protocol: writes only, no readback
==============================================

Every mixer and clock function is one of a handful of USB vendor
control requests (``bmRequestType 0x40``, i.e. host-to-device,
vendor, device-recipient), each identified by its request number and
a 16-bit value/index pair - there is no larger command structure.
The commonly used ones are:

======  ========================================
0x10    settings word, rate family, stream trigger
0x12    16-bit crosspoint and output-master writes
0x14    stream session arm
0x16    cold-init register clear
0x17    front-panel + preamp state (read and write)
0x1a    8-bit gain / output-master writes
0x1b    varispeed (DDS quad)
0x1d    stream session start
======  ========================================

The full register map, decoded from Windows USB captures and
cross-checked against hardware, lives in the driver's own development
repository (not shipped in-tree) - the constants and the comments
next to each vendor write in the source are the authoritative
in-tree reference.

The one property that shapes the rest of the driver: **almost nothing
here can be read back**.  The 0x17 request returns the front-panel
and preamp state, but the crosspoint matrix, the output masters, the
routing flags and the clock all have to be tracked host-side - the
device will accept a write blindly and never confirm what it actually
holds.  Two consequences follow directly from this:

* The ``struct snd_usb_babyface`` device state (see
  ``babyfacepro.h``) is not a cache in the usual sense of "avoid a
  slow read" - it is the *only* record of what the hardware should
  currently hold.  Every mixer control's ``.get`` callback reads this
  state directly; none of them ever talks to the device.

* A full reset of the device's registers - which the cold init at
  probe and at resume does - has to be followed by replaying the
  *entire* cached state back, in the right order, or the card comes
  back silent or at the wrong levels.  This is what
  ``babyface_restore_state()`` and ``bf_state_apply_flags()`` do (see
  "Mixer-state persistence" below).

The stream model
================

Playback and capture share one physical stream: the device only
advances it while both interrupt endpoints have a pending URB, so the
IN and OUT URBs are always submitted as a pair, and both directions run
at one sample rate.  The driver runs one stream *session* for both
substreams:

* ``hw_params`` counts a substream as a user of the session;
* ``prepare`` starts the session if it is not running - the rate write,
  the session trigger pair, the IN/OUT URBs, then the arm - which is
  what the RME Windows driver sends at a stream start;
* trigger START/STOP only decides whether the URB handlers move that
  substream's audio.  The URBs keep running either way, carrying
  silence while nothing plays, so an xrun restart does not restart the
  session;
* ``hw_free`` drops the user, and the last one stops the session.

The device keeps its mixer registers between sessions, so a session
start writes no mixer state.  A session triggered within about 15 ms of
the previous one stopping comes up with the outputs silent, so a
session start waits until 50 ms have passed since the last stop.

Both directions share one clock, so while another application has the
other direction set up, the rate belongs to it: ``open()`` offers that
rate alone, and the sound server resamples, rather than the device
changing rate under a running stream - as with RME's own drivers, which
grey the sample rate out while a stream runs.  The application that
holds both directions may change the rate or the buffer size itself, as
a DAW does from its settings; the direction it left running then stops
with an xrun and is set up again.

The size of the URBs follows the period the application asks for, so
the latency follows the application's buffer instead of a fixed queue:
``hw_params`` splits the period into the fewest URBs of at most
``frames_per_urb`` frames, each a whole number of the device's IN
packets, and keeps two periods' worth in flight (at most ``nurbs``).
The URB handlers copy to and from the ring the core allocates and take
the application's position from the shared control page, so the ring can
also be mapped by the application, which JACK requires.
``runtime->delay`` reports the audio queued in the URBs plus the fixed
delay of the converters and of the device, per speed, so that an
application that uses ALSA directly can line up what it records.

URB completions run in interrupt context; the work that needs to sleep
(stopping the session after repeated URB errors) runs from
``stream_work``.

Mixer-state persistence across re-probes
==========================================

A userspace client can claim the proprietary interface directly via
``usbfs`` (``USBDEVFS_DISCONNECT_CLAIM``) - both PipeWire grabbing the
device for a sink and the project's own TuxMix userspace daemon do
this via libusb.  That detaches the kernel driver and the ALSA card
disappears for the duration; when the client releases the interface,
the driver re-probes.  The device keeps its register contents across
this detach, but the cold init the probe runs clears them - so the
driver saves the in-memory mixer state at ``disconnect()`` and
restores it at the next ``probe()``, keyed by the device's USB serial
number (or its sysfs path, if it has no serial) so the same physical
unit gets its state back across the cycle.  The same state is also
what a system-suspend resume replays, since the device loses its
registers across a suspend the same way.

Front-panel emulation: the driver plays TotalMix's role
==========================================================

The front panel (IN/OUT/SET/MIX/SELECT/DIM buttons, the rotary
wheel) has no on-device intelligence of its own for turning a wheel
click into a mixer change - on Windows/Mac, RME's TotalMix
application polls the same 0x17 status register this driver polls,
decodes button/wheel deltas, and performs the resulting mixer writes
itself.  Standalone (no host software) mode exists on the hardware,
but the proprietary USB mode this driver targets always has a host
attached, so this driver has to do what TotalMix does: poll 0x17 on
a delayed work item (``panel_poll_ms`` module parameter, default
20 ms to match TotalMix's own ~50 Hz), decode the button flash and
signed wheel delta, and apply the resulting change (an output level
step, a preamp gain step, a monitoring level step, a phantom toggle)
exactly like the corresponding ALSA control's ``.put`` would.  A DIM
press is only counted, for a mixer application to act on.  The
front-panel ALSA
controls this driver exposes are the read side of this: a way for
userspace (WirePlumber, TuxMix) to observe what the physical panel is
doing, not a way to drive the hardware.

Some panel state - which channel SELECT currently has chosen, for
instance - is not part of the 0x17 readback at all and exists only on
the device's own internal state machine, which the driver cannot
read.  The unit keeps one such selection for each IN pair, across IN
switches and even across a power cycle; its LEDs stay dark until the
next SELECT press, which only shows the selection again, and later
presses step it.  The driver follows the presses and exposes the
selection of each pair as a "Front Panel Selection" control (index 0
is Ch 1/2), so that alsactl keeps it across boots like any other
mixer setting.  It cannot know what the unit holds before it has been
told once, so a pair starts out unknown, and SET and the wheel then do
nothing instead of acting on a channel that may not be the lit one;
setting the control to what the LEDs show for the pair tells it.  A
re-probe, which does not change the unit, keeps what the driver had,
and the older values alsactl restores shortly after probe are ignored
for a pair the driver already knows.  The stored values can be wrong
only if the selection was changed while the driver was not running
(the unit used on its own, or with another host).  The relevant code
comments (``babyface_panel_start()``, the ``panel_select_armed``
handling in ``bf_panel_tick()``) explain the details.
