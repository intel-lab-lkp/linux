.. SPDX-License-Identifier: (GPL-2.0+ OR MIT)

=================
Xe HWMON support
=================

The xe driver exposes hardware monitoring sensors (power, energy,
temperature, voltage and fan speed) through the kernel hwmon subsystem,
typically consumed via ``/sys/class/hwmon/hwmonX/`` or tools such as
``sensors``.

Fan speed reporting
===================

Fan speed (``fanN_input``) is reported in RPM and computed from a tach
pulse counter: the driver reads an accumulating pulse register, divides
the delta between two subsequent readings by two pulses per rotation,
and time-averages the result.

Number of fan channels
-----------------------

The number of ``fanN_input`` attributes exposed in sysfs is the fan
count returned by the ``FSC_READ_NUM_FANS`` pcode command. On DG2 this
command has been found to return an incorrect value on some boards, so
the driver hardcodes a fan count of two there. As a result up to
``fan1_input`` and ``fan2_input`` are always exposed on DG2 regardless
of how many tach lines are actually wired.

Zero RPM on DG2 is not necessarily a bug
----------------------------------------

How physical fans map onto the tach channels is left to the board
vendor. Some OEMs route several physical fans through a single shared
tach line, while others wire each fan to its own channel 1:1. The
driver has no reliable way to tell these layouts apart, and the same PCI
device ID can ship in either configuration.

When a channel has no tach line driving it, its pulse counter never
accumulates, so the corresponding ``fanN_input`` reads a constant 0 RPM.
On DG2 this is most often seen on ``fan2_input`` for boards that drive
both physical fans from a single tach line. This is expected behaviour
for such boards, not a driver fault, and reflects the board wiring
rather than a missing or stalled fan.

For this reason the fan count on DG2 is intentionally left at a flat
value rather than tracked per board: there is no driver-visible signal
that distinguishes a shared-tach layout from a genuinely silent fan.
