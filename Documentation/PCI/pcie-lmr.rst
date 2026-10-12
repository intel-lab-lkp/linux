.. SPDX-License-Identifier: GPL-2.0

====================================
PCIe Lane Margining at the Receiver
====================================

Lane Margining at the Receiver lets software move the sampling point of a
receiver away from its normal setting, in time or in voltage, while the link
stays up, and read back the number of errors the receiver sees.  It is
available on links running at 16.0 GT/s or faster, through the Lane Margining
at the Receiver Extended Capability.

With ``CONFIG_PCIE_LMR``, the kernel exposes the margining commands of each
port that has the capability in debugfs.  The kernel does not run margining
sweeps or interpret results; user-space tools do that.

debugfs layout
==============

There is one directory per port that has the capability::

  /sys/kernel/debug/pcie_lmr_<domain:bus:dev.fn>/
      enable
      receiver
      port
      lane0/
          caps
          margin_timing
          margin_voltage
          status
      lane1/
      ...

The capability is used on Downstream Ports, and on Function 0 of the device
below a Downstream Port.  There is one ``laneN`` directory for each lane of
the port's Maximum Link Width.

``enable``
  Write 1 to start a margining session on the link, 0 to end it.  Both are
  accepted when the port is already in that state.  Reads return 1 while a
  session started on this port is active, and 0 otherwise.

  Starting a session turns off the ASPM states that are enabled on the link,
  sets Hardware Autonomous Width Disable and Hardware Autonomous Speed Disable
  on both ends, keeps both ends runtime resumed, and waits for Margining
  Ready.  Ending the session undoes these steps and returns every lane that
  was margined to normal settings.

  Only one session can be active on a link at a time.  A session is ended by
  writing 0 to ``enable`` in the directory where it was started.

``receiver``
  The Receiver Number (1 to 6) that commands are sent to.  A Downstream Port
  reaches receiver 1 (its own receiver) and, when retimers are present,
  receivers 2 and 3 for the first retimer and 4 and 5 for the second.
  Function 0 below a Downstream Port reaches only receiver 6 (its own
  receiver).  A session starts with receiver 1 or 6.  Selecting another
  receiver first returns the lanes margined by the current one to normal
  settings.

``port``
  Margining Port Capabilities and Status bits: ``uses_driver_software``,
  ``margining_ready`` and ``software_ready``.  This file can be read without a
  session.

``laneN/caps``
  The parameters the selected receiver reports for the lane, one ``key:
  value`` pair per line.  The values are read from the receiver the first time
  the file is read or a step is written in a session, and again after the
  receiver changes.  ``max_lanes`` is the number of lanes that can be margined
  at the same time.

``laneN/margin_timing``, ``laneN/margin_voltage``
  Write a number of steps to move the sampling point of the selected receiver
  on this lane.  Negative values move left or down and are accepted only when
  the receiver supports independent left/right timing or up/down voltage
  margining.  Writing 0 returns the lane to normal settings; because that
  clears both offsets, the other offset is then applied again.  Reads return
  the offset that was last applied.

``laneN/status``
  The receiver's response to the last step command on this lane, as
  ``<timing|voltage> <setup|margining|too_many_errors|nak> <error count>``,
  or ``idle`` when the last command was not a step command.  Reading this
  file does not send a command.

All files are only accessible to root.

Example
=======

Measure one timing step to the right on lane 0, from a Downstream Port::

  # cd /sys/kernel/debug/pcie_lmr_0000:00:01.1
  # echo 1 > enable
  # cat lane0/caps
  receiver: 1
  ...
  # echo 1 > lane0/margin_timing
  # sleep 1; cat lane0/status
  timing margining 0
  # echo 0 > lane0/margin_timing
  # echo 0 > enable

Errors
======

Reads and writes return these errors in addition to the usual parsing errors:

=================  ===========================================================
``-ENOTCONN``      No session is active on this port.
``-EBUSY``         Another session is active on the link, the port or its
                   link partner is being probed, removed or reset (try again),
                   too many lanes are margined already, or ASPM was found
                   enabled on the link.
``-ENODEV``        The link partner is missing or was removed, or the device
                   does not respond.
``-ENOLINK``       The link is down.
``-EOPNOTSUPP``    The link runs below 16.0 GT/s, the receiver does not
                   support voltage margining, or it answered a step command
                   with a NAK.
``-ERANGE``        The step count is larger than the receiver supports.
``-EINVAL``        A negative step that the receiver does not support, or a
                   receiver number that is not reachable from this port.
``-EIO``           The receiver reported too many errors immediately on a step
                   command and was returned to normal settings, or the link
                   changed speed or width, lost Margining Ready, or went
                   through system suspend since the session started.
``-ETIMEDOUT``     Margining Software Ready is not set, Margining Ready was
                   not set within the timeout, or the receiver did not respond
                   to a command (for example on an inactive lane).
=================  ===========================================================

Limitations
===========

- ASPM is turned off and back on with ``pci_disable_link_state()`` and
  ``pci_force_enable_link_state()``, like drivers that need ASPM off for a
  while.  This round trip is not exact:

  - If another driver disables an ASPM state during a session, ending the
    session turns it back on.
  - The ASPM default for the link becomes the set of states that were enabled
    when the session started.
  - Disabling L1 also disables the L1 substates, so substates that were
    disabled at the start stay disabled.
  - Without OS control of ASPM, the states are turned off but cannot be turned
    back on, and stay off until reboot.

  If nothing was enabled when the session started, the ASPM API is not called
  at all.  Before each command the ASPM Control field of both ends is checked,
  so a session does not continue if ASPM was turned back on, for example
  through the ``link/*_aspm`` files in sysfs.

- Receivers lose their margining state over system suspend.  Commands on a
  session that was active before a suspend fail with ``-EIO``.

- One receiver is margined at a time.  The error count limit is the
  receiver's default; the kernel does not set it.

- Another program writing the Lane Margining registers directly, such as
  ``pcilmr`` or ``setpci``, will interfere with a session.
