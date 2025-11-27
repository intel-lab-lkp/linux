.. SPDX-License-Identifier: GPL-2.0

.. _per_console_loglevel:

Per-console loglevel support
============================

Motivation
----------

Consoles can have vastly different latencies and throughputs. For example,
writing a message to the serial console can take on the order of tens of
milliseconds to get the UART to successfully write a message. While this might
be fine for a single, one-off message, this can cause significant
application-level stalls in situations where the kernel writes large amounts of
information to the console.

This means that while you might want to send at least INFO level messages to
(for example) netconsole, which is relatively fast, you may only want to send at
least WARN level messages to the serial console. This permits debugging
using the serial console in cases that netconsole doesn't receive messages
during particularly bad system issues, while still keeping the noise low enough
to avoid inducing latency in userspace applications.

Loglevel
--------

Kernel loglevels are defined thus:

+---+--------------+-----------------------------------+
| 0 | KERN_EMERG   | system is unusable                |
+---+--------------+-----------------------------------+
| 1 | KERN_ALERT   | action must be taken immediately  |
+---+--------------+-----------------------------------+
| 2 | KERN_CRIT    | critical conditions               |
+---+--------------+-----------------------------------+
| 3 | KERN_ERR     | error conditions                  |
+---+--------------+-----------------------------------+
| 4 | KERN_WARNING | warning conditions                |
+---+--------------+-----------------------------------+
| 5 | KERN_NOTICE  | normal but significant condition  |
+---+--------------+-----------------------------------+
| 6 | KERN_INFO    | informational                     |
+---+--------------+-----------------------------------+
| 7 | KERN_DEBUG   | debug-level messages              |
+---+--------------+-----------------------------------+

Tunables
--------

In order to allow tuning per-console loglevels, the following controls exist:

Global
~~~~~~

The global loglevel is set by the ``kernel.console_loglevel`` sysctl, which can
also be set as ``loglevel=`` on the kernel command line.

The printk module also takes two parameters which modify this behaviour
further:

* ``ignore_loglevel`` on the kernel command line or set in printk parameters:
  Emit all messages. All other controls are ignored if this is present.

* ``ignore_per_console_loglevel`` on the kernel command line or set in printk
  parameters: Ignore all per-console loglevels and use the global loglevel.

The default value for ``kernel.console_loglevel`` comes from
``CONFIG_CONSOLE_LOGLEVEL_DEFAULT``, or ``CONFIG_CONSOLE_LOGLEVEL_QUIET`` if
``quiet`` is passed on the kernel command line.

Console attributes
~~~~~~~~~~~~~~~~~~

Registered consoles are exposed at ``/sys/class/console``. For example, if you
are using ``ttyS0``, the console backing it can be viewed at
``/sys/class/console/ttyS0/``. The following files are available:

* ``effective_loglevel`` (r): The effective loglevel after considering all
  loglevel authorities. For example, it shows the value of the console-specific
  loglevel when a console-specific loglevel is defined, and shows the global
  console loglevel value when the console-specific one is not defined.

* ``effective_loglevel_source`` (r): The loglevel authority which resulted in
  the effective loglevel being set. The following values can be present:

    * ``local``: The console-specific loglevel is in effect.

    * ``global``: The global loglevel (``kernel.console_loglevel``) is in
      effect. Set a console-specific loglevel to override it.

    * ``ignore_loglevel``: ``ignore_loglevel`` was specified on the kernel
      command line or at ``/sys/module/printk/parameters/ignore_loglevel``.
      Disable it to use level controls.

* ``loglevel`` (rw): The local, console-specific loglevel for this console.
  This will be in effect if no other global control overrides it. Look at
  ``effective_loglevel`` and ``effective_loglevel_source`` to verify that.

Examples
--------

Setting per-console loglevel at runtime
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Set serial console to only show warnings and above (level 4)::

    echo 5 > /sys/class/console/ttyS0/loglevel

Set netconsole to show info and above (level 6)::

    echo 7 > /sys/class/console/netcon0/loglevel

Reset a console to use the global loglevel::

    echo -1 > /sys/class/console/ttyS0/loglevel

Checking effective loglevel
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Check what loglevel is actually in effect for a console::

    $ cat /sys/class/console/ttyS0/effective_loglevel
    4
    $ cat /sys/class/console/ttyS0/effective_loglevel_source
    local

If the source shows ``global``, the console is using the global loglevel.
If it shows ``local``, the console is using its per-console loglevel.
If it shows ``ignore_loglevel``, all loglevel controls are being ignored.

