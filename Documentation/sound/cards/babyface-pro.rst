.. SPDX-License-Identifier: GPL-2.0

=================================================
RME Babyface Pro / Pro FS (snd-usb-babyface-pro)
=================================================

This document describes the design of the ``snd-usb-babyface-pro``
driver for reviewers who need the big picture before reading the
patches - what problem the driver solves, why it is a standalone
driver instead of a snd-usb-audio quirk, and the four design
decisions (stream model, protocol shape, mixer-state persistence,
front-panel emulation) that shape most of the code.  The patch series
itself is split by feature (core+PCM, then masters+crosspoint,
preamp, routing flags, suspend/resume, front panel, DSP EQ, in that
order) so each patch can be read and built on its own; this document
does not repeat what each patch's own commit message already covers.

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
0x10    settings word / stream start trigger
0x12    16-bit crosspoint and output-master writes
0x16    cold-init register clear
0x17    front-panel + preamp state (read and write)
0x1a    8-bit gain / output-master companion writes
0x1b    clock DDS quads (base rate and varispeed)
======  ========================================

The full register map, decoded from Windows USB captures and
cross-checked against hardware, lives in the driver's own development
repository (not shipped in-tree) - the constants and the comments
next to each vendor write in the source are the authoritative
in-tree reference.

The one property that shapes the rest of the driver: **almost nothing
here is readable back**.  The 0x17 request returns the front-panel
and preamp state, but the crosspoint matrix, the output masters, the
routing flags and the clock all have to be tracked host-side - the
device will accept a write blindly and never confirm what it actually
holds.  Two consequences follow directly from this:

* The ``struct snd_usb_babyface`` device state (see
  ``babyface.h``) is not a cache in the usual sense of "avoid a
  slow read" - it is the *only* record of what the hardware should
  currently hold.  Every mixer control's ``.get`` callback reads this
  state directly; none of them ever talks to the device.

* A full reset of the device's registers - which happens on every
  cold init - has to be followed by replaying the *entire* cached
  state back, in the right order, or the card comes back silent or at
  the wrong levels.  This is what ``babyface_restore_state()`` and
  ``bf_state_apply_flags()`` do (see "Mixer-state persistence" below).

The asynchronous stream model
==============================

The PCM stream is not started or stopped directly by
``.trigger()``.  Instead, ``.trigger()`` only adjusts a shared
``stream_users`` counter (0..2, one per running substream - playback
and capture share one physical stream) and schedules
``stream_work``, a work item that runs in process context because
starting a session means sleeping USB control transfers
(cold init, the session-arm sequence) followed by submitting the
interrupt URBs:

* **users 0 -> 1** (first substream starts): cold-init the device,
  send the session-start trigger pair, submit the IN/OUT URBs (always
  as a matched pair - the device does not advance the stream unless
  both directions have a pending transfer), arm the session, then
  replay the entire cached mixer state (masters, crosspoints, preamp,
  flags, pitch) since cold-init just wiped it.

* **users 1 -> 0** (last substream stops): kill the URBs and let the
  session go idle.

So the device always has exactly one live session regardless of how
many ALSA substreams are open, and a rate or format change on one
substream transparently restarts that shared session under the other
one - the other side sees a brief rate step (PipeWire's resampler
absorbs it) rather than the ``open()`` failing with ``-EBUSY``.

Mixer-state persistence across re-probes
==========================================

A userspace client can claim the proprietary interface directly via
``usbfs`` (``USBDEVFS_DISCONNECT_CLAIM``) - both PipeWire grabbing the
device for a sink and the project's own TuxMix userspace daemon do
this via libusb.  That detaches the kernel driver and the ALSA card
disappears for the duration; when the client releases the interface,
the driver re-probes.  The device keeps its register contents across
this detach, but the driver's own cold-init (required at every
session start, see above) clears them - so the driver saves the
in-memory mixer state at ``disconnect()`` and restores it at the next
``probe()``, keyed by the device's USB serial number (or its sysfs
path, if it has no serial) so the same physical unit gets its state
back across the cycle.  The same state is also what a system-suspend
resume replays, since the device loses its registers across a suspend
the same way.

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
signed wheel delta, and apply the resulting change (an output fader
step, a preamp gain step, a phantom toggle, DIM) exactly like the
corresponding ALSA control's ``.put`` would.  The front-panel ALSA
controls this driver exposes are the read side of this: a way for
userspace (WirePlumber, TuxMix) to observe what the physical panel is
doing, not a way to drive the hardware.

Some panel state - which channel SELECT currently has chosen, for
instance - is not part of the 0x17 readback at all and exists only on
the device's own internal state machine, which the driver cannot
read.  That state is tracked host-side and deliberately re-synced to
a known value (nothing selected) for the first few seconds after
probe, because a stale alsactl-restored value would otherwise
silently desync from the physical LEDs.  The relevant code comments
(``babyface_panel_start()``, the ``panel_select_armed`` handling in
``bf_panel_tick()``) explain the specific desync scenarios this
guards against.
