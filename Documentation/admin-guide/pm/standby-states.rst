.. SPDX-License-Identifier: GPL-2.0
.. include:: <isonum.txt>

=====================
Runtime Standby States
=====================

:Copyright: |copy| 2026 Antheas Kapenekakis

:Author: Antheas Kapenekakis <lkml@antheas.dev>

This document describes the runtime standby states ABI available in the Linux
kernel, which is designed as a generic superset of the s0ix/Modern Standby
firmware notifications. Devices with these notifications support hardware states
where they look like they are asleep, while still performing basic computation.
Specifically, those are "Sleep", "Inactive", and "Active" states, with an
additional state "Resume". Transitioning between these states follows the
flowchart below.

Runtime Standby States
==================================
The following runtime standby are supported::

    <S2idle> ↔ <Sleep> ↔ <Inactive> ↔ <Active>
        →       →  <Resume>  ↑

.. _s2idle_drips:


.. _s2idle_active:

Active
------

The "Active" state is the default state of the system and the one it has when
it is turned on. It is the state where the device is on, and the user is
interacting with it.

.. _s2idle_screen_off:

Inactive
----------

The "Inactive" state is a state in which users have stopped interacting with
the device, e.g., 5 seconds after the displays have turned off due to inactivity
or due to the user pressing the power button. It is the responsibility of
userspace to keep track of user interaction so it can inform the kernel to
transition to this state. The response to this state for devices that support
is to turn off their keyboard backlight, and some might pulse their power light.

.. _s2idle_sleep:

Sleep
-----

In the sleep state, certain devices will limit their thermal envelope so it is
safe for them to be put into a bag and still perform basic computation such as
fetching email. Then, some devices will pulse their power light. Userspace can
use this state to perform basic tasks such as wake-up checks while maintaining
the appearance the device is asleep.

.. _s2idle_resume:

Resume
------

The resume state is a transient state that may only be entered from the sleep
state. It can be used to notify hardware that the device should boost its
thermal envelope as preparation for the user interacting with it. As in, it
undoes the thermal envelope effects of the "sleep" state while keeping its
appearance.

S2idle
-----

The "S2idle" state in the diagram corresponds to suspending normally by writing
``mem`` to ``/sys/power/state``. Userspace is fully frozen, and the kernel parks
the CPUs and turns off most devices. It is shown in the graph as a reference.
If the runtime standby state is not "sleep" when entering s2idle, the kernel
will first transition to "sleep" before entering s2idle.

Basic ``sysfs`` Interface for runtime standby transitions
=============================================================

The file :file:`/sys/power/standby` can be used to transition the system between
the different standby states. The file accepts the following values: ``active``,
``inactive``, ``sleep``, and ``resume``. File writes will block until the
transition completes. The system will cross all states shown in the flowchart
above to reach the desired state. It will return ``-EINVAL`` when asking for an
unsupported state or, e.g., requesting ``resume`` when not in the ``sleep``
state. If there is an error during the transition, the transition will pause on
the last error-free state and return an error.

The file can be read to retrieve the current state (and potential ones) with the
following format: ``[active] inactive sleep resume``. Only supported states
will be shown.

Userspace may transition between all supported states including s2idle
arbitrarily, except for the ``resume`` state which may only be requested from
the ``sleep`` state.