Boot-time configuration
~~~~~~~~~~~~~~~~~~~~~~~~

Set different loglevels for different consoles at boot::

    console=ttyS0,115200n8,loglevel:3 console=tty0,loglevel:5

This sets the serial console (ttyS0) to level 3 (KERN_ERR) and the VGA
console (tty0) to level 5 (KERN_NOTICE).

For netconsole::

    netconsole=@/,@192.168.1.1/ console=netcon0,loglevel:6

Common use case - high performance with serial fallback
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A common configuration is to set netconsole to a verbose level for normal
debugging, while keeping the serial console quiet to avoid performance impact,
but still available for emergencies::

    # Netconsole gets INFO and above (verbose)
    echo 7 > /sys/class/console/netcon0/loglevel

    # Serial console gets only WARN and above (quiet, for emergencies)
    echo 5 > /sys/class/console/ttyS0/loglevel

This allows you to see informational messages on the fast netconsole without
the latency impact of writing them to the slow serial port.

Performance Impact
------------------

Kernel messages used to be flushed to the consoles immediately even from a
context where the scheduling is not possible. It increases a chance to see the
messages even when the system is in a bad state. But it might cause significant
application-level stalls (e.g., during network debugging or block I/O tracing).
Note that serial console writes can take tens of milliseconds per message.

The console drivers are being converted to nbcon API (the letter 'N'
in /proc/consoles output). These drivers write the messages in a dedicated
kthreads when the system is working properly. It reduces the risk of stalls. But
the messages are still flushed immediately when the system detects an emergency
situation, for example Oops, stall, or a warning. Also the messages can get lost
when the ring buffer is full and the console driver is far behind with flushing.

For example, setting a serial console to WARN level (4) while keeping
netconsole at INFO level (6) prevents INFO and NOTICE messages from being
written to the slow serial port. It reduces the risk of application stalls or
message loses during verbose logging periods.

Troubleshooting
---------------

Messages not appearing on console despite setting loglevel
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. Check effective loglevel source::

       cat /sys/class/console/<name>/effective_loglevel_source

   If it shows ``ignore_loglevel``, you have the ``printk.ignore_loglevel``
   kernel parameter set, which overrides all level controls. Remove it from
   your kernel command line or set it to N in sysfs::

       echo N > /sys/module/printk/parameters/ignore_loglevel

2. Check if per-console loglevels are being ignored::

       cat /sys/module/printk/parameters/ignore_per_console_loglevel

   If it shows ``Y``, per-console settings are disabled. Set it to N::

       echo N > /sys/module/printk/parameters/ignore_per_console_loglevel

3. Verify the message priority is high enough::

       cat /sys/class/console/<name>/effective_loglevel

   Messages must have priority less than this value to appear. For example,
   if effective_loglevel is 4, only messages with priority 0-3 (EMERG, ALERT,
   CRIT, ERR) will be printed. If you want to see WARN messages (priority 4),
   you need to increase the effective_loglevel to at least 5.

Cannot set loglevel to 0
~~~~~~~~~~~~~~~~~~~~~~~~~

Per-console loglevels cannot be set to 0 (KERN_EMERG). This is by design, as
level 0 is reserved for the most critical system messages that should always
go to all consoles. To use the global loglevel, set the per-console loglevel
to -1::

    echo -1 > /sys/class/console/<name>/loglevel

Edge cases
~~~~~~~~~~

**Setting all consoles to high loglevels**: If you set all consoles to
very high loglevels (e.g., 1 or 2), most messages won't appear on any
console. They remain accessible to userspace via ``dmesg`` or ``syslogd``,
but keep at least one console at a reasonable level for monitoring.

**Console unregistration while sysfs file is open**: If a console is
unregistered (e.g., module unloaded) while you have its sysfs files open,
the files will become stale. Close and reopen them, or they will eventually
return errors.

**Global loglevel changes**: If you change the global console_loglevel
via sysctl, consoles set to -1 (use global) will immediately reflect the
new level. Consoles with explicit per-console levels are unaffected.

Deprecated
~~~~~~~~~~

* ``kernel.printk`` sysctl: this takes four values, setting
  ``kernel.console_loglevel``, ``kernel.default_message_loglevel``, the minimum
  console loglevel, and a fourth unused value. The interface is generally
  considered to be quite confusing, doesn't perform checks on the values given,
  and is unaware of per-console loglevel semantics.

Chris Down <chris@chrisdown.name>, 18-November-2025